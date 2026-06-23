#ifndef CHECKERS_UI_H
#define CHECKERS_UI_H

#include "board_driver.h"
#include "checkers_engine.h"

// ---------------------------
// Shared checkers board/LED helpers
// ---------------------------
// Free functions shared between the player-vs-player (CheckersGame) and
// player-vs-Stockchicken (CheckersBotGame) modes, so the sensor/LED
// plumbing for a human turn only has to live in one place.
namespace CheckersUI {

// Far corner squares used for the physical quit gesture: place a piece on
// both simultaneously and hold to back out to game selection. These are the
// two light/unplayable corners - (row+col) even - so they never legitimately
// hold a checkers piece during normal play.
constexpr int QUIT_ROW_A = 0, QUIT_COL_A = 0;
constexpr int QUIT_ROW_B = 7, QUIT_COL_B = 7;

// Tracks whether the quit gesture is "armed" - both corners must be seen
// NOT-both-occupied at least once before the gesture can fire, so it can't
// trigger from stale state carried over between games.
struct QuitGestureState {
  bool armed = false;
};

// Blocks until the physical board matches `initial`, guiding setup with
// White/Blue (missing piece) and Red (extra piece) LEDs, then plays the
// "ready" firework animation.
void waitForBoardSetup(BoardDriver* boardDriver, const char initial[8][8]);

// Checks the quit gesture (pieces on both QUIT_ROW_A/QUIT_COL_A and
// QUIT_ROW_B/QUIT_COL_B). Call every polling cycle from any blocking wait
// loop. Returns true once the ~2s hold (with a cyan countdown, mirroring
// ChessGame's resign/draw gesture) completes; false otherwise, including
// while the hold is in progress (it blocks internally until completion or
// abort) or when the gesture isn't currently armed.
bool checkPhysicalQuit(BoardDriver* boardDriver, QuitGestureState& state);

// Lights every `color` piece that has a legal move (mandatory-capture
// aware) white.
void highlightMovablePieces(BoardDriver* boardDriver, const CheckersEngine& engine, const char board[8][8], char color);

// Clears the board's LEDs and lights the given destinations green, or blue
// for destinations that would crown `piece`.
void highlightDestinations(BoardDriver* boardDriver, const CheckersEngine& engine, const CheckersEngine::Move moves[], int moveCount, char piece);

// Plays one full turn for `color`: lights movable pieces, waits for a
// pickup (flagging illegal pickups red until returned), then resolves the
// move including any forced multi-jump continuation. Mutates `board` and
// leaves the board's LEDs cleared. Does not touch whose-turn-it-is or
// game-over state - that's the caller's responsibility. Returns false (and
// leaves `board` mid-turn) if the quit gesture fires instead of a move.
bool playHumanTurn(BoardDriver* boardDriver, const CheckersEngine& engine, char board[8][8], char color, QuitGestureState& quitState);

// Fires the win animation (white or blue) for `winnerColor`.
void announceGameOver(BoardDriver* boardDriver, char winnerColor);

inline char oppositeColor(char color) { return color == 'w' ? 'b' : 'w'; }

} // namespace CheckersUI

#endif // CHECKERS_UI_H
