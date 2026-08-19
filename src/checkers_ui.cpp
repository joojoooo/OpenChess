#include "checkers_ui.h"
#include "led_colors.h"
#include "web_logger.h"
#include <Arduino.h>

namespace CheckersUI {

void waitForBoardSetup(BoardDriver* boardDriver, const char initial[8][8]) {
  boardDriver->acquireLEDs();
  bool allCorrect = false;
  while (!allCorrect) {
    boardDriver->readSensors();
    allCorrect = true;
    boardDriver->clearAllLEDs(false);

    for (int row = 0; row < 8; row++) {
      for (int col = 0; col < 8; col++) {
        bool shouldHavePiece = (initial[row][col] != ' ');
        bool hasPiece = boardDriver->getSensorState(row, col);
        if (shouldHavePiece == hasPiece)
          continue;

        allCorrect = false;
        if (shouldHavePiece && !hasPiece)
          boardDriver->setSquareLED(row, col, initial[row][col] == 'w' ? LedColors::White : LedColors::Blue);
        else
          boardDriver->setSquareLED(row, col, LedColors::Red);
      }
    }

    boardDriver->showLEDs();
    if (!allCorrect)
      delay(SENSOR_READ_DELAY_MS);
  }
  boardDriver->clearAllLEDs();
  boardDriver->releaseLEDs();
  boardDriver->fireworkAnimation();
  boardDriver->updateSensorPrev();
}

bool checkPhysicalQuit(BoardDriver* boardDriver, QuitGestureState& state) {
  bool cornerA = boardDriver->getSensorState(QUIT_ROW_A, QUIT_COL_A);
  bool cornerB = boardDriver->getSensorState(QUIT_ROW_B, QUIT_COL_B);

  if (!(cornerA && cornerB)) {
    state.armed = true;
    return false;
  }
  if (!state.armed)
    return false; // both corners occupied, but not seen empty since last trigger - ignore
  state.armed = false;

  webLog.println("Checkers: quit gesture detected (pieces on both far corners). Hold to confirm...");

  constexpr unsigned long QUIT_HOLD_MS = 2000;
  constexpr int QUIT_PROGRESS_STEPS = 8;

  boardDriver->acquireLEDs();
  boardDriver->clearAllLEDs(false);

  unsigned long start = millis();
  int shownProgress = -1;
  while (millis() - start < QUIT_HOLD_MS) {
    boardDriver->readSensors();
    if (!boardDriver->getSensorState(QUIT_ROW_A, QUIT_COL_A) || !boardDriver->getSensorState(QUIT_ROW_B, QUIT_COL_B)) {
      boardDriver->clearAllLEDs();
      boardDriver->releaseLEDs();
      webLog.println("Checkers: quit gesture aborted (a corner piece was lifted)");
      return false;
    }

    unsigned long elapsed = millis() - start;
    int progress = ((elapsed + 1) * QUIT_PROGRESS_STEPS) / QUIT_HOLD_MS;
    if (progress > QUIT_PROGRESS_STEPS)
      progress = QUIT_PROGRESS_STEPS;

    if (progress != shownProgress) {
      boardDriver->clearAllLEDs(false);
      for (int i = 0; i < progress; i++) {
        boardDriver->setSquareLED(7 - i, 3, LedColors::Cyan);
        boardDriver->setSquareLED(i, 4, LedColors::Cyan);
      }
      boardDriver->showLEDs();
      shownProgress = progress;
    }

    delay(SENSOR_READ_DELAY_MS);
  }

  boardDriver->clearAllLEDs();
  boardDriver->releaseLEDs();
  webLog.println("Checkers: quit confirmed, returning to game selection");
  return true;
}

void highlightMovablePieces(BoardDriver* boardDriver, const CheckersEngine& engine, const char board[8][8], char color) {
  CheckersEngine::Move moves[CheckersEngine::MAX_MOVES];
  int moveCount;
  bool mustCapture = engine.hasAnyCapture(board, color);

  boardDriver->acquireLEDs();
  boardDriver->clearAllLEDs(false);
  for (int r = 0; r < 8; r++) {
    for (int c = 0; c < 8; c++) {
      if (CheckersEngine::getPieceColor(board[r][c]) != color)
        continue;
      engine.getLegalMoves(board, r, c, color, mustCapture, moves, moveCount);
      if (moveCount > 0)
        boardDriver->setSquareLED(r, c, LedColors::White);
    }
  }
  boardDriver->showLEDs();
  boardDriver->releaseLEDs();
}

void highlightDestinations(BoardDriver* boardDriver, const CheckersEngine& engine, const CheckersEngine::Move moves[], int moveCount, char piece) {
  boardDriver->acquireLEDs();
  boardDriver->clearAllLEDs(false);
  for (int i = 0; i < moveCount; i++) {
    bool promotes = engine.isPromotion(piece, moves[i].toRow);
    boardDriver->setSquareLED(moves[i].toRow, moves[i].toCol, promotes ? LedColors::Blue : LedColors::Green);
    if (moves[i].isCapture)
      boardDriver->setSquareLED(moves[i].capturedRow, moves[i].capturedCol, LedColors::Red);
  }
  boardDriver->showLEDs();
  boardDriver->releaseLEDs();
}

bool playHumanTurn(BoardDriver* boardDriver, const CheckersEngine& engine, char board[8][8], char color, QuitGestureState& quitState) {
  // Loop so an aborted pickup (returned to its square) simply restarts the
  // selection phase without recursing.
  for (;;) {
    highlightMovablePieces(boardDriver, engine, board, color);
    bool mustCapture = engine.hasAnyCapture(board, color);

    // --- Phase 1: monitor all squares and wait for exactly one legal pickup ---
    // Every polling cycle, compare the logical board state against sensors.
    // Any square that should have a piece but doesn't is "displaced". If
    // it's the current player's piece with a legal move it's an intentional
    // pickup; everything else - wrong colour, own piece with no legal move,
    // a piece accidentally knocked - shows red until returned.
    int row = -1, col = -1;
    CheckersEngine::Move moves[CheckersEngine::MAX_MOVES];
    int moveCount = 0;
    bool showingRed = false;

    while (row == -1) {
      boardDriver->readSensors();

      if (checkPhysicalQuit(boardDriver, quitState)) {
        boardDriver->clearAllLEDs();
        return false;
      }

      int legalRow = -1, legalCol = -1, legalCount = 0;
      int illegalR[24], illegalC[24], illegalCount = 0;

      for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
          if (board[r][c] == ' ' || boardDriver->getSensorState(r, c))
            continue; // empty square or piece correctly seated

          if (CheckersEngine::getPieceColor(board[r][c]) == color) {
            CheckersEngine::Move m[CheckersEngine::MAX_MOVES];
            int mc;
            engine.getLegalMoves(board, r, c, color, mustCapture, m, mc);
            if (mc > 0) {
              legalRow = r;
              legalCol = c;
              memcpy(moves, m, mc * sizeof(CheckersEngine::Move));
              moveCount = mc;
              legalCount++;
              continue;
            }
          }

          illegalR[illegalCount] = r;
          illegalC[illegalCount] = c;
          illegalCount++;
        }
      }

      if (illegalCount > 0) {
        boardDriver->acquireLEDs();
        for (int i = 0; i < illegalCount; i++)
          boardDriver->setSquareLED(illegalR[i], illegalC[i], LedColors::Red);
        boardDriver->showLEDs();
        boardDriver->releaseLEDs();
        showingRed = true;
      } else if (showingRed) {
        // All misplacements corrected - restore movable piece highlights
        highlightMovablePieces(boardDriver, engine, board, color);
        showingRed = false;
      }

      if (legalCount == 1 && illegalCount == 0) {
        row = legalRow;
        col = legalCol;
        break;
      }

      delay(SENSOR_READ_DELAY_MS);
    }

    char piece = board[row][col];
    webLog.printf("Checkers: picked up %c%d\n", (char)('a' + col), 8 - row);
    highlightDestinations(boardDriver, engine, moves, moveCount, piece);

    // --- Phase 2: resolve the move, including any forced multi-jump continuation ---
    bool cancelled = false;
    for (;;) {
      int chosen = -1;
      while (chosen == -1) {
        boardDriver->readSensors();

        if (checkPhysicalQuit(boardDriver, quitState)) {
          boardDriver->clearAllLEDs();
          return false;
        }

        if (boardDriver->getSensorState(row, col)) {
          cancelled = true;
          break;
        }

        for (int i = 0; i < moveCount; i++) {
          const CheckersEngine::Move& m = moves[i];
          bool landed = boardDriver->getSensorState(m.toRow, m.toCol);
          bool captureCleared = !m.isCapture || !boardDriver->getSensorState(m.capturedRow, m.capturedCol);
          if (landed && captureCleared) {
            chosen = i;
            break;
          }
        }

        if (chosen == -1 && !cancelled)
          delay(SENSOR_READ_DELAY_MS);
      }

      if (cancelled)
        break;

      const CheckersEngine::Move& move = moves[chosen];
      webLog.printf("Checkers: %c%d -> %c%d%s\n", (char)('a' + col), 8 - row, (char)('a' + move.toCol), 8 - move.toRow, move.isCapture ? " (capture)" : "");

      board[move.toRow][move.toCol] = piece;
      board[row][col] = ' ';
      if (move.isCapture)
        board[move.capturedRow][move.capturedCol] = ' ';

      boardDriver->clearAllLEDs();
      boardDriver->blinkSquare(move.toRow, move.toCol, LedColors::Green, 1);
      boardDriver->updateSensorPrev();

      if (engine.isPromotion(piece, move.toRow)) {
        piece = (color == 'w') ? 'W' : 'B';
        board[move.toRow][move.toCol] = piece;
        webLog.println("Checkers: piece crowned!");
        boardDriver->blinkSquare(move.toRow, move.toCol, LedColors::Blue, 4);
        break; // promotion ends the turn even mid-capture per WCDF rules
      }

      if (!move.isCapture)
        break; // non-capture move always ends the turn

      row = move.toRow;
      col = move.toCol;
      engine.getContinuationCaptures(board, row, col, color, moves, moveCount);
      if (moveCount == 0)
        break;

      webLog.println("Checkers: capture again with the same piece");
      highlightDestinations(boardDriver, engine, moves, moveCount, piece);
    }

    if (cancelled) {
      webLog.println("Checkers: pickup cancelled");
      boardDriver->clearAllLEDs();
      boardDriver->updateSensorPrev();
      continue; // restart the selection phase for the same turn
    }

    break;
  }

  boardDriver->clearAllLEDs();
  return true;
}

void announceGameOver(BoardDriver* boardDriver, char winnerColor) {
  webLog.printf("Checkers: %s wins!\n", winnerColor == 'w' ? "White" : "Black");
  boardDriver->fireworkAnimation(winnerColor == 'w' ? LedColors::White : LedColors::Blue);
}

} // namespace CheckersUI
