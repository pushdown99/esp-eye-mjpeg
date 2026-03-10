#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"
#include "driver/i2s.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

/* --- CONFIGURATION --- */
const char* ssid = "imsenz";
const char* password = "imsenz2601";

// ID: admin / PW: test123 -> "Basic YWRtaW46dGVzdDEyMw=="
#define AUTH_BASIC "Basic YWRtaW46dGVzdDEyMw=="

/* Camera Pins (ESP-EYE Standard) */
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

/* Mic Pins (ESP-EYE PDM) */
#define I2S_WS            26
#define I2S_SD            33
#define I2S_PORT          I2S_NUM_0 
#define SAMPLE_RATE       16000

#define PART_BOUNDARY "123456789000000000000987654321"

bool i2s_running = false;

/* --- AUTHENTICATION --- */
static esp_err_t check_auth(httpd_req_t *req) {
    size_t buf_len = httpd_req_get_hdr_value_len(req, "Authorization") + 1;
    if (buf_len > 1) {
        char *buf = (char *)malloc(buf_len);
        if (httpd_req_get_hdr_value_str(req, "Authorization", buf, buf_len) == ESP_OK) {
            if (strcmp(buf, AUTH_BASIC) == 0) {
                free(buf);
                return ESP_OK;
            }
        }
        free(buf);
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP-EYE\"");
    httpd_resp_send(req, NULL, 0);
    return ESP_FAIL;
}

/* --- HANDLERS --- */

// 1. Snapshot Handler (/image)
static esp_err_t image_handler(httpd_req_t *req) {
    if (check_auth(req) != ESP_OK) return ESP_FAIL;
    camera_fb_t * fb = esp_camera_fb_get();
    if (!fb) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    return res;
}

// 2. Audio Stream Handler (/audio) - With Crash Protection
static esp_err_t audio_handler(httpd_req_t *req) {
    if (check_auth(req) != ESP_OK) return ESP_FAIL;

    if (!i2s_running) {
        i2s_config_t i2s_config = {
            .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM),
            .sample_rate = SAMPLE_RATE,
            .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
            .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
            .communication_format = I2S_COMM_FORMAT_STAND_I2S,
            .intr_alloc_flags = ESP_INTR_FLAG_SHARED, // Try sharing interrupt
            .dma_buf_count = 4,
            .dma_buf_len = 256,
            .use_apll = false
        };
        i2s_pin_config_t pin_config = { .bck_io_num = -1, .ws_io_num = I2S_WS, .data_out_num = -1, .data_in_num = I2S_SD };
        
        esp_err_t err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
        if (err != ESP_OK) {
            Serial.printf("I2S Driver Install Failed: 0x%x\n", err);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "I2S Init Failed");
        }
        i2s_set_pin(I2S_PORT, &pin_config);
        i2s_running = true;
        Serial.println("I2S Driver Installed");
    }

    int16_t *buffer = (int16_t *)malloc(1024);
    if (!buffer) return ESP_FAIL;

    size_t bytes_read = 0;
    httpd_resp_set_type(req, "audio/x-wav");
    
    while (true) {
        // Only read if driver is confirmed running
        esp_err_t r_err = i2s_read(I2S_PORT, buffer, 1024, &bytes_read, portMAX_DELAY);
        if (r_err == ESP_OK && bytes_read > 0) {
            if (httpd_resp_send_chunk(req, (const char *)buffer, bytes_read) != ESP_OK) break;
        } else break;
    }
    free(buffer);
    return ESP_OK;
}

// 3. Video Stream Handler (/mjpeg)
static esp_err_t mjpeg_handler(httpd_req_t *req) {
    if (check_auth(req) != ESP_OK) return ESP_FAIL;
    camera_fb_t * fb = NULL;
    char part_buf[64];
    static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
    static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
    static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";
    httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);

    while (true) {
        fb = esp_camera_fb_get();
        if (!fb) { delay(10); continue; }
        size_t hlen = snprintf(part_buf, 64, _STREAM_PART, fb->len);
        if (httpd_resp_send_chunk(req, part_buf, hlen) != ESP_OK) { esp_camera_fb_return(fb); break; }
        if (httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len) != ESP_OK) { esp_camera_fb_return(fb); break; }
        if (httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY)) != ESP_OK) { esp_camera_fb_return(fb); break; }
        esp_camera_fb_return(fb);
    }
    return ESP_OK;
}

// 4. Dashboard (/)
static esp_err_t index_handler(httpd_req_t *req) {
    if (check_auth(req) != ESP_OK) return ESP_FAIL;
    const char* html = "<html><head><meta charset='utf-8'><style>"
                       "body{text-align:center; background:#1e1e2e; color:#cdd6f4; font-family:sans-serif; padding-top:20px;}"
                       "a{color:#89b4fa; text-decoration:none;}</style></head>"
                       "<body><h2>ESP-EYE Monitor</h2>"
                       "<img src='/mjpeg' style='width:90%; max-width:640px; border-radius:10px;'><br><br>"
                       "<audio controls autoplay style='width:90%; max-width:640px;'><source src='/audio' type='audio/x-wav'></audio>"
                       "<p><a href='/image' target='_blank'>[ View Snapshot ]</a></p></body></html>";
    return httpd_resp_send(req, html, strlen(html));
}

void setup() {
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
    Serial.begin(115200);

    /* Camera Initialization First */
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0; config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM; config.pin_d1 = Y3_GPIO_NUM; config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM; config.pin_d4 = Y6_GPIO_NUM; config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM; config.pin_d7 = Y9_GPIO_NUM; config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM; config.pin_vsync = VSYNC_GPIO_NUM; config.pin_href = HREF_GPIO_NUM;
    config.pin_sscb_sda = SIOD_GPIO_NUM; config.pin_sscb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM; config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000; config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size = FRAMESIZE_VGA; config.jpeg_quality = 12; config.fb_count = 1;

    if (esp_camera_init(&config) == ESP_OK) Serial.println("Camera Init OK");

    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    Serial.println("\nWiFi Connected");

    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    server_config.max_uri_handlers = 8;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &server_config) == ESP_OK) {
        httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = index_handler };
        httpd_uri_t mjpeg_uri = { .uri = "/mjpeg", .method = HTTP_GET, .handler = mjpeg_handler };
        httpd_uri_t audio_uri = { .uri = "/audio", .method = HTTP_GET, .handler = audio_handler };
        httpd_uri_t image_uri = { .uri = "/image", .method = HTTP_GET, .handler = image_handler };
        httpd_register_uri_handler(server, &index_uri);
        httpd_register_uri_handler(server, &mjpeg_uri);
        httpd_register_uri_handler(server, &audio_uri);
        httpd_register_uri_handler(server, &image_uri);
    }
}

void loop() { delay(1000); }