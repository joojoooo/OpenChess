#include "chess_bot.h"
#include "chess_common.h"
#include "chess_utils.h"
#include "led_colors.h"
#include "stockfish_api.h"
#include "wifi_manager_esp32.h"
#include <Arduino.h>

ChessBot::ChessBot(BoardDriver* boardDriver, ChessEngine* chessEngine, WiFiManagerESP32* wifiManager, BotConfig config) {
  _boardDriver = boardDriver;
  _chessEngine = chessEngine;
  _wifiManager = wifiManager;
  botConfig = config;
  isWhiteTurn = true;
  gameStarted = false;
  gameOver = false;
  botThinking = false;
  wifiConnected = false;
  currentEvaluation = 0.0;
}

void ChessBot::begin() {
  Serial.println("=== Starting Chess Bot Mode ===");
  Serial.printf("Player plays: %s\n", botConfig.playerIsWhite ? "White" : "Black");
  Serial.printf("Bot plays: %s\n", botConfig.playerIsWhite ? "Black" : "White");
  Serial.printf("Bot Difficulty: Depth %d, Timeout %dms\n", botConfig.stockfishSettings.depth, botConfig.stockfishSettings.timeoutMs);
  Serial.println("====================================");

  _boardDriver->clearAllLEDs();

  // Connect to WiFi
  if (_wifiManager->connectToWiFi(_wifiManager->getWiFiSSID(), _wifiManager->getWiFiPassword())) {
    Serial.println("WiFi connected! Bot mode ready.");
    wifiConnected = true;

    // Show success animation
    for (int i = 0; i < 3; i++) {
      _boardDriver->clearAllLEDs();
      delay(200);

      // Light up entire board green briefly
      for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
          _boardDriver->setSquareLED(row, col, LedColors::ConfirmGreen.r, LedColors::ConfirmGreen.g, LedColors::ConfirmGreen.b); // Green
        }
      }
      _boardDriver->showLEDs();
      delay(200);
    }

    isWhiteTurn = true;
    gameStarted = false;
    gameOver = false;
    botThinking = false;
    currentEvaluation = 0.0;
    initializeBoard();
    waitForBoardSetup();
  } else {
    Serial.println("Failed to connect to WiFi. Bot mode unavailable.");
    wifiConnected = false;

    // Show error animation (red flashing)
    for (int i = 0; i < 5; i++) {
      _boardDriver->clearAllLEDs();
      delay(300);

      // Light up entire board red briefly
      for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
          _boardDriver->setSquareLED(row, col, LedColors::ErrorRed.r, LedColors::ErrorRed.g, LedColors::ErrorRed.b); // Red
        }
      }
      _boardDriver->showLEDs();
      delay(300);
    }

    _boardDriver->clearAllLEDs();
  }
}

void ChessBot::update() {
  if (!wifiConnected) {
    return; // No WiFi, can't play against bot
  }

  if (!gameStarted) {
    return; // Waiting for initial setup
  }

  if (gameOver) {
    return; // Game ended (checkmate/stalemate)
  }

  if (botThinking) {
    showBotThinking();
    return;
  }

  _boardDriver->readSensors();

  // Detect piece movements (player's turn)
  bool isPlayerTurn = (botConfig.playerIsWhite && isWhiteTurn) || (!botConfig.playerIsWhite && !isWhiteTurn);
  if (isPlayerTurn) {
    static unsigned long lastTurnDebug = 0;
    if (millis() - lastTurnDebug > 5000) {
      Serial.printf("Your turn! Move a %s\n", botConfig.playerIsWhite ? "WHITE piece (uppercase letters)" : "BLACK piece (lowercase letters)");
      lastTurnDebug = millis();
    }
    // Look for piece pickups and placements
    static int selectedRow = -1, selectedCol = -1;
    static bool piecePickedUp = false;

    // Check for piece pickup
    if (!piecePickedUp) {
      for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
          if (!_boardDriver->getSensorState(row, col) && _boardDriver->getSensorPrev(row, col)) {
            // Check what piece was picked up
            char piece = board[row][col];

            if (piece != ' ') {
              // Player should only be able to move their own pieces
              if ((botConfig.playerIsWhite && piece >= 'A' && piece <= 'Z') || (!botConfig.playerIsWhite && piece >= 'a' && piece <= 'z')) {
                selectedRow = row;
                selectedCol = col;
                piecePickedUp = true;

                Serial.printf("Player picked up %s piece '%c' at %c%d (array position %d,%d)\n", botConfig.playerIsWhite ? "WHITE" : "BLACK", board[row][col], (char)('a' + col), 8 - row, row, col);

                // Show selected square
                _boardDriver->setSquareLED(row, col, LedColors::PickupCyan.r, LedColors::PickupCyan.g, LedColors::PickupCyan.b);

                // Show possible moves
                int moveCount = 0;
                int moves[27][2];
                _chessEngine->setCastlingRights(castlingRights);
                _chessEngine->getPossibleMoves(board, row, col, moveCount, moves);

                for (int i = 0; i < moveCount; i++) {
                  int r = moves[i][0];
                  int c = moves[i][1];
                  if (board[r][c] != ' ') {
                    _boardDriver->setSquareLED(r, c, LedColors::AttackRed.r, LedColors::AttackRed.g, LedColors::AttackRed.b);
                  } else {
                    _boardDriver->setSquareLED(r, c, LedColors::MoveWhite.r, LedColors::MoveWhite.g, LedColors::MoveWhite.b);
                  }
                }
                _boardDriver->showLEDs();
                break;
              } else {
                // Player tried to pick up the wrong color piece
                Serial.printf("ERROR: You tried to pick up %s piece '%c' at %c%d. You can only move %s pieces!\n", (piece >= 'A' && piece <= 'Z') ? "WHITE" : "BLACK", piece, (char)('a' + col), 8 - row, botConfig.playerIsWhite ? "WHITE" : "BLACK");

                // Flash red to indicate error
                _boardDriver->blinkSquare(row, col, LedColors::ErrorRed.r, LedColors::ErrorRed.g, LedColors::ErrorRed.b); // Blink red
              }
            }
          }
        }
        if (piecePickedUp)
          break;
      }
    }

    // Check for piece placement
    if (piecePickedUp) {
      for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
          if (_boardDriver->getSensorState(row, col) && !_boardDriver->getSensorPrev(row, col)) {
            // Check if piece was returned to its original position
            if (row == selectedRow && col == selectedCol) {
              // Piece returned to original position - cancel selection
              Serial.println("Piece returned to original position. Selection cancelled.");
              piecePickedUp = false;
              selectedRow = selectedCol = -1;

              // Clear all indicators
              _boardDriver->clearAllLEDs();
              break;
            }

            // Piece placed somewhere else - validate move
            int moveCount = 0;
            int moves[27][2];
            _chessEngine->setCastlingRights(castlingRights);
            _chessEngine->getPossibleMoves(board, selectedRow, selectedCol, moveCount, moves);

            bool validMove = false;
            for (int i = 0; i < moveCount; i++) {
              if (moves[i][0] == row && moves[i][1] == col) {
                validMove = true;
                break;
              }
            }

            if (validMove) {
              char piece = board[selectedRow][selectedCol];

              // Complete LED animations BEFORE API request
              processPlayerMove(selectedRow, selectedCol, row, col, piece);

              // Immediately publish the player's move to the web UI.
              // The main loop may be blocked while the bot thinks/moves, so relying on polling there
              // can delay UI updates.
              if (_wifiManager) {
                _wifiManager->updateBoardState(board, currentEvaluation);
              }

              // Flash confirmation on destination square for player move
              ChessCommon::confirmSquareCompletion(_boardDriver, row, col);

              piecePickedUp = false;
              selectedRow = selectedCol = -1;

              // Switch to bot's turn
              // If player is White, bot is Black (isWhiteTurn = false)
              // If player is Black, bot is White (isWhiteTurn = true)
              isWhiteTurn = !botConfig.playerIsWhite;

              // After the player's move, check if the bot is checkmated/stalemated (or in check).
              char sideToMove = isWhiteTurn ? 'w' : 'b';
              gameOver = ChessCommon::handleGameState(_boardDriver, _chessEngine, board, sideToMove);

              if (!gameOver) {
                botThinking = true;
                Serial.println("Player move completed. Bot thinking...");
                // Start bot move calculation
                makeBotMove();
              }
            } else {
              Serial.println("Invalid move! Please try again.");
              _boardDriver->blinkSquare(row, col, LedColors::ErrorRed.r, LedColors::ErrorRed.g, LedColors::ErrorRed.b); // Blink red for invalid move

              // Restore move indicators - piece is still selected
              _boardDriver->clearAllLEDs();

              // Show selected square again
              _boardDriver->setSquareLED(selectedRow, selectedCol, LedColors::PickupCyan.r, LedColors::PickupCyan.g, LedColors::PickupCyan.b);

              // Show possible moves again
              int moveCount = 0;
              int moves[27][2];
              _chessEngine->setCastlingRights(castlingRights);
              _chessEngine->getPossibleMoves(board, selectedRow, selectedCol, moveCount, moves);

              for (int i = 0; i < moveCount; i++) {
                int r = moves[i][0];
                int c = moves[i][1];
                if (board[r][c] != ' ') {
                  _boardDriver->setSquareLED(r, c, LedColors::AttackRed.r, LedColors::AttackRed.g, LedColors::AttackRed.b);
                } else {
                  _boardDriver->setSquareLED(r, c, LedColors::MoveWhite.r, LedColors::MoveWhite.g, LedColors::MoveWhite.b);
                }
              }
              _boardDriver->showLEDs();

              Serial.println("Piece is still selected. Place it on a valid move or return it to its original position.");
            }
            break;
          }
        }
      }
    }
  } else if ((!botThinking && !botConfig.playerIsWhite && isWhiteTurn) || (!botThinking && botConfig.playerIsWhite && !isWhiteTurn)) {
    botThinking = true;
    makeBotMove();
  }

  _boardDriver->updateSensorPrev();
}

String ChessBot::makeStockfishRequest(String fen) {
  WiFiSSLClient client;
  // Set insecure mode for SSL (or add proper certificate validation)
  client.setInsecure();
  String path = StockfishAPI::buildRequestURL(fen, botConfig.stockfishSettings.depth);
  Serial.println("Stockfish request: " STOCKFISH_API_URL + path);
  // Retry logic
  for (int attempt = 1; attempt <= botConfig.stockfishSettings.maxRetries; attempt++) {
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
  showBotThinking();
  String rights = ChessUtils::castlingRightsToString(castlingRights);
  String fen = ChessUtils::boardToFEN(board, isWhiteTurn, rights.c_str());
  botThinking = false;
  String bestMove;
  String response = makeStockfishRequest(fen);
  if (parseStockfishResponse(response, bestMove, currentEvaluation)) {
    Serial.println("=== STOCKFISH EVALUATION ===");
    Serial.printf("%s advantage: %.2f pawns\n", currentEvaluation > 0 ? "White" : "Black", currentEvaluation);

    int fromRow, fromCol, toRow, toCol;
    String validationError;
    if (StockfishAPI::validateUCIMove(bestMove, validationError, fromRow, fromCol, toRow, toCol)) {
      Serial.printf("Move string: %s Parsed: %c%c -> %c%c | Array coords: (%d,%d) to (%d,%d)", bestMove.c_str(), bestMove[0], bestMove[1], bestMove[2], bestMove[3], fromRow, fromCol, toRow, toCol);
      if (bestMove.length() >= 5)
        Serial.printf(" Promotion to: %c", bestMove[4]);
      Serial.println("\n============================");
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
      // Execute the validated bot move
      executeBotMove(fromRow, fromCol, toRow, toCol);
      isWhiteTurn = botConfig.playerIsWhite;

      // After the bot's move, check if the player is checkmated/stalemated (or in check).
      {
        char sideToMove = isWhiteTurn ? 'w' : 'b';
        gameOver = ChessCommon::handleGameState(_boardDriver, _chessEngine, board, sideToMove);
      }

      if (!gameOver) {
        Serial.println("Bot move completed. Your turn!");
      }
    } else {
      Serial.printf("Failed to parse bot move - %s\n", validationError.c_str());
    }
  }
}

void ChessBot::executeBotMove(int fromRow, int fromCol, int toRow, int toCol) {
  char piece = board[fromRow][fromCol];
  char capturedPiece = board[toRow][toCol];

  bool castling = ChessCommon::isCastlingMove(fromRow, fromCol, toRow, toCol, piece);

  // Update board state
  board[toRow][toCol] = piece;
  board[fromRow][fromCol] = ' ';

  if (castling)
    ChessCommon::applyCastling(_boardDriver, board, fromRow, fromCol, toRow, toCol, piece);

  ChessCommon::updateCastlingRightsAfterMove(castlingRights, fromRow, fromCol, toRow, toCol, piece, capturedPiece);
  _chessEngine->setCastlingRights(castlingRights);

  if (!castling) {
    Serial.printf("Bot wants to move piece from %c%d to %c%d\nPlease make this move on the physical board...\n", (char)('a' + fromCol), 8 - fromRow, (char)('a' + toCol), 8 - toRow);

    bool isCapture = (capturedPiece != ' ');

    // Show the move that needs to be made
    showBotMoveIndicator(fromRow, fromCol, toRow, toCol, isCapture);

    // Wait for user to physically complete the bot's move
    waitForBotMoveCompletion(fromRow, fromCol, toRow, toCol, isCapture);

    // Clear LEDs after move completion
    _boardDriver->clearAllLEDs();
  } else {
    int rookFromCol = ((toCol - fromCol) == 2) ? 7 : 0;
    int rookToCol = ((toCol - fromCol) == 2) ? 5 : 3;
    Serial.printf("Bot wants to castle: move king %c%d -> %c%d and rook %c%d -> %c%d\nPlease make this move on the physical board...\n",
                  (char)('a' + fromCol), 8 - fromRow,
                  (char)('a' + toCol), 8 - toRow,
                  (char)('a' + rookFromCol), 8 - fromRow,
                  (char)('a' + rookToCol), 8 - toRow);

    waitForBotCastlingCompletion(fromRow, fromCol, toRow, toCol);
  }

  if (capturedPiece != ' ') {
    Serial.printf("Piece captured: %c\n", capturedPiece);
    _boardDriver->captureAnimation();
  }

  // Flash confirmation on the destination square
  ChessCommon::confirmSquareCompletion(_boardDriver, toRow, toCol);

  // Publish the bot move only after the physical move is completed.
  if (_wifiManager) {
    _wifiManager->updateBoardState(board, currentEvaluation);
  }

  Serial.println("Bot move completed. Your turn!");
}

void ChessBot::showBotThinking() {
  LedRGB thinkingColor = isWhiteTurn ? LedColors::BotThinkingWhite : LedColors::BotThinkingBlack;
  _boardDriver->clearAllLEDs();
  // Corner LEDs blue if bot is Black, green/blue if bot is White
  _boardDriver->setSquareLED(0, 0, thinkingColor.r, thinkingColor.g, thinkingColor.b);
  _boardDriver->setSquareLED(0, 7, thinkingColor.r, thinkingColor.g, thinkingColor.b);
  _boardDriver->setSquareLED(7, 0, thinkingColor.r, thinkingColor.g, thinkingColor.b);
  _boardDriver->setSquareLED(7, 7, thinkingColor.r, thinkingColor.g, thinkingColor.b);
  _boardDriver->showLEDs();
}

void ChessBot::initializeBoard() {
  ChessCommon::copyBoard(INITIAL_BOARD, board);
  castlingRights = 0x0F;
  _chessEngine->setCastlingRights(castlingRights);
}

void ChessBot::waitForBoardSetup() {
  Serial.println("Please set up the chess board in starting position...");

  while (!_boardDriver->checkInitialBoard(INITIAL_BOARD)) {
    _boardDriver->readSensors();
    _boardDriver->updateSetupDisplay(INITIAL_BOARD);
    _boardDriver->showLEDs();
    delay(100);
  }

  Serial.println("Board setup complete! Game starting...");
  _boardDriver->fireworkAnimation();
  gameStarted = true;

  // Show initial board state
  ChessUtils::printBoard(board);
}

void ChessBot::processPlayerMove(int fromRow, int fromCol, int toRow, int toCol, char piece) {
  char capturedPiece = board[toRow][toCol];
  bool castling = ChessCommon::isCastlingMove(fromRow, fromCol, toRow, toCol, piece);

  // Update board state
  board[toRow][toCol] = piece;
  board[fromRow][fromCol] = ' ';

  if (castling)
    ChessCommon::applyCastling(_boardDriver, board, fromRow, fromCol, toRow, toCol, piece);

  ChessCommon::updateCastlingRightsAfterMove(castlingRights, fromRow, fromCol, toRow, toCol, piece, capturedPiece);
  _chessEngine->setCastlingRights(castlingRights);

  Serial.printf("Player moved %c from %c%d to %c%d\n", piece, (char)('a' + fromCol), 8 - fromRow, (char)('a' + toCol), 8 - toRow);

  if (capturedPiece != ' ') {
    Serial.printf("Captured %c\n", capturedPiece);
    _boardDriver->captureAnimation();
  }

  // Check for pawn promotion
  char promotedPiece = ' ';
  if (ChessCommon::applyPawnPromotionIfNeeded(_chessEngine, board, toRow, toCol, piece, promotedPiece)) {
    Serial.printf("Pawn promoted to %c\n", promotedPiece);
    _boardDriver->promotionAnimation(toCol);
  }
}

void ChessBot::showBotMoveIndicator(int fromRow, int fromCol, int toRow, int toCol, bool isCapture) {
  // Clear all LEDs first
  _boardDriver->clearAllLEDs();

  // Show source square (where to pick up from)
  _boardDriver->setSquareLED(fromRow, fromCol, LedColors::PickupCyan.r, LedColors::PickupCyan.g, LedColors::PickupCyan.b);

  // Show destination square (where to place)
  if (isCapture) {
    _boardDriver->setSquareLED(toRow, toCol, LedColors::AttackRed.r, LedColors::AttackRed.g, LedColors::AttackRed.b);
  } else {
    _boardDriver->setSquareLED(toRow, toCol, LedColors::MoveWhite.r, LedColors::MoveWhite.g, LedColors::MoveWhite.b);
  }

  _boardDriver->showLEDs();
}

void ChessBot::waitForBotMoveCompletion(int fromRow, int fromCol, int toRow, int toCol, bool isCapture) {
  bool piecePickedUp = false;
  bool capturedPieceRemoved = false;
  bool moveCompleted = false;

  Serial.println("Waiting for you to complete the bot's move...");

  // Set LEDs once at the beginning to avoid flickering
  _boardDriver->clearAllLEDs();
  _boardDriver->setSquareLED(fromRow, fromCol, LedColors::PickupCyan.r, LedColors::PickupCyan.g, LedColors::PickupCyan.b);
  if (isCapture) {
    _boardDriver->setSquareLED(toRow, toCol, LedColors::AttackRed.r, LedColors::AttackRed.g, LedColors::AttackRed.b);
  } else {
    _boardDriver->setSquareLED(toRow, toCol, LedColors::MoveWhite.r, LedColors::MoveWhite.g, LedColors::MoveWhite.b);
  }
  _boardDriver->showLEDs();

  while (!moveCompleted) {
    _boardDriver->readSensors();

    // For capture moves, ensure captured piece is removed first
    if (isCapture && !capturedPieceRemoved) {
      if (!_boardDriver->getSensorState(toRow, toCol)) {
        capturedPieceRemoved = true;
        Serial.println("Captured piece removed, now complete the bot's move...");
      }
    }

    // Check if piece was picked up from source
    if (!piecePickedUp && !_boardDriver->getSensorState(fromRow, fromCol)) {
      piecePickedUp = true;
      Serial.println("Bot piece picked up, now place it on the destination...");
    }

    // Check if piece was placed on destination
    // For captures: wait until captured piece is removed AND bot piece is placed
    // For normal moves: just wait for bot piece to be placed
    if (piecePickedUp && _boardDriver->getSensorState(toRow, toCol)) {
      if (!isCapture || (isCapture && capturedPieceRemoved)) {
        moveCompleted = true;
        Serial.println("Bot move completed on physical board!");
      }
    }

    delay(50);
    _boardDriver->updateSensorPrev();
  }
}

void ChessBot::waitForBotCastlingCompletion(int kingFromRow, int kingFromCol, int kingToRow, int kingToCol) {
  int deltaCol = kingToCol - kingFromCol;
  int rookFromCol = (deltaCol == 2) ? 7 : 0;
  int rookToCol = (deltaCol == 2) ? 5 : 3;

  Serial.println("Waiting for you to complete the bot's castling move...");

  // Set LEDs once at the beginning to avoid flickering
  _boardDriver->clearAllLEDs();
  _boardDriver->setSquareLED(kingFromRow, kingFromCol, LedColors::PickupCyan.r, LedColors::PickupCyan.g, LedColors::PickupCyan.b);
  _boardDriver->setSquareLED(kingFromRow, rookFromCol, LedColors::PickupCyan.r, LedColors::PickupCyan.g, LedColors::PickupCyan.b);
  _boardDriver->setSquareLED(kingToRow, kingToCol, LedColors::MoveWhite.r, LedColors::MoveWhite.g, LedColors::MoveWhite.b);
  _boardDriver->setSquareLED(kingToRow, rookToCol, LedColors::MoveWhite.r, LedColors::MoveWhite.g, LedColors::MoveWhite.b);
  _boardDriver->showLEDs();

  bool kingSourceEmpty = false;
  bool rookSourceEmpty = false;
  bool kingDestOccupied = false;
  bool rookDestOccupied = false;

  while (!(kingSourceEmpty && rookSourceEmpty && kingDestOccupied && rookDestOccupied)) {
    _boardDriver->readSensors();

    kingSourceEmpty = !_boardDriver->getSensorState(kingFromRow, kingFromCol);
    rookSourceEmpty = !_boardDriver->getSensorState(kingFromRow, rookFromCol);
    kingDestOccupied = _boardDriver->getSensorState(kingToRow, kingToCol);
    rookDestOccupied = _boardDriver->getSensorState(kingToRow, rookToCol);

    delay(10);
  }

  _boardDriver->clearAllLEDs();
}

void ChessBot::getBoardState(char boardState[8][8]) {
  ChessCommon::copyBoard(board, boardState);
}

void ChessBot::setBoardState(char newBoardState[8][8]) {
  Serial.println("Board state updated via WiFi edit");
  ChessCommon::copyBoard(newBoardState, board);
  castlingRights = ChessCommon::recomputeCastlingRightsFromBoard(board);
  _chessEngine->setCastlingRights(castlingRights);
  // Update sensor previous state to match new board
  _boardDriver->readSensors();
  _boardDriver->updateSensorPrev();
  ChessUtils::printBoard(board);
}