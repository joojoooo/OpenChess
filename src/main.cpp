#include "board_driver.h"
#include "chess_bot.h"
#include "chess_engine.h"
#include "chess_lichess.h"
#include "chess_moves.h"
#include "chess_utils.h"
#include "web_logger.h"
#ifdef CHESSCONNECT_ENABLED
#include "chessconnect/chess_connect.h"
#include "chessconnect/chessconnect_service.h"
#endif
#include "led_colors.h"
#include "move_history.h"
#include "ota_updater.h"
#include "sensor_test.h"
#include "version.h"
#include "wifi_manager_esp32.h"
#include <LittleFS.h>
#include <time.h>

// ---------------------------
// Game State and Configuration
// ---------------------------

enum GameMode {
  MODE_SELECTION = 0,
  MODE_CHESS_MOVES = 1,
  MODE_BOT = 2,
  MODE_LICHESS = 3,
  MODE_SENSOR_TEST = 4,
  MODE_CHESS_CONNECT = 5,
  MODE_CERTABO = 6
};

BotConfig botConfig = {StockfishSettings::medium(), true};
LichessConfig lichessConfig = {""};

BoardDriver boardDriver;
ChessEngine chessEngine;
MoveHistory moveHistory;
WiFiManagerESP32 wifiManager(&boardDriver, &moveHistory);
ChessMoves* chessMoves = nullptr;
ChessBot* chessBot = nullptr;
ChessLichess* chessLichess = nullptr;
#ifdef CHESSCONNECT_ENABLED
ChessConnect* chessConnect = nullptr;
#endif
SensorTest* sensorTest = nullptr;

GameMode currentMode = MODE_SELECTION;
bool modeInitialized = false;
bool resumingGame = false;
bool resetGameSelection = true;

void showGameSelection();
void handleGameSelection();
void handleBotConfigSelection();
void initializeSelectedMode(GameMode mode);
bool resumeLiveGameFromFlash(bool requireBoardMatch);

void setup() {
  Serial.begin(115200);
  delay(3000);
  webLog.println();
  webLog.println("================================================");
  webLog.println("         OpenChess Starting Up");
  webLog.println("         Firmware version: " FIRMWARE_VERSION);
  webLog.println("================================================");
  if (!ChessUtils::ensureNvsInitialized())
    webLog.println("WARNING: NVS init failed (Preferences may not work)");
  if (!LittleFS.begin(true))
    webLog.println("ERROR: LittleFS mount failed!");
  else
    webLog.println("LittleFS mounted successfully");
  moveHistory.begin();
  boardDriver.beginHardware();
#ifdef CHESSCONNECT_ENABLED
  chessConnectInit(wifiManager.getServer());
#endif
  wifiManager.begin();
  boardDriver.checkCalibration();
  configTime(0, 0, "pool.ntp.org", "time.nist.gov"); // NTP time sync (non-blocking)
  if (!resumeLiveGameFromFlash(true))
    showGameSelection();
}

bool resumeLiveGameFromFlash(bool requireBoardMatch) {
  uint8_t resumeMode = 0, resumePlayerColor = 0, resumeBotDepth = 0;
  if (moveHistory.hasLiveGame() && moveHistory.getLiveGameInfo(resumeMode, resumePlayerColor, resumeBotDepth)) {
    webLog.println("========== Live game found on flash ==========");
    GameMode nextMode = MODE_SELECTION;
    switch (resumeMode) {
      case GAME_MODE_CHESS_MOVES:
        webLog.println("Checking Chess Moves game for possible resume...");
        nextMode = MODE_CHESS_MOVES;
        break;
      case GAME_MODE_BOT:
        webLog.printf("Checking Bot game for possible resume (player=%c, depth=%d)...\n", (char)resumePlayerColor, resumeBotDepth);
        nextMode = MODE_BOT;
        botConfig.playerIsWhite = (resumePlayerColor == 'w');
        botConfig.stockfishSettings = StockfishSettings(resumeBotDepth);
        break;
      default:
        webLog.println("Unknown live game mode, discarding");
        moveHistory.discardLiveGame();
        break;
    }
    webLog.println("================================================");
    if (nextMode != MODE_SELECTION) {
      currentMode = nextMode;
      modeInitialized = false;
      resumingGame = true;
      moveHistory.setRequireBoardMatchOnResume(requireBoardMatch);
      return true;
    }
  }

  return false;
}

void loop() {
  if (wifiManager.getPendingGameResume()) {
    wifiManager.clearPendingGameResume();
    webLog.println("Resume requested from WebUI GameHistory");
    if (!resumeLiveGameFromFlash(false))
      showGameSelection();
  }

  // Check for pending board edits from WiFi (FEN-based)
  String editFen;
  if (wifiManager.getPendingBoardEdit(editFen)) {
    webLog.println("Applying board edit from WiFi interface...");
    if (currentMode == MODE_CHESS_MOVES && modeInitialized && chessMoves != nullptr) {
      chessMoves->setBoardStateFromFEN(editFen);
      webLog.println("Board edit applied to Chess Moves mode");
    } else if (currentMode == MODE_BOT && modeInitialized && chessBot != nullptr) {
      chessBot->setBoardStateFromFEN(editFen);
      webLog.println("Board edit applied to Chess Bot mode");
    } else if (currentMode == MODE_LICHESS && modeInitialized && chessLichess != nullptr) {
      chessLichess->setBoardStateFromFEN(editFen);
      webLog.println("Board edit applied to Lichess mode");
#ifdef CHESSCONNECT_ENABLED
    } else if (currentMode == MODE_CHESS_CONNECT && modeInitialized && chessConnect != nullptr) {
      chessConnect->setBoardStateFromFEN(editFen);
      webLog.println("Board edit applied to ChessConnect mode");
#endif
    } else {
      webLog.println("Warning: Board edit received but no active game mode");
    }

    wifiManager.clearPendingEdit();
  }

  // Check for pending resign from WiFi
  char resignColor;
  if (wifiManager.getPendingResign(resignColor)) {
    webLog.printf("Processing resign from web UI: %c resigns\n", resignColor);
    if (currentMode == MODE_CHESS_MOVES && modeInitialized && chessMoves != nullptr) {
      chessMoves->resignGame(resignColor);
    } else if (currentMode == MODE_BOT && modeInitialized && chessBot != nullptr) {
      chessBot->resignGame(resignColor);
    } else if (currentMode == MODE_LICHESS && modeInitialized && chessLichess != nullptr) {
      chessLichess->resignGame(resignColor);
#ifdef CHESSCONNECT_ENABLED
    } else if (currentMode == MODE_CHESS_CONNECT && modeInitialized && chessConnect != nullptr) {
      chessConnect->resignGame(resignColor);
#endif
    } else {
      webLog.println("Warning: Resign received but no active game mode");
    }
    wifiManager.clearPendingResign();
  }

  // Check for pending draw from WiFi
  if (wifiManager.getPendingDraw()) {
    webLog.println("Processing draw from web UI");
    if (currentMode == MODE_CHESS_MOVES && modeInitialized && chessMoves != nullptr) {
      chessMoves->drawGame();
    } else if (currentMode == MODE_BOT && modeInitialized && chessBot != nullptr) {
      chessBot->drawGame();
    } else if (currentMode == MODE_LICHESS && modeInitialized && chessLichess != nullptr) {
      chessLichess->drawGame();
#ifdef CHESSCONNECT_ENABLED
    } else if (currentMode == MODE_CHESS_CONNECT && modeInitialized && chessConnect != nullptr) {
      chessConnect->drawGame();
#endif
    } else {
      webLog.println("Warning: Draw received but no active game mode");
    }
    wifiManager.clearPendingDraw();
  }

  // Check for WiFi game selection
  int selectedMode = wifiManager.getSelectedGameMode();
  if (selectedMode > 0) {
    webLog.printf("WiFi game selection detected: %d\n", selectedMode);
    switch (selectedMode) {
      case 1:
        currentMode = MODE_CHESS_MOVES;
        break;
      case 2:
        currentMode = MODE_BOT;
        botConfig = wifiManager.getBotConfig();
        break;
      case 3:
        currentMode = MODE_LICHESS;
        lichessConfig = wifiManager.getLichessConfig();
        break;
      case 4:
        currentMode = MODE_SENSOR_TEST;
        break;
      default:
        webLog.println("Invalid game mode selected via WiFi");
        selectedMode = 0;
        break;
    }
    if (selectedMode > 0) {
      modeInitialized = false;
      wifiManager.resetGameSelection();
      boardDriver.clearAllLEDs();
    }
  }

#ifdef CHESSCONNECT_ENABLED
  BLEBoard* activeBleBoard = chessConnectGetActiveBoard();
  if (activeBleBoard && activeBleBoard->usesPresenceDumbMode() && activeBleBoard->isConnected()) {
    if (currentMode == MODE_SELECTION) {
      webLog.println("[BLE] Certabo dumb e-board connected");
      currentMode = MODE_CERTABO;
      modeInitialized = false;
      boardDriver.clearAllLEDs();
    }
  }

  if (chessConnectHasNewGame()) {
    if (currentMode == MODE_SELECTION || currentMode == MODE_CHESS_CONNECT) {
      webLog.println("[BLE] New game from ChessConnect");
      currentMode = MODE_CHESS_CONNECT;
      modeInitialized = false;
      boardDriver.clearAllLEDs();
    } else {
      webLog.println("[BLE] New game from ChessConnect - waiting for current game to end");
    }
  }
#endif

  if (currentMode == MODE_SELECTION) {
    handleGameSelection();
    return;
  }
  // Game mode selected
  if (!modeInitialized) {
    initializeSelectedMode(currentMode);
    modeInitialized = true;
    delay(1); // HACK: Ensure any starting animations acquire the LED mutex before proceeding
  }

  switch (currentMode) {
    case MODE_CHESS_MOVES:
      if (chessMoves != nullptr) {
        if (chessMoves->isGameOver())
          showGameSelection();
        else
          chessMoves->update();
      }
      break;
    case MODE_BOT:
      if (chessBot != nullptr) {
        if (chessBot->isGameOver())
          showGameSelection();
        else
          chessBot->update();
      }
      break;
    case MODE_LICHESS:
      if (chessLichess != nullptr) {
        if (chessLichess->isGameOver())
          showGameSelection();
        else
          chessLichess->update();
      }
      break;
    case MODE_SENSOR_TEST:
      if (sensorTest != nullptr) {
        if (sensorTest->isComplete())
          showGameSelection();
        else
          sensorTest->update();
      }
      break;
#ifdef CHESSCONNECT_ENABLED
    case MODE_CHESS_CONNECT:
      if (chessConnect != nullptr) {
        if (chessConnect->isGameOver())
          showGameSelection();
        else
          chessConnect->update();
      }
      break;
    case MODE_CERTABO: {
      BLEBoard* active = chessConnectGetActiveBoard();
      if (!active || !active->usesPresenceDumbMode() || !active->isConnected()) {
        showGameSelection();
      } else {
        active->updatePresence(boardDriver);
      }
      break;
    }
#endif
    default:
      showGameSelection();
      break;
  }

  delay(SENSOR_READ_DELAY_MS);
}

void showGameSelection() {
  currentMode = MODE_SELECTION;
  modeInitialized = false;
  resetGameSelection = true;
  boardDriver.acquireLEDs();
  boardDriver.clearAllLEDs(false);
  // Light up the 4 selector positions in the middle of the board
  // Position 1: Chess Moves (row 3, col 3) - Blue
  boardDriver.setSquareLED(3, 3, LedColors::Blue);
  // Position 2: Chess Bot (row 3, col 4) - Green
  boardDriver.setSquareLED(3, 4, LedColors::Green);
  // Position 3: Lichess (row 4, col 3) - Yellow
  boardDriver.setSquareLED(4, 3, LedColors::Yellow);
  // Position 4: Sensor Test (row 4, col 4) - Red
  boardDriver.setSquareLED(4, 4, LedColors::Red);
  boardDriver.showLEDs();
  boardDriver.releaseLEDs();
  webLog.println("=============== Game Selection Mode ===============");
  webLog.println("Four LEDs are lit in the center of the board:");
  webLog.println("  Blue:   Chess Moves (Human vs Human)");
  webLog.println("  Green:  Chess Bot (Human vs AI)");
  webLog.println("  Yellow: Lichess (Play online games)");
  webLog.println("  Red:    Sensor Test");
  webLog.println("Place any chess piece on a LED to select that mode");
  webLog.println("===================================================");
}

void handleGameSelection() {
  boardDriver.readSensors();
  bool currState[4] = {boardDriver.getSensorState(3, 3), boardDriver.getSensorState(3, 4), boardDriver.getSensorState(4, 3), boardDriver.getSensorState(4, 4)};

  struct SelectorState {
    int emptyCount;
    int occupiedCount;
    bool readyForSelection;
  };
  const int DEBOUNCE_CYCLES = (DEBOUNCE_MS / SENSOR_READ_DELAY_MS) + 2;
  static SelectorState selectorStates[4] = {};

  if (resetGameSelection) {
    for (int i = 0; i < 4; ++i) {
      selectorStates[i].emptyCount = 0;
      selectorStates[i].occupiedCount = 0;
      selectorStates[i].readyForSelection = false;
    }
    resetGameSelection = false;
  }
  for (int i = 0; i < 4; ++i) {
    if (!currState[i]) {
      if (selectorStates[i].emptyCount < DEBOUNCE_CYCLES)
        selectorStates[i].emptyCount++;
      selectorStates[i].occupiedCount = 0;
      if (selectorStates[i].emptyCount >= DEBOUNCE_CYCLES)
        selectorStates[i].readyForSelection = true;
    } else {
      selectorStates[i].emptyCount = 0;
      if (selectorStates[i].readyForSelection) {
        if (selectorStates[i].occupiedCount < DEBOUNCE_CYCLES)
          selectorStates[i].occupiedCount++;
      } else {
        selectorStates[i].occupiedCount = 0;
      }
    }
  }

  // Check for valid rising edge (empty for DEBOUNCE_CYCLES, then occupied for DEBOUNCE_CYCLES)
  for (int i = 0; i < 4; ++i) {
    if (selectorStates[i].readyForSelection && selectorStates[i].occupiedCount >= DEBOUNCE_CYCLES) {
      switch (i) {
        case 0:
          webLog.println("Mode: 'Chess Moves' selected!");
          currentMode = MODE_CHESS_MOVES;
          modeInitialized = false;
          boardDriver.clearAllLEDs();
          break;
        case 1:
          webLog.println("Mode: 'Chess Bot' Selected! Showing bot configuration...");
          currentMode = MODE_BOT;
          modeInitialized = false;
          boardDriver.clearAllLEDs();
          handleBotConfigSelection();
          break;
        case 2:
          webLog.println("Mode: 'Lichess' Selected!");
          currentMode = MODE_LICHESS;
          modeInitialized = false;
          boardDriver.clearAllLEDs();
          lichessConfig = wifiManager.getLichessConfig();
          break;
        case 3:
          webLog.println("Mode: 'Sensor Test' Selected!");
          currentMode = MODE_SENSOR_TEST;
          modeInitialized = false;
          boardDriver.clearAllLEDs();
          break;
      }
      break;
    }
  }

  delay(SENSOR_READ_DELAY_MS);
}

void initializeSelectedMode(GameMode mode) {
  if (resumingGame)
    resumingGame = false;
  else if (moveHistory.hasLiveGame())
    moveHistory.finishGame(RESULT_IN_PROGRESS, '?');

  switch (mode) {
    case MODE_CHESS_MOVES:
      webLog.println("Starting 'Chess Moves'...");
      if (chessMoves != nullptr)
        delete chessMoves;
      chessMoves = new ChessMoves(&boardDriver, &chessEngine, &wifiManager, &moveHistory);
      chessMoves->begin();
      break;
    case MODE_BOT:
      webLog.printf("Starting 'Chess Bot' (Depth: %d, Player is %s)...\n", botConfig.stockfishSettings.depth, botConfig.playerIsWhite ? "White" : "Black");
      if (chessBot != nullptr)
        delete chessBot;
      chessBot = new ChessBot(&boardDriver, &chessEngine, &wifiManager, &moveHistory, botConfig);
      chessBot->begin();
      break;
    case MODE_LICHESS:
      webLog.println("Starting 'Lichess Mode'...");
      if (chessLichess != nullptr)
        delete chessLichess;
      chessLichess = new ChessLichess(&boardDriver, &chessEngine, &wifiManager, lichessConfig);
      chessLichess->begin();
      break;
    case MODE_SENSOR_TEST:
      webLog.println("Starting 'Sensor Test'...");
      if (sensorTest != nullptr)
        delete sensorTest;
      sensorTest = new SensorTest(&boardDriver);
      sensorTest->begin();
      break;
#ifdef CHESSCONNECT_ENABLED
    case MODE_CHESS_CONNECT:
      webLog.println("Starting 'ChessConnect Mode'...");
      if (chessConnect != nullptr)
        delete chessConnect;
      chessConnect = new ChessConnect(&boardDriver, &chessEngine, &wifiManager);
      chessConnect->begin();
      break;
    case MODE_CERTABO:
      webLog.println("Starting 'Certabo dumb e-board Mode'...");
      boardDriver.clearAllLEDs();
      break;
#endif
    default:
      showGameSelection();
      break;
  }
}

void handleBotConfigSelection() {
  webLog.println("====== Bot Configuration Selection ======");
  webLog.println("Select Bot Color:");
  webLog.println("- Rank 6: Bot is Black");
  webLog.println("- Rank 3: Bot is White");
  webLog.println("Select Difficulty:");
  webLog.println("- File B: Easy");
  webLog.println("- File D: Medium");
  webLog.println("- File F: Hard");
  webLog.println("- File H: Expert");
  webLog.println("Example: Place piece at Rank 3, File D = White Bot Medium");

  boardDriver.acquireLEDs();
  // Easy (col 1) - Green
  boardDriver.setSquareLED(2, 1, LedColors::Green);
  boardDriver.setSquareLED(5, 1, LedColors::Green);

  // Medium (col 3) - Orange/Gold
  boardDriver.setSquareLED(2, 3, LedColors::Yellow);
  boardDriver.setSquareLED(5, 3, LedColors::Yellow);

  // Hard (col 5) - Red
  boardDriver.setSquareLED(2, 5, LedColors::Red);
  boardDriver.setSquareLED(5, 5, LedColors::Red);

  // Expert (col 7) - Purple
  boardDriver.setSquareLED(2, 7, LedColors::Purple);
  boardDriver.setSquareLED(5, 7, LedColors::Purple);

  boardDriver.showLEDs();
  boardDriver.releaseLEDs();

  // Wait for selection
  webLog.println("Waiting for bot configuration selection...");

  static bool prevState[2][8] = {};
  bool firstLoop = true;

  while (true) {
    boardDriver.readSensors();

    for (int rowIdx = 0; rowIdx < 2; ++rowIdx) {
      int row = (rowIdx == 0) ? 2 : 5;
      for (int col : {1, 3, 5, 7}) {
        bool curr = boardDriver.getSensorState(row, col);
        // Only accept selection if square was previously empty and is now occupied
        if (!firstLoop && !prevState[rowIdx][col] && curr) {
          botConfig.playerIsWhite = (row == 2);
          const char* colorName = botConfig.playerIsWhite ? "White" : "Black";
          if (col == 1) {
            botConfig.stockfishSettings = StockfishSettings::easy();
            webLog.printf("Configuration: Play as %s, Easy difficulty\n", colorName);
          } else if (col == 3) {
            botConfig.stockfishSettings = StockfishSettings::medium();
            webLog.printf("Configuration: Play as %s, Medium difficulty\n", colorName);
          } else if (col == 5) {
            botConfig.stockfishSettings = StockfishSettings::hard();
            webLog.printf("Configuration: Play as %s, Hard difficulty\n", colorName);
          } else if (col == 7) {
            botConfig.stockfishSettings = StockfishSettings::expert();
            webLog.printf("Configuration: Play as %s, Expert difficulty\n", colorName);
          }
          firstLoop = true;
          boardDriver.clearAllLEDs();
          return;
        }
        prevState[rowIdx][col] = curr;
      }
    }

    firstLoop = false;
    delay(SENSOR_READ_DELAY_MS);
  }
}
