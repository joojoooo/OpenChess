#ifndef CHESS_BOT_H
#define CHESS_BOT_H

#include "chess_game.h"
#include "chess_utils.h"
#include "stockfish_api.h"
#include "stockfish_settings.h"

// ESP32 WiFi includes
#include <WiFi.h>
#include <WiFiClientSecure.h>
#define WiFiSSLClient WiFiClientSecure

class ChessBot : public ChessGame {
 private:
  BotConfig botConfig;

  // WiFi and API (Stockfish-specific)
  String makeStockfishRequest(const String& fen);
  bool parseStockfishResponse(const String& response, String& bestMove, float& evaluation);

  // Game flow (Stockfish-specific). Returns false if no move was actually
  // applied (parse failure, desynced piece colour, etc.) so the caller
  // doesn't advance the turn on a no-op.
  bool makeBotMove();

 protected:
  float currentEvaluation; // Evaluation (in pawns, positive = White advantage)

  // Remote move hooks (LED indicator + physical move wait)
  void waitForRemoteMoveCompletion(int fromRow, int fromCol, int toRow, int toCol, bool isCapture, bool isEnPassant = false, int enPassantCapturedPawnRow = -1) override;

 public:
  ChessBot(BoardDriver* bd, ChessEngine* ce, WiFiManagerESP32* wm, MoveHistory* mh, BotConfig cfg);
  void begin() override;
  void update() override;

  // Get current evaluation
  float getEvaluation() const { return currentEvaluation; }
};

#endif // CHESS_BOT_H
