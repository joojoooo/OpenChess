#include "wifi_manager_esp32.h"
#include "chess_lichess.h"
#include "chess_utils.h"
#include "move_history.h"
#include "version.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Update.h>
#include <esp_wifi.h>

static const char* INITIAL_FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
// Samsung captive portal detection is more reliable when SoftAP is not in RFC1918 ranges.
static const IPAddress AP_IP(200, 200, 200, 1);
static const IPAddress AP_GATEWAY(200, 200, 200, 1);
static const IPAddress AP_SUBNET(255, 255, 255, 0);

WiFiManagerESP32::WiFiManagerESP32(BoardDriver* bd, MoveHistory* mh) : boardDriver(bd), moveHistory(mh), server(HTTP_PORT), gameMode("0"), lichessToken(""), botConfig(), scanAllChannels(false), profileCount(0), connectedProfileIndex(-1), scanResults(nullptr), scanResultCount(0), currentFen(INITIAL_FEN), hasPendingEdit(false), hasPendingResign(false), hasPendingDraw(false), pendingResignColor('?'), promotion{}, lastBoardPollTime(0), boardEvaluation(0.0f), otaUpdater(bd), autoOtaEnabled(false), otaChecked(false) {
  promotion.reset();
  pendingWiFi.reset();
}

void WiFiManagerESP32::begin() {
  Serial.println("=== Starting OpenChess WiFi Manager ===");

  if (ChessUtils::ensureNvsInitialized()) {
    // Load WiFi profiles
    loadProfiles();
    // Load Lichess token
    prefs.begin("lichess", false);
    if (prefs.isKey("token"))
      lichessToken = prefs.getString("token", "");
    prefs.end();
    if (lichessToken.length() > 0)
      Serial.println("Lichess API token loaded from NVS");
    // Load OTA auto-update preference
    prefs.begin("ota", false);
    autoOtaEnabled = prefs.getBool("autoUpdate", false);
    prefs.end();
  }
  loadDeviceConfig();
  bool connected = connectToSavedProfile();
  Serial.println("==== WiFi Connection Information ====");
  if (connected) {
    Serial.println("Connected to WiFi network:");
    Serial.println("- SSID: " + profiles[0].ssid);
    Serial.println("- Password: " + profiles[0].password);
    Serial.println("- Website: http://" MDNS_HOSTNAME ".local (" + WiFi.localIP().toString() + ")");
  } else {
    startAPFallback();
    Serial.println("A WiFi Access Point was created:");
    Serial.println("- SSID: " AP_SSID);
    Serial.println("- Password: " AP_PASSWORD);
    Serial.println("- Website: http://" MDNS_HOSTNAME ".local (" + WiFi.softAPIP().toString() + ")");
    Serial.println("- MAC Address: " + WiFi.softAPmacAddress());
    Serial.println("Configure WiFi credentials from WebUI to join your WiFi network");
  }
  Serial.println("=====================================\n");

  if (autoOtaEnabled && lastUpdateInfo.available)
    otaUpdater.applyUpdate(lastUpdateInfo);

  auto sendCaptiveRedirect = [this](AsyncWebServerRequest* request) {
    AsyncWebServerResponse* response = request->beginResponse(302);
    response->addHeader("Location", "http://" + WiFi.softAPIP().toString() + "/");
    response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    response->addHeader("Pragma", "no-cache");
    response->addHeader("Expires", "0");
    request->send(response);
  };
  // Captive portal detection endpoints
  // References: https://github.com/tonyp7/esp32-wifi-manager/issues/57 https://github.com/tripflex/captive-portal/blob/master/src/mgos_captive_portal.c#L369-L375
  // Android/Samsung
  server.on("/mobile/status.php", HTTP_GET, [sendCaptiveRedirect](AsyncWebServerRequest* request) { sendCaptiveRedirect(request); });
  server.on("/generate_204", HTTP_GET, [sendCaptiveRedirect](AsyncWebServerRequest* request) { sendCaptiveRedirect(request); });
  server.on("/gen_204", HTTP_GET, [sendCaptiveRedirect](AsyncWebServerRequest* request) { sendCaptiveRedirect(request); });
  // Windows
  server.on("/ncsi.txt", HTTP_GET, [sendCaptiveRedirect](AsyncWebServerRequest* request) { sendCaptiveRedirect(request); });
  // Firefox/OSX
  server.on("/success.txt", HTTP_GET, [sendCaptiveRedirect](AsyncWebServerRequest* request) { sendCaptiveRedirect(request); });
  // Apple
  server.on("/hotspot-detect.html", HTTP_GET, [sendCaptiveRedirect](AsyncWebServerRequest* request) { sendCaptiveRedirect(request); });
  // Set up OpenChess web server routes
  server.on("/board-update", HTTP_GET, [this](AsyncWebServerRequest* request) { request->send(200, "application/json", this->getBoardUpdateJSON()); });
  server.on("/board-update", HTTP_POST, [this](AsyncWebServerRequest* request) { this->handleBoardEditSuccess(request); });
  server.on("/promotion", HTTP_POST, [this](AsyncWebServerRequest* request) { this->handlePromotion(request); });
  server.on("/resign", HTTP_POST, [this](AsyncWebServerRequest* request) { this->handleResign(request); });
  server.on("/draw", HTTP_POST, [this](AsyncWebServerRequest* request) { this->handleDraw(request); });
  server.on("/wifi", HTTP_GET, [this](AsyncWebServerRequest* request) { request->send(200, "application/json", this->getWiFiInfoJSON()); });
  server.on("/wifi", HTTP_POST, [this](AsyncWebServerRequest* request) { this->handleConnectWiFi(request); });
  server.on("/gameselect", HTTP_POST, [this](AsyncWebServerRequest* request) { this->handleGameSelection(request); });
  server.on("/lichess", HTTP_GET, [this](AsyncWebServerRequest* request) { request->send(200, "application/json", this->getLichessInfoJSON()); });
  server.on("/lichess", HTTP_POST, [this](AsyncWebServerRequest* request) { this->handleSaveLichessToken(request); });
  server.on("/board-settings", HTTP_GET, [this](AsyncWebServerRequest* request) { request->send(200, "application/json", this->getBoardSettingsJSON()); });
  server.on("/board-settings", HTTP_POST, [this](AsyncWebServerRequest* request) { this->handleBoardSettings(request); });
  server.on("/board-calibrate", HTTP_POST, [this](AsyncWebServerRequest* request) { this->handleBoardCalibration(request); });
  server.on("/games", HTTP_GET, [this](AsyncWebServerRequest* request) { this->handleGamesRequest(request); });
  server.on("/games", HTTP_DELETE, [this](AsyncWebServerRequest* request) { this->handleDeleteGame(request); });
  server.on("/hardware-config", HTTP_GET, [this](AsyncWebServerRequest* request) { this->getHardwareConfigJSON(request); });
  server.on("/hardware-config", HTTP_POST, [this](AsyncWebServerRequest* request) { this->handleHardwareConfig(request); });
  // Set device name
  server.on(
  "/device-config", HTTP_GET, 
  [this](AsyncWebServerRequest* request) {
      JsonDocument doc;
      doc["deviceName"] = deviceName;
      String output;
      serializeJson(doc, output);
      request->send(200, "application/json", output);
  });
  server.on(
  "/device-config", HTTP_POST, 
  [this](AsyncWebServerRequest* request) {
      if (request->hasArg("name")) {
          String newName = request->arg("name");
          newName.trim();
          if (newName.length() > 0 && newName.length() <= 32 && newName.indexOf(' ') == -1) {
              setDeviceName(newName);
              request->send(200, "text/plain", "Device name updated");
              // Optionally restart mDNS after a delay
              xTaskCreate([](void*) {
                  delay(500);
                  MDNS.end();
                  // Restart with new name (will be picked up on next WiFi operation)
                  vTaskDelete(NULL);
              }, "mdns_restart", 2048, NULL, 1, NULL);
          } else {
              request->send(400, "text/plain", "Invalid name (1-32 chars, no spaces)");
          }
      } else {
          request->send(400, "text/plain", "Missing name parameter");
      }
  });
  // OTA update endpoints
  server.on("/ota/status", HTTP_GET, [this](AsyncWebServerRequest* request) { this->handleOtaStatus(request); });
  server.on("/ota/settings", HTTP_POST, [this](AsyncWebServerRequest* request) { this->handleOtaSettings(request); });
  server.on("/ota/apply", HTTP_POST, [this](AsyncWebServerRequest* request) { this->handleOtaApply(request); });
  // OTA manual upload endpoints - JS sends raw binary body (application/octet-stream), so only the body handler (3rd callback) fires; the multipart file handler (2nd) is unused.
  server.on(
      "/ota/upload/firmware", HTTP_POST,
      [](AsyncWebServerRequest* request) {},
      NULL,
      [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) { this->onFirmwareUploadBody(request, data, len, index, total); });
  server.on(
      "/ota/upload/web", HTTP_POST,
      [](AsyncWebServerRequest* request) {},
      NULL,
      [this](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) { this->onWebAssetsUploadBody(request, data, len, index, total); });
  // Serve sound files directly (no gzip variant exists, avoids .gz probe errors)
  server.serveStatic("/sounds/", LittleFS, "/sounds/").setTryGzipFirst(false);
  // Serve piece SVGs with aggressive caching, otherwise chrome doesn't actually use the cached versions
  server.serveStatic("/pieces/", LittleFS, "/pieces/").setCacheControl("max-age=31536000, immutable");
  // Serve all other static files from LittleFS (gzip handled automatically)
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
  server.onNotFound([](AsyncWebServerRequest* request) { request->send(404, "text/plain", "Not Found"); });
  server.begin();
  Serial.println("Web server started on port: " + String(HTTP_PORT));

  xTaskCreate(pendingWiFiBackgroundTask, "WiFi_Pending_Task", 8192, this, 4, &pendingWiFiTaskHandle);
}

String WiFiManagerESP32::getBoardUpdateJSON() {
  this->lastBoardPollTime = millis();
  JsonDocument doc;
  doc["fen"] = currentFen;
  doc["evaluation"] = serialized(String(boardEvaluation, 2));
  if (promotion.pending) {
    JsonObject promo = doc["promotion"].to<JsonObject>();
    promo["color"] = String(promotion.color);
  }
  String output;
  serializeJson(doc, output);
  return output;
}

String WiFiManagerESP32::getWiFiInfoJSON() {
  bool connected = (WiFi.status() == WL_CONNECTED);
  JsonDocument doc;
  doc["connected"] = connected;
  doc["scanAllChannels"] = scanAllChannels;
  doc["connectedIndex"] = connectedProfileIndex;

  // Saved networks (never include passwords)
  JsonArray saved = doc["saved"].to<JsonArray>();
  for (int i = 0; i < profileCount; i++) {
    JsonObject net = saved.add<JsonObject>();
    net["ssid"] = profiles[i].ssid;
    net["channel"] = profiles[i].channel;

    bool found = false;
    // Add RSSI from scan results if available
    for (int j = 0; j < scanResultCount; j++) {
      if (scanResults[j].ssid == profiles[i].ssid) {
        net["rssi"] = scanResults[j].rssi;
        net["enc"] = scanResults[j].encryptionType;
        found = true;
        break;
      }
    }
    // For the connected profile, use live WiFi info if scan didn't cover it
    wifi_ap_record_t apInfo;
    if (!found && connected && i == connectedProfileIndex && esp_wifi_sta_get_ap_info(&apInfo) == ESP_OK) {
      net["rssi"] = apInfo.rssi;
      net["enc"] = (uint8_t)apInfo.authmode;
    }
  }

  // Scanned networks (filter out already-saved SSIDs)
  JsonArray scanned = doc["scanned"].to<JsonArray>();
  for (int i = 0; i < scanResultCount; i++) {
    bool alreadySaved = false;
    for (int j = 0; j < profileCount; j++) {
      if (scanResults[i].ssid == profiles[j].ssid) {
        alreadySaved = true;
        break;
      }
    }
    if (alreadySaved || scanResults[i].ssid.length() == 0) continue;
    // Deduplicate: skip if this SSID was already added (multiple APs with same SSID)
    bool duplicate = false;
    for (int k = 0; k < i; k++) {
      if (scanResults[k].ssid == scanResults[i].ssid) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) continue;

    JsonObject net = scanned.add<JsonObject>();
    net["ssid"] = scanResults[i].ssid;
    net["rssi"] = scanResults[i].rssi;
    net["channel"] = scanResults[i].channel;
    net["enc"] = scanResults[i].encryptionType;
  }

  String output;
  serializeJson(doc, output);
  return output;
}

void WiFiManagerESP32::handleBoardEditSuccess(AsyncWebServerRequest* request) {
  if (request->hasArg("fen")) {
    pendingFenEdit = request->arg("fen");
    hasPendingEdit = true;
    Serial.println("Board edit received (FEN): " + pendingFenEdit);
    request->send(200, "text/plain", "OK");
  } else {
    Serial.println("Board edit failed: no FEN parameter");
    request->send(400, "text/plain", "Missing FEN parameter");
  }
}

void WiFiManagerESP32::handleResign(AsyncWebServerRequest* request) {
  if (request->hasArg("color")) {
    String color = request->arg("color");
    if (color == "w" || color == "b") {
      pendingResignColor = color.charAt(0);
      hasPendingResign = true;
      Serial.printf("Resign received from web: %s resigns\n", color == "w" ? "White" : "Black");
      request->send(200, "text/plain", "OK");
    } else {
      request->send(400, "text/plain", "Invalid color (use 'w' or 'b')");
    }
  } else {
    request->send(400, "text/plain", "Missing 'color' parameter");
  }
}

void WiFiManagerESP32::handleDraw(AsyncWebServerRequest* request) {
  hasPendingDraw = true;
  Serial.println("Draw agreement received from web");
  request->send(200, "text/plain", "OK");
}

void WiFiManagerESP32::handleConnectWiFi(AsyncWebServerRequest* request) {
  // Handle scanAllChannels toggle
  if (request->hasArg("scanAllChannels")) {
    bool newScanAll = request->arg("scanAllChannels") == "1";
    if (newScanAll != scanAllChannels) {
      scanAllChannels = newScanAll;
      saveProfiles(false);
      Serial.printf("WiFi scan all channels: %s\n", scanAllChannels ? "enabled" : "disabled");
    }
    // If only the toggle was sent, respond OK
    if (!request->hasArg("action")) {
      request->send(200, "text/plain", "OK");
      return;
    }
  }

  String action = request->arg("action");

  // action=scan - request a WiFi scan (deferred to main loop)
  if (action == "scan") {
    pendingWiFi.action = SCAN_NETWORKS;
    request->send(200, "text/plain", "OK");
    return;
  }

  // action=delete&index=N - delete a saved profile
  if (action == "delete" && request->hasArg("index")) {
    int idx = request->arg("index").toInt();
    if (idx < 0 || idx >= profileCount) {
      request->send(400, "text/plain", "Invalid index");
      return;
    }
    pendingWiFi.action = DELETE_PROFILE;
    pendingWiFi.profileIndex = idx;
    request->send(200, "text/plain", "OK");
    return;
  }

  // action=connect&index=N - connect to a saved profile (deferred)
  if (action == "connect" && request->hasArg("index")) {
    int idx = request->arg("index").toInt();
    if (idx < 0 || idx >= profileCount) {
      request->send(400, "text/plain", "Invalid index");
      return;
    }
    pendingWiFi.action = CONNECT_SAVED;
    pendingWiFi.profileIndex = idx;
    request->send(200, "text/plain", "OK");
    return;
  }

  // action=connect with ssid (and optional password) - connect to a new/scanned network (deferred)
  if (action == "connect" && request->hasArg("ssid")) {
    pendingWiFi.newSSID = request->arg("ssid");
    pendingWiFi.newPassword = request->hasArg("password") ? request->arg("password") : "";
    pendingWiFi.newChannel = 0;
    memset(pendingWiFi.newBssid, 0, 6);

    // Look up BSSID and channel from scan results for faster initial connect
    for (int i = 0; i < scanResultCount; i++) {
      if (scanResults[i].ssid == pendingWiFi.newSSID) {
        pendingWiFi.newChannel = scanResults[i].channel;
        memcpy(pendingWiFi.newBssid, scanResults[i].bssid, 6);
        break;
      }
    }

    pendingWiFi.action = CONNECT_NEW;
    request->send(200, "text/plain", "OK");
    return;
  }

  request->send(400, "text/plain", "Missing or invalid parameters");
}

void WiFiManagerESP32::handleGameSelection(AsyncWebServerRequest* request) {
  int mode = 0;
  if (request->hasArg("gamemode"))
    mode = request->arg("gamemode").toInt();
  gameMode = String(mode);
  // If bot game mode, also handle bot config
  if (mode == 2) {
    if (request->hasArg("difficulty") && request->hasArg("playerColor")) {
      switch (request->arg("difficulty").toInt()) {
        case 1:
          botConfig.stockfishSettings = StockfishSettings::easy();
          break;
        case 2:
          botConfig.stockfishSettings = StockfishSettings::medium();
          break;
        case 3:
          botConfig.stockfishSettings = StockfishSettings::hard();
          break;
        case 4:
          botConfig.stockfishSettings = StockfishSettings::expert();
          break;
        default:
          botConfig.stockfishSettings = StockfishSettings::medium();
          break;
      }
      botConfig.playerIsWhite = request->arg("playerColor") == "white";
      Serial.printf("Bot configuration received: Depth=%d, Player is %s\n", botConfig.stockfishSettings.depth, botConfig.playerIsWhite ? "White" : "Black");
    } else {
      request->send(400, "text/plain", "Missing bot parameters");
      return;
    }
  }
  // If Lichess mode, verify token exists
  if (mode == 3) {
    if (lichessToken.length() == 0) {
      request->send(400, "text/plain", "No Lichess API token configured");
      return;
    }
    Serial.println("Lichess mode selected via web");
  }
  Serial.println("Game mode selected via web: " + gameMode);
  request->send(200, "text/plain", "OK");
}

String WiFiManagerESP32::getLichessInfoJSON() {
  // Don't expose the actual token, just whether it exists and a masked version
  String maskedToken = "";
  if (lichessToken.length() > 8) {
    maskedToken = lichessToken.substring(0, 4) + "..." + lichessToken.substring(lichessToken.length() - 4);
  } else if (lichessToken.length() > 0) {
    maskedToken = "****";
  }
  JsonDocument doc;
  doc["hasToken"] = (lichessToken.length() > 0);
  doc["maskedToken"] = maskedToken;
  String output;
  serializeJson(doc, output);
  return output;
}

void WiFiManagerESP32::handleSaveLichessToken(AsyncWebServerRequest* request) {
  if (!request->hasArg("token")) {
    request->send(400, "text/plain", "Missing token parameter");
    return;
  }

  String newToken = request->arg("token");
  newToken.trim();

  if (newToken.length() < 10) {
    request->send(400, "text/plain", "Token too short");
    return;
  }

  // Save to NVS
  if (!ChessUtils::ensureNvsInitialized()) {
    request->send(500, "text/plain", "NVS init failed");
    return;
  }

  prefs.begin("lichess", false);
  prefs.putString("token", newToken);
  prefs.end();

  lichessToken = newToken;
  Serial.println("Lichess API token saved to NVS");

  request->send(200, "text/plain", "OK");
}

String WiFiManagerESP32::getBoardSettingsJSON() {
  JsonDocument doc;
  doc["brightness"] = boardDriver->getBrightness();
  doc["dimMultiplier"] = boardDriver->getDimMultiplier();
  String output;
  serializeJson(doc, output);
  return output;
}

void WiFiManagerESP32::handleBoardSettings(AsyncWebServerRequest* request) {
  bool changed = false;

  if (request->hasArg("brightness")) {
    int brightness = request->arg("brightness").toInt();
    if (brightness >= 0 && brightness <= 255) {
      boardDriver->setBrightness((uint8_t)brightness);
      changed = true;
    }
  }

  if (request->hasArg("dimMultiplier")) {
    int dimMult = request->arg("dimMultiplier").toInt();
    if (dimMult >= 0 && dimMult <= 100) {
      boardDriver->setDimMultiplier((uint8_t)dimMult);
      changed = true;
    }
  }

  if (changed) {
    boardDriver->saveLedSettings();
    Serial.println("Board settings updated via web interface");
    request->send(200, "text/plain", "OK");
  } else {
    request->send(400, "text/plain", "No valid settings provided");
  }
}

void WiFiManagerESP32::handleBoardCalibration(AsyncWebServerRequest* request) {
  boardDriver->triggerCalibration();
  request->send(200, "text/plain", "Calibration will start on next reboot");
}

void WiFiManagerESP32::getHardwareConfigJSON(AsyncWebServerRequest* request) {
  const HardwareConfig& hw = boardDriver->getHardwareConfig();
  JsonDocument doc;
  doc["ledPin"] = hw.ledPin;
  doc["srClkPin"] = hw.srClkPin;
  doc["srLatchPin"] = hw.srLatchPin;
  doc["srDataPin"] = hw.srDataPin;
  doc["srInvertOutputs"] = hw.srInvertOutputs;
  JsonArray arr = doc["rowPins"].to<JsonArray>();
  for (int i = 0; i < NUM_ROWS; i++) arr.add(hw.rowPins[i]);
  String output;
  serializeJson(doc, output);
  request->send(200, "application/json", output);
}

void WiFiManagerESP32::handleHardwareConfig(AsyncWebServerRequest* request) {
  HardwareConfig config = boardDriver->getHardwareConfig();
  bool changed = false;

  auto getPin = [&](const char* name, uint8_t& pin) {
    if (request->hasArg(name)) {
      int val = request->arg(name).toInt();
      if (val >= 0 && val <= 39) { // ESP32 GPIO range
        if ((uint8_t)val != pin) {
          pin = (uint8_t)val;
          changed = true;
        }
      }
    }
  };

  getPin("ledPin", config.ledPin);
  getPin("srClkPin", config.srClkPin);
  getPin("srLatchPin", config.srLatchPin);
  getPin("srDataPin", config.srDataPin);

  if (request->hasArg("srInvertOutputs") && (request->arg("srInvertOutputs") == "1") != config.srInvertOutputs) {
    config.srInvertOutputs = !config.srInvertOutputs;
    changed = true;
  }

  for (int i = 0; i < NUM_ROWS; i++) {
    String key = "rowPin" + String(i);
    getPin(key.c_str(), config.rowPins[i]);
  }

  if (changed) {
    boardDriver->saveHardwareConfig(config);
    request->send(200, "text/plain", "Hardware config saved. Rebooting...");
    // Defer reboot to a separate task so the HTTP response is fully sent first
    xTaskCreate(
        [](void*) {
          delay(500);
          ESP.restart();
          vTaskDelete(NULL);
        },
        "hw_reboot", 2048, NULL, 1, NULL);
  } else {
    request->send(200, "text/plain", "No changes detected");
  }
}

void WiFiManagerESP32::loadDeviceConfig() {
  if (ChessUtils::ensureNvsInitialized()) {
    prefs.begin("deviceConfig", false);
    deviceName = prefs.getString("name", "OpenChess");
    prefs.end();
    Serial.println("Device name loaded: " + deviceName);
  }
}

void WiFiManagerESP32::saveDeviceConfig() {
  if (!ChessUtils::ensureNvsInitialized()) return;
  prefs.begin("deviceConfig", false);
  prefs.putString("name", deviceName);
  prefs.end();
  Serial.println("Device name saved: " + deviceName);
}

void WiFiManagerESP32::setDeviceName(const String& name) {
  if (name.length() > 0 && name.length() <= 32) {
    deviceName = name;
    saveDeviceConfig();
  }
}

LichessConfig WiFiManagerESP32::getLichessConfig() {
  LichessConfig config;
  config.apiToken = lichessToken;
  return config;
}

void WiFiManagerESP32::updateBoardState(const String& fen, float evaluation) {
  currentFen = fen;
  boardEvaluation = evaluation;
}

bool WiFiManagerESP32::getPendingBoardEdit(String& fenOut) {
  if (hasPendingEdit) {
    fenOut = pendingFenEdit;
    return true;
  }
  return false;
}

void WiFiManagerESP32::clearPendingEdit() {
  currentFen = pendingFenEdit;
  hasPendingEdit = false;
}

bool WiFiManagerESP32::getPendingResign(char& resignColor) {
  if (hasPendingResign) {
    resignColor = pendingResignColor;
    return true;
  }
  return false;
}

bool WiFiManagerESP32::getPendingDraw() {
  return hasPendingDraw;
}

void WiFiManagerESP32::clearPendingResign() {
  hasPendingResign = false;
  pendingResignColor = '?';
}

void WiFiManagerESP32::clearPendingDraw() {
  hasPendingDraw = false;
}

void WiFiManagerESP32::handlePromotion(AsyncWebServerRequest* request) {
  if (!promotion.pending) {
    request->send(400, "text/plain", "No promotion pending");
    return;
  }
  if (request->hasArg("piece")) {
    String piece = request->arg("piece");
    piece.toLowerCase();
    if (piece == "q" || piece == "r" || piece == "b" || piece == "n") {
      promotion.choice = piece.charAt(0);
      Serial.printf("Promotion choice received from web: %c\n", (char)promotion.choice);
      request->send(200, "text/plain", "OK");
    } else {
      request->send(400, "text/plain", "Invalid piece (use 'q', 'r', 'b', or 'n')");
    }
  } else {
    request->send(400, "text/plain", "Missing 'piece' parameter");
  }
}

void WiFiManagerESP32::startPromotionWait(char color) {
  promotion.color = color;
  promotion.choice = ' ';
  promotion.pending = true;
  Serial.printf("Promotion wait started for %s\n", color == 'w' ? "White" : "Black");
}

void WiFiManagerESP32::clearPromotion() {
  promotion.reset();
}

bool WiFiManagerESP32::isWebClientConnected() const {
  // Consider web client connected if it polled within the last 2 seconds
  return lastBoardPollTime > 0 && (millis() - lastBoardPollTime < 2000);
}

void WiFiManagerESP32::checkPendingWiFi() {
  // Auto-reconnect: if we were connected to a network and lost it, try to reconnect
  if (connectedProfileIndex >= 0 && WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi connection lost, attempting reconnect...");
    connectedProfileIndex = -1;
    if (!connectToSavedProfile()) startAPFallback();
  }

  if (pendingWiFi.action == NONE) return;
  PendingAction action = pendingWiFi.action;
  pendingWiFi.action = NONE;

  switch (action) {
    case SCAN_NETWORKS:
      performScan();
      break;

    case DELETE_PROFILE: {
      int idx = pendingWiFi.profileIndex;
      if (idx >= 0 && idx < profileCount) {
        bool deletingConnected = (idx == connectedProfileIndex);
        Serial.printf("Deleting WiFi profile %d: %s\n", idx, profiles[idx].ssid.c_str());
        // If deleting the connected profile, disconnect
        if (deletingConnected) {
          WiFi.disconnect(false, true);
          connectedProfileIndex = -1;
        } else if (connectedProfileIndex > idx) {
          connectedProfileIndex--;
        }
        // Shift remaining profiles up
        for (int i = idx; i < profileCount - 1; i++)
          profiles[i] = profiles[i + 1];
        profileCount--;
        saveProfiles();
        if (deletingConnected && !connectToSavedProfile()) startAPFallback();
      }
      break;
    }

    case CONNECT_SAVED: {
      int idx = pendingWiFi.profileIndex;
      if (idx >= 0 && idx < profileCount) {
        if (tryConnectProfile(idx)) {
          promoteProfile(idx);
          saveProfiles();
        } else {
          if (!connectToSavedProfile()) startAPFallback();
        }
      }
      break;
    }

    case CONNECT_NEW: {
      String ssid = pendingWiFi.newSSID;
      String password = pendingWiFi.newPassword;
      bool connected = false;
      if (pendingWiFi.newChannel != 0 && !scanAllChannels)
        connected = tryConnect(ssid, password, pendingWiFi.newBssid, pendingWiFi.newChannel);
      if (!connected)
        connected = tryConnect(ssid, password);

      if (connected) {
        // Check if this SSID already exists in profiles (update it)
        int existingIdx = -1;
        for (int i = 0; i < profileCount; i++) {
          if (profiles[i].ssid == ssid) {
            existingIdx = i;
            break;
          }
        }

        WiFiProfile newProfile;
        newProfile.ssid = ssid;
        newProfile.password = password;
        uint8_t* connBssid = WiFi.BSSID();
        if (connBssid) {
          memcpy(newProfile.bssid, connBssid, 6);
          newProfile.channel = WiFi.channel();
          newProfile.hasBssid = true;
        }

        if (existingIdx >= 0) {
          // Update existing and promote to top
          profiles[existingIdx] = newProfile;
          promoteProfile(existingIdx);
        } else {
          // Add new profile at top, shift others down
          if (profileCount >= MAX_WIFI_PROFILES) profileCount = MAX_WIFI_PROFILES - 1;
          for (int i = profileCount; i > 0; i--)
            profiles[i] = profiles[i - 1];
          profiles[0] = newProfile;
          profileCount++;
        }
        connectedProfileIndex = 0;
        saveProfiles();
        Serial.println("New WiFi profile saved and connected!");
      } else {
        Serial.println("Failed to connect to new network, trying saved profiles...");
        if (!connectToSavedProfile()) startAPFallback();
      }
      break;
    }

    default:
      break;
  }
}

bool WiFiManagerESP32::ensureConnected() {
  if (WiFi.status() == WL_CONNECTED) return true;
  Serial.println("WiFi not connected, attempting reconnect...");
  if (connectToSavedProfile()) return true;
  startAPFallback();
  return false;
}

void WiFiManagerESP32::startMDNS() {
  MDNS.end();
  if (MDNS.begin(deviceName.c_str())) {
    MDNS.addService("http", "tcp", HTTP_PORT);
    Serial.println("mDNS started: http://" + deviceName + ".local");
  } else {
    Serial.println("mDNS failed to start");
  }
}

void WiFiManagerESP32::handleGamesRequest(AsyncWebServerRequest* request) {
  if (request->hasArg("id")) {
    String idStr = request->arg("id");

    // GET /games?id=live1 — return live moves file directly
    if (idStr == "live1") {
      if (!MoveHistory::quietExists("/games/live.bin")) {
        request->send(404, "text/plain", "No live game");
        return;
      }
      AsyncWebServerResponse* response = request->beginResponse(LittleFS, "/games/live.bin", "application/octet-stream", true);
      request->send(response);
      return;
    }

    // GET /games?id=live2 — return live FEN table file directly
    if (idStr == "live2") {
      if (!MoveHistory::quietExists("/games/live_fen.bin")) {
        request->send(404, "text/plain", "No live FEN table");
        return;
      }
      AsyncWebServerResponse* response = request->beginResponse(LittleFS, "/games/live_fen.bin", "application/octet-stream", true);
      request->send(response);
      return;
    }

    // GET /games?id=N — return binary of game N
    int id = idStr.toInt();
    if (id <= 0) {
      request->send(400, "text/plain", "Invalid game id");
      return;
    }

    String path = MoveHistory::gamePath(id);
    if (!MoveHistory::quietExists(path.c_str())) {
      request->send(404, "text/plain", "Game not found");
      return;
    }
    // Serve file directly from LittleFS
    AsyncWebServerResponse* response = request->beginResponse(LittleFS, path, "application/octet-stream", true);
    request->send(response);
  } else {
    // GET /games — return JSON list of all saved games
    request->send(200, "application/json", moveHistory->getGameListJSON());
  }
}

void WiFiManagerESP32::handleDeleteGame(AsyncWebServerRequest* request) {
  if (!request->hasArg("id")) {
    request->send(400, "text/plain", "Missing id parameter");
    return;
  }

  int id = request->arg("id").toInt();
  if (id <= 0) {
    request->send(400, "text/plain", "Invalid game id");
    return;
  }

  if (moveHistory->deleteGame(id))
    request->send(200, "text/plain", "OK");
  else
    request->send(404, "text/plain", "Game not found");
}

// ========== OTA Update Handlers ==========

void WiFiManagerESP32::handleOtaStatus(AsyncWebServerRequest* request) {
  if (lastUpdateInfo.version.isEmpty()) {
    request->send(400, "text/plain", "No update info available.");
    return;
  }

  JsonDocument doc;
  doc["version"] = FIRMWARE_VERSION;
  doc["autoUpdate"] = autoOtaEnabled;
  doc["available"] = lastUpdateInfo.available;
  doc["latestVersion"] = lastUpdateInfo.version;
  doc["hasFirmware"] = lastUpdateInfo.firmwareUrl.length() > 0;
  doc["hasWebAssets"] = lastUpdateInfo.webAssetsUrl.length() > 0;
  String output;
  serializeJson(doc, output);
  request->send(200, "application/json", output);
}

void WiFiManagerESP32::handleOtaSettings(AsyncWebServerRequest* request) {
  if (request->hasArg("autoUpdate")) {
    autoOtaEnabled = request->arg("autoUpdate") == "1";
    if (ChessUtils::ensureNvsInitialized()) {
      Preferences p;
      p.begin("ota", false);
      p.putBool("autoUpdate", autoOtaEnabled);
      p.end();
    }
    Serial.printf("OTA: Auto-update %s\n", autoOtaEnabled ? "enabled" : "disabled");
    request->send(200, "text/plain", "OK");
  } else {
    request->send(400, "text/plain", "Missing parameter");
  }
}

// Parameters passed to the OTA apply task via heap allocation (avoids static variable race conditions)
struct OtaApplyParams {
  OtaUpdateInfo info;
  OtaUpdater* updater;
};

void WiFiManagerESP32::handleOtaApply(AsyncWebServerRequest* request) {
  if (!lastUpdateInfo.available) {
    request->send(400, "text/plain", "No update available. Check for updates first.");
    return;
  }

  request->send(200, "text/plain", "Updating... The board will reboot when complete.");

  // Run update in a separate task to not block the web server response.
  // Heap-allocate params so the info survives after this function returns.
  auto* params = new OtaApplyParams{lastUpdateInfo, &otaUpdater};
  lastUpdateInfo.available = false; // Prevent concurrent apply requests

  xTaskCreate(
      [](void* param) {
        auto* p = static_cast<OtaApplyParams*>(param);
        delay(500); // Give time for the HTTP response to be sent
        p->updater->applyUpdate(p->info);
        delete p;
        vTaskDelete(NULL);
      },
      "ota_apply", 8192, params, 1, NULL);
}

void WiFiManagerESP32::onFirmwareUploadBody(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
  // Can't use applyFirmwareFromStream() here — ESPAsyncWebServer delivers the body in async chunks,
  // not as a Stream. We call Update.begin/write/end incrementally across chunk callbacks instead.
  static std::atomic<bool>* stopFlag = nullptr;
  if (index == 0) {
    if (stopFlag == nullptr)
      stopFlag = boardDriver->startWaitingAnimation();
    Serial.printf("OTA: Firmware upload started (%d bytes)\n", total);
    if (!Update.begin(total, U_FLASH)) {
      Serial.printf("OTA: Not enough space: %s\n", Update.errorString());
      return;
    }
  }
  if (Update.isRunning()) {
    if (Update.write(data, len) != len) {
      Serial.printf("OTA: Write failed: %s\n", Update.errorString());
      Update.abort();
    }
  }
  if (index + len == total) {
    if (stopFlag) {
      stopFlag->store(true);
      stopFlag = nullptr;
    }
    if (!Update.isRunning()) {
      // Update.begin() failed or a write error aborted the update
      request->send(500, "text/plain", "Firmware update failed");
    } else if (Update.end(true)) {
      Serial.println("OTA: Firmware upload complete, rebooting...");
      request->send(200, "text/plain", "Firmware updated! Rebooting...");
      boardDriver->flashBoardAnimation(LedColors::Blue, 2);
      delay(500);
      ESP.restart();
    } else {
      Serial.printf("OTA: Finalize failed: %s\n", Update.errorString());
      request->send(500, "text/plain", String("Firmware update failed: ") + Update.errorString());
    }
  }
}

void WiFiManagerESP32::onWebAssetsUploadBody(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
  // Can't use applyWebAssetsFromStream() directly — the TAR parser reads 512-byte headers
  // sequentially from a Stream, but async chunks can split a header across callbacks.
  // So we buffer the TAR to a temp file, then pass it as a Stream to the parser.
  static std::atomic<bool>* stopFlag = nullptr;
  if (index == 0) {
    if (stopFlag == nullptr)
      stopFlag = boardDriver->startWaitingAnimation();
    Serial.printf("OTA: Web assets upload started (%d bytes)\n", total);
    otaTarFile = LittleFS.open("/ota_temp.tar", "w");
    if (!otaTarFile) {
      Serial.println("OTA: Failed to create temp file");
      return;
    }
  }
  if (otaTarFile)
    otaTarFile.write(data, len);
  if (index + len == total) {
    if (otaTarFile) {
      otaTarFile.close();
      File tarFile = LittleFS.open("/ota_temp.tar", "r");
      if (tarFile) {
        size_t tarSize = tarFile.size();
        bool success = otaUpdater.applyWebAssetsFromStream(tarFile, tarSize);
        tarFile.close();
        LittleFS.remove("/ota_temp.tar");
        if (success)
          request->send(200, "text/plain", "Web assets updated successfully!");
        else
          request->send(500, "text/plain", "Web assets update failed");
      } else {
        request->send(500, "text/plain", "Failed to read temp file");
      }
    } else {
      request->send(500, "text/plain", "Upload failed");
    }
    if (stopFlag) {
      stopFlag->store(true);
      stopFlag = nullptr;
    }
  }
}

void WiFiManagerESP32::loadProfiles() {
  profileCount = 0;
  connectedProfileIndex = -1;

  prefs.begin(NVS_WIFI_NAMESPACE, false);
  profileCount = prefs.getUChar("count", 0);
  if (profileCount > MAX_WIFI_PROFILES) profileCount = MAX_WIFI_PROFILES;
  scanAllChannels = prefs.getBool("scanAll", false);

  for (int i = 0; i < profileCount; i++) {
    String idx = String(i);
    profiles[i].ssid = prefs.getString(("ssid" + idx).c_str(), "");
    profiles[i].password = prefs.getString(("pass" + idx).c_str(), "");
    profiles[i].hasBssid = prefs.getBool(("hasBss" + idx).c_str(), false);
    profiles[i].channel = prefs.getUChar(("chan" + idx).c_str(), 0);
    if (profiles[i].hasBssid) {
      size_t len = prefs.getBytes(("bssid" + idx).c_str(), profiles[i].bssid, 6);
      if (len != 6) {
        profiles[i].hasBssid = false;
        memset(profiles[i].bssid, 0, 6);
      }
    }
    Serial.printf("  Profile %d: SSID=%s, hasBSSID=%s, channel=%d\n", i, profiles[i].ssid.c_str(), profiles[i].hasBssid ? "yes" : "no", profiles[i].channel);
  }
  prefs.end();
  Serial.printf("Loaded %d WiFi profile(s) from NVS\n", profileCount);
}

void WiFiManagerESP32::saveProfiles(bool saveAllProfiles) {
  if (!ChessUtils::ensureNvsInitialized()) return;

  prefs.begin(NVS_WIFI_NAMESPACE, false);
  prefs.putBool("scanAll", scanAllChannels);
  if (!saveAllProfiles) {
    prefs.end();
    return;
  }

  prefs.putUChar("count", (uint8_t)profileCount);
  for (int i = 0; i < MAX_WIFI_PROFILES; i++) {
    String idx = String(i);
    if (i < profileCount) {
      prefs.putString(("ssid" + idx).c_str(), profiles[i].ssid);
      prefs.putString(("pass" + idx).c_str(), profiles[i].password);
      prefs.putBool(("hasBss" + idx).c_str(), profiles[i].hasBssid);
      prefs.putUChar(("chan" + idx).c_str(), profiles[i].channel);
      if (profiles[i].hasBssid)
        prefs.putBytes(("bssid" + idx).c_str(), profiles[i].bssid, 6);
    } else {
      if (prefs.isKey(("ssid" + idx).c_str())) prefs.remove(("ssid" + idx).c_str());
      if (prefs.isKey(("pass" + idx).c_str())) prefs.remove(("pass" + idx).c_str());
      if (prefs.isKey(("hasBss" + idx).c_str())) prefs.remove(("hasBss" + idx).c_str());
      if (prefs.isKey(("chan" + idx).c_str())) prefs.remove(("chan" + idx).c_str());
      if (prefs.isKey(("bssid" + idx).c_str())) prefs.remove(("bssid" + idx).c_str());
    }
  }
  prefs.end();
}

void WiFiManagerESP32::promoteProfile(int index) {
  if (index <= 0 || index >= profileCount) return;
  WiFiProfile promoted = profiles[index];
  for (int i = index; i > 0; i--)
    profiles[i] = profiles[i - 1];
  profiles[0] = promoted;
  connectedProfileIndex = 0;
}

bool WiFiManagerESP32::waitForConnection(int maxAttempts) {
  for (int i = 0; i < maxAttempts; i++) {
    boardDriver->showConnectingAnimation();
    wl_status_t st = WiFi.status();
    Serial.printf("  Attempt %d/%d - Status: %d\n", i + 1, maxAttempts, st);
    if (st == WL_CONNECTED) return true;
    if (i >= 3 && (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL)) break;
  }
  return false;
}

bool WiFiManagerESP32::tryConnect(const String& ssid, const String& password, const uint8_t* bssid, uint8_t channel) {
  bool isFast = (bssid != nullptr && channel > 0);
  if (isFast)
    Serial.printf("  Fast connect: SSID=%s, Password=%s, Channel=%d, BSSID=%02X:%02X:%02X:%02X:%02X:%02X\n", ssid.c_str(), password.c_str(), channel, bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
  else
    Serial.printf("  Standard connect: SSID=%s, Password=%s\n", ssid.c_str(), password.c_str());

  stopCaptivePortal();
  WiFi.disconnect(false, true);
  delay(100);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(deviceName.c_str());
  WiFi.mode(WIFI_STA);

  int maxAttempts = isFast ? 5 : 10;
  if (!isFast && scanAllChannels) {
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
    maxAttempts = 25;
  }

  if (isFast) {
    WiFi.begin(ssid.c_str(), password.c_str(), channel, bssid);
  } else {
    WiFi.begin(ssid.c_str(), password.c_str());
  }

  if (waitForConnection(maxAttempts)) {
    startMDNS();
    if (!otaChecked) {
      lastUpdateInfo = otaUpdater.checkForUpdate();
      otaChecked = true;
    }
    return true;
  }
  return false;
}

bool WiFiManagerESP32::tryConnectProfile(int index) {
  if (index < 0 || index >= profileCount) return false;
  WiFiProfile& p = profiles[index];

  // Try fast BSSID+channel connect first (skip if scanAllChannels is enabled)
  if (p.hasBssid && !scanAllChannels) {
    if (tryConnect(p.ssid, p.password, p.bssid, p.channel)) {
      Serial.println("  Fast connect succeeded!");
      connectedProfileIndex = index;
      return true;
    }
    Serial.println("  Fast connect failed, trying standard...");
  }

  // Standard connect (scans for the SSID)
  if (tryConnect(p.ssid, p.password)) {
    // Update cached BSSID and channel
    uint8_t* connBssid = WiFi.BSSID();
    if (connBssid && (!p.hasBssid || memcmp(p.bssid, connBssid, 6) != 0 || p.channel != WiFi.channel())) {
      memcpy(p.bssid, connBssid, 6);
      p.channel = WiFi.channel();
      p.hasBssid = true;
      Serial.printf("  Cached BSSID=%02X:%02X:%02X:%02X:%02X:%02X, Channel=%d\n", p.bssid[0], p.bssid[1], p.bssid[2], p.bssid[3], p.bssid[4], p.bssid[5], p.channel);
    }
    connectedProfileIndex = index;
    return true;
  }

  connectedProfileIndex = -1;
  Serial.printf("  Failed to connect to %s\n", p.ssid.c_str());
  return false;
}

bool WiFiManagerESP32::connectToSavedProfile() {
  for (int i = 0; i < profileCount; i++) {
    if (tryConnectProfile(i)) {
      promoteProfile(i);
      saveProfiles();
      return true;
    }
  }
  return false;
}

void WiFiManagerESP32::startAPFallback() {
  Serial.println("Starting AP fallback...");
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET))
    Serial.println("ERROR: Failed to configure AP IP settings!");
  if (!WiFi.softAP(AP_SSID, AP_PASSWORD))
    Serial.println("ERROR: Failed to create Access Point!");
  startMDNS();
  connectedProfileIndex = -1;
  startCaptivePortal();
  pendingWiFi.action = SCAN_NETWORKS;
}

void WiFiManagerESP32::startCaptivePortal() {
  if (dnsTaskHandle != nullptr) return;
  // Start DNS server for captive portal - resolves all domains to this AP's IP
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", WiFi.softAPIP());
  Serial.println("Captive portal DNS started, IP: " + WiFi.softAPIP().toString());
  xTaskCreate(dnsTask, "DNS_Task", 2048, this, 5, &dnsTaskHandle);
}

void WiFiManagerESP32::stopCaptivePortal() {
  if (dnsTaskHandle == nullptr) return;
  vTaskDelete(dnsTaskHandle);
  dnsTaskHandle = nullptr;
  dnsServer.stop();
  Serial.println("Captive portal DNS stopped");
}

void WiFiManagerESP32::dnsTask(void* param) {
  WiFiManagerESP32* manager = static_cast<WiFiManagerESP32*>(param);
  while (true) {
    manager->dnsServer.processNextRequest();
    vTaskDelay(pdMS_TO_TICKS(25));
  }
  vTaskDelete(nullptr);
}

void WiFiManagerESP32::pendingWiFiBackgroundTask(void* param) {
  WiFiManagerESP32* manager = static_cast<WiFiManagerESP32*>(param);
  while (true) {
    manager->checkPendingWiFi();
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  vTaskDelete(nullptr);
}

void WiFiManagerESP32::performScan() {
  static bool scanInProgress = false;
  if (scanInProgress) return;
  scanInProgress = true;
  Serial.println("Starting WiFi scan...");
  int n = WiFi.scanNetworks(false, false);
  Serial.printf("Scan found %d networks\n", n);
  if (n > 0) {
    if (scanResults) {
      delete[] scanResults;
      scanResults = nullptr;
    }
    scanResultCount = 0;
    scanResults = new ScannedNetwork[n];
    scanResultCount = n;
    for (int i = 0; i < n; i++) {
      scanResults[i].ssid = WiFi.SSID(i);
      scanResults[i].rssi = WiFi.RSSI(i);
      scanResults[i].channel = WiFi.channel(i);
      scanResults[i].encryptionType = (uint8_t)WiFi.encryptionType(i);
      uint8_t* bssid = WiFi.BSSID(i);
      if (bssid)
        memcpy(scanResults[i].bssid, bssid, 6);
      else
        memset(scanResults[i].bssid, 0, 6);
    }
  }
  WiFi.scanDelete();
  scanInProgress = false;
}