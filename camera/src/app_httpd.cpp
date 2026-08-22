#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "img_converters.h"
#include "Arduino.h"
#include <WiFi.h>

extern bool cameraAvailable;

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

static ra_filter_t ra_filter;
static httpd_handle_t stream_httpd = NULL;
static httpd_handle_t api_httpd = NULL;

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
        Serial.println("Camera capture failed");
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
    while (true) {
        camera_fb_t *fb = esp_camera_fb_get();
        uint8_t *jpg_buf = NULL;
        size_t jpg_len = 0;
        bool converted = false;

        if (!fb) {
            Serial.println("Camera capture failed");
            result = ESP_FAIL;
        } else if (fb->format == PIXFORMAT_JPEG) {
            jpg_buf = fb->buf;
            jpg_len = fb->len;
        } else {
            converted = frame2jpg(fb, 80, &jpg_buf, &jpg_len);
            if (!converted) {
                Serial.println("JPEG conversion failed");
                result = ESP_FAIL;
            }
        }

        if (result == ESP_OK) {
            char part_buf[64];
            size_t header_len = snprintf(part_buf, sizeof(part_buf), _STREAM_PART, jpg_len);
            result = httpd_resp_send_chunk(req, part_buf, header_len);
        }
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
        if (result != ESP_OK) {
            break;
        }

        int64_t now = esp_timer_get_time();
        uint32_t frame_ms = (uint32_t)((now - last_frame) / 1000);
        last_frame = now;
        uint32_t average_ms = ra_filter_run(&ra_filter, frame_ms);
        Serial.printf("MJPG: %uB %ums, avg %ums\n", (unsigned int)jpg_len, frame_ms, average_ms);
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

    if (!cameraAvailable) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camera unavailable");
    }
    sensor_t *sensor = esp_camera_sensor_get();
    int val = atoi(value);
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
    snprintf(response, sizeof(response),
             "{\"ip\":\"%s\",\"hostname\":\"fpv-car.local\",\"uptime_ms\":%lu,"
             "\"free_heap\":%lu,\"psram_size\":%lu,\"free_psram\":%lu,\"wifi_rssi\":%d,"
             "\"cpu_mhz\":%u,"
             "\"camera_available\":%s,\"framesize\":%d,\"quality\":%d,"
             "\"brightness\":%d,\"contrast\":%d,\"saturation\":%d}",
             ip.c_str(), (unsigned long)millis(), (unsigned long)ESP.getFreeHeap(),
             (unsigned long)ESP.getPsramSize(), (unsigned long)ESP.getFreePsram(), WiFi.RSSI(),
             (unsigned int)ESP.getCpuFreqMHz(), sensor ? "true" : "false", framesize, quality,
             brightness, contrast, saturation);

    httpd_resp_set_type(req, "application/json");
    set_cors(req);
    return httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
}

static void register_uri(httpd_handle_t server, httpd_uri_t *uri) {
    esp_err_t result = httpd_register_uri_handler(server, uri);
    if (result != ESP_OK) {
        Serial.printf("Failed to register %s: 0x%x\n", uri->uri, result);
    }
}

void startCameraServer() {
    ra_filter_init(&ra_filter, 20);

    httpd_config_t api_config = HTTPD_DEFAULT_CONFIG();
    api_config.max_uri_handlers = 8;

    httpd_uri_t status_uri = {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = NULL};
    httpd_uri_t camera_uri = {.uri = "/api/camera", .method = HTTP_GET, .handler = camera_control_handler, .user_ctx = NULL};
    httpd_uri_t capture_uri = {.uri = "/api/capture", .method = HTTP_GET, .handler = capture_handler, .user_ctx = NULL};

    Serial.printf("Starting API server on port %d\n", api_config.server_port);
    if (httpd_start(&api_httpd, &api_config) == ESP_OK) {
        register_uri(api_httpd, &status_uri);
        if (cameraAvailable) {
            register_uri(api_httpd, &camera_uri);
            register_uri(api_httpd, &capture_uri);
        }
    }

    if (cameraAvailable) {
        httpd_config_t stream_config = HTTPD_DEFAULT_CONFIG();
        stream_config.server_port = 81;
        stream_config.ctrl_port = api_config.ctrl_port + 1;
        httpd_uri_t stream_uri = {.uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL};

        Serial.printf("Starting stream server on port %d\n", stream_config.server_port);
        if (httpd_start(&stream_httpd, &stream_config) == ESP_OK) {
            register_uri(stream_httpd, &stream_uri);
        }
    } else {
        Serial.println("Camera unavailable; stream server disabled");
    }
}
