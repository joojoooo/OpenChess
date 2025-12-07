#include <Adafruit_NeoPixel.h>

// ---------------------------
// NeoPixel Setup (WS2812B)
// ---------------------------
#define LED_PIN 32 // WS2812B LED Data IN GPIO pin
#define NUM_ROWS 2
#define NUM_COLS 2
#define LED_COUNT (NUM_ROWS * NUM_COLS)
#define BRIGHTNESS 255
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

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

// ---------------------------
// Row Input Pins (Safe pins for ESP32: 4, 13, 14, [16-17], 18, 19, 21, 22, 23, 25, 26, 27, 32, 33)
// ---------------------------
int rowPins[NUM_ROWS] = {4, 16 /*, 17, 18, 19, 21, 22, 23*/};

// ---------------------------
// LED Strip Col/Row to Pixel index Map
// ---------------------------
int LEDStripRowColMap[NUM_ROWS][NUM_COLS] = {
    {1, 0},
    {2, 3},
};

void loadShiftRegister(byte data)
{
  digitalWrite(SR_LATCH_PIN, LOW);
  for (int i = 7; i >= 0; i--)
  {
    digitalWrite(SR_SER_DATA_PIN, !!(data & (1 << i)));
    digitalWrite(SR_CLK_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(SR_CLK_PIN, LOW);
    delayMicroseconds(10);
  }
  digitalWrite(SR_LATCH_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(SR_LATCH_PIN, LOW);
}
void disableAllCols()
{
  loadShiftRegister(0);
}
void enableCol(int col)
{
  loadShiftRegister(((byte)((1 << (col)) & 0xFF)));
  delayMicroseconds(100); // Allow time for the column to stabilize, otherwise random readings occur
}

void setup()
{
  Serial.begin(115200);
  Serial.println("Starting up...");
  // NeoPixel setup
  strip.begin();
  strip.show(); // Initialize all pixels to 'off'
  strip.setBrightness(BRIGHTNESS);
  // Shift register pins as outputs
  pinMode(SR_SER_DATA_PIN, OUTPUT);
  pinMode(SR_CLK_PIN, OUTPUT);
  pinMode(SR_LATCH_PIN, OUTPUT);
  disableAllCols();
  // Row pins as inputs
  for (int r = 0; r < NUM_ROWS; r++)
    pinMode(rowPins[r], INPUT);
  Serial.println("Setup complete!");
}

void loop()
{
  for (int i = 0; i < LED_COUNT; i++)
    strip.setPixelColor(i, 0);

  for (int col = 0; col < NUM_COLS; col++)
  {
    enableCol(col);
    for (int row = 0; row < NUM_ROWS; row++)
    {
      if (digitalRead(rowPins[row]) == LOW)
      {
        int pixelIndex = LEDStripRowColMap[row][col];
        uint32_t color;
        if (pixelIndex % 2 == 0)
          color = strip.Color(0, 80, 0);
        else
          color = strip.Color(0, 200, 0);
        strip.setPixelColor(pixelIndex, color);
        Serial.println("[" + String(row) + "][" + String(col) + "]=" + String(pixelIndex));
      }
    }
  }

  strip.show();
  disableAllCols();
  delay(100);
}