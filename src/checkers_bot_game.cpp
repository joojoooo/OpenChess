#include "checkers_bot_game.h"
#include "checkers_ui.h"
#include "led_colors.h"
#include "web_logger.h"
#include <Arduino.h>
#include <string.h>

const char CheckersBotGame::INITIAL_BOARD[8][8] = {
    {' ', 'b', ' ', 'b', ' ', 'b', ' ', 'b'},
    {'b', ' ', 'b', ' ', 'b', ' ', 'b', ' '},
    {' ', 'b', ' ', 'b', ' ', 'b', ' ', 'b'},
    {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '},
    {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '},
    {'w', ' ', 'w', ' ', 'w', ' ', 'w', ' '},
    {' ', 'w', ' ', 'w', ' ', 'w', ' ', 'w'},
    {'w', ' ', 'w', ' ', 'w', ' ', 'w', ' '}};

CheckersBotGame::CheckersBotGame(BoardDriver* bd, const StockchickenConfig& config) : boardDriver(bd), bot(config), currentTurn('w'), gameOver(false) {}

void CheckersBotGame::begin() {
  webLog.println("=== Starting Checkers vs Stockchicken ===");
  webLog.println("You play White (near rows). Stockchicken plays Black.");
  initializeBoard();

  webLog.println("Set up checkers: 12 white pieces on the near rows, 12 black on the far rows (dark squares only)");
  CheckersUI::waitForBoardSetup(boardDriver, INITIAL_BOARD);
  webLog.println("Checkers board ready. White moves first.");
}

void CheckersBotGame::initializeBoard() {
  memcpy(board, INITIAL_BOARD, sizeof(INITIAL_BOARD));
  currentTurn = 'w';
  gameOver = false;
}

void CheckersBotGame::update() {
  if (gameOver)
    return;

  boardDriver->readSensors();
  bool completed = (currentTurn == 'w') ? CheckersUI::playHumanTurn(boardDriver, engine, board, 'w', quitState) : playBotTurn();
  boardDriver->updateSensorPrev();

  if (!completed) {
    gameOver = true;
    return;
  }

  char next = CheckersUI::oppositeColor(currentTurn);
  if (engine.hasNoPieces(board, next) || !engine.hasAnyLegalMove(board, next)) {
    CheckersUI::announceGameOver(boardDriver, currentTurn);
    gameOver = true;
  } else {
    currentTurn = next;
  }
}

bool CheckersBotGame::playBotTurn() {
  Stockchicken::Turn turn;
  if (!bot.chooseTurn(board, 'b', turn)) {
    // No legal move - the game-over check in update() will catch this.
    return true;
  }

  for (int i = 0; i < turn.stepCount; i++) {
    const Stockchicken::Step& step = turn.steps[i];
    const CheckersEngine::Move& move = step.move;
    char piece = board[step.fromRow][step.fromCol];
    bool promotes = engine.isPromotion(piece, move.toRow);

    webLog.printf("Stockchicken: %c%d -> %c%d%s\n", (char)('a' + step.fromCol), 8 - step.fromRow, (char)('a' + move.toCol), 8 - move.toRow, move.isCapture ? " (capture)" : "");

    // Show the move: cyan = pick this piece up, green/blue = where to place
    // it (blue if it crowns), red = piece to remove on a capture.
    boardDriver->acquireLEDs();
    boardDriver->clearAllLEDs(false);
    boardDriver->setSquareLED(step.fromRow, step.fromCol, LedColors::Cyan);
    boardDriver->setSquareLED(move.toRow, move.toCol, promotes ? LedColors::Blue : LedColors::Green);
    if (move.isCapture)
      boardDriver->setSquareLED(move.capturedRow, move.capturedCol, LedColors::Red);
    boardDriver->showLEDs();
    boardDriver->releaseLEDs();

    // Wait for the human to physically perform this step.
    for (;;) {
      boardDriver->readSensors();

      if (CheckersUI::checkPhysicalQuit(boardDriver, quitState)) {
        boardDriver->clearAllLEDs();
        return false;
      }

      bool lifted = !boardDriver->getSensorState(step.fromRow, step.fromCol);
      bool placed = boardDriver->getSensorState(move.toRow, move.toCol);
      bool captureCleared = !move.isCapture || !boardDriver->getSensorState(move.capturedRow, move.capturedCol);
      if (lifted && placed && captureCleared)
        break;
      delay(SENSOR_READ_DELAY_MS);
    }

    board[step.fromRow][step.fromCol] = ' ';
    if (move.isCapture)
      board[move.capturedRow][move.capturedCol] = ' ';
    board[move.toRow][move.toCol] = promotes ? 'B' : piece;
    if (promotes)
      webLog.println("Stockchicken: piece crowned!");

    boardDriver->acquireLEDs();
    boardDriver->clearAllLEDs();
    boardDriver->releaseLEDs();
    boardDriver->updateSensorPrev();
  }

  return true;
}
