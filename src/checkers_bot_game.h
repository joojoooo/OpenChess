#ifndef CHECKERS_BOT_GAME_H
#define CHECKERS_BOT_GAME_H

#include "board_driver.h"
#include "checkers_engine.h"
#include "checkers_ui.h"
#include "stockchicken.h"

// ---------------------------
// Checkers Game Mode (player vs Stockchicken)
// ---------------------------
// The human always plays white (the near rows); Stockchicken plays black.
// Human turns reuse the same sensor/LED flow as CheckersGame (CheckersUI).
// On the AI's turn, Stockchicken picks a full turn and the board shows it
// the same way ChessBot shows computer moves: cyan on the square to lift,
// green/blue on where to place it (blue if it crowns), red on a captured
// piece to remove - then waits for the human to perform it physically.
class CheckersBotGame {
 public:
  CheckersBotGame(BoardDriver* bd, const StockchickenConfig& config);

  void begin();
  void update();
  bool isGameOver() const { return gameOver; }

 private:
  BoardDriver* boardDriver;
  CheckersEngine engine;
  Stockchicken bot;
  CheckersUI::QuitGestureState quitState;

  char board[8][8];
  char currentTurn; // 'w' (human) or 'b' (Stockchicken)
  bool gameOver;

  static const char INITIAL_BOARD[8][8];

  void initializeBoard();
  bool playBotTurn(); // returns false if the quit gesture fired
};

#endif // CHECKERS_BOT_GAME_H
