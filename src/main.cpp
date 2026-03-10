#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"
#include "driver/i2s.h"
#include "soc/soc.h"           // Required for Brownout settings
#include "soc/rtc_cntl_reg.h"  // Required for Brownout settings

/* ======================================================================
 * NETWORK & AUTHENTICATION CONFIGURATION
 * ====================================================================== */
const char* ssid = "imsenz";
const char* password = "imsenz2601";

#define STREAM_USER "admin"
#define STREAM_PASS "test123"

/* ESP-EYE Camera Pin Mapping */
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

/* ESP-EYE Digital Microphone (PDM) I2S Pin Mapping */
#define I2S_WS            26
#define I2S_SD            33
#define I2S_PORT          I2S_NUM_0  // Try I2S_NUM_1 if error persists
#define SAMPLE_RATE       16000

#define PART_BOUNDARY "123456789000000000000987654321"

/* ======================================================================
 * SERVER HANDLERS
 * ====================================================================== */

static esp_err_t check_auth(httpd_req_t *req) {
    size_t buf_len = httpd_req_get_hdr_value_len(req, "Authorization") + 1;
    if (buf_len > 1) return ESP_OK;

    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP-EYE Monitor\"");
    httpd_resp_send(req, NULL, 0);
    return ESP_FAIL;
}

// Snapshot Handler (/image)
static esp_err_t image_handler(httpd_req_t *req) {
    if (check_auth(req) != ESP_OK) return ESP_FAIL;
    camera_fb_t * fb = esp_camera_fb_get();
    if (!fb) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    return res;
}

// Audio Stream Handler (/audio)
static esp_err_t audio_handler(httpd_req_t *req) {
    if (check_auth(req) != ESP_OK) return ESP_FAIL;
    size_t bytes_read = 0;
    const size_t chunk_size = 1024;
    int16_t *buffer = (int16_t *)malloc(chunk_size);
    if (!buffer) return ESP_FAIL;
    httpd_resp_set_type(req, "audio/x-wav");
    esp_err_t res = ESP_OK;
    while (true) {
        i2s_read(I2S_PORT, buffer, chunk_size, &bytes_read, portMAX_DELAY);
        res = httpd_resp_send_chunk(req, (const char *)buffer, bytes_read);
        if (res != ESP_OK) break;
    }
    free(buffer);
    return res;
}

// MJPEG Video Stream Handler (/mjpeg)
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

// Main Dashboard (/)
static esp_err_t index_handler(httpd_req_t *req) {
    if (check_auth(req) != ESP_OK) return ESP_FAIL;
    const char* html = 
        "<html><head><meta charset='utf-8'><title>ESP-EYE Dashboard</title></head>"
        "<body style='text-align:center; background:#121212; color:#ffffff; font-family:Arial,sans-serif;'>"
        "<h2>ESP-EYE Live Monitor</h2>"
        "<div style='margin-bottom:15px;'><img src='/mjpeg' style='width:90%; max-width:640px; border:2px solid #333;'></div>"
        "<div><p>Live Audio</p><audio autoplay controls style='width:90%; max-width:640px;'><source src='/audio' type='audio/x-wav'></audio></div>"
        "<div style='margin-top:20px; font-size:1.2em;'><a href='/image' target='_blank' style='color:#00d1b2;'>[ View Snapshot ]</a></div>"
        "</body></html>";
    return httpd_resp_send(req, html, strlen(html));
}

/* ======================================================================
 * HARDWARE INITIALIZATION
 * ====================================================================== */

void init_i2s() {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = 0, // FIXED: Use 0 for auto-allocation to avoid conflict
        .dma_buf_count = 4,
        .dma_buf_len = 256,
        .use_apll = false
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = -1,
        .ws_io_num = I2S_WS,
        .data_out_num = -1,
        .data_in_num = I2S_SD
    };

    if (i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL) == ESP_OK) {
        i2s_set_pin(I2S_PORT, &pin_config);
        Serial.println("I2S initialized successfully");
    } else {
        Serial.println("I2S driver installation failed!");
    }
}

void setup() {
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // Disable brownout
    Serial.begin(115200);

    // FIXED: Initialize I2S BEFORE Camera to secure interrupt resources
    init_i2s(); 

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

    if (esp_camera_init(&config) != ESP_OK) {
        Serial.println("Camera Init Failed!");
        // If camera fails, sometimes it's due to I2S conflict on I2S_NUM_0
        return;
    }

    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi Connected");

    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    server_config.max_uri_handlers = 6;
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
    Serial.printf("Dashboard: http://%s\n", WiFi.localIP().toString().c_str());
}

void loop() {
    delay(1000);
}