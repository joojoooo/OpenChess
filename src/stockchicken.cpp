#include "stockchicken.h"
#include <string.h>

namespace {
constexpr int MAN_VALUE = 100;
constexpr int KING_VALUE = 130;
constexpr int INF = 1000000;
constexpr int WIN_SCORE = 100000;
} // namespace

Stockchicken::Stockchicken(const StockchickenConfig& config) : depth(config.depth) {}

void Stockchicken::applyMove(char board[8][8], int fromRow, int fromCol, const CheckersEngine::Move& move, char& piece) const {
  piece = board[fromRow][fromCol];
  board[fromRow][fromCol] = ' ';
  if (move.isCapture)
    board[move.capturedRow][move.capturedCol] = ' ';
  if (engine.isPromotion(piece, move.toRow))
    piece = (CheckersEngine::getPieceColor(piece) == 'w') ? 'W' : 'B';
  board[move.toRow][move.toCol] = piece;
}

int Stockchicken::evaluate(const char board[8][8], char color) {
  int score = 0;
  for (int r = 0; r < 8; r++) {
    for (int c = 0; c < 8; c++) {
      char piece = board[r][c];
      if (piece == ' ')
        continue;

      int value;
      if (CheckersEngine::isKing(piece)) {
        value = KING_VALUE;
      } else {
        char pieceColor = CheckersEngine::getPieceColor(piece);
        int advancement = (pieceColor == 'w') ? (7 - r) : r; // 0..7, higher = closer to promotion
        value = MAN_VALUE + advancement;
      }

      score += (CheckersEngine::getPieceColor(piece) == color) ? value : -value;
    }
  }
  return score;
}

int Stockchicken::bestTurnScore(char board[8][8], char color, int plies, int alpha, int beta) const {
  if (plies <= 0)
    return evaluate(board, color);

  bool any = false;
  int best = -INF;
  bool mustCapture = engine.hasAnyCapture(board, color);

  for (int r = 0; r < 8; r++) {
    for (int c = 0; c < 8; c++) {
      if (CheckersEngine::getPieceColor(board[r][c]) != color)
        continue;

      CheckersEngine::Move moves[CheckersEngine::MAX_MOVES];
      int moveCount;
      engine.getLegalMoves(board, r, c, color, mustCapture, moves, moveCount);

      for (int i = 0; i < moveCount; i++) {
        any = true;
        char child[8][8];
        memcpy(child, board, sizeof(child));
        char piece;
        applyMove(child, r, c, moves[i], piece);

        int score;
        if (moves[i].isCapture && !engine.isPromotion(board[r][c], moves[i].toRow)) {
          CheckersEngine::Move cont[CheckersEngine::MAX_MOVES];
          int contCount;
          engine.getContinuationCaptures(child, moves[i].toRow, moves[i].toCol, color, cont, contCount);
          if (contCount > 0)
            score = continueTurnScore(child, color, moves[i].toRow, moves[i].toCol, plies, alpha, beta, MAX_TURN_STEPS - 1);
          else
            score = -bestTurnScore(child, opposite(color), plies - 1, -beta, -alpha);
        } else {
          score = -bestTurnScore(child, opposite(color), plies - 1, -beta, -alpha);
        }

        if (score > best)
          best = score;
        if (best > alpha)
          alpha = best;
        if (alpha >= beta)
          return best;
      }
    }
  }

  if (!any)
    return -WIN_SCORE; // `color` has no legal move - a loss

  return best;
}

int Stockchicken::continueTurnScore(char board[8][8], char color, int row, int col, int plies, int alpha, int beta, int chainsLeft) const {
  CheckersEngine::Move moves[CheckersEngine::MAX_MOVES];
  int moveCount;
  engine.getContinuationCaptures(board, row, col, color, moves, moveCount);

  int best = -INF;
  for (int i = 0; i < moveCount; i++) {
    char child[8][8];
    memcpy(child, board, sizeof(child));
    char piece;
    applyMove(child, row, col, moves[i], piece);

    int score;
    if (engine.isPromotion(board[row][col], moves[i].toRow)) {
      score = -bestTurnScore(child, opposite(color), plies - 1, -beta, -alpha);
    } else {
      CheckersEngine::Move cont[CheckersEngine::MAX_MOVES];
      int contCount;
      engine.getContinuationCaptures(child, moves[i].toRow, moves[i].toCol, color, cont, contCount);
      if (contCount > 0 && chainsLeft > 0)
        score = continueTurnScore(child, color, moves[i].toRow, moves[i].toCol, plies, alpha, beta, chainsLeft - 1);
      else
        score = -bestTurnScore(child, opposite(color), plies - 1, -beta, -alpha);
    }

    if (score > best)
      best = score;
    if (best > alpha)
      alpha = best;
    if (alpha >= beta)
      break;
  }
  return best;
}

int Stockchicken::exploreStep(char board[8][8], char color, int row, int col, char pieceBeforeMove, const CheckersEngine::Move moves[CheckersEngine::MAX_MOVES], int moveCount, Turn& turn, int stepIndex, int plies, int alpha, int beta, int chainsLeft) const {
  int best = -INF;
  Step bestStep = {row, col, moves[0]};
  Turn bestContinuation;
  bestContinuation.stepCount = 0;

  for (int i = 0; i < moveCount; i++) {
    char child[8][8];
    memcpy(child, board, sizeof(child));
    char piece;
    applyMove(child, row, col, moves[i], piece);

    Turn continuation;
    continuation.stepCount = 0;
    int score;

    if (moves[i].isCapture && !engine.isPromotion(pieceBeforeMove, moves[i].toRow) && chainsLeft > 0) {
      CheckersEngine::Move cont[CheckersEngine::MAX_MOVES];
      int contCount;
      engine.getContinuationCaptures(child, moves[i].toRow, moves[i].toCol, color, cont, contCount);
      if (contCount > 0)
        score = exploreStep(child, color, moves[i].toRow, moves[i].toCol, piece, cont, contCount, continuation, 0, plies, alpha, beta, chainsLeft - 1);
      else
        score = -bestTurnScore(child, opposite(color), plies - 1, -beta, -alpha);
    } else {
      score = -bestTurnScore(child, opposite(color), plies - 1, -beta, -alpha);
    }

    if (score > best) {
      best = score;
      bestStep = {row, col, moves[i]};
      bestContinuation = continuation;
    }
    if (best > alpha)
      alpha = best;
    if (alpha >= beta)
      break;
  }

  turn.steps[stepIndex] = bestStep;
  turn.stepCount = stepIndex + 1;
  for (int i = 0; i < bestContinuation.stepCount; i++)
    turn.steps[stepIndex + 1 + i] = bestContinuation.steps[i];
  turn.stepCount += bestContinuation.stepCount;

  return best;
}

bool Stockchicken::chooseTurn(const char board[8][8], char color, Turn& turn) const {
  char rootBoard[8][8];
  memcpy(rootBoard, board, sizeof(rootBoard));

  int alpha = -INF;
  int beta = INF;
  int best = alpha;
  bool found = false;
  Turn bestTurn;
  bestTurn.stepCount = 0;
  bool mustCapture = engine.hasAnyCapture(rootBoard, color);

  for (int r = 0; r < 8; r++) {
    for (int c = 0; c < 8; c++) {
      if (CheckersEngine::getPieceColor(rootBoard[r][c]) != color)
        continue;

      CheckersEngine::Move moves[CheckersEngine::MAX_MOVES];
      int moveCount;
      engine.getLegalMoves(rootBoard, r, c, color, mustCapture, moves, moveCount);
      if (moveCount == 0)
        continue;

      Turn candidate;
      candidate.stepCount = 0;
      int score = exploreStep(rootBoard, color, r, c, rootBoard[r][c], moves, moveCount, candidate, 0, depth, alpha, beta, MAX_TURN_STEPS - 1);

      if (!found || score > best) {
        best = score;
        bestTurn = candidate;
        found = true;
      }
      if (best > alpha)
        alpha = best;
    }
  }

  if (found)
    turn = bestTurn;
  return found;
}
