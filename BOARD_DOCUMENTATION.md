# OpenChess Board Documentation

## Table of Contents
1. [Physical Board Overview](#physical-board-overview)
2. [Game Modes](#game-modes)
3. [User Interactions & Gestures](#user-interactions--gestures)
4. [LED Visualization & Animations](#led-visualization--animations)
5. [Board Settings & Options](#board-settings--options)
6. [Move Navigation & Review Features](#move-navigation--review-features)

---

## Physical Board Overview

### Hardware Architecture
The OpenChess board is an **8×8 smart chessboard** with integrated sensors and LED feedback, built on an **ESP32** microcontroller.

**Key Components:**
- **Hall-Effect Sensors**: Detect piece placement via magnetic markers embedded in chess pieces
  - **8 row pins (GPIO)** scan rows sequentially
  - **8 column pins (74HC595 Shift Register)** select columns
  - Each square has one sensor at the intersection
  - Debouncing: 125ms stablization time to prevent false detections

- **WS2812B RGB LED Strip** (NeoPixel): 64 addressable LEDs (one per square)
  - Provides visual feedback for valid moves, captures, checks, and game events
  - Controlled via GPIO 32 (configurable at runtime)
  - Global brightness: 0–255 (default 255)
  - Dark squares can be dimmed via "Dim Multiplier" (20–100%, default 70%)

- **Shift Register (74HC595)**: Controls column selection for sensor polling
  - SRCLK (Serial Register Clock): GPIO 14
  - RCLK (Register Latch Clock): GPIO 26
  - SER (Serial Data): GPIO 33
  - SR_INVERT_OUTPUTS: 1 (outputs drive PNP transistors)

- **Row Input Pins (GPIO)**: 4, 16, 17, 18, 19, 21, 22, 23
  - Pulled low when a piece (magnet) is detected on that row

### Coordinate System
- **Rows**: 0–7 (row 0 = rank 8 / Black's back rank; row 7 = rank 1 / White's back rank)
- **Columns**: 0–7 (col 0 = file a; col 7 = file h)
- **Board notation**: Standard algebraic notation (a1–h8)

### Calibration
The board must be **calibrated once** to map physical sensor positions to logical chess squares:
1. No calibration data exists at first boot → Interactive calibration runs automatically
2. System sequentially prompts placement of a piece on each square (a8, a7, ..., h1)
3. Sensor readings map raw GPIO/shift-register pins to logical row/col positions
4. Calibration saved to **NVS** (non-volatile storage) and persists across reboots
5. If pin configuration changes, saved calibration is invalidated and recalibration is triggered

### Sensor Technology
- **Detection Method**: Magnetic field (Hall-effect sensors) activated by small magnets in piece bases
- **Sensitivity**: Responds to presence/absence of a magnet on a square
- **Activation Signal**: LOW on GPIO when magnet detected (shift register column must be enabled for that square)
- **Reading Cycle**: Shift register cycles through 8 columns, reading all 8 rows per column (40ms sensor read delay)

---

## Game Modes

### Mode Selection
When the board starts or a game ends, **4 LEDs light up in the center** to indicate available modes:
- **Blue (row 3, col 3)**: Chess Moves (Human vs Human)
- **Green (row 3, col 4)**: Chess Bot (Human vs Stockfish AI)
- **Yellow (row 4, col 3)**: Lichess (Online play)
- **Red (row 4, col 4)**: Sensor Test

**To select a mode**: Place any chess piece on the corresponding LED square. After debounce stabilization, the mode initializes.

---

### 1. Chess Moves (Human vs Human)
**Mode Code**: `MODE_CHESS_MOVES`

**Description**: Local two-player chess game. Both players take turns moving pieces on the physical board. The system detects piece movements and validates them using chess rules.

**Class**: `ChessMoves` (inherits from `ChessGame`)
- Minimal override: only `begin()` and `update()` methods
- Delegates all game logic to `ChessGame` base class

**Gameplay**:
- **Turn Display**: Player pieces glow to indicate whose turn it is
- **Move Detection**: Player lifts a piece from its square → sensors detect the change → system shows valid move destinations
- **Move Validation**: System validates moves against chess rules (piece-specific movement, no moving into check, etc.)
- **Move Confirmation**: Player places piece on destination → LEDs flash green to confirm
- **Game End**: Checkmate, stalemate, or mutual draw
- **Physical Resign/Draw**: Lift both kings off the board simultaneously to offer resign or draw

**LED Feedback**:
- **Cyan**: Origin square of the piece being moved
- **White**: Valid destinations for that piece
- **Red**: Illegal moves or invalid positions
- **Green**: Move confirmed/completed
- **Yellow**: King in check or pawn promotion

**Recorded**: Games are saved locally in binary format with full move history

---

### 2. Chess Bot (Human vs Stockfish AI)
**Mode Code**: `MODE_BOT`

**Description**: Play against a Stockfish chess engine via HTTPS API. Human player chooses a color (White or Black) and difficulty level, then plays against the AI.

**Class**: `ChessBot` (inherits from `ChessGame`)
- Extends base class with `makeBotMove()` method
- Manages Stockfish API communication
- Tracks evaluation (pawn advantage) and displays it on web UI

**Configuration**:
- **Difficulty Levels**: 
  - **Easy**: Depth 5
  - **Medium**: Depth 8 (default)
  - **Hard**: Depth 11
  - **Expert**: Depth 15
- **Player Color**: White (first to move) or Black (respond to AI)
- **Max Retries**: Configurable retry attempts for API requests

**Gameplay**:
- **Setup**: System displays required piece positions via LED indicators
- **Player Move**: Human places piece → system validates → sends to Stockfish API
- **AI Response**: 
  - LEDs at 4 board corners show **breathing blue animation** ("thinking") while waiting for API response
  - Stockfish engine analyzes position at specified depth
  - Returns best move with evaluation score
- **AI Move Execution**: LEDs show white/green animations as bot move is applied physically
- **Evaluation Display**: Web UI shows current position evaluation as a bar (positive = White advantage, negative = Black advantage)

**Move Retries**:
- Network failures trigger automatic retries (3 attempts, 500ms delay between attempts)
- Times out gracefully with user-friendly messages

**Recorded**: Games saved locally with move history, mode, difficulty, and player color

**External API**:
- **Endpoint**: `https://stockfish.online:443/api/s/v2.php`
- **Parameters**: FEN position + depth
- **Response**: Best move + evaluation in pawns

---

### 3. Lichess (Online Play)
**Mode Code**: `MODE_LICHESS`

**Description**: Play real-time games against other players on the Lichess online chess platform. Board synchronizes with Lichess servers every 500ms via polling.

**Class**: `ChessLichess` (inherits from `ChessBot`)
- Reuses Stockfish infrastructure but redirects API calls to Lichess
- Maintains separate sync loop for polling game state
- Does **not** save games locally (they live on Lichess cloud)

**Configuration**:
- **API Token**: Personal Lichess authentication token (user provides via web settings)
- **Polling Interval**: 500ms (checks for opponent's moves)
- **Move Retries**: 3 attempts, 500ms delay

**Gameplay**:
- **Authentication**: User logs in with Lichess API token (saved in NVS)
- **Game Selection**: System waits for an active Lichess game to be found
- **Polling Loop**: Every 500ms, board queries current game state from Lichess
- **Opponent Move Detection**: When opponent moves on Lichess, board receives updated FEN and updates the display
- **Player Move**: 
  - Human places piece on board
  - Move validated against Lichess's FEN
  - Sent back to Lichess servers
  - Opponent's board receives update automatically
- **Real-Time Sync**: If position diverges from Lichess FEN (e.g., player placed wrong piece), board corrects itself on next poll

**LED Behavior**:
- While waiting for opponent: **white LEDs march around board perimeter** ("waiting animation")
- Game events (captures, checks, etc.) animate same as bot mode

**Game Recording**: 
- **Not** saved locally (Lichess maintains all history)
- Can be reviewed on Lichess website after play

**External APIs**:
- **Endpoint**: `https://lichess.org:443`
- **Authentication**: Bearer token in headers
- **Polling**: Retrieves game state, handles moves bidirectionally

---

### 4. Sensor Test Mode
**Mode Code**: `MODE_SENSOR_TEST`

**Description**: Diagnostic tool to verify all 64 sensors are functioning correctly. Not a playable game mode.

**Class**: `SensorTest` (standalone, not a `ChessGame` subclass)

**Functionality**:
- Prompts user to sequentially place a piece on each board square (a8 through h1)
- Tracks which squares have been "visited" (detected by sensor)
- After all 64 squares are visited, displays **firework animation** and returns to mode selection
- Useful for:
  - Verifying sensor calibration worked
  - Testing for dead sensors
  - Debugging hardware connectivity issues

**LED Feedback**:
- Visited squares: **Green** LED
- Unvisited squares: **Red** LED (indicating which squares still need to be checked)

---

## User Interactions & Gestures

### Physical Board Interactions

#### 1. Piece Movement (Primary Interaction)
**Gesture**: Lift piece → Place on new square

**Detection**:
1. System detects piece lifted from origin square (sensor change from occupied → empty)
2. **Cyan LED** lights on origin square
3. Valid destination squares light up as **White LEDs**
4. Invalid/illegal moves show **Red LEDs**
5. Player places piece on destination square
6. System validates move, applies if legal, plays **move sound** (if enabled)
7. **Green LED** flashes to confirm move acceptance

**Constraints**:
- Must follow chess rules (piece-specific movement, cannot move into check, etc.)
- Promoted pawns require additional UI interaction (see below)

#### 2. Pawn Promotion
**Trigger**: Pawn moves to 8th rank (white) or 1st rank (black)

**Interaction Flow**:
1. **Yellow LED** lights on promotion square
2. **Yellow waterfall animation** cycles through promotion column as visual indicator
3. **Web UI Promotion Overlay** appears with 4 buttons: Queen (Q), Rook (R), Bishop (B), Knight (N)
4. Player selects promotion piece from web interface (or mobile app)
5. **Selection timeout**: 2 minutes; defaults to **Queen** if no selection made
6. **Green LED** confirms promotion acceptance

#### 3. Resign Gesture (Physical)
**Gesture**: Lift both kings off the board simultaneously

**Detection**:
- If both kings are lifted at the same time, system interprets as resign gesture
- Board queries web UI for confirmation or auto-accepts based on configuration
- Resigning player loses

**Alternative**: Use "Resign" button in web UI game controls

#### 4. Draw Gesture (Physical)
**Gesture**: Lift both kings and place them back on the same squares

**Detection**:
- Both kings lifted → Board waits for re-placement
- If both kings placed back → Interpreted as draw offer
- Opponent can accept or reject via web UI

**Alternative**: Use "Draw" button in web UI

---

### Web UI Interactions

The web interface (`board.html`) provides complementary controls and status visualization:

#### Move Navigation (Scrubbing)
**Controls**: Four navigation buttons above the board
- **⏮ (First)**: Jump to game start
- **◀ (Previous)**: Go back one move
- **▶ (Next)**: Go forward one move
- **⏭ (Last)**: Jump to current/live position
- **Move Counter**: Displays current move number / total moves, or "live" if viewing latest position

**Context**: Useful during board review or when playing in physical move mode (can review past positions without touching the board)

#### Board Edit Mode
**Trigger**: "Edit Board" button

**Capabilities**:
- **Drag pieces**: Move any piece to any square
- **Add pieces**: Drag pieces from "spare pieces" panel
- **Remove pieces**: Drag pieces off board to trash
- **FEN Input/Edit**: 
  - View full FEN notation
  - Manually edit FEN for complex positions
  - Copy/paste FEN between sessions
- **Turn Toggle**: Select whose turn it is (White or Black)
- **Castling Rights**: Toggle available castling moves (K, Q, k, q)
- **En Passant Square**: Automatically detected or manually set
- **Controls**:
  - **Sync**: Load current board state from device
  - **Reset**: Restore starting position
  - **Clear**: Remove all pieces
  - **Apply Changes**: Send edited board back to device

**Interaction**: Click pieces to select/drag, place on destination, adjust FEN fields as needed, then click "Apply Changes" to sync back to device.

#### Game History & Review
**Trigger**: "↺" (Game History) button

**Features**:
- **Game List**: Displays all completed games sorted by date (most recent first)
- **Game Cards**: Show date, opponent/mode, result, move count
- **Game Selection**: Click card to enter "Review Mode"
- **Delete Games**: Toggle trash icon to select and delete games
- **Review Panel**: When viewing a game:
  - **Move List**: PGN notation with board edit markers
  - **Move Navigation**: Click any move to jump to that position
  - **Metadata**: Game date, opponent, result, difficulty (if vs bot)

**Context**: Allows post-game analysis and historical tracking of play

---

#### Settings Panel
**Trigger**: "⚙️ Settings" button

**Available Settings**:

1. **Display Settings**:
   - **Show board notation**: Toggle a-h/1-8 labels (default: on)

2. **Sound Settings**:
   - **Play move sounds**: Toggle audio feedback (default: on)
   - Move sound: Plays when piece moves normally
   - Capture sound: Plays when piece captures

3. **Board Colors**:
   - **Light square color**: Default #f0d9b5 (beige)
   - **Dark square color**: Default #b58863 (brown)
   - **Reset Colors**: Restore defaults
   - Colors persist in browser localStorage

4. **Piece Themes**:
   - **Default theme**: Local SVG (default)
   - **Built-in themes**: Wikipedia, Alpha
   - **Custom themes**: Add external piece image URLs
   - **Delete theme**: Remove custom themes
   - Preview link shows theme URL with sample piece

**Persistence**: Settings saved to browser `localStorage`, persist across sessions

---

#### Additional Board Controls
- **⇅ Flip**: Rotate board 180° (useful for reviewing from opponent's perspective)
- **⛶ Focus Mode**: Expand board to full screen (hides toolbar, maximizes board display)

---

## LED Visualization & Animations

### Color Semantics
Each LED color has a **fixed meaning** across all game modes:

| Color | Meaning | Use Cases |
|-------|---------|-----------|
| **Cyan** | Piece origin square | Marks where piece is being moved from |
| **White** | Valid move destination | Shows legal destinations for selected piece |
| **Red** | Capture/Attack/Error | Marks enemy pieces to be captured; also invalid moves; error states |
| **Purple** | En passant / Expert mode | Special move indicator (rarely used in default mode) |
| **Green** | Confirmation/Move completion | Confirms accepted moves, game start, etc. |
| **Yellow** | King in check / Promotion | Indicates king is under attack; also marks promotion square |
| **Blue** | Bot thinking | 4 board corners breathe blue while Stockfish analyzes |
| **Off** | No LED | Clear/idle state |

---

### Animation Types

#### 1. **Blink Animation** (Square Blink)
- **Usage**: Highlight a specific square briefly
- **Pattern**: Square flashes color on/off, 200ms on + 200ms off per cycle
- **Default**: 3 blinks, then LED clears
- **Example**: Move confirmation

#### 2. **Capture Animation**
- **Trigger**: When a piece is captured
- **Pattern**: 
  - Multiple expanding rings of Red and Yellow LEDs emanate from capture square
  - 3 concurrent wave rings stagger outward
  - Center square stays Red throughout
  - Animation lasts ~1 second (20 frames × 50ms)
- **Effect**: Visual feedback that opponent's piece was removed

#### 3. **Promotion Animation**
- **Trigger**: Pawn reaches final rank
- **Pattern**:
  - **Yellow waterfall** cascades up/down the promotion column
  - 8 steps × 100ms = 1.6 second cycle
  - Repeats while waiting for promotion choice
- **Context**: Alerts player to select promotion piece on web UI
- **End State**: Yellow LED stays on promotion square until promotion is chosen

#### 4. **Firework Animation**
- **Trigger**: Game start confirmation, mode selection confirmation, or sensor test completion
- **Pattern**:
  - **Contraction phase**: Rings of color spiral inward from board edges to center (6 frames)
  - **Expansion phase**: Rings spiral outward from center to edges (6 frames)
  - **Timing**: 100ms per step, smooth radius progression
- **Color**: Configurable (default White)
- **Effect**: Celebratory feedback for significant game events

#### 5. **Thinking Animation** (Stockfish Waiting)
- **Trigger**: While waiting for bot move (Stockfish API response)
- **Pattern**:
  - **4 corner LEDs** at board corners: (0,0), (0,7), (7,0), (7,7)
  - LEDs **"breathe"** with smooth sinusoidal brightness curve
  - Hue shifts from blue (peak brightness) toward purple (minimum brightness)
  - HSV-to-RGB conversion for smooth color gradients
  - 30ms refresh rate
- **Duration**: Continues until move received or timeout
- **Cancellation**: Animation stops and clears when move completes or times out

#### 6. **Waiting Animation** (Lichess / Remote Move Waiting)
- **Trigger**: Waiting for opponent move in online/network modes
- **Pattern**:
  - **White LEDs march around board perimeter** in a continuous loop
  - Perimeter positions: top row (L→R), right column (T→B), bottom row (R→L), left column (B→T)
  - 4 concurrent lit LEDs at staggered positions
  - 200ms delay between steps
  - Smooth continuous motion
- **Duration**: Continues until opponent moves
- **Cancellation**: Stops when move received

#### 7. **Flash Animation** (Full Board Flash)
- **Trigger**: Check, stalemate, game end, error conditions
- **Pattern**:
  - Entire board flashes specified color on/off
  - 200ms on + 200ms off per cycle
  - Repeats 3 times (default)
- **Color**: Red (error/check), Green (success), Yellow (warning)
- **Effect**: Grab attention for critical game events

#### 8. **Connecting Animation** (WiFi Connection Attempt)
- **Trigger**: WiFi connection in progress
- **Pattern**:
  - Blue LEDs light up sequentially left-to-right across rows 3-4 (middle of board)
  - 8 columns × 2 rows = 16 LEDs
  - 125ms delay between columns
  - Repeats for each connection attempt
- **Restoration**: Previous LED state restored after sequence

---

### LED Intensity & Brightness Control

**Global Brightness**:
- **Range**: 0–255 (0 = off, 255 = max)
- **Default**: 255
- **Adjustment**: Web UI settings or REST API `/board-settings` endpoint
- **Persistence**: Saved to NVS

**Dim Multiplier** (Dark Squares):
- **Range**: 20–100% (20% = heavily dimmed, 100% = full brightness)
- **Default**: 70%
- **Purpose**: Reduce LED intensity on dark-colored squares for visual balance
- **Adjustment**: Web UI settings or REST API
- **Application**: Applied automatically to squares marked as dark in board colors

**Concurrency**: LED operations are protected by mutex (`ledMutex`) to prevent conflicts between animation task and main game loop.

---

## Board Settings & Options

### Persistent Settings (NVS)

#### LED Settings
- **Namespace**: `"ledSettings"`
- **brightness** (uint8_t): 0–255, default 255
- **dimMultiplier** (uint8_t): 20–100 (stored as percentage), default 70

#### Hardware Configuration
- **Namespace**: `"hwConfig"`
- **ledPin**: GPIO pin for LED data (default GPIO 32)
- **srClkPin**: Shift register clock (default GPIO 14)
- **srLatchPin**: Shift register latch (default GPIO 26)
- **srDataPin**: Shift register serial data (default GPIO 33)
- **srInvertOutputs**: Boolean, whether outputs drive transistors (default 1)
- **rowPins[8]**: Array of GPIO pins for row inputs (defaults: 4, 16, 17, 18, 19, 21, 22, 23)

**Runtime Configuration**:
- Changed via web UI → `/hardware-config` endpoint
- System stores in NVS and reboots to apply changes

#### Board Calibration
- **Namespace**: `"boardCal"`
- **ver**: Calibration format version (current: 1)
- **rowPins**: Saved hardware pins for validation
- **srPins**: Saved shift register pins (clk, latch, data)
- **swap**: Whether axes are swapped (0 or 1)
- **row[8]**: Logical→physical row mapping
- **col[8]**: Logical→physical column mapping
- **led[64]**: Pixel index for each board square

#### Game Configuration (Saved via Web UI)
- **WiFi Credentials** (Namespace: `"wifiCreds"`): SSID, password
- **Lichess Token** (Namespace: `"lichess"`): API authentication token
- **OTA Settings** (Namespace: `"ota"`): Auto-update flag

---

### Web UI Settings (Browser Storage)

Settings saved to browser `localStorage` under key `"chessBoardSettings"`:

```json
{
  "showNotation": true,
  "playSound": true,
  "lightSquareColor": "#f0d9b5",
  "darkSquareColor": "#b58863",
  "pieceTheme": "./pieces/{piece}.svg",
  "customThemes": [],
  "instructionsHidden": false
}
```

**Piece Theme URLs**:
- **Local SVG** (default): `./pieces/{piece}.svg` (served from board `/pieces/` directory)
- **Wikipedia**: `https://chessboardjs.com/img/chesspieces/wikipedia/{piece}.png`
- **Alpha**: `https://chessboardjs.com/img/chesspieces/alpha/{piece}.png`
- **{piece}** placeholder: Replaced with piece code (e.g., `wN` = white knight, `bQ` = black queen)

---

### Runtime Configuration Options

#### Mode Selection
- **Chess Moves**: Two human players
- **Chess Bot**: Against Stockfish AI
  - Difficulty: Easy (D5), Medium (D8), Hard (D11), Expert (D15)
  - Player Color: White or Black
- **Lichess**: Online multiplayer
  - Requires active Lichess API token
- **Sensor Test**: Diagnostic mode

#### Stockfish Bot Settings (via Web UI)
- **Difficulty Level**: Dropdown (Easy/Medium/Hard/Expert)
- **Player Color**: Radio buttons (White/Black)
- **Max Retries**: Configurable (default 3)

#### Lichess Settings
- **API Token**: Text input field
- **Polling Interval**: Fixed at 500ms (not user-configurable)

---

## Move Navigation & Review Features

### Live Game Mode
While a game is in progress:

**Move Counter Display**:
- Shows "live" when viewing the current position
- Updates in real-time as moves are made
- Navigation buttons allow scrubbing through move history

**Navigation During Live Play**:
- **Previous moves**: Click ◀ button or ⏮ to jump to earlier position
- **Leaves live mode**: Counter shows "N/M" instead of "live"
- **Back to live**: Click ⏭ or ▶ to return to current position

**Evaluation Bar** (Bot Mode Only):
- Real-time position evaluation from Stockfish (in pawns)
- White bar = White advantage, Black bar = Black advantage
- Ranges from –10 to +10 pawns (clamped for display)
- Text shows exact evaluation (e.g., "+2.45", "–1.12", "0.00")

---

### Game Review Mode

**Accessing Archived Games**:
1. Click "↺" Game History button
2. Select game from list (sorted newest first)
3. Enter Review Mode

**Review Panel Layout**:

```
[Metadata]
Date | Mode | Result | Winner
─────────────────────────────
[Move List]
1. e4              1... c5
2. Nf3            2... d6
3. d4             3... cxd4
[Navigation buttons]
⏮ ◀ move# ▶ ⏭
```

**Move List Features**:
- **PGN notation**: Standard chess notation (e4, Nf3, cxd4, etc.)
- **Board edits**: Marked with "✏️ Board Edit #N" section headers
- **Clickable moves**: Click any move to jump to that position
- **Auto-scroll**: Highlighted move scrolls into view
- **Active highlight**: Current move highlighted for easy tracking

**Navigation**:
- Click move → board updates to that position
- Click move number → jump to start of that move pair
- Click board edit marker → jump to edited position

**Segment-Based Architecture**:
- Each FEN checkpoint creates a new "segment"
- Segment 0: Game start position
- Segment 1+: Board edits within a game
- Each segment has independent move list and engine state
- Moves span across segments; navigation seamlessly transitions

**Metadata Display**:
- **Date**: DD/MM/YYYY HH:MM format
- **Opponent/Mode**: 
  - Human vs Human
  - vs Stockfish (Difficulty)
- **Result**: 
  - White wins / Black wins / Draw
  - Termination reason: Checkmate, Stalemate, 50-move rule, 3-fold repetition, Draw by agreement, Insufficient material, Resignation
- **Winner**: Color and piece-square indicators

---

### Game File Format (MoveHistory)

**Binary Encoding** (client-side parsing in JavaScript):

```
Header (16 bytes):
  version (1) | mode (1) | result (1) | winner (1) | player_color (1) | 
  bot_depth (1) | move_count (2) | fen_entries (2) | last_fen_offset (2) | 
  timestamp (4)

Moves (2 bytes each, move_count total):
  [6 bits: from square] [6 bits: to square] [4 bits: promotion code]
  
  Promotion codes: 0=none, 1=queen, 2=rook, 3=bishop, 4=knight
  
  FEN Markers: 0xFFFF (starts new segment)

FEN Table (appended):
  [fen_entries total]
  Each: [1 byte: length] [N bytes: FEN string]
```

**Storage Locations**:
- Live games: `/games/live.bin` (moves) + `/games/live_fen.bin` (FENs)
- Completed: `/games/game_NN.bin` (1-based, zero-padded)
- Max games: 50 (oldest deleted when limit reached)
- Max usage: 80% of LittleFS space

**Parsing** (JavaScript):
- Moves decoded using bitwise shifts
- FEN markers signal segment boundaries
- Each segment maintains independent chess.js engine
- Move scrubbing reconstructs position at any point

---

### Move Validation & Legal Move Checking

The ChessEngine validates all moves against standard FIDE rules:

**Constraints Checked**:
- **Piece-specific movement**: Pawn, Rook, Bishop, Knight, Queen, King
- **Capture legality**: Can only capture opponent pieces
- **Check/Checkmate**: Cannot move into check; recognizes checkmate/stalemate
- **Castling rights**: Tracks when rooks/king move (rights revoked)
- **En passant**: Validates two-square pawn advances and en passant captures
- **50-move rule**: Draw when 50 half-moves pass without pawn move/capture
- **3-fold repetition**: Zobrist hashing tracks repeated positions

**FEN Validation** (Edit Mode):
- Position format validation
- Piece count limits (8 pawns max, 16 total per side)
- King presence (exactly 1 per side)
- Pawn rank validation (not on 1st/8th rank)
- Castling rights consistency
- En passant square legality
- Turn validity

---

## API Endpoints Summary

### Game State & Control
- `GET/POST /board-update` – FEN polling / board edit
- `POST /promotion` – Pawn promotion choice
- `POST /resign` – Resign game
- `POST /draw` – Offer/accept draw

### Game History
- `GET /games` – List of all saved games (JSON)
- `GET /games?id=GAME_ID` – Download game binary
- `DELETE /games?id=GAME_ID` – Delete game

### Settings & Configuration
- `GET/POST /board-settings` – LED brightness, dim multiplier
- `GET/POST /hardware-config` – GPIO pin configuration
- `POST /gameselect` – Select game mode + config
- `GET/POST /wifi` – WiFi credentials
- `GET/POST /lichess` – Lichess API token
- `POST /ota/upload/firmware` – Upload firmware binary
- `POST /ota/upload/web` – Upload web assets tar

---

## Quick Reference: Game Flow

1. **Startup** → NVS init → calibration check → WiFi init → live game resume check
2. **Mode Selection** → 4 colored LEDs light; player places piece to select
3. **Game Init** → Board setup confirmed with firework animation
4. **Gameplay Loop** (40ms cycles):
   - Read sensors (physical board state)
   - Detect changes (pieces lifted/placed)
   - Validate moves using ChessEngine
   - Show LED feedback
   - For bots: call Stockfish API, wait for response, execute move
   - For online: poll Lichess, sync board state
5. **Game End** → Save game, return to mode selection
6. **Anytime**: Web UI accessible for review, settings, board edit

---

## Additional Resources

- **Source Files**:
  - `board.html` – Web UI with move navigation and review
  - `board_driver.h/.cpp` – Hardware abstraction, LED animations, sensor polling
  - `chess_game.h/.cpp` – Base game logic, move validation, LED feedback
  - `chess_moves.h/.cpp` – Human vs human mode
  - `chess_bot.h/.cpp` – Stockfish integration
  - `chess_lichess.h/.cpp` – Lichess online mode
  - `sensor_test.h/.cpp` – Diagnostic mode
  - `led_colors.h` – Color constants and semantics
  - `stockfish_api.h/.cpp` – HTTP/S API client for Stockfish
  - `lichess_api.h/.cpp` – HTTP/S API client for Lichess
  - `move_history.h/.cpp` – Game save/load/resume system

- **External APIs**:
  - Stockfish: `https://stockfish.online:443/api/s/v2.php`
  - Lichess: `https://lichess.org:443`

---

*Documentation generated from OpenChess source code analysis*
