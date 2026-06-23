#include "checkers_engine.h"

namespace {
struct DirSet {
  int dirs[4][2];
  int count;
};

// Diagonal step directions {dRow, dCol} a piece is allowed to move/capture in.
DirSet directionsFor(char piece) {
  if (CheckersEngine::isKing(piece))
    return {{{-1, -1}, {-1, 1}, {1, -1}, {1, 1}}, 4};
  if (piece == 'w')
    return {{{-1, -1}, {-1, 1}, {0, 0}, {0, 0}}, 2}; // white pieces move toward row 0
  return {{{1, -1}, {1, 1}, {0, 0}, {0, 0}}, 2};      // black pieces move toward row 7
}
} // namespace

char CheckersEngine::getPieceColor(char piece) {
  if (piece == ' ')
    return ' ';
  return (piece == 'w' || piece == 'W') ? 'w' : 'b';
}

bool CheckersEngine::isKing(char piece) {
  return piece == 'W' || piece == 'B';
}

bool CheckersEngine::isOnBoard(int row, int col) {
  return row >= 0 && row < 8 && col >= 0 && col < 8;
}

void CheckersEngine::getCapturesForSquare(const char board[8][8], int row, int col, char color, Move moves[MAX_MOVES], int& moveCount) const {
  moveCount = 0;
  char piece = board[row][col];
  if (getPieceColor(piece) != color)
    return;

  DirSet dirs = directionsFor(piece);
  for (int i = 0; i < dirs.count; i++) {
    int midRow = row + dirs.dirs[i][0];
    int midCol = col + dirs.dirs[i][1];
    int toRow = row + 2 * dirs.dirs[i][0];
    int toCol = col + 2 * dirs.dirs[i][1];

    if (!isOnBoard(toRow, toCol))
      continue;

    char midPiece = board[midRow][midCol];
    if (midPiece == ' ' || getPieceColor(midPiece) == color)
      continue;

    if (board[toRow][toCol] != ' ')
      continue;

    moves[moveCount++] = {toRow, toCol, true, midRow, midCol};
  }
}

void CheckersEngine::getNonCaptureMoves(const char board[8][8], int row, int col, char color, Move moves[MAX_MOVES], int& moveCount) const {
  moveCount = 0;
  char piece = board[row][col];
  if (getPieceColor(piece) != color)
    return;

  DirSet dirs = directionsFor(piece);
  for (int i = 0; i < dirs.count; i++) {
    int toRow = row + dirs.dirs[i][0];
    int toCol = col + dirs.dirs[i][1];

    if (!isOnBoard(toRow, toCol) || board[toRow][toCol] != ' ')
      continue;

    moves[moveCount++] = {toRow, toCol, false, -1, -1};
  }
}

bool CheckersEngine::hasAnyCapture(const char board[8][8], char color) const {
  Move moves[MAX_MOVES];
  int moveCount;
  for (int r = 0; r < 8; r++)
    for (int c = 0; c < 8; c++) {
      if (getPieceColor(board[r][c]) != color)
        continue;
      getCapturesForSquare(board, r, c, color, moves, moveCount);
      if (moveCount > 0)
        return true;
    }
  return false;
}

void CheckersEngine::getLegalMoves(const char board[8][8], int row, int col, char color, bool mustCapture, Move moves[MAX_MOVES], int& moveCount) const {
  moveCount = 0;
  if (getPieceColor(board[row][col]) != color)
    return;

  getCapturesForSquare(board, row, col, color, moves, moveCount);
  if (moveCount > 0 || mustCapture)
    return; // this piece has a capture, or another piece must capture instead

  getNonCaptureMoves(board, row, col, color, moves, moveCount);
}

void CheckersEngine::getContinuationCaptures(const char board[8][8], int row, int col, char color, Move moves[MAX_MOVES], int& moveCount) const {
  getCapturesForSquare(board, row, col, color, moves, moveCount);
}

bool CheckersEngine::isPromotion(char piece, int toRow) const {
  if (isKing(piece))
    return false;
  return (piece == 'w' && toRow == 0) || (piece == 'b' && toRow == 7);
}

bool CheckersEngine::hasNoPieces(const char board[8][8], char color) const {
  for (int r = 0; r < 8; r++)
    for (int c = 0; c < 8; c++)
      if (getPieceColor(board[r][c]) == color)
        return false;
  return true;
}

bool CheckersEngine::hasAnyLegalMove(const char board[8][8], char color) const {
  if (hasAnyCapture(board, color))
    return true;

  Move moves[MAX_MOVES];
  int moveCount;
  for (int r = 0; r < 8; r++)
    for (int c = 0; c < 8; c++) {
      if (getPieceColor(board[r][c]) != color)
        continue;
      getLegalMoves(board, r, c, color, false, moves, moveCount);
      if (moveCount > 0)
        return true;
    }
  return false;
}
