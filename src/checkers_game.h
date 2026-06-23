#ifndef CHECKERS_GAME_H
#define CHECKERS_GAME_H

#include "board_driver.h"
#include "checkers_engine.h"
#include "checkers_ui.h"

// ---------------------------
// Checkers Game Mode (local player vs player)
// ---------------------------
// Standalone mode independent of the chess classes - depends only on
// BoardDriver/CheckersEngine plus the shared CheckersUI helpers for the
// sensor/LED turn logic.
class CheckersGame {
 public:
  explicit CheckersGame(BoardDriver* bd);

  void begin();
  void update();
  bool isGameOver() const { return gameOver; }

 private:
  BoardDriver* boardDriver;
  CheckersEngine engine;
  CheckersUI::QuitGestureState quitState;

  char board[8][8];
  char currentTurn; // 'w' or 'b'
  bool gameOver;

  static const char INITIAL_BOARD[8][8];

  void initializeBoard();
};

#endif // CHECKERS_GAME_H
