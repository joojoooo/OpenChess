#ifndef BOARD_DRIVER_H
#define BOARD_DRIVER_H

#include "led_colors.h"
#include <NeoPixelBusLg.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

// ---------------------------
// Default Hardware Configuration
// These defaults are used if no pin configuration is saved in NVS.
// Users with pre-built firmware can change pins via the web UI at runtime.
// ---------------------------

// ---------------------------
// WS2812B LED Data IN GPIO Pin
// The strip doesn't need to have a specific layout, calibration will map it correctly
// ---------------------------
#define LED_PIN 32
#define NUM_ROWS 8
#define NUM_COLS 8
#define LED_COUNT (NUM_ROWS * NUM_COLS)
#define BRIGHTNESS 255 // Default LED brightness: 0-255 (0=off, 255=max). (can be changed later from webUI)

// ---------------------------
// Shift Register (74HC595) Pins
// ---------------------------
// Pin 10 (SRCLR') 5V = don't clear the register
// Pin 13 (OE') GND = always enabled
// Pin 11 (SRCLK) GPIO = Shift Register Clock
#define SR_CLK_PIN 14
// Pin 12 (RCLK) GPIO = Latch Clock
#define SR_LATCH_PIN 26
// Pin 14 (SER) GPIO = Serial data input
#define SR_SER_DATA_PIN 33
// Set to 1 if the shift register outputs drive PNP transistors
#define SR_INVERT_OUTPUTS 1

// ---------------------------
// Row and column pins don't need to be in any particular order, calibration will map them correctly
// ---------------------------

// ---------------------------
// Row Input Pins (Safe GPIOs for ESP32: 4, 13, 14, [16-17], 18, 19, 21, 22, 23, 25, 26, 27, 32, 33)
// ---------------------------
#define ROW_PIN_0 4
#define ROW_PIN_1 16
#define ROW_PIN_2 17
#define ROW_PIN_3 18
#define ROW_PIN_4 19
#define ROW_PIN_5 21
#define ROW_PIN_6 22
#define ROW_PIN_7 23

// ---------------------------
// Runtime hardware configuration (loaded from NVS, editable via web UI)
// Falls back to the #define defaults above if no NVS data exists.
// ---------------------------
struct HardwareConfig {
  uint8_t ledPin;
  uint8_t srClkPin;
  uint8_t srLatchPin;
  uint8_t srDataPin;
  bool srInvertOutputs;
  uint8_t rowPins[NUM_ROWS];

  // Initialize with compile-time defaults
  static HardwareConfig defaults() {
    return {LED_PIN, SR_CLK_PIN, SR_LATCH_PIN, SR_SER_DATA_PIN, SR_INVERT_OUTPUTS != 0, {ROW_PIN_0, ROW_PIN_1, ROW_PIN_2, ROW_PIN_3, ROW_PIN_4, ROW_PIN_5, ROW_PIN_6, ROW_PIN_7}};
  }
};

// ---------------------------
// Sensor Polling Delay and Debounce
// ---------------------------
#define SENSOR_READ_DELAY_MS 40
#define DEBOUNCE_MS 125
#define CALIBRATION_WARNING_INTERVAL_MS 4000

// Animation job types for async queue
enum class AnimationType : uint8_t { CAPTURE,
                                     PROMOTION,
                                     BLINK,
                                     WAITING,
                                     THINKING,
                                     FIREWORK,
                                     FLASH };

// Animation job with parameters union for queue
struct AnimationJob {
  AnimationType type;
  std::atomic<bool>* stopFlag; // For cancellable animations
  union {
    struct {
      int row, col;
    } capture;
    struct {
      int row, col;
    } promotion;
    struct {
      int row, col;
      LedRGB color;
      int times;
      bool clearAfter;
    } blink;
    struct {
      LedRGB color;
      int times;
    } flash;
    struct {
      LedRGB color;
    } firework;
  } params;
};

// ---------------------------
// Board Driver Class
// Logical board coordinates: row 0 = rank 8, column 0 = file a
// ---------------------------
class BoardDriver {
 private:
  NeoPixelBusLg<NeoGrbFeature, NeoEsp32I2s1Ws2812xMethod, NeoGammaNullMethod>* strip;

  // Animation queue system
  static QueueHandle_t animationQueue;
  static TaskHandle_t animationTaskHandle;
  static SemaphoreHandle_t ledMutex;
  static BoardDriver* instance;
  static void animationWorkerTask(void* param);
  void executeAnimation(const AnimationJob& job);
  void doCapture(int row, int col);
  void doPromotion(int row, int col);
  void doBlink(int row, int col, LedRGB color, int times, bool clearAfter);
  void doWaiting(std::atomic<bool>* stopFlag);
  void doThinking(std::atomic<bool>* stopFlag);
  void doFirework(LedRGB color);
  void doFlash(LedRGB color, int times);
  bool sensorState[NUM_ROWS][NUM_COLS];
  bool sensorPrev[NUM_ROWS][NUM_COLS];
  bool sensorRaw[NUM_ROWS][NUM_COLS];
  unsigned long sensorDebounceTime[NUM_ROWS][NUM_COLS];
  int lastEnabledCol; // Tracks last enabled column for efficient sequential shifting

  enum Axis {
    RowsAxis = 0,
    ColsAxis = 1,
    UnknownAxis = 2,
  };
  // LED settings (persisted in NVS)
  uint8_t brightness;                       // Global brightness 0-255
  uint8_t dimMultiplier;                    // Dark square dim factor 0-100 (stored as percentage)
  LedRGB currentColors[NUM_ROWS][NUM_COLS]; // Track current colors for dim multiplier updates

  // Runtime hardware pin configuration (persisted in NVS)
  HardwareConfig hwConfig;

  // True while interactive calibration is running (guards concurrent web handler access)
  std::atomic<bool> calibrating;
  bool calibrated;

  // Calibration data
  uint8_t swapAxes;
  uint8_t toLogicalRow[NUM_ROWS];
  uint8_t toLogicalCol[NUM_COLS];
  uint8_t ledIndexMap[NUM_ROWS][NUM_COLS];

  bool loadCalibration();
  void saveCalibration();
  bool runCalibration();
  void loadLedSettings();
  void loadHardwareConfig();
  void readRawSensors(bool rawState[NUM_ROWS][NUM_COLS]);
  bool waitForBoardEmpty(unsigned long stableMs = 500);
  bool waitForSingleRawPress(int& rawRow, int& rawCol, unsigned long stableMs = 500);
  void showCalibrationError();
  bool calibrateAxis(Axis axis, uint8_t* axisPinsOrder, size_t NUM_PINS, bool firstAxisSwapped);
  String axisToChessRankFile(Axis axis) const { return (axis == RowsAxis) ? "Rank" : ((axis == ColsAxis) ? "File" : "Unknown"); };

  void loadShiftRegister(byte data, int bits = 8);
  void disableAllCols();
  void enableCol(int col);
  int getPixelIndex(int row, int col);

 public:
  BoardDriver();
  void beginHardware();
  void checkCalibration();
  void readSensors();
  bool getSensorState(int row, int col);
  bool getSensorPrev(int row, int col);
  void updateSensorPrev();

  // LED Control (use acquireLEDs/releaseLEDs for multi-call sequences)
  void acquireLEDs(); // Block until LED strip available
  void releaseLEDs(); // Release LED strip
  void clearAllLEDs(bool show = true);
  void setSquareLED(int row, int col, LedRGB color);
  void showLEDs();

  // Animation Functions (queued for async execution)
  void fireworkAnimation(LedRGB color = LedColors::White);
  void captureAnimation(int row, int col);
  void promotionAnimation(int row, int col);
  void blinkSquare(int row, int col, LedRGB color, int times = 3, bool clearAfter = true);
  void showConnectingAnimation();
  void flashBoardAnimation(LedRGB color, int times = 3);

  // Start a cancellable animation. Returns a non-owning pointer to a stop flag.
  // Ownership: the animation task owns and deletes the flag after the animation loop exits.
  // Caller must ONLY call store(true) to signal stop, never delete the pointer.
  std::atomic<bool>* startThinkingAnimation();
  std::atomic<bool>* startWaitingAnimation();

  // Board settings
  uint8_t getBrightness() const { return brightness; }
  uint8_t getDimMultiplier() const { return dimMultiplier; }
  void setBrightness(uint8_t value);
  void setDimMultiplier(uint8_t value);
  void saveLedSettings();
  void triggerCalibration();

  // Hardware pin configuration (runtime, persisted in NVS)
  const HardwareConfig& getHardwareConfig() const { return hwConfig; }
  void saveHardwareConfig(const HardwareConfig& config);
};

#endif // BOARD_DRIVER_H
