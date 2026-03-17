#include "http_server.h"
#include "wifi_manager.h"
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_crc.h"
#include "esp_system.h"

#define TAG "HTTP"

// E6UPプロトコル定義
#define MAGIC0 'E'
#define MAGIC1 '6'
#define MAGIC2 'U'
#define MAGIC3 'P'

typedef struct __attribute__((packed)) {
    uint8_t magic[4];      // "E6UP"
    uint8_t ver;           // 1
    uint16_t w;            // 1200
    uint16_t h;            // 1600
    uint8_t fmt;           // 0: 4bpp palette
    uint32_t payload_len;  // 960000
    uint32_t crc32;        // payload crc32
} frame_hdr_t;

// EPD描画関数（main.cで定義）
extern esp_err_t epd_draw_4bpp(const uint8_t *buf, size_t len, uint16_t w, uint16_t h);

static httpd_handle_t server = NULL;

// HTTPリクエストからバイト列を正確に受信する
static esp_err_t recv_exact(httpd_req_t *req, void *dst, size_t len)
{
    size_t received = 0;
    while (received < len) {
        int ret = httpd_req_recv(req, (char *)dst + received, len - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return ESP_FAIL;
        }
        received += ret;
    }
    return ESP_OK;
}

// POST /image ハンドラ
static esp_err_t image_post_handler(httpd_req_t *req)
{
    int total_len = req->content_len;
    ESP_LOGI(TAG, "POST /image content_len=%d", total_len);

    // サイズ検証
    if (total_len < (int)sizeof(frame_hdr_t)) {
        ESP_LOGW(TAG, "Content too small: %d", total_len);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Too small");
        return ESP_FAIL;
    }

    // ヘッダ受信
    frame_hdr_t hdr;
    if (recv_exact(req, &hdr, sizeof(hdr)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Header receive error");
        return ESP_FAIL;
    }

    // マジック確認
    if (hdr.magic[0] != MAGIC0 || hdr.magic[1] != MAGIC1 ||
        hdr.magic[2] != MAGIC2 || hdr.magic[3] != MAGIC3) {
        ESP_LOGW(TAG, "Bad magic: %c%c%c%c",
                 hdr.magic[0], hdr.magic[1], hdr.magic[2], hdr.magic[3]);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad magic");
        return ESP_FAIL;
    }

    // バージョン/フォーマット確認
    if (hdr.ver != 1 || hdr.fmt != 0) {
        ESP_LOGW(TAG, "Bad ver/fmt: ver=%u fmt=%u", hdr.ver, hdr.fmt);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad version/format");
        return ESP_FAIL;
    }

    // 解像度確認
    if (hdr.w != 1200 || hdr.h != 1600) {
        ESP_LOGW(TAG, "Bad size: %ux%u", hdr.w, hdr.h);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad size");
        return ESP_FAIL;
    }

    // ペイロード長確認
    uint32_t expected_len = (uint32_t)(1200u * 1600u / 2u);
    if (hdr.payload_len != expected_len) {
        ESP_LOGW(TAG, "Bad payload_len: %" PRIu32 " (expected %" PRIu32 ")",
                 hdr.payload_len, expected_len);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad payload length");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Header OK: %ux%u, %" PRIu32 " bytes",
             hdr.w, hdr.h, hdr.payload_len);

    // PSRAMにバッファ確保
    uint8_t *buf = (uint8_t *)heap_caps_malloc(hdr.payload_len,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate %" PRIu32 " bytes", hdr.payload_len);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }

    // ペイロード受信
    if (recv_exact(req, buf, hdr.payload_len) != ESP_OK) {
        ESP_LOGE(TAG, "Payload receive error");
        heap_caps_free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Payload receive error");
        return ESP_FAIL;
    }

    // CRC32検証
    uint32_t crc = esp_crc32_le(0, buf, hdr.payload_len);
    if (crc != hdr.crc32) {
        ESP_LOGW(TAG, "CRC mismatch: got=%08" PRIx32 " exp=%08" PRIx32,
                 crc, hdr.crc32);
        heap_caps_free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "CRC mismatch");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Payload OK, drawing...");

    // EPD描画（数十秒かかる場合がある）
    esp_err_t err = epd_draw_4bpp(buf, hdr.payload_len, hdr.w, hdr.h);
    heap_caps_free(buf);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Draw complete");
        httpd_resp_sendstr(req, "OK\n");
    } else {
        ESP_LOGE(TAG, "Draw failed: %d", err);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Draw failed");
    }

    return err;
}

// GET /status ハンドラ
static esp_err_t status_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ready\"}");
    return ESP_OK;
}

// POST /wifi/reset ハンドラ
static esp_err_t wifi_reset_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "WiFi credential reset requested");

    esp_err_t err = wifi_clear_credentials();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS erase failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"credentials cleared, restarting\"}");

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    /* NOTREACHED */
    return ESP_OK;
}

esp_err_t http_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 16384;         // EPD描画がスタックを使うため大きめに
    config.recv_wait_timeout = 60;     // 高レイテンシ環境での大容量受信用
    config.send_wait_timeout = 60;

    ESP_LOGI(TAG, "Starting HTTP server on port %d", config.server_port);

    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start server: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t image_uri = {
        .uri = "/image",
        .method = HTTP_POST,
        .handler = image_post_handler,
    };
    httpd_register_uri_handler(server, &image_uri);

    const httpd_uri_t status_uri = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
    };
    httpd_register_uri_handler(server, &status_uri);

    const httpd_uri_t wifi_reset_uri = {
        .uri = "/wifi/reset",
        .method = HTTP_POST,
        .handler = wifi_reset_handler,
    };
    httpd_register_uri_handler(server, &wifi_reset_uri);

    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}

void http_server_stop(void)
{
    if (server) {
        httpd_stop(server);
        server = NULL;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
}
