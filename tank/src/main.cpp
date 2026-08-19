#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include "pins.h"

namespace {

constexpr char HOSTNAME[] = "esp32-tank";
constexpr char DEFAULT_SSID[] = "Redmi_0DAC";
constexpr char DEFAULT_PASSWORD[] = "16716811";
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

// Motor pins are fixed at compile time (see pins.h) and match the physical wiring.
void attachMotorPins() {
  const uint8_t pins[] = {LEFT_FORWARD_PIN, LEFT_REVERSE_PIN, RIGHT_FORWARD_PIN, RIGHT_REVERSE_PIN};
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
// Web server: light control API only, no embedded web page (page lives on PC)
// ---------------------------------------------------------------------------

void sendStatus() {
  String ip = apFallbackActive ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  char body[256];
  snprintf(body, sizeof(body),
           "{\"speed\":%u,\"motion\":\"%s\",\"left\":%d,\"right\":%d,"
           "\"wifi_mode\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d}",
           speedSetting, motionName(), leftDirection, rightDirection,
           apFallbackActive ? "ap" : "sta", wifiSsid.c_str(), ip.c_str(), WiFi.RSSI());
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

  setupMotors();

  loadWiFiCredentials();

  // Connect to the configured 2.4 GHz network.
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(HOSTNAME);
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

  server.on("/api/control", HTTP_GET, handleControl);
  server.on("/api/status", HTTP_GET, sendStatus);
  server.on("/api/wifi", HTTP_GET, handleWifi);
  server.begin();

  Serial.printf("Web API ready at http://%s.local/ (mDNS)\n", HOSTNAME);
  printWiFiStatus();
}

void loop() {
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
