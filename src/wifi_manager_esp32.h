#ifndef WIFI_MANAGER_ESP32_H
#define WIFI_MANAGER_ESP32_H

#include "board_driver.h"
#include "ota_updater.h"
#include "stockfish_settings.h"
#include <Arduino.h>
#include <AsyncTCP.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>

// Forward declarations
struct LichessConfig;
class MoveHistory;

// ---------------------------
// WiFi Configuration
// ---------------------------
#define AP_SSID "OpenChess"
#define AP_PASSWORD "chess123"
#define HTTP_PORT 80
#define MDNS_HOSTNAME "openchess"
#define MAX_WIFI_PROFILES 3
#define NVS_WIFI_NAMESPACE "wifiProfiles"

// ---------------------------
// WiFi Profile (saved network)
// ---------------------------
struct WiFiProfile {
  String ssid;
  String password;
  uint8_t bssid[6]; // MAC address for fast reconnect
  uint8_t channel;  // Channel for fast reconnect
  bool hasBssid;    // Whether bssid/channel are valid cached values

  WiFiProfile() : channel(0), hasBssid(false) { memset(bssid, 0, sizeof(bssid)); }
};

// ---------------------------
// Scanned network info (sent to frontend)
// ---------------------------
struct ScannedNetwork {
  String ssid;
  int32_t rssi;
  uint8_t bssid[6];
  uint8_t channel;
  uint8_t encryptionType; // wifi_auth_mode_t cast to uint8_t
};

// ---------------------------
// WiFi Manager Class for ESP32
// ---------------------------
class WiFiManagerESP32 {
 private:
  AsyncWebServer server;
  DNSServer dnsServer;

  TaskHandle_t pendingWiFiTaskHandle = nullptr;
  static void pendingWiFiBackgroundTask(void* param);
  TaskHandle_t dnsTaskHandle = nullptr;
  static void dnsTask(void* param);
  void startCaptivePortal();
  void stopCaptivePortal();

  Preferences prefs;
  String gameMode;
  String lichessToken;
  
  // Set device name
  String deviceName = "OpenChess";
  
  // Saved WiFi profiles (up to MAX_WIFI_PROFILES, index 0 = most recently connected)
  WiFiProfile profiles[MAX_WIFI_PROFILES];
  int profileCount;
  bool scanAllChannels;
  int connectedProfileIndex; // Index of currently connected profile, or -1

  // Scan results (populated by deferred scan task)
  ScannedNetwork* scanResults;
  int scanResultCount;

  BotConfig botConfig = {StockfishSettings::medium(), true};

  MoveHistory* moveHistory;
  BoardDriver* boardDriver;
  String currentFen;
  float boardEvaluation;

  // Board edit storage (pending edits from web interface)
  String pendingFenEdit;
  volatile bool hasPendingEdit;

  // Pending resign/draw from web interface
  volatile bool hasPendingResign;
  volatile bool hasPendingDraw;
  char pendingResignColor; // 'w' or 'b' — the side resigning

  // Promotion state for web-based piece selection
  struct PromotionState {
    volatile bool pending; // True while waiting for web client to choose a piece
    volatile char choice;  // Piece chosen by web client ('q','r','b','n') or ' ' if none yet
    char color;            // 'w' or 'b' — color of the promoting pawn
    void reset() {
      pending = false;
      choice = ' ';
      color = ' ';
    }
  };
  PromotionState promotion;

  // Web client heartbeat (tracks whether board.html is actively polling)
  unsigned long lastBoardPollTime; // millis() of last /board-update GET request

  // Deferred WiFi actions (set by web handler, processed by worker task)
  enum PendingAction {
    NONE,
    CONNECT_SAVED,
    CONNECT_NEW,
    DELETE_PROFILE,
    SCAN_NETWORKS
  };
  struct PendingWiFi {
    volatile PendingAction action;
    int profileIndex;    // For CONNECT_SAVED / DELETE_PROFILE
    String newSSID;      // For CONNECT_NEW
    String newPassword;  // For CONNECT_NEW
    uint8_t newBssid[6]; // For CONNECT_NEW (from scan)
    uint8_t newChannel;  // For CONNECT_NEW (from scan)
    void reset() {
      action = NONE;
      profileIndex = -1;
      newSSID = "";
      newPassword = "";
      memset(newBssid, 0, 6);
      newChannel = 0;
    }
  };
  PendingWiFi pendingWiFi;

  // NVS persistence for WiFi profiles
  void loadProfiles();
  void saveProfiles(bool saveAllProfiles = true);
  void promoteProfile(int index);

  // WiFi connection helpers
  bool waitForConnection(int maxAttempts);
  bool tryConnect(const String& ssid, const String& password, const uint8_t* bssid = nullptr, uint8_t channel = 0);
  bool tryConnectProfile(int index);
  bool connectToSavedProfile(); // Try all saved profiles, promote winner; returns true if connected
  void startAPFallback();
  void performScan();

  // Web interface methods
  String getWiFiInfoJSON();
  String getBoardUpdateJSON();
  String getLichessInfoJSON();
  String getBoardSettingsJSON();
  void handleBoardEditSuccess(AsyncWebServerRequest* request);
  void handlePromotion(AsyncWebServerRequest* request);
  void handleConnectWiFi(AsyncWebServerRequest* request);
  void handleGameSelection(AsyncWebServerRequest* request);
  void handleSaveLichessToken(AsyncWebServerRequest* request);
  void handleBoardSettings(AsyncWebServerRequest* request);
  void handleBoardCalibration(AsyncWebServerRequest* request);
  void handleResign(AsyncWebServerRequest* request);
  void handleDraw(AsyncWebServerRequest* request);
  void getHardwareConfigJSON(AsyncWebServerRequest* request);
  void handleHardwareConfig(AsyncWebServerRequest* request);
  void handleGamesRequest(AsyncWebServerRequest* request);
  void handleDeleteGame(AsyncWebServerRequest* request);
  // OTA update handlers
  void handleOtaStatus(AsyncWebServerRequest* request);
  void handleOtaSettings(AsyncWebServerRequest* request);
  void handleOtaApply(AsyncWebServerRequest* request);
  // ESPAsyncWebServer body handlers receive data in chunks via callbacks (data, len, index, total),
  // not as a continuous Stream. This means we can't reuse the Stream-based OtaUpdater methods directly.
  // For firmware: we call Update.begin/write/end incrementally across chunks.
  // For web assets: we buffer the TAR to a temp file first, then pass it as a Stream to the TAR parser
  // (the TAR format requires sequential 512-byte header reads that can't be split across async chunks).
  void onFirmwareUploadBody(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total);
  void onWebAssetsUploadBody(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total);
  void startMDNS();

  // OTA state
  OtaUpdater otaUpdater;
  OtaUpdateInfo lastUpdateInfo;
  bool otaChecked;
  bool autoOtaEnabled;
  // Temporary file for web asset TAR upload (needed because the TAR parser requires a seekable Stream)
  File otaTarFile;

 public:
  WiFiManagerESP32(BoardDriver* boardDriver, MoveHistory* moveHistory);
  void begin();
  
  // Set device name
  void setDeviceName(const String& name);
  String getDeviceName() const { return deviceName; }
  void loadDeviceConfig();  // Call in begin()
  void saveDeviceConfig();  // Call after updating name
  
  // OTA update support
  OtaUpdater& getOtaUpdater() { return otaUpdater; }
  bool isAutoOtaEnabled() const { return autoOtaEnabled; }

  // Game selection via web
  int getSelectedGameMode() const { return gameMode.toInt(); }
  void resetGameSelection() { gameMode = "0"; };
  // Bot configuration
  BotConfig getBotConfig() { return botConfig; }
  // Lichess configuration
  LichessConfig getLichessConfig();
  String getLichessToken() { return lichessToken; }
  // Board state management (FEN-based)
  void updateBoardState(const String& fen, float evaluation = 0.0f);
  String getCurrentFen() const { return currentFen; }
  float getEvaluation() const { return boardEvaluation; }
  // Board edit management (FEN-based)
  bool getPendingBoardEdit(String& fenOut);
  void clearPendingEdit();
  // Resign/Draw management (from web interface)
  bool getPendingResign(char& resignColor);
  bool getPendingDraw();
  void clearPendingResign();
  void clearPendingDraw();
  // Promotion management (from web interface)
  void startPromotionWait(char color);
  bool isPromotionPending() const { return promotion.pending; }
  char getPromotionChoice() const { return promotion.choice; }
  void clearPromotion();
  // Web client connection check
  bool isWebClientConnected() const;
  // Check if WiFi is connected (re-attempts if not)
  bool ensureConnected();
  // Processes deferred WiFi actions (called by pendingWiFiBackgroundTask)
  void checkPendingWiFi();
};

#endif // WIFI_MANAGER_ESP32_H
