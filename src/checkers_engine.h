#ifndef CHECKERS_ENGINE_H
#define CHECKERS_ENGINE_H

// ---------------------------
// Checkers Rules Engine (standard American/English draughts)
// ---------------------------
// Board notation - independent of the chess engine's:
//   ' ' = empty square
//   'w' / 'b' = white / black man
//   'W' / 'B' = white / black king
//
// Board orientation matches the chess board: row 0 = far rank, row 7 =
// near rank. White's pieces start on rows 5-7 and move toward row 0;
// black's pieces start on rows 0-2 and move toward row 7.
//
// Rules implemented:
//  - Pieces move and capture diagonally forward only (not backward).
//  - Kings move and capture one diagonal step in any of the 4 directions
//    (no "flying" kings).
//  - Captures are mandatory: if any piece of the side to move can capture,
//    only capture moves are legal, and only for pieces that have one.
//  - Multi-jump continuation is mandatory: after a capture, the same piece
//    must keep capturing if another jump is available from its new square.
//  - A man that lands on the back row is crowned and its turn ends
//    immediately, even if a further jump would otherwise be available.
class CheckersEngine {
 public:
  static constexpr int MAX_MOVES = 4;

  // A single move/jump destination for a piece. `isCapture` indicates a
  // jump, with the captured piece located at (capturedRow, capturedCol).
  struct Move {
    int toRow, toCol;
    bool isCapture;
    int capturedRow, capturedCol;
  };

  // 'w'/'b' for either color of man or king, ' ' for an empty square.
  static char getPieceColor(char piece);
  static bool isKing(char piece);
  static bool isOnBoard(int row, int col);

  // True if `color` has at least one capturing move available anywhere on the board.
  bool hasAnyCapture(const char board[8][8], char color) const;

  // Legal destinations for the piece at (row, col), applying the mandatory
  // capture rule for `color` as a whole: if `mustCapture` is true, only
  // capture moves are returned, and only for pieces that have one (pieces
  // without a capture return zero moves). Pass the result of
  // hasAnyCapture(board, color), computed once per position rather than
  // once per piece.
  void getLegalMoves(const char board[8][8], int row, int col, char color, bool mustCapture, Move moves[MAX_MOVES], int& moveCount) const;

  // Further captures available for a piece that just landed at (row, col),
  // used to enforce mandatory multi-jump continuation. Unlike getLegalMoves,
  // this only looks at this piece - other pieces' captures are irrelevant
  // mid-sequence.
  void getContinuationCaptures(const char board[8][8], int row, int col, char color, Move moves[MAX_MOVES], int& moveCount) const;

  // True if a piece landing on toRow would be crowned.
  bool isPromotion(char piece, int toRow) const;

  // True if `color` has no pieces left on the board.
  bool hasNoPieces(const char board[8][8], char color) const;

  // True if `color` has at least one legal move anywhere (mandatory-capture aware).
  bool hasAnyLegalMove(const char board[8][8], char color) const;

 private:
  void getCapturesForSquare(const char board[8][8], int row, int col, char color, Move moves[MAX_MOVES], int& moveCount) const;
  void getNonCaptureMoves(const char board[8][8], int row, int col, char color, Move moves[MAX_MOVES], int& moveCount) const;
};

#endif // CHECKERS_ENGINE_H
