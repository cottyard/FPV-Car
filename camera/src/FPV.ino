#include "esp_camera.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include "camera_pins.h"

// Firmware defaults are only a fallback; the working credentials are read from
// NVS (see loadWiFiCredentials). Configure them with the 'wifi config' command.
const char *defaultSsid = "fhjqr";
const char *defaultPassword = "12345678";
const char *hostname = "fpv-car";
const char *preferencesNamespace = "fpv-wifi";
const uint32_t wifiConnectTimeoutMs = 20000;

String WiFiAddr;
String wifiSsid;
String wifiPassword;

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

void loadWiFiCredentials() {
  Preferences preferences;
  if (!preferences.begin(preferencesNamespace, true)) {
    wifiSsid = defaultSsid;
    wifiPassword = defaultPassword;
    return;
  }

  wifiSsid = preferences.getString("ssid", defaultSsid);
  wifiPassword = preferences.getString("password", defaultPassword);
  preferences.end();
}

bool saveWiFiCredentials(const String &ssid, const String &password) {
  Preferences preferences;
  if (!preferences.begin(preferencesNamespace, false)) {
    return false;
  }
  bool saved = preferences.putString("ssid", ssid) == ssid.length();
  saved = preferences.putString("password", password) == password.length() && saved;
  preferences.end();
  return saved;
}

void printWiFiStatus() {
  Serial.printf("Configured SSID: %s\n", wifiSsid.isEmpty() ? "<not configured>" : wifiSsid.c_str());
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Connected: yes, IP=%s, RSSI=%d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    Serial.printf("Connected: no, status=%d\n", WiFi.status());
  }
}

void printSerialHelp() {
  Serial.println("Wi-Fi serial commands:");
  Serial.println("  wifi config  - enter and save a new SSID/password");
  Serial.println("  wifi status  - show the current SSID and connection status");
  Serial.println("  wifi clear   - restore firmware-default credentials");
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
  Serial.setDebugOutput(true);
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
  config.frame_size = FRAMESIZE_CIF;//FRAMESIZE_QVGA;//FRAMESIZE_QQVGA;
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

  loadWiFiCredentials();

  // Connect to the configured local 2.4 GHz Wi-Fi network.
  WiFi.setSleep(false);
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(hostname);
  WiFi.onEvent(handleWiFiEvent);

  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  Serial.printf("Connecting to Wi-Fi %s", wifiSsid.c_str());
  uint32_t connectStartedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - connectStartedAt < wifiConnectTimeoutMs) {
    serviceSerialConfiguration();
    delay(100);
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi connection timed out; automatic reconnect remains enabled.");
    Serial.println("Enter 'wifi config' to set different credentials.");
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
