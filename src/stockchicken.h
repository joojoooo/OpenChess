#ifndef STOCKCHICKEN_H
#define STOCKCHICKEN_H

#include "checkers_engine.h"

// ---------------------------
// Stockchicken: checkers AI opponent
// ---------------------------
// A small alpha-beta search built on top of CheckersEngine's move
// generation. Pure C++, no Arduino dependencies - same as CheckersEngine.
//
// "Depth" is the number of full turns (one side's complete move, including
// any forced multi-jump continuation) to look ahead. Each side's turn
// counts as one ply, regardless of how many individual jumps it contains.
struct StockchickenConfig {
  int depth;

  static StockchickenConfig easy() { return {2}; }
  static StockchickenConfig medium() { return {4}; }
  static StockchickenConfig hard() { return {6}; }
};

class Stockchicken {
 public:
  explicit Stockchicken(const StockchickenConfig& config);

  // One step (pick up fromRow/fromCol, place per `move`) within a turn. A
  // turn has more than one step only when the same piece is forced into
  // consecutive captures.
  struct Step {
    int fromRow, fromCol;
    CheckersEngine::Move move;
  };

  // Generous bound on consecutive captures by a single piece in one turn.
  static constexpr int MAX_TURN_STEPS = 6;

  struct Turn {
    Step steps[MAX_TURN_STEPS];
    int stepCount;
  };

  // Picks the best full turn for `color` on `board`. Returns false if
  // `color` has no legal move (game over).
  bool chooseTurn(const char board[8][8], char color, Turn& turn) const;

 private:
  CheckersEngine engine;
  int depth;

  // Pure scoring (no move recording), from `color`'s perspective, with
  // `plies` whole turns left to search including this one.
  int bestTurnScore(char board[8][8], char color, int plies, int alpha, int beta) const;

  // Continues a forced multi-jump for the piece at (row, col) and returns
  // the resulting score from `color`'s perspective. `chainsLeft` bounds how
  // many further jumps in this turn will be explored.
  int continueTurnScore(char board[8][8], char color, int row, int col, int plies, int alpha, int beta, int chainsLeft) const;

  // Like bestTurnScore's root iteration, but records the chosen move (and
  // any forced continuation) into `turn` starting at `stepIndex`. Returns
  // the score from `color`'s perspective.
  int exploreStep(char board[8][8], char color, int row, int col, char pieceBeforeMove, const CheckersEngine::Move moves[CheckersEngine::MAX_MOVES], int moveCount, Turn& turn, int stepIndex, int plies, int alpha, int beta, int chainsLeft) const;

  // Applies `move` (including capture removal and promotion) to `board`,
  // reporting the resulting piece (possibly promoted) in `piece`.
  void applyMove(char board[8][8], int fromRow, int fromCol, const CheckersEngine::Move& move, char& piece) const;

  // Material + advancement score from `color`'s perspective.
  static int evaluate(const char board[8][8], char color);

  static char opposite(char color) { return color == 'w' ? 'b' : 'w'; }
};

#endif // STOCKCHICKEN_H
