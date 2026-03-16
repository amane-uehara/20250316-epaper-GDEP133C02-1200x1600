#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "epd_driver.h"
#include "wifi_manager.h"
#include "http_server.h"
#include "uart_receiver.h"

#define TAG "MAIN"

// EPD排他制御（HTTP/UARTの同時描画を防止）
static SemaphoreHandle_t epd_mutex = NULL;

void app_main(void)
{
    epd_mutex = xSemaphoreCreateMutex();

    ESP_LOGI(TAG, "Initializing EPD...");
    esp_err_t err = epd_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "EPD init failed: %d", err);
    } else {
        ESP_LOGI(TAG, "EPD initialized");
    }

    // UART受信タスク開始
    ESP_LOGI(TAG, "Starting UART receiver...");
    uart_receiver_start();

    // WiFi接続
    ESP_LOGI(TAG, "Connecting to WiFi...");
    err = wifi_init_sta();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connection failed");
    }

    // HTTPサーバー起動
    ESP_LOGI(TAG, "Starting HTTP server...");
    err = http_server_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %d", err);
    }

    // メインタスクは待機
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t epd_draw_4bpp(const uint8_t *buf, size_t len, uint16_t w, uint16_t h)
{
    // mutex取得（最大5秒待機、描画中なら呼び出し元にビジーを返す）
    if (xSemaphoreTake(epd_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(TAG, "EPD busy, draw rejected");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "epd_draw_4bpp: len=%u w=%u h=%u", (unsigned)len, (unsigned)w, (unsigned)h);

    esp_err_t err = epd_write_image(buf, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "epd_write_image failed: %d", err);
        xSemaphoreGive(epd_mutex);
        return err;
    }

    err = epd_refresh();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "epd_refresh failed: %d", err);
        xSemaphoreGive(epd_mutex);
        return err;
    }

    ESP_LOGI(TAG, "EPD draw complete");
    xSemaphoreGive(epd_mutex);
    return ESP_OK;
}
