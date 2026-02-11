#include "chess_bot.h"
#include "chess_utils.h"
#include "led_colors.h"
#include "stockfish_api.h"
#include "wifi_manager_esp32.h"
#include <Arduino.h>

ChessBot::ChessBot(BoardDriver* bd, ChessEngine* ce, WiFiManagerESP32* wm, BotConfig cfg) : ChessGame(bd, ce, wm), botConfig(cfg), currentEvaluation(0.0) {}

void ChessBot::begin() {
  Serial.println("=== Starting Chess Bot Mode ===");
  Serial.printf("Player plays: %s\n", botConfig.playerIsWhite ? "White" : "Black");
  Serial.printf("Bot plays: %s\n", botConfig.playerIsWhite ? "Black" : "White");
  Serial.printf("Bot Difficulty: Depth %d, Timeout %dms\n", botConfig.stockfishSettings.depth, botConfig.stockfishSettings.timeoutMs);
  Serial.println("====================================");
  if (wifiManager->connectToWiFi(wifiManager->getWiFiSSID(), wifiManager->getWiFiPassword())) {
    initializeBoard();
    waitForBoardSetup();
  } else {
    Serial.println("Failed to connect to WiFi. Bot mode unavailable.");
    boardDriver->flashBoardAnimation(LedColors::Red);
    gameOver = true;
    return;
  }
}

void ChessBot::update() {
  if (gameOver)
    return;

  boardDriver->readSensors();

  if ((botConfig.playerIsWhite && currentTurn == 'w') || (!botConfig.playerIsWhite && currentTurn == 'b')) {
    // Player's turn
    int fromRow, fromCol, toRow, toCol;
    char piece;
    if (tryPlayerMove(currentTurn, fromRow, fromCol, toRow, toCol, piece)) {
      processPlayerMove(fromRow, fromCol, toRow, toCol, piece);
      updateGameStatus();
      wifiManager->updateBoardState(ChessUtils::boardToFEN(board, currentTurn, chessEngine), currentEvaluation);
    }
  } else {
    // Bot's turn
    makeBotMove();
    updateGameStatus();
    wifiManager->updateBoardState(ChessUtils::boardToFEN(board, currentTurn, chessEngine), currentEvaluation);
  }

  boardDriver->updateSensorPrev();
}

String ChessBot::makeStockfishRequest(String fen) {
  WiFiSSLClient client;
  // Set insecure mode for SSL (or add proper certificate validation)
  client.setInsecure();
  String path = StockfishAPI::buildRequestURL(fen, botConfig.stockfishSettings.depth);
  Serial.println("Stockfish request: " STOCKFISH_API_URL + path);
  // Retry logic
  for (int attempt = 1; attempt <= botConfig.stockfishSettings.maxRetries; attempt++) {
    if (attempt > 1)
      Serial.println("Attempt: " + String(attempt) + "/" + String(botConfig.stockfishSettings.maxRetries));
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

    Serial.println("API request timeout or empty response");
    if (attempt < botConfig.stockfishSettings.maxRetries) {
      Serial.println("Retrying...");
      delay(500);
    }
  }

  Serial.println("All API request attempts failed");
  return "";
}

bool ChessBot::parseStockfishResponse(String response, String& bestMove, float& evaluation) {
  StockfishResponse stockfishResp;
  if (!StockfishAPI::parseResponse(response, stockfishResp)) {
    Serial.printf("Failed to parse Stockfish response: %s\n", stockfishResp.errorMessage.c_str());
    return false;
  }
  bestMove = stockfishResp.bestMove;
  if (stockfishResp.hasMate) {
    Serial.printf("Mate in %d moves\n", stockfishResp.mateInMoves);
    // Convert mate to a large evaluation (positive or negative based on direction)
    evaluation = stockfishResp.mateInMoves > 0 ? 100.0f : -100.0f;
  } else {
    // Regular evaluation (already in pawns from API)
    evaluation = stockfishResp.evaluation;
  }
  return true;
}

void ChessBot::makeBotMove() {
  Serial.println("=== BOT MOVE CALCULATION ===");
  std::atomic<bool>* stopAnimation = boardDriver->startThinkingAnimation();
  String bestMove;
  String response = makeStockfishRequest(ChessUtils::boardToFEN(board, currentTurn, chessEngine));
  stopAnimation->store(true);
  if (parseStockfishResponse(response, bestMove, currentEvaluation)) {
    Serial.println("=== STOCKFISH EVALUATION ===");
    Serial.printf("%s advantage: %.2f pawns\n", currentEvaluation > 0 ? "White" : "Black", currentEvaluation);

    int fromRow, fromCol, toRow, toCol;
    String validationError;
    if (StockfishAPI::validateUCIMove(bestMove, validationError, fromRow, fromCol, toRow, toCol)) {
      Serial.printf("Move string: %s Parsed: %c%c -> %c%c | Array coords: (%d,%d) to (%d,%d)", bestMove.c_str(), bestMove[0], bestMove[1], bestMove[2], bestMove[3], fromRow, fromCol, toRow, toCol);
      
      // Verify the move is from the correct color piece
      char piece = board[fromRow][fromCol];
      bool botPlaysWhite = !botConfig.playerIsWhite;
      bool isBotPiece = (botPlaysWhite && piece >= 'A' && piece <= 'Z') || (!botPlaysWhite && piece >= 'a' && piece <= 'z');
      if (!isBotPiece) {
        Serial.printf("ERROR: Bot tried to move a %s piece, but bot plays %s. Piece at source: %c\n", (piece >= 'A' && piece <= 'Z') ? "WHITE" : "BLACK", botPlaysWhite ? "WHITE" : "BLACK", piece);
        return;
      }
      if (piece == ' ') {
        Serial.println("ERROR: Bot tried to move from an empty square!");
        return;
      }
      executeOpponentMove(fromRow, fromCol, toRow, toCol);
    } else {
      Serial.printf("Failed to parse bot move - %s\n", validationError.c_str());
    }
  }
}

// TODO: Merge with `processPlayerMove` to avoid code duplication and rename it to `processMove`
void ChessBot::executeOpponentMove(int fromRow, int fromCol, int toRow, int toCol) {
  char piece = board[fromRow][fromCol];
  char capturedPiece = board[toRow][toCol];

  updateEnPassantTarget(fromRow, fromCol, toRow, piece);
  bool isEnPassantCapture = ChessUtils::isEnPassantMove(fromRow, fromCol, toRow, toCol, piece, capturedPiece);
  int capturedPawnRow = ChessUtils::getEnPassantCapturedPawnRow(toRow, piece);
  if (isEnPassantCapture) {
    capturedPiece = applyEnPassant(toRow, toCol, piece);
  }

  chessEngine->updateHalfmoveClock(piece, capturedPiece);

  board[toRow][toCol] = piece;
  board[fromRow][fromCol] = ' ';

  if (!ChessUtils::isCastlingMove(fromRow, fromCol, toRow, toCol, piece)) {
    if (isEnPassantCapture)
      Serial.printf("Opponent wants to perform en passant: move pawn from %c%d to %c%d and remove captured pawn at %c%d\n", (char)('a' + fromCol), 8 - fromRow, (char)('a' + toCol), 8 - toRow, (char)('a' + toCol), 8 - capturedPawnRow);
    else
      Serial.printf("Opponent wants to move piece from %c%d to %c%d\n", (char)('a' + fromCol), 8 - fromRow, (char)('a' + toCol), 8 - toRow);

    bool isCapture = (capturedPiece != ' ');
    showOpponentMoveIndicator(fromRow, fromCol, toRow, toCol, isCapture, isEnPassantCapture, capturedPawnRow);
    waitForOpponentMoveCompletion(fromRow, fromCol, toRow, toCol, isCapture, isEnPassantCapture, capturedPawnRow);
    boardDriver->clearAllLEDs();
  } else {
    Serial.println("Opponent wants to castle");
    applyCastling(fromRow, fromCol, toRow, toCol, piece, true);
  }
  updateCastlingRightsAfterMove(fromRow, fromCol, toRow, toCol, piece, capturedPiece);

  if (capturedPiece != ' ') {
    Serial.printf("Piece captured: %c\n", capturedPiece);
    boardDriver->captureAnimation(toRow, toCol);
  } else {
    confirmSquareCompletion(toRow, toCol);
  }

  char promotedPiece = ' ';
  if (applyPawnPromotionIfNeeded(toRow, toCol, piece, promotedPiece)) {
    Serial.printf("Pawn promoted to %c\n", promotedPiece);
    boardDriver->promotionAnimation(toCol);
  }
}

void ChessBot::showOpponentMoveIndicator(int fromRow, int fromCol, int toRow, int toCol, bool isCapture, bool isEnPassant, int enPassantCapturedPawnRow) {
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
  boardDriver->releaseLEDs();
}

void ChessBot::waitForOpponentMoveCompletion(int fromRow, int fromCol, int toRow, int toCol, bool isCapture, bool isEnPassant, int enPassantCapturedPawnRow) {
  bool piecePickedUp = false;
  bool capturedPieceRemoved = false;
  bool moveCompleted = false;

  Serial.println("Waiting for you to complete the opponent's move...");

  while (!moveCompleted) {
    boardDriver->readSensors();

    // For capture moves, ensure captured piece is removed first
    // For en passant, check the actual captured pawn square (not the destination)
    if (isCapture && !capturedPieceRemoved) {
      int captureCheckRow = isEnPassant ? enPassantCapturedPawnRow : toRow;
      if (!boardDriver->getSensorState(captureCheckRow, toCol)) {
        capturedPieceRemoved = true;
        if (isEnPassant)
          Serial.println("En passant captured pawn removed, now complete the move...");
        else
          Serial.println("Captured piece removed, now complete the move...");
      }
    }

    // Check if piece was picked up from source
    if (!piecePickedUp && !boardDriver->getSensorState(fromRow, fromCol)) {
      piecePickedUp = true;
      Serial.println("Piece picked up, now place it on the destination...");
    }

    // Check if piece was placed on destination
    // For captures: wait until captured piece is removed AND piece is placed
    // For normal moves: just wait for piece to be placed
    if (piecePickedUp && boardDriver->getSensorState(toRow, toCol))
      if (!isCapture || (isCapture && capturedPieceRemoved)) {
        moveCompleted = true;
        Serial.println("Move completed on physical board!");
      }

    delay(SENSOR_READ_DELAY_MS);
    boardDriver->updateSensorPrev();
  }
}