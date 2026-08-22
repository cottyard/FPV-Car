#include "esp_camera.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include "camera_pins.h"

// Firmware defaults are only a fallback; the working credentials are read from
// NVS (see loadWifiNetworks). Configure networks with the 'wifi add' command.
#define MAX_WIFI_NETWORKS 8
const char *defaultSsid = "fhjqr";
const char *defaultPassword = "12345678";
const char *hostname = "cam";
const char *preferencesNamespace = "fpv-wifi";
const uint32_t wifiConnectTimeoutMs = 10000;

String WiFiAddr;

typedef struct {
  String ssid;
  String password;
} WifiNetwork;

WifiNetwork wifiNetworks[MAX_WIFI_NETWORKS];
int wifiNetworkCount = 0;

enum SerialConfigState {
  SERIAL_COMMAND,
  SERIAL_WIFI_SSID,
  SERIAL_WIFI_PASSWORD
};

SerialConfigState serialConfigState = SERIAL_COMMAND;
String serialLine;
String pendingWifiSsid;
bool mdnsStarted = false;
bool wifiWasConnected = false;
bool cameraAvailable = false;

void startCameraServer();
void serviceSerialConfiguration();

void loadWifiNetworks() {
  Preferences preferences;
  if (!preferences.begin(preferencesNamespace, true)) {
    wifiNetworkCount = 0;
  } else {
    int count = preferences.getInt("count", 0);
    wifiNetworkCount = 0;
    for (int i = 0; i < count && i < MAX_WIFI_NETWORKS; i++) {
      String ssid = preferences.getString(("s" + String(i)).c_str(), "");
      if (!ssid.isEmpty()) {
        wifiNetworks[wifiNetworkCount].ssid = ssid;
        wifiNetworks[wifiNetworkCount].password = preferences.getString(("p" + String(i)).c_str(), "");
        wifiNetworkCount++;
      }
    }
    preferences.end();
  }
  // No saved networks: fall back to the firmware defaults.
  if (wifiNetworkCount == 0) {
    wifiNetworks[0].ssid = defaultSsid;
    wifiNetworks[0].password = defaultPassword;
    wifiNetworkCount = 1;
  }
}

bool addWifiNetwork(const String &ssid, const String &password) {
  Preferences preferences;
  if (!preferences.begin(preferencesNamespace, false)) {
    return false;
  }
  int count = preferences.getInt("count", 0);
  // Update the password if the SSID is already saved.
  for (int i = 0; i < count; i++) {
    if (preferences.getString(("s" + String(i)).c_str(), "") == ssid) {
      preferences.putString(("p" + String(i)).c_str(), password);
      preferences.end();
      return true;
    }
  }
  if (count >= MAX_WIFI_NETWORKS) {
    preferences.end();
    return false;
  }
  bool ok = preferences.putString(("s" + String(count)).c_str(), ssid) == ssid.length();
  ok = preferences.putString(("p" + String(count)).c_str(), password) == password.length() && ok;
  ok = preferences.putInt("count", count + 1) != 0 && ok;
  preferences.end();
  return ok;
}

bool removeWifiNetwork(int index) {
  Preferences preferences;
  if (!preferences.begin(preferencesNamespace, false)) {
    return false;
  }
  int count = preferences.getInt("count", 0);
  if (index < 0 || index >= count) {
    preferences.end();
    return false;
  }
  for (int i = index; i < count - 1; i++) {
    preferences.putString(("s" + String(i)).c_str(), preferences.getString(("s" + String(i + 1)).c_str(), ""));
    preferences.putString(("p" + String(i)).c_str(), preferences.getString(("p" + String(i + 1)).c_str(), ""));
  }
  preferences.remove(("s" + String(count - 1)).c_str());
  preferences.remove(("p" + String(count - 1)).c_str());
  preferences.putInt("count", count - 1);
  preferences.end();
  return true;
}

void printWifiNetworks() {
  if (wifiNetworkCount == 0) {
    Serial.println("  <none>");
    return;
  }
  for (int i = 0; i < wifiNetworkCount; i++) {
    Serial.printf("  [%d] %s\n", i, wifiNetworks[i].ssid.c_str());
  }
}

void printWiFiStatus() {
  Serial.println("Saved networks:");
  printWifiNetworks();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Connected: yes, IP=%s, RSSI=%d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    Serial.printf("Connected: no, status=%d\n", WiFi.status());
  }
}

void printSerialHelp() {
  Serial.println("Wi-Fi serial commands:");
  Serial.println("  wifi list       - show saved networks and connection status");
  Serial.println("  wifi add        - add/save a new SSID/password");
  Serial.println("  wifi remove <n> - remove saved network #n (see 'wifi list')");
  Serial.println("  wifi status     - show connection status");
  Serial.println("  wifi clear      - clear all saved networks");
}

void restartDevice(const char *message) {
  Serial.println(message);
  Serial.flush();
  delay(200);
  ESP.restart();
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
    if (!addWifiNetwork(pendingWifiSsid, line)) {
      Serial.printf("Failed to save network to NVS (max %d reached?)\n", MAX_WIFI_NETWORKS);
      serialConfigState = SERIAL_COMMAND;
      return;
    }
    restartDevice("Wi-Fi network saved. Restarting...");
    return;
  }

  line.trim();
  if (line.equalsIgnoreCase("wifi add")) {
    serialConfigState = SERIAL_WIFI_SSID;
    Serial.println("Enter SSID:");
  } else if (line.startsWith("wifi remove")) {
    String arg = line.substring(strlen("wifi remove"));
    arg.trim();
    if (arg.isEmpty()) {
      Serial.println("Usage: wifi remove <n>  (see 'wifi list' for indices)");
      return;
    }
    int index = arg.toInt();
    if (removeWifiNetwork(index)) {
      restartDevice("Wi-Fi network removed. Restarting...");
    } else {
      Serial.println("Invalid network index. Use 'wifi list' to see indices.");
    }
  } else if (line.equalsIgnoreCase("wifi list")) {
    printWiFiStatus();
  } else if (line.equalsIgnoreCase("wifi status")) {
    printWiFiStatus();
  } else if (line.equalsIgnoreCase("wifi clear")) {
    Preferences preferences;
    if (!preferences.begin(preferencesNamespace, false)) {
      Serial.println("Failed to open Wi-Fi preferences");
      return;
    }
    bool cleared = preferences.clear();
    preferences.end();
    if (!cleared) {
      Serial.println("Failed to clear Wi-Fi credentials");
      return;
    }
    restartDevice("Wi-Fi networks cleared. Restarting...");
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

void serviceNetworkStatus() {
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !wifiWasConnected) {
    WiFiAddr = WiFi.localIP().toString();
    if (!mdnsStarted) {
      mdnsStarted = MDNS.begin(hostname);
      if (mdnsStarted) {
        MDNS.addService("http", "tcp", 80);
        Serial.printf("mDNS ready: http://%s.local/\n", hostname);
      } else {
        Serial.println("mDNS setup failed");
      }
    }
    Serial.printf("Camera Ready! Open http://%s/\n", WiFiAddr.c_str());
  }
  wifiWasConnected = connected;
}

void handleWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    Serial.printf("Wi-Fi disconnected, reason=%d\n", info.wifi_sta_disconnected.reason);
  } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    Serial.printf("Wi-Fi connected, IP=%s\n", WiFi.localIP().toString().c_str());
  }
}

void setupOTA() {
  ArduinoOTA.setHostname(hostname);
  ArduinoOTA.onStart([]() {
    Serial.println("OTA: update started");
  });
  ArduinoOTA.onEnd([]() { Serial.println("OTA: done"); });
  ArduinoOTA.begin();
  Serial.println("OTA ready (WiFi-updatable)");
}

void setup() {
  Serial.begin(115200);
  // Serial.setDebugOutput(true); // 关闭：esp32-camera 驱动日志高频走 USB CDC，电池供电(无USB主机)时会触发 int_wdt 重启
  Serial.println();
  Serial.println("Enter 'wifi help' for Wi-Fi configuration commands.");

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size = FRAMESIZE_QQVGA;//FRAMESIZE_QVGA;//FRAMESIZE_CIF;
  config.pixel_format = PIXFORMAT_JPEG;  // for streaming
  // config.pixel_format = PIXFORMAT_RGB565; // for face detection/recognition
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;//CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;//CAMERA_FB_IN_DRAM
  config.jpeg_quality = 12;
  config.fb_count = 1;

  // if PSRAM IC present, init with UXGA resolution and higher JPEG quality
  //                      for larger pre-allocated frame buffer.
  if (config.pixel_format == PIXFORMAT_JPEG) {
    if (psramFound()) {
      // 本模块 OV3660 在低 quality 值(高画质/大帧)下 JPEG 编码负载过大会永久卡死：
      // QVGA(320x240) quality<=3 卡死、QQVGA(160x120) quality<=5 开机即卡死(esp_camera_fb_get 阻塞，
      // esp_camera_return_all 无法恢复，需抬 quality 或重启)。稳定值取 10 作为默认。
      // 用户经网页下调到更低值时，推流 handler 会自动恢复(抬到安全档位)，见 app_httpd.cpp。
      config.jpeg_quality = 10;
      config.fb_count = 2;
      config.grab_mode = CAMERA_GRAB_LATEST;
      Serial.printf("Found\n");
    } else {
      Serial.printf("Not Found\n");
      // Limit the frame size when PSRAM is not available
      config.frame_size = FRAMESIZE_QVGA;
      config.fb_location = CAMERA_FB_IN_DRAM;
    }
  } else {
    // Best option for face detection/recognition
    config.frame_size = FRAMESIZE_240X240;//FRAMESIZE_240X240;
  }

  // camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x; continuing without video\n", err);
  } else {
    cameraAvailable = true;
    sensor_t *s = esp_camera_sensor_get();
    // initial sensors are flipped vertically and colors are a bit saturated
    if (s && s->id.PID == OV2640_PID) {
      // s->set_vflip(s, 1);        // flip it back
      s->set_brightness(s, 1);   // up the brightness just a bit
      s->set_saturation(s, -2);  // lower the saturation
    }
  }
  // drop down frame size for higher initial frame rate
  // if (config.pixel_format == PIXFORMAT_JPEG) {
  //   s->set_framesize(s, FRAMESIZE_QVGA);
  // }

  loadWifiNetworks();
  if (wifiNetworkCount > 0) {
    Serial.println("Saved Wi-Fi networks:");
    printWifiNetworks();
  }

  // Connect to the first reachable saved 2.4 GHz Wi-Fi network.
  WiFi.setSleep(false);
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(hostname);
  WiFi.onEvent(handleWiFiEvent);

  for (int i = 0; i < wifiNetworkCount && WiFi.status() != WL_CONNECTED; i++) {
    Serial.printf("Trying network [%d] %s...\n", i, wifiNetworks[i].ssid.c_str());
    WiFi.begin(wifiNetworks[i].ssid.c_str(), wifiNetworks[i].password.c_str());
    uint32_t connectStartedAt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - connectStartedAt < wifiConnectTimeoutMs) {
      serviceSerialConfiguration();
      delay(100);
    }
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("All saved networks failed; automatic reconnect remains enabled.");
    Serial.println("Enter 'wifi add' to add a network.");
  }

  startCameraServer();
  serviceNetworkStatus();
  setupOTA();
}

void loop() {
  ArduinoOTA.handle();
  serviceSerialConfiguration();
  serviceNetworkStatus();
  delay(20);
}
