#include "chess_bot.h"
#include "chess_utils.h"
#include "led_colors.h"
#include "move_history.h"
#include "stockfish_api.h"
#include "web_logger.h"
#include "wifi_manager_esp32.h"
#include <Arduino.h>

ChessBot::ChessBot(BoardDriver* bd, ChessEngine* ce, WiFiManagerESP32* wm, MoveHistory* mh, BotConfig cfg) : ChessGame(bd, ce, wm, mh), botConfig(cfg), currentEvaluation(0.0) {}

void ChessBot::begin() {
  webLog.println("=== Starting Chess Bot Mode ===");
  webLog.printf("Player plays: %s\n", botConfig.playerIsWhite ? "White" : "Black");
  webLog.printf("Bot plays: %s\n", botConfig.playerIsWhite ? "Black" : "White");
  webLog.printf("Bot Difficulty: Depth %d, Timeout %dms\n", botConfig.stockfishSettings.depth, botConfig.stockfishSettings.timeoutMs);
  webLog.println("====================================");
  if (wifiManager->ensureConnected()) {
    initializeBoard();
    if (moveHistory->hasLiveGame()) {
      if (!resumeLiveGameIfBoardMatches()) {
        gameOver = true;
        return;
      }
    } else {
      moveHistory->startGame(GAME_MODE_BOT, botConfig.playerIsWhite ? 'w' : 'b', (uint8_t)botConfig.stockfishSettings.depth);
      moveHistory->addFen(currentFEN());
    }
    waitForBoardSetup(board);
  } else {
    webLog.println("Failed to connect to WiFi. Bot mode unavailable.");
    boardDriver->flashBoardAnimation(LedColors::Red);
    gameOver = true;
    return;
  }
}

void ChessBot::update() {
  if (gameOver)
    return;

  boardDriver->readSensors();

  // Check for physical resign/draw gesture (both kings lifted)
  if (checkPhysicalResignOrDraw()) return;

  if ((botConfig.playerIsWhite && currentTurn == 'w') || (!botConfig.playerIsWhite && currentTurn == 'b')) {
    // Player's turn
    int fromRow, fromCol, toRow, toCol;
    char piece;
    if (tryPlayerMove(currentTurn, fromRow, fromCol, toRow, toCol)) {
      applyMove(fromRow, fromCol, toRow, toCol);
      updateGameStatus();
      wifiManager->updateBoardState(currentFEN(), currentEvaluation);
    }
  } else {
    // Bot's turn
    if (makeBotMove()) {
      updateGameStatus();
      wifiManager->updateBoardState(currentFEN(), currentEvaluation);
    }
  }

  boardDriver->updateSensorPrev();
}

String ChessBot::makeStockfishRequest(const String& fen) {
  WiFiSSLClient client;
  // Set insecure mode for SSL (or add proper certificate validation)
  client.setInsecure();
  String path = StockfishAPI::buildRequestURL(fen, botConfig.stockfishSettings.depth);
  webLog.println("Stockfish request: " STOCKFISH_API_URL + path);
  // Retry logic
  for (int attempt = 1; attempt <= botConfig.stockfishSettings.maxRetries; attempt++) {
    if (attempt > 1)
      webLog.println("Attempt: " + String(attempt) + "/" + String(botConfig.stockfishSettings.maxRetries));
    if (client.connect(STOCKFISH_API_URL, STOCKFISH_API_PORT)) {
      client.println("GET " + path + " HTTP/1.1");
      client.println("Host: " STOCKFISH_API_URL);
      client.println("Connection: close");
      client.println();
      // Wait for response
      unsigned long startTime = millis();
      String response = "";
      bool gotResponse = false;
      while (client.connected() && (millis() - startTime < botConfig.stockfishSettings.timeoutMs)) {
        if (client.available()) {
          response = client.readString();
          gotResponse = true;
          break;
        }
        delay(10);
      }
      client.stop();

      if (gotResponse && response.length() > 0)
        return response;
    }

    webLog.println("API request timeout or empty response");
    if (attempt < botConfig.stockfishSettings.maxRetries) {
      webLog.println("Retrying...");
      delay(500);
    }
  }

  webLog.println("All API request attempts failed");
  return "";
}

bool ChessBot::parseStockfishResponse(const String& response, String& bestMove, float& evaluation) {
  StockfishResponse stockfishResp;
  if (!StockfishAPI::parseResponse(response, stockfishResp)) {
    webLog.printf("Failed to parse Stockfish response: %s\n", stockfishResp.errorMessage.c_str());
    return false;
  }
  bestMove = stockfishResp.bestMove;
  if (stockfishResp.hasMate) {
    webLog.printf("Mate in %d moves\n", stockfishResp.mateInMoves);
    // Convert mate to a large evaluation (positive or negative based on direction)
    evaluation = stockfishResp.mateInMoves > 0 ? 100.0f : -100.0f;
  } else {
    // Regular evaluation (already in pawns from API)
    evaluation = stockfishResp.evaluation;
  }
  return true;
}

bool ChessBot::makeBotMove() {
  webLog.println("=== BOT MOVE CALCULATION ===");
  std::atomic<bool>* stopAnimation = boardDriver->startThinkingAnimation();
  String bestMove;
  String response = makeStockfishRequest(currentFEN());
  if (stopAnimation) stopAnimation->store(true);
  if (parseStockfishResponse(response, bestMove, currentEvaluation)) {
    webLog.println("=== STOCKFISH EVALUATION ===");
    webLog.printf("%s advantage: %.2f pawns\n", currentEvaluation > 0 ? "White" : "Black", currentEvaluation);

    int fromRow, fromCol, toRow, toCol;
    char promotion;
    if (ChessUtils::parseUCIMove(bestMove, fromRow, fromCol, toRow, toCol, promotion)) {
      webLog.printf("Stockfish UCI move: %s = (%d,%d) -> (%d,%d)%s%c\n", bestMove.c_str(), fromRow, fromCol, toRow, toCol, promotion == ' ' ? "" : " Promotion to: ", promotion);
      webLog.println("============================");
      // Verify the move is from the correct color piece
      char piece = board[fromRow][fromCol];
      bool botPlaysWhite = !botConfig.playerIsWhite;
      bool isBotPiece = (botPlaysWhite && piece >= 'A' && piece <= 'Z') || (!botPlaysWhite && piece >= 'a' && piece <= 'z');
      if (!isBotPiece) {
        webLog.printf("ERROR: Bot tried to move a %s piece, but bot plays %s. Piece at source: %c\n", (piece >= 'A' && piece <= 'Z') ? "WHITE" : "BLACK", botPlaysWhite ? "WHITE" : "BLACK", piece);
        delay(1000);
        return false;
      }
      if (piece == ' ') {
        webLog.println("ERROR: Bot tried to move from an empty square!");
        delay(1000);
        return false;
      }
      applyMove(fromRow, fromCol, toRow, toCol, (bestMove.length() >= 5) ? bestMove[4] : ' ', true);
      return true;
    }
    webLog.println("Failed to parse Stockfish UCI move: " + bestMove);
    delay(1000);
    return false;
  }
  return false;
}

void ChessBot::waitForRemoteMoveCompletion(int fromRow, int fromCol, int toRow, int toCol, bool isCapture, bool isEnPassant, int enPassantCapturedPawnRow) {
  boardDriver->acquireLEDs();
  boardDriver->clearAllLEDs(false);
  // Show source square (where to pick up from)
  boardDriver->setSquareLED(fromRow, fromCol, LedColors::Cyan);
  // Show destination square (where to place)
  if (isCapture)
    boardDriver->setSquareLED(toRow, toCol, LedColors::Red);
  else
    boardDriver->setSquareLED(toRow, toCol, LedColors::White);
  if (isEnPassant)
    boardDriver->setSquareLED(enPassantCapturedPawnRow, toCol, LedColors::Purple);
  boardDriver->showLEDs();

  bool piecePickedUp = false;
  bool capturedPieceRemoved = false;
  bool moveCompleted = false;

  webLog.println("Waiting for you to complete the remote move...");

  while (!moveCompleted) {
    boardDriver->readSensors();

    // For capture moves, ensure captured piece is removed first
    // For en passant, check the actual captured pawn square (not the destination)
    if (isCapture && !capturedPieceRemoved) {
      int captureCheckRow = isEnPassant ? enPassantCapturedPawnRow : toRow;
      if (!boardDriver->getSensorState(captureCheckRow, toCol)) {
        capturedPieceRemoved = true;
        if (isEnPassant)
          webLog.println("En passant captured pawn removed, now complete the move...");
        else
          webLog.println("Captured piece removed, now complete the move...");
      }
    }

    // Check if piece was picked up from source
    if (!piecePickedUp && !boardDriver->getSensorState(fromRow, fromCol)) {
      piecePickedUp = true;
      webLog.println("Piece picked up, now place it on the destination...");
    }

    // Check if piece was placed on destination
    // For captures: wait until captured piece is removed AND piece is placed
    // For normal moves: just wait for piece to be placed
    if (piecePickedUp && boardDriver->getSensorState(toRow, toCol))
      if (!isCapture || (isCapture && capturedPieceRemoved)) {
        moveCompleted = true;
        webLog.println("Move completed on physical board!");
      }

    delay(SENSOR_READ_DELAY_MS);
    boardDriver->updateSensorPrev();
  }

  boardDriver->clearAllLEDs();
  boardDriver->releaseLEDs();
}
