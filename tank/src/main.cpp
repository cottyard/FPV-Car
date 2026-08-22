#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include "pins.h"
#include "index_html.h"

namespace {

constexpr char HOSTNAME[] = "esp32-tank";
constexpr char DEFAULT_SSID[] = "fhjqr";
constexpr char DEFAULT_PASSWORD[] = "12345678";
constexpr char WIFI_NS[] = "tank-wifi";
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;
constexpr uint32_t DRIVE_TIMEOUT_MS = 350;
constexpr uint32_t MOTION_UPDATE_INTERVAL_MS = 10;
constexpr uint8_t RAMP_STEP = 8;
constexpr uint32_t PWM_FREQUENCY = 18000;
constexpr uint8_t PWM_RESOLUTION = 8;
constexpr uint8_t DEFAULT_SPEED = 5;
constexpr uint8_t MIN_SPEED = 5;
constexpr uint8_t MAX_SPEED = 255;

WebServer server(80);
Preferences preferences;

// Wi-Fi configuration, kept in NVS so it can be changed without reflashing.
String wifiSsid = DEFAULT_SSID;
String wifiPassword = DEFAULT_PASSWORD;
bool apFallbackActive = false;

// Runtime diagnostics: help tell apart a full reset (power sag) from a plain
// WiFi radio drop (motor EMI) without needing a serial connection.
static constexpr char DIAG_NS[] = "tank-diag";
static uint32_t rebootCount = 0;
static uint32_t wifiDropCount = 0;

// Motor pin configuration, kept in NVS so it can be changed without reflashing.
constexpr char PIN_NS[] = "tank-pins";
struct PinConfig {
  uint8_t leftForward = LEFT_FORWARD_PIN;
  uint8_t leftReverse = LEFT_REVERSE_PIN;
  uint8_t rightForward = RIGHT_FORWARD_PIN;
  uint8_t rightReverse = RIGHT_REVERSE_PIN;
};
PinConfig pinCfg;

// Serial configuration state.
enum SerialConfigState {
  SERIAL_COMMAND,
  SERIAL_WIFI_SSID,
  SERIAL_WIFI_PASSWORD
};
SerialConfigState serialConfigState = SERIAL_COMMAND;
String serialLine;
String pendingWifiSsid;

// Motor state.
uint8_t speedSetting = DEFAULT_SPEED;
int8_t leftDirection = 0;
int8_t rightDirection = 0;
uint32_t lastDriveCommandAt = 0;
// Currently applied PWM duty per track (ramped, see updateMotion).
uint8_t leftDuty = 0;
uint8_t rightDuty = 0;
uint32_t lastMotionUpdateAt = 0;

constexpr uint8_t LEFT_FORWARD_CHANNEL = 0;
constexpr uint8_t LEFT_REVERSE_CHANNEL = 1;
constexpr uint8_t RIGHT_FORWARD_CHANNEL = 2;
constexpr uint8_t RIGHT_REVERSE_CHANNEL = 3;

// ---------------------------------------------------------------------------
// Motor control
// ---------------------------------------------------------------------------

// Ramp a track's duty toward its target. Starting from 0 limits the motor
// inrush current so a USB-only power supply does not brown out the ESP32.
// Stopping is immediate for safety.
void updateTrack(uint8_t &duty, int8_t direction, uint8_t forwardChannel, uint8_t reverseChannel) {
  uint8_t target = direction != 0 ? speedSetting : 0;
  if (target > duty) {
    duty = min<uint8_t>(target, (uint8_t)(duty + RAMP_STEP));
  } else if (target == 0) {
    duty = 0;
  } else if (duty > target) {
    duty = max<uint8_t>(target, (uint8_t)(duty - RAMP_STEP));
  }
  ledcWrite(forwardChannel, direction > 0 ? duty : 0);
  ledcWrite(reverseChannel, direction < 0 ? duty : 0);
}

void updateMotion() {
  updateTrack(leftDuty, leftDirection, LEFT_FORWARD_CHANNEL, LEFT_REVERSE_CHANNEL);
  updateTrack(rightDuty, rightDirection, RIGHT_FORWARD_CHANNEL, RIGHT_REVERSE_CHANNEL);
}

void applyMotion(int8_t left, int8_t right) {
  leftDirection = constrain(left, -1, 1);
  rightDirection = constrain(right, -1, 1);
}

void stopMotors() { applyMotion(0, 0); }

const char* motionName() {
  if (!leftDirection && !rightDirection) return "已停止";
  if (leftDirection > 0 && rightDirection > 0) return "前进";
  if (leftDirection < 0 && rightDirection < 0) return "后退";
  return "履带独立驱动";
}

// Motor pins come from NVS (see loadPinConfig); they can be changed without reflashing.
void attachMotorPins() {
  const uint8_t pins[] = {pinCfg.leftForward, pinCfg.leftReverse,
                          pinCfg.rightForward, pinCfg.rightReverse};
  const uint8_t channels[] = {LEFT_FORWARD_CHANNEL, LEFT_REVERSE_CHANNEL, RIGHT_FORWARD_CHANNEL, RIGHT_REVERSE_CHANNEL};
  for (uint8_t index = 0; index < 4; ++index) {
    ledcSetup(channels[index], PWM_FREQUENCY, PWM_RESOLUTION);
    ledcAttachPin(pins[index], channels[index]);
    ledcWrite(channels[index], 0);
  }
}

void setupMotors() {
  attachMotorPins();
  stopMotors();
}

// ---------------------------------------------------------------------------
// Motor pin configuration (NVS, no reflashing required)
// ---------------------------------------------------------------------------

// GPIOs usable as motor outputs. Wired pins that the ROM bootloader drives in
// download mode (UART0 1/3) are excluded. Pins must differ from each other.
bool isValidPin(uint8_t pin) {
  static const bool usable[40] = {
    /*0*/false, /*1*/false, /*2*/false, /*3*/false, /*4*/false, /*5*/false,
    /*6*/false, /*7*/false, /*8*/false, /*9*/false, /*10*/false, /*11*/false,
    /*12*/true, /*13*/true, /*14*/true, /*15*/true, /*16*/true, /*17*/true,
    /*18*/true, /*19*/true, /*20*/false, /*21*/false, /*22*/true, /*23*/true,
    /*24*/false, /*25*/true, /*26*/true, /*27*/true, /*28*/false, /*29*/false,
    /*30*/false, /*31*/false, /*32*/true, /*33*/true, /*34*/false, /*35*/false,
    /*36*/false, /*37*/false, /*38*/false, /*39*/false};
  return pin < 40 && usable[pin];
}

bool arePinsValid(const PinConfig& config) {
  const uint8_t pins[4] = {config.leftForward, config.leftReverse,
                           config.rightForward, config.rightReverse};
  for (uint8_t index = 0; index < 4; ++index) {
    if (!isValidPin(pins[index])) return false;
    for (uint8_t other = index + 1; other < 4; ++other) {
      if (pins[index] == pins[other]) return false;
    }
  }
  return true;
}

// Default pins (14/15/12/13) are all usable, so the config is always valid when empty.
void loadPinConfig() {
  if (!preferences.begin(PIN_NS, true)) return;
  pinCfg.leftForward = preferences.getUChar("lf", LEFT_FORWARD_PIN);
  pinCfg.leftReverse = preferences.getUChar("lr", LEFT_REVERSE_PIN);
  pinCfg.rightForward = preferences.getUChar("rf", RIGHT_FORWARD_PIN);
  pinCfg.rightReverse = preferences.getUChar("rr", RIGHT_REVERSE_PIN);
  preferences.end();
  if (!arePinsValid(pinCfg)) {
    pinCfg = PinConfig();  // fall back to defaults if an invalid value was saved
  }
}

bool savePinConfig(const PinConfig& config) {
  if (!arePinsValid(config)) return false;
  if (!preferences.begin(PIN_NS, false)) return false;
  preferences.putUChar("lf", config.leftForward);
  preferences.putUChar("lr", config.leftReverse);
  preferences.putUChar("rf", config.rightForward);
  preferences.putUChar("rr", config.rightReverse);
  preferences.end();
  return true;
}

// ---------------------------------------------------------------------------
// Wi-Fi configuration (NVS + serial + web, no reflashing required)
// ---------------------------------------------------------------------------

void loadWiFiCredentials() {
  if (!preferences.begin(WIFI_NS, true)) {
    wifiSsid = DEFAULT_SSID;
    wifiPassword = DEFAULT_PASSWORD;
    return;
  }
  wifiSsid = preferences.getString("ssid", DEFAULT_SSID);
  wifiPassword = preferences.getString("password", DEFAULT_PASSWORD);
  preferences.end();
}

bool saveWiFiCredentials(const String &ssid, const String &password) {
  if (!preferences.begin(WIFI_NS, false)) {
    return false;
  }
  bool saved = preferences.putString("ssid", ssid) == (ssize_t)ssid.length();
  saved = preferences.putString("password", password) == (ssize_t)password.length() && saved;
  preferences.end();
  return saved;
}

bool clearWiFiCredentials() {
  if (!preferences.begin(WIFI_NS, false)) {
    return false;
  }
  bool cleared = preferences.clear();
  preferences.end();
  return cleared;
}

// Count boots across resets (persisted in NVS) and listen for STA disconnects.
void loadRebootCount() {
  if (!preferences.begin(DIAG_NS, false)) {
    return;
  }
  rebootCount = preferences.getUInt("boots", 0) + 1;
  preferences.putUInt("boots", rebootCount);
  preferences.end();
}

void onWiFiEvent(WiFiEvent_t event) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    wifiDropCount++;
  }
}

void printWiFiStatus() {
  Serial.printf("Configured SSID: %s\n", wifiSsid.isEmpty() ? "<not configured>" : wifiSsid.c_str());
  if (apFallbackActive) {
    Serial.printf("AP fallback active: %s at http://%s/\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Connected: yes, IP=%s, RSSI=%d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    Serial.printf("Connected: no, status=%d\n", WiFi.status());
  }
}

void restartDevice(const char *message) {
  Serial.println(message);
  Serial.flush();
  delay(200);
  ESP.restart();
}

void printSerialHelp() {
  Serial.println("Wi-Fi serial commands:");
  Serial.println("  wifi config  - enter and save a new SSID/password");
  Serial.println("  wifi status  - show the current SSID and connection status");
  Serial.println("  wifi clear   - restore firmware-default credentials");
}

void handleSerialLine(String line) {
  if (serialConfigState == SERIAL_WIFI_SSID) {
    line.trim();
    if (line.isEmpty() || line.length() > 32) {
      Serial.println("SSID must contain 1-32 characters. Enter SSID:");
      return;
    }
    pendingWifiSsid = line;
    serialConfigState = SERIAL_WIFI_PASSWORD;
    Serial.println("Enter password (empty for an open network):");
    return;
  }

  if (serialConfigState == SERIAL_WIFI_PASSWORD) {
    if (line.length() > 63 || (line.length() > 0 && line.length() < 8)) {
      Serial.println("Password must be empty or contain 8-63 characters. Enter password:");
      return;
    }
    if (!saveWiFiCredentials(pendingWifiSsid, line)) {
      Serial.println("Failed to save Wi-Fi credentials to NVS");
      serialConfigState = SERIAL_COMMAND;
      return;
    }
    restartDevice("Wi-Fi credentials saved. Restarting...");
    return;
  }

  line.trim();
  if (line.equalsIgnoreCase("wifi config")) {
    serialConfigState = SERIAL_WIFI_SSID;
    Serial.println("Enter SSID:");
  } else if (line.equalsIgnoreCase("wifi status")) {
    printWiFiStatus();
  } else if (line.equalsIgnoreCase("wifi clear")) {
    if (!clearWiFiCredentials()) {
      Serial.println("Failed to clear Wi-Fi credentials");
      return;
    }
    restartDevice("Wi-Fi credentials cleared. Restarting...");
  } else if (line.equalsIgnoreCase("help") || line.equalsIgnoreCase("wifi help")) {
    printSerialHelp();
  } else if (!line.isEmpty()) {
    Serial.println("Unknown command. Enter 'wifi help' for available commands.");
  }
}

void serviceSerialConfiguration() {
  while (Serial.available() > 0) {
    char value = (char)Serial.read();
    if (value == '\r') {
      continue;
    }
    if (value == '\n') {
      handleSerialLine(serialLine);
      serialLine = "";
    } else if (serialLine.length() < 128) {
      serialLine += value;
    }
  }
}

// ---------------------------------------------------------------------------
// Web server: light control API plus the embedded web page (served at "/")
// ---------------------------------------------------------------------------

void handleRoot() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", INDEX_HTML);
}

void sendPins() {
  char body[128];
  snprintf(body, sizeof(body),
           "{\"left_forward\":%u,\"left_reverse\":%u,\"right_forward\":%u,\"right_reverse\":%u}",
           pinCfg.leftForward, pinCfg.leftReverse, pinCfg.rightForward, pinCfg.rightReverse);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", body);
}

void handlePins() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  const String lfArg = server.arg("lf");
  const String lrArg = server.arg("lr");
  const String rfArg = server.arg("rf");
  const String rrArg = server.arg("rr");
  if (lfArg.isEmpty() && lrArg.isEmpty() && rfArg.isEmpty() && rrArg.isEmpty()) {
    sendPins();
    return;
  }
  if (lfArg.isEmpty() || lrArg.isEmpty() || rfArg.isEmpty() || rrArg.isEmpty()) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing pins\"}");
    return;
  }
  PinConfig next;
  next.leftForward = (uint8_t)lfArg.toInt();
  next.leftReverse = (uint8_t)lrArg.toInt();
  next.rightForward = (uint8_t)rfArg.toInt();
  next.rightReverse = (uint8_t)rrArg.toInt();
  if (!savePinConfig(next)) {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"invalid pins: must be 4 distinct usable GPIOs\"}");
    return;
  }
  server.send(200, "application/json", "{\"ok\":true}");
  Serial.printf("Motor pins updated via web (lf=%u lr=%u rf=%u rr=%u); restarting\n",
                next.leftForward, next.leftReverse, next.rightForward, next.rightReverse);
  delay(300);
  ESP.restart();
}

void sendStatus() {
  String ip = apFallbackActive ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  char body[384];
  snprintf(body, sizeof(body),
           "{\"speed\":%u,\"motion\":\"%s\",\"left\":%d,\"right\":%d,"
           "\"wifi_mode\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,"
           "\"uptime_s\":%u,\"reboots\":%u,\"wifi_drops\":%u,"
           "\"pins\":{\"left_forward\":%u,\"left_reverse\":%u,"
           "\"right_forward\":%u,\"right_reverse\":%u}}",
           speedSetting, motionName(), leftDirection, rightDirection,
           apFallbackActive ? "ap" : "sta", wifiSsid.c_str(), ip.c_str(), WiFi.RSSI(),
           (uint32_t)(millis() / 1000), rebootCount, wifiDropCount,
           pinCfg.leftForward, pinCfg.leftReverse, pinCfg.rightForward, pinCfg.rightReverse);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", body);
}

void handleControl() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  String command = server.arg("cmd");
  if (command == "drive") {
    String left = server.arg("left");
    String right = server.arg("right");
    if (server.hasArg("speed")) {
      speedSetting = constrain(server.arg("speed").toInt(), MIN_SPEED, MAX_SPEED);
    }
    int8_t leftValue = left == "f" ? 1 : left == "r" ? -1 : 0;
    int8_t rightValue = right == "f" ? 1 : right == "r" ? -1 : 0;
    applyMotion(leftValue, rightValue);
    lastDriveCommandAt = millis();
  } else if (command == "stop") {
    stopMotors();
  } else {
    server.send(400, "application/json", "{\"error\":\"unknown command\"}");
    return;
  }
  sendStatus();
}

void handleWifi() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  String ssid = server.arg("ssid");
  String password = server.arg("password");
  if (ssid.isEmpty() || ssid.length() > 32) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid ssid\"}");
    return;
  }
  if (password.length() > 63) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid password\"}");
    return;
  }
  if (!saveWiFiCredentials(ssid, password)) {
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"nvs write failed\"}");
    return;
  }
  server.send(200, "application/json", "{\"ok\":true}");
  Serial.printf("Wi-Fi credentials updated via web (%s); restarting\n", ssid.c_str());
  delay(300);
  ESP.restart();
}

}  // namespace

// ---------------------------------------------------------------------------
// Setup / loop (kept outside the anonymous namespace for Arduino linkage)
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();
  Serial.println("ESP32 Tank - enter 'wifi help' for Wi-Fi configuration commands.");

  loadPinConfig();
  loadRebootCount();

  setupMotors();

  loadWiFiCredentials();

  // Connect to the configured 2.4 GHz network.
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(HOSTNAME);
  WiFi.onEvent(onWiFiEvent);
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  Serial.printf("Connecting to Wi-Fi %s", wifiSsid.c_str());
  uint32_t connectStartedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - connectStartedAt < WIFI_CONNECT_TIMEOUT_MS) {
    serviceSerialConfiguration();
    delay(100);
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Connected, IP=%s, RSSI=%d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    // Fallback: start an AP config portal so credentials can be changed without a serial terminal.
    apFallbackActive = true;
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    Serial.printf("STA connect failed; started AP '%s' (password '%s') at http://%s/\n",
                  AP_SSID, AP_PASSWORD, WiFi.softAPIP().toString().c_str());
    Serial.println("Configure Wi-Fi via the web page or serial 'wifi config'.");
  }

  MDNS.begin(HOSTNAME);
  MDNS.addService("http", "tcp", 80);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/control", HTTP_GET, handleControl);
  server.on("/api/status", HTTP_GET, sendStatus);
  server.on("/api/wifi", HTTP_GET, handleWifi);
  server.on("/api/pins", HTTP_GET, handlePins);
  server.begin();

  setupOTA();

  Serial.printf("Web API ready at http://%s.local/ (mDNS)\n", HOSTNAME);
  printWiFiStatus();
}

void setupOTA() {
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.onStart([]() {
    Serial.println("OTA: update started");
    stopMotors();
  });
  ArduinoOTA.onEnd([]() { Serial.println("OTA: done"); });
  ArduinoOTA.begin();
  Serial.println("OTA ready (WiFi-updatable)");
}

void loop() {
  ArduinoOTA.handle();
  serviceSerialConfiguration();
  server.handleClient();
  uint32_t now = millis();
  if (now - lastMotionUpdateAt >= MOTION_UPDATE_INTERVAL_MS) {
    lastMotionUpdateAt = now;
    updateMotion();
  }
  if ((leftDirection || rightDirection) && now - lastDriveCommandAt > DRIVE_TIMEOUT_MS) {
    stopMotors();
  }
}
