#include "checkers_game.h"
#include "checkers_ui.h"
#include "web_logger.h"
#include <string.h>

const char CheckersGame::INITIAL_BOARD[8][8] = {
    {' ', 'b', ' ', 'b', ' ', 'b', ' ', 'b'},
    {'b', ' ', 'b', ' ', 'b', ' ', 'b', ' '},
    {' ', 'b', ' ', 'b', ' ', 'b', ' ', 'b'},
    {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '},
    {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '},
    {'w', ' ', 'w', ' ', 'w', ' ', 'w', ' '},
    {' ', 'w', ' ', 'w', ' ', 'w', ' ', 'w'},
    {'w', ' ', 'w', ' ', 'w', ' ', 'w', ' '}};

CheckersGame::CheckersGame(BoardDriver* bd) : boardDriver(bd), currentTurn('w'), gameOver(false) {}

void CheckersGame::begin() {
  webLog.println("=== Starting Checkers Mode ===");
  initializeBoard();

  webLog.println("Set up checkers: 12 white pieces on the near rows, 12 black on the far rows (dark squares only)");
  CheckersUI::waitForBoardSetup(boardDriver, INITIAL_BOARD);
  webLog.println("Checkers board ready. White moves first.");
}

void CheckersGame::initializeBoard() {
  memcpy(board, INITIAL_BOARD, sizeof(INITIAL_BOARD));
  currentTurn = 'w';
  gameOver = false;
}

void CheckersGame::update() {
  if (gameOver)
    return;

  boardDriver->readSensors();
  if (!CheckersUI::playHumanTurn(boardDriver, engine, board, currentTurn, quitState)) {
    boardDriver->updateSensorPrev();
    gameOver = true;
    return;
  }
  boardDriver->updateSensorPrev();

  char next = CheckersUI::oppositeColor(currentTurn);
  if (engine.hasNoPieces(board, next) || !engine.hasAnyLegalMove(board, next)) {
    CheckersUI::announceGameOver(boardDriver, currentTurn);
    gameOver = true;
  } else {
    currentTurn = next;
  }
}
