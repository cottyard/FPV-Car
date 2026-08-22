#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "img_converters.h"
#include "esp_system.h"
#include "Arduino.h"
#include <WiFi.h>
#include <stdarg.h>
#include "web_index.h"
#include "driver/temp_sensor.h"

// 内存日志环形缓冲：记录推流相关日志，可通过 /api/log 经 WiFi 读取。
#define LOG_RING_SIZE 200
#define LOG_LINE_MAX 96
static char logRing[LOG_RING_SIZE][LOG_LINE_MAX];
static int logWriteIdx = 0;
static int logCount = 0;

// 写入一行日志：同时输出到串口和内存缓冲。
static void logLine(const char *fmt, ...) {
    char line[LOG_LINE_MAX];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    // 仅写入内存缓冲，经 /api/log 读取。
    // 不向 Serial 打印：电池供电时 USB CDC 无主机，高频串口输出会触发 int_wdt 重启。
    strncpy(logRing[logWriteIdx], line, LOG_LINE_MAX - 1);
    logRing[logWriteIdx][LOG_LINE_MAX - 1] = '\0';
    logWriteIdx = (logWriteIdx + 1) % LOG_RING_SIZE;
    if (logCount < LOG_RING_SIZE) {
        logCount++;
    }
}

// 将上次复位原因转为可读字符串，用于区分供电(brownout)/固件(panic/wdt)问题。
static const char *resetReasonStr(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:    return "power_on";
        case ESP_RST_EXT:        return "external_pin";
        case ESP_RST_SW:         return "software";
        case ESP_RST_PANIC:      return "panic";
        case ESP_RST_INT_WDT:    return "int_wdt";
        case ESP_RST_TASK_WDT:   return "task_wdt";
        case ESP_RST_WDT:        return "other_wdt";
        case ESP_RST_DEEPSLEEP:  return "deep_sleep";
        case ESP_RST_BROWNOUT:   return "brownout";
        case ESP_RST_SDIO:       return "sdio";
        default:                 return "unknown";
    }
}

extern bool cameraAvailable;

typedef struct {
  String ssid;
  String password;
} WifiNetwork;

#define MAX_WIFI_NETWORKS 8
extern WifiNetwork wifiNetworks[];
extern int wifiNetworkCount;
extern bool addWifiNetwork(const String &ssid, const String &password);
extern bool removeWifiNetwork(int index);
extern const char *hostname;

// ESP32-S3 内置温度传感器：懒初始化，失败时返回 NAN。
static float readChipTemp() {
    static bool tsensInit = false;
    if (!tsensInit) {
        temp_sensor_config_t cfg = TSENS_CONFIG_DEFAULT();
        if (temp_sensor_set_config(cfg) == ESP_OK && temp_sensor_start() == ESP_OK) {
            tsensInit = true;
        } else {
            return NAN;
        }
    }
    float celsius = NAN;
    if (temp_sensor_read_celsius(&celsius) != ESP_OK) {
        return NAN;
    }
    return celsius;
}

typedef struct {
    size_t size;
    size_t index;
    size_t count;
    int sum;
    int *values;
} ra_filter_t;

typedef struct {
    httpd_req_t *req;
    size_t len;
} jpg_chunking_t;

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *_STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *_STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *_STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// 推流帧率上限：只限制发送节奏，采集仍按相机原始帧率进行。
// 运行时可通过 /api/camera?var=fps&val=N 调整（1-30）。
uint32_t gStreamFps = 10;

// 自动恢复状态：本模块 OV3660 在低 quality 值(高画质/大帧)下编码负载过大可能永久卡死
// (esp_camera_fb_get 阻塞，仅靠 esp_camera_return_all 无法恢复)。实测 quality>=10 稳定；
// 因此推流长时间无帧时自动把 quality 抬到安全档位解除卡死，触发后不再自动降回，避免反复卡死。
static int gRecoveryQuality = 0; // 0=未触发自动恢复；>0=当前恢复档位

static ra_filter_t ra_filter;
static httpd_handle_t stream_httpd = NULL;
static httpd_handle_t api_httpd = NULL;
// 单客户端锁：esp32-camera 驱动不支持并发抓帧，多个 stream 连接会互相干扰导致驱动卡死。
static SemaphoreHandle_t streamLock = NULL;

static esp_err_t index_handler(httpd_req_t *req);
static void set_cors(httpd_req_t *req);

static esp_err_t index_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    set_cors(req);
    return httpd_resp_send(req, (const char *)index_htm, strlen(index_htm));
}

static void set_cors(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

static ra_filter_t *ra_filter_init(ra_filter_t *filter, size_t sample_size) {
    memset(filter, 0, sizeof(ra_filter_t));
    filter->values = (int *)malloc(sample_size * sizeof(int));
    if (!filter->values) {
        return NULL;
    }
    memset(filter->values, 0, sample_size * sizeof(int));
    filter->size = sample_size;
    return filter;
}

static int ra_filter_run(ra_filter_t *filter, int value) {
    if (!filter->values) {
        return value;
    }
    filter->sum -= filter->values[filter->index];
    filter->values[filter->index] = value;
    filter->sum += value;
    filter->index = (filter->index + 1) % filter->size;
    if (filter->count < filter->size) {
        filter->count++;
    }
    return filter->sum / filter->count;
}

static size_t jpg_encode_stream(void *arg, size_t index, const void *data, size_t len) {
    jpg_chunking_t *chunk = (jpg_chunking_t *)arg;
    if (!index) {
        chunk->len = 0;
    }
    if (httpd_resp_send_chunk(chunk->req, (const char *)data, len) != ESP_OK) {
        return 0;
    }
    chunk->len += len;
    return len;
}

static esp_err_t capture_handler(httpd_req_t *req) {
    if (!cameraAvailable) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camera unavailable");
    }
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        logLine("Camera capture failed\n");
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    set_cors(req);

    esp_err_t result;
    if (fb->format == PIXFORMAT_JPEG) {
        result = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    } else {
        jpg_chunking_t chunk = {req, 0};
        result = frame2jpg_cb(fb, 80, jpg_encode_stream, &chunk) ? ESP_OK : ESP_FAIL;
        httpd_resp_send_chunk(req, NULL, 0);
    }
    esp_camera_fb_return(fb);
    return result;
}

static esp_err_t stream_handler(httpd_req_t *req) {
    if (!cameraAvailable) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camera unavailable");
    }
    esp_err_t result = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if (result != ESP_OK) {
        return result;
    }
    set_cors(req);

    int64_t last_frame = esp_timer_get_time();
    // 连续抓帧失败计数：用于跳过单帧 + 重置驱动，避免偶发变慢导致推流中断卡死。
    int consecutive_failures = 0;
    const int FAIL_RESET_THRESHOLD = 5;   // 连续失败达到该次数时重置驱动
    const int64_t STALL_RESET_US = 3000000; // 距上次成功帧超过 3s 仍未取到帧时重置驱动
    const int64_t RECOVERY_STALL_US = 20000000; // 距上次成功帧超过 20s 仍取不到帧 → 自动恢复(抬 quality)

    while (true) {
        // 按目标帧率控制发送节奏，未到间隔则等待。
        uint32_t frameIntervalMs = 1000 / gStreamFps;
        int64_t elapsed_ms = (esp_timer_get_time() - last_frame) / 1000;
        if (elapsed_ms < frameIntervalMs) {
            delay(frameIntervalMs - (uint32_t)elapsed_ms);
        }

        // 自动恢复：长时间无帧说明编码器已永久卡死，仅重置驱动无法恢复。
        // 实测将 JPEG quality 抬到安全档位(减轻编码负载)可解除卡死；档位 10 -> 15 -> 20。
        if ((esp_timer_get_time() - last_frame) > RECOVERY_STALL_US) {
            int target = gRecoveryQuality == 0 ? 10 : min(gRecoveryQuality + 5, 20);
            sensor_t *s = esp_camera_sensor_get();
            if (s != NULL) {
                logLine("Auto-recovery: stalled %ums, raising quality -> %d\n",
                        (unsigned int)((esp_timer_get_time() - last_frame) / 1000), target);
                if (s->set_quality(s, target) == 0) {
                    gRecoveryQuality = target;
                }
            }
            consecutive_failures = 0;
            last_frame = esp_timer_get_time();
            delay(50);
            continue;
        }

        // 超时保护：长时间取不到帧说明驱动卡住，重置摄像头驱动恢复数据链路。
        if (consecutive_failures > 0 && (esp_timer_get_time() - last_frame) > STALL_RESET_US) {
            logLine("Stream stalled %ums, resetting camera driver\n",
                    (unsigned int)((esp_timer_get_time() - last_frame) / 1000));
            esp_camera_return_all();
            consecutive_failures = 0;
        }

        // 每帧抓取+发送期间持锁：串行化 esp_camera_fb_get/return（驱动不支持并发抓帧），
        // 同时避免“整连接持锁”——若摄像头卡住，整连接持锁会让该连接永远占用 socket，
        // 后续新连接(含浏览器重连)永久得不到响应。
        if (streamLock == NULL || xSemaphoreTake(streamLock, pdMS_TO_TICKS(200)) != pdTRUE) {
            delay(20); // 另一客户端正在抓帧/发送，稍后重试
            continue;
        }

        camera_fb_t *fb = esp_camera_fb_get();
        uint8_t *jpg_buf = NULL;
        size_t jpg_len = 0;
        bool converted = false;

        if (!fb) {
            xSemaphoreGive(streamLock);
            // 抓帧偶发失败/超时：跳过本帧继续推流，不中断连接。
            consecutive_failures++;
            logLine("Camera capture failed (skipped, %d consecutive)\n", consecutive_failures);
            if (consecutive_failures >= FAIL_RESET_THRESHOLD) {
                logLine("Repeated capture failures, resetting camera driver\n");
                esp_camera_return_all();
                consecutive_failures = 0;
            }
            delay(20);
            continue;
        }

        if (fb->format == PIXFORMAT_JPEG) {
            jpg_buf = fb->buf;
            jpg_len = fb->len;
        } else {
            converted = frame2jpg(fb, 80, &jpg_buf, &jpg_len);
            if (!converted) {
                logLine("JPEG conversion failed (skipped)\n");
                esp_camera_fb_return(fb);
                xSemaphoreGive(streamLock);
                consecutive_failures++;
                delay(20);
                continue;
            }
        }

        consecutive_failures = 0;

        char part_buf[64];
        size_t header_len = snprintf(part_buf, sizeof(part_buf), _STREAM_PART, jpg_len);
        result = httpd_resp_send_chunk(req, part_buf, header_len);
        if (result == ESP_OK) {
            result = httpd_resp_send_chunk(req, (const char *)jpg_buf, jpg_len);
        }
        if (result == ESP_OK) {
            result = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }

        if (fb) {
            esp_camera_fb_return(fb);
        }
        if (converted && jpg_buf) {
            free(jpg_buf);
        }
        xSemaphoreGive(streamLock);
        if (result != ESP_OK) {
            break;
        }

        int64_t now = esp_timer_get_time();
        uint32_t frame_ms = (uint32_t)((now - last_frame) / 1000);
        last_frame = now;
        uint32_t average_ms = ra_filter_run(&ra_filter, frame_ms);
        logLine("MJPG: %uB %ums, avg %ums\n", (unsigned int)jpg_len, frame_ms, average_ms);
    }
    return result;
}

static esp_err_t camera_control_handler(httpd_req_t *req) {
    char query[128] = {0};
    char variable[32] = {0};
    char value[32] = {0};
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len == 0 || query_len >= sizeof(query) ||
        httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "var", variable, sizeof(variable)) != ESP_OK ||
        httpd_query_key_value(query, "val", value, sizeof(value)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected var and val");
    }

    int val = atoi(value);
    if (!strcmp(variable, "fps")) {
        gStreamFps = constrain(val, 1, 30);
        Serial.printf("Stream fps set to %u\n", gStreamFps);
        set_cors(req);
        return httpd_resp_send(req, NULL, 0);
    }

    if (!cameraAvailable) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camera unavailable");
    }
    sensor_t *sensor = esp_camera_sensor_get();
    int result = -1;
    if (!strcmp(variable, "framesize") && sensor->pixformat == PIXFORMAT_JPEG) result = sensor->set_framesize(sensor, (framesize_t)val);
    else if (!strcmp(variable, "quality")) result = sensor->set_quality(sensor, val);
    else if (!strcmp(variable, "contrast")) result = sensor->set_contrast(sensor, val);
    else if (!strcmp(variable, "brightness")) result = sensor->set_brightness(sensor, val);
    else if (!strcmp(variable, "saturation")) result = sensor->set_saturation(sensor, val);
    else if (!strcmp(variable, "hmirror")) result = sensor->set_hmirror(sensor, val);
    else if (!strcmp(variable, "vflip")) result = sensor->set_vflip(sensor, val);

    if (result != 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unsupported camera setting");
    }
    set_cors(req);
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t status_handler(httpd_req_t *req) {
    sensor_t *sensor = cameraAvailable ? esp_camera_sensor_get() : NULL;
    int framesize = sensor ? sensor->status.framesize : -1;
    int quality = sensor ? sensor->status.quality : -1;
    int brightness = sensor ? sensor->status.brightness : 0;
    int contrast = sensor ? sensor->status.contrast : 0;
    int saturation = sensor ? sensor->status.saturation : 0;

    char response[640];
    String ip = WiFi.localIP().toString();
    char tempStr[16];
    {
        float t = readChipTemp();
        if (isnan(t)) {
            strcpy(tempStr, "null");
        } else {
            snprintf(tempStr, sizeof(tempStr), "%.1f", t);
        }
    }
    snprintf(response, sizeof(response),
             "{\"ip\":\"%s\",\"hostname\":\"%s.local\",\"uptime_ms\":%lu,"
             "\"free_heap\":%lu,\"psram_size\":%lu,\"free_psram\":%lu,\"wifi_rssi\":%d,"
             "\"cpu_mhz\":%u,\"temp_c\":%s,\"reset_reason\":\"%s\","
             "\"camera_available\":%s,\"framesize\":%d,\"quality\":%d,"
             "\"brightness\":%d,\"contrast\":%d,\"saturation\":%d,\"fps\":%u}",
             ip.c_str(), hostname, (unsigned long)millis(), (unsigned long)ESP.getFreeHeap(),
             (unsigned long)ESP.getPsramSize(), (unsigned long)ESP.getFreePsram(), WiFi.RSSI(),
             (unsigned int)ESP.getCpuFreqMHz(), tempStr, resetReasonStr(esp_reset_reason()),
             sensor ? "true" : "false", framesize, quality,
             brightness, contrast, saturation, gStreamFps);

    httpd_resp_set_type(req, "application/json");
    set_cors(req);
    return httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t log_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/plain");
    set_cors(req);
    String body;
    body.reserve((size_t)logCount * LOG_LINE_MAX);
    int start = (logWriteIdx - logCount + LOG_RING_SIZE) % LOG_RING_SIZE;
    for (int i = 0; i < logCount; i++) {
        int idx = (start + i) % LOG_RING_SIZE;
        body += logRing[idx];
    }
    return httpd_resp_send(req, body.c_str(), body.length());
}

static esp_err_t wifi_list_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    set_cors(req);
    String body = "{\"connected\":";
    body += (WiFi.status() == WL_CONNECTED) ? "true" : "false";
    body += ",\"rssi\":" + String(WiFi.RSSI());
    body += ",\"current\":\"";
    body += (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : String("");
    body += "\",\"max\":" + String(MAX_WIFI_NETWORKS);
    body += ",\"networks\":[";
    for (int i = 0; i < wifiNetworkCount; i++) {
        if (i > 0) {
            body += ",";
        }
        body += "{\"ssid\":\"" + wifiNetworks[i].ssid + "\"}";
    }
    body += "]}";
    return httpd_resp_send(req, body.c_str(), HTTPD_RESP_USE_STRLEN);
}

static esp_err_t wifi_add_handler(httpd_req_t *req) {
    char query[256] = {0};
    char ssid[33] = {0};
    char password[64] = {0};
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len == 0 || query_len >= sizeof(query) ||
        httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "ssid", ssid, sizeof(ssid)) != ESP_OK ||
        ssid[0] == '\0') {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected ssid");
    }
    httpd_query_key_value(query, "password", password, sizeof(password));
    if (!addWifiNetwork(ssid, password)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to save (max reached?)");
    }
    httpd_resp_set_type(req, "application/json");
    set_cors(req);
    httpd_resp_sendstr(req, "{\"saved\":true}");
    Serial.printf("WiFi network added via web: %s\n", ssid);
    delay(300);
    ESP.restart();
    return ESP_OK;
}

static esp_err_t wifi_remove_handler(httpd_req_t *req) {
    char query[128] = {0};
    char index_str[8] = {0};
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len == 0 || query_len >= sizeof(query) ||
        httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "index", index_str, sizeof(index_str)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected index");
    }
    int index = atoi(index_str);
    if (!removeWifiNetwork(index)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid index");
    }
    httpd_resp_set_type(req, "application/json");
    set_cors(req);
    httpd_resp_sendstr(req, "{\"removed\":true}");
    Serial.printf("WiFi network removed via web: index %d\n", index);
    delay(300);
    ESP.restart();
    return ESP_OK;
}

static esp_err_t wifi_options_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "600");
    return httpd_resp_send(req, NULL, 0);
}

static void register_uri(httpd_handle_t server, httpd_uri_t *uri) {
    esp_err_t result = httpd_register_uri_handler(server, uri);
    if (result != ESP_OK) {
        Serial.printf("Failed to register %s: 0x%x\n", uri->uri, result);
    }
}

void startCameraServer() {
    ra_filter_init(&ra_filter, 20);
    streamLock = xSemaphoreCreateMutex();

    httpd_config_t api_config = HTTPD_DEFAULT_CONFIG();
    api_config.max_uri_handlers = 10;
    api_config.stack_size = 16384; // 默认 4096 太小，推流/日志等调用链会栈溢出(Stack canary watchpoint)
    // 网页轮询(status 1s + wifi 3s)会占满默认的 7 个 socket；增大池并启用 LRU 淘汰，
    // 避免连接堆积后新连接(含推流)完全得不到响应(code=000 / CONNECTING 卡死)。
    api_config.max_open_sockets = 10;
    api_config.lru_purge_enable = true;

    httpd_uri_t index_uri = {.uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = NULL};
    httpd_uri_t status_uri = {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = NULL};
    httpd_uri_t log_uri = {.uri = "/api/log", .method = HTTP_GET, .handler = log_handler, .user_ctx = NULL};
    httpd_uri_t camera_uri = {.uri = "/api/camera", .method = HTTP_GET, .handler = camera_control_handler, .user_ctx = NULL};
    httpd_uri_t capture_uri = {.uri = "/api/capture", .method = HTTP_GET, .handler = capture_handler, .user_ctx = NULL};
    httpd_uri_t wifi_list_uri = {.uri = "/api/wifi", .method = HTTP_GET, .handler = wifi_list_handler, .user_ctx = NULL};
    httpd_uri_t wifi_add_uri = {.uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_add_handler, .user_ctx = NULL};
    httpd_uri_t wifi_remove_uri = {.uri = "/api/wifi", .method = HTTP_DELETE, .handler = wifi_remove_handler, .user_ctx = NULL};
    httpd_uri_t wifi_options_uri = {.uri = "/api/wifi", .method = HTTP_OPTIONS, .handler = wifi_options_handler, .user_ctx = NULL};

    Serial.printf("Starting API server on port %d\n", api_config.server_port);
    if (httpd_start(&api_httpd, &api_config) == ESP_OK) {
        register_uri(api_httpd, &index_uri);
        register_uri(api_httpd, &status_uri);
        register_uri(api_httpd, &log_uri);
        register_uri(api_httpd, &wifi_list_uri);
        register_uri(api_httpd, &wifi_add_uri);
        register_uri(api_httpd, &wifi_remove_uri);
        register_uri(api_httpd, &wifi_options_uri);
        if (cameraAvailable) {
            register_uri(api_httpd, &camera_uri);
            register_uri(api_httpd, &capture_uri);
        }
    }

    if (cameraAvailable) {
        httpd_config_t stream_config = HTTPD_DEFAULT_CONFIG();
        stream_config.server_port = 81;
        stream_config.ctrl_port = api_config.ctrl_port + 1;
        stream_config.stack_size = 16384; // 推流 handler 调用链深，默认栈会溢出
        // 与 API 服务器同样的连接池调整：默认 7 socket + 不淘汰，浏览器长连接推流会占满，
        // 导致新连接(含重连)完全得不到响应而卡在 CONNECTING。
        stream_config.max_open_sockets = 10;
        stream_config.lru_purge_enable = true;
        httpd_uri_t stream_uri = {.uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL};

        Serial.printf("Starting stream server on port %d\n", stream_config.server_port);
        if (httpd_start(&stream_httpd, &stream_config) == ESP_OK) {
            register_uri(stream_httpd, &stream_uri);
        }
    } else {
        Serial.println("Camera unavailable; stream server disabled");
    }
}
