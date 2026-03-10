#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"
#include "driver/i2s.h"

// ===========================
// 사용자 설정
// ===========================
const char* ssid = "imsenz";
const char* password = "imsenz2601";

#define STREAM_USER "admin"
#define STREAM_PASS "test123"

// ESP-EYE 핀 설정
#define PWDN_GPIO_NUM    -1
#define RESET_GPIO_NUM   -1
#define XCLK_GPIO_NUM     4
#define SIOD_GPIO_NUM    18
#define SIOC_GPIO_NUM    23
#define Y9_GPIO_NUM      36
#define Y8_GPIO_NUM      37
#define Y7_GPIO_NUM      38
#define Y6_GPIO_NUM      39
#define Y5_GPIO_NUM      35
#define Y4_GPIO_NUM      14
#define Y3_GPIO_NUM      13
#define Y2_GPIO_NUM      34
#define VSYNC_GPIO_NUM    5
#define HREF_GPIO_NUM    27
#define PCLK_GPIO_NUM    25

// 마이크 I2S 핀 (ESP-EYE 표준)
#define I2S_WS            26
#define I2S_SD            33
#define I2S_PORT          I2S_NUM_0
#define SAMPLE_RATE       16000

// MJPEG 헤더
#define PART_BOUNDARY "123456789000000000000987654321"

// --- 인증 확인 함수 ---
static esp_err_t check_auth(httpd_req_t *req) {
    size_t buf_len = httpd_req_get_hdr_value_len(req, "Authorization") + 1;
    if (buf_len > 1) {
        // 실제 운영 시에는 여기서 Base64 디코딩 후 ID/PW를 대조합니다.
        // 현재는 헤더 존재 여부만 체크하는 기초 인증 구조입니다.
        return ESP_OK;
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP-EYE Login\"");
    httpd_resp_send(req, NULL, 0);
    return ESP_FAIL;
}

// --- 오디오 핸들러 (/audio) ---
static esp_err_t audio_handler(httpd_req_t *req) {
    if (check_auth(req) != ESP_OK) return ESP_FAIL;

    esp_err_t res = ESP_OK;
    size_t bytes_read = 0;
    const size_t chunk_size = 2048;
    int16_t *buffer = (int16_t *)malloc(chunk_size);

    httpd_resp_set_type(req, "audio/x-wav");

    while (true) {
        i2s_read(I2S_PORT, buffer, chunk_size, &bytes_read, portMAX_DELAY);
        res = httpd_resp_send_chunk(req, (const char *)buffer, bytes_read);
        if (res != ESP_OK) break;
    }

    free(buffer);
    return res;
}

// --- MJPEG 핸들러 (/mjpeg) ---
static esp_err_t mjpeg_handler(httpd_req_t *req) {
    if (check_auth(req) != ESP_OK) return ESP_FAIL;

    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;
    char part_buf[64];
    static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
    static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
    static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

    httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);

    while (true) {
        fb = esp_camera_fb_get();
        if (!fb) break;
        size_t hlen = snprintf(part_buf, 64, _STREAM_PART, fb->len);
        res = httpd_resp_send_chunk(req, part_buf, hlen);
        if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        if (res == ESP_OK) res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        esp_camera_fb_return(fb);
        if (res != ESP_OK) break;
    }
    return res;
}

// --- 메인 페이지 (/): 영상과 소리를 한 화면에 출력 ---
static esp_err_t index_handler(httpd_req_t *req) {
    if (check_auth(req) != ESP_OK) return ESP_FAIL;

    const char* html = 
        "<html><head><title>ESP-EYE Monitor</title></head><body>"
        "<h1>ESP-EYE Live</h1>"
        "<img src='/mjpeg' style='width:640px; transform:rotate(0deg);'><br><br>"
        "<audio autoplay controls><source src='/audio' type='audio/x-wav'></audio>"
        "</body></html>";
    return httpd_resp_send(req, html, strlen(html));
}

void init_i2s() {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 512
    };
    i2s_pin_config_t pin_config = {
        .bck_io_num = -1,
        .ws_io_num = I2S_WS,
        .data_out_num = -1,
        .data_in_num = I2S_SD
    };
    i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_PORT, &pin_config);
}

void setup() {
    Serial.begin(115200);

    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM; config.pin_d1 = Y3_GPIO_NUM; config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM; config.pin_d4 = Y6_GPIO_NUM; config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM; config.pin_d7 = Y9_GPIO_NUM; config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM; config.pin_vsync = VSYNC_GPIO_NUM; config.pin_href = HREF_GPIO_NUM;
    config.pin_sscb_sda = SIOD_GPIO_NUM; config.pin_sscb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM; config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size = FRAMESIZE_VGA;
    config.jpeg_quality = 12;
    config.fb_count = 2;

    if (esp_camera_init(&config) != ESP_OK) return;

    init_i2s();

    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) delay(500);

    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    server_config.max_uri_handlers = 8;
    httpd_handle_t server = NULL;
    
    if (httpd_start(&server, &server_config) == ESP_OK) {
        httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = index_handler };
        httpd_uri_t mjpeg_uri = { .uri = "/mjpeg", .method = HTTP_GET, .handler = mjpeg_handler };
        httpd_uri_t audio_uri = { .uri = "/audio", .method = HTTP_GET, .handler = audio_handler };
        httpd_register_uri_handler(server, &index_uri);
        httpd_register_uri_handler(server, &mjpeg_uri);
        httpd_register_uri_handler(server, &audio_uri);
    }

    Serial.printf("\nESP-EYE Server Ready: http://%s\n", WiFi.localIP().toString().c_str());
}

void loop() { delay(1); }