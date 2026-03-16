#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "epd_driver.h"

#define TAG "MAIN"

void uart_receiver_start(void);

void app_main(void)
{
    ESP_LOGI(TAG, "Initializing EPD...");
    esp_err_t err = epd_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "EPD init failed: %d", err);
        // 初期化失敗してもUART受信は動かす（デバッグ用）
    } else {
        ESP_LOGI(TAG, "EPD initialized");
    }

    ESP_LOGI(TAG, "Starting UART receiver...");
    uart_receiver_start();

    // 何もしないで待機（受信タスクが動く）
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t epd_draw_4bpp(const uint8_t *buf, size_t len, uint16_t w, uint16_t h)
{
    ESP_LOGI(TAG, "epd_draw_4bpp: len=%u w=%u h=%u", (unsigned)len, (unsigned)w, (unsigned)h);

    // 画像データをEPDに書き込む
    esp_err_t err = epd_write_image(buf, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "epd_write_image failed: %d", err);
        return err;
    }

    // 表示を更新（リフレッシュ）
    err = epd_refresh();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "epd_refresh failed: %d", err);
        return err;
    }

    ESP_LOGI(TAG, "EPD draw complete");
    return ESP_OK;
}
