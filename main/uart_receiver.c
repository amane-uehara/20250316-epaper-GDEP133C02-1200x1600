// main/uart_receiver.c
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_crc.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"

#define TAG "UART_RX"

#define UART_PORT      UART_NUM_0        // まずはUART0（=今のttyUSB0に乗ってる可能性が高い）
#define UART_BAUD      921600
#define UART_RX_BUF    (64 * 1024)

#define MAGIC0 'E'
#define MAGIC1 '6'
#define MAGIC2 'U'
#define MAGIC3 'P'

typedef struct __attribute__((packed)) {
    uint8_t magic[4];      // "E6UP"
    uint8_t ver;           // 1
    uint16_t w;            // 1600
    uint16_t h;            // 1200
    uint8_t fmt;           // 0: 4bpp palette
    uint32_t payload_len;  // 960000
    uint32_t crc32;        // payload crc32
} frame_hdr_t;

// ★ここを公式サンプルの描画に繋ぐ（要実装）
extern esp_err_t epd_draw_4bpp(const uint8_t *buf, size_t len, uint16_t w, uint16_t h);

static esp_err_t uart_read_exact(uint8_t *dst, size_t len)
{
    size_t got = 0;
    while (got < len) {
        int r = uart_read_bytes(UART_PORT, dst + got, len - got, pdMS_TO_TICKS(2000));
        if (r <= 0) return ESP_ERR_TIMEOUT;
        got += (size_t)r;
    }
    return ESP_OK;
}

static void uart_write_str(const char *s)
{
    uart_write_bytes(UART_PORT, s, (int)strlen(s));
}

static void receiver_task(void *arg)
{
    uart_write_str("READY\n");

    while (1) {
        frame_hdr_t hdr;
        esp_err_t err = uart_read_exact((uint8_t *)&hdr, sizeof(hdr));
        if (err != ESP_OK) continue;

        if (hdr.magic[0]!=MAGIC0 || hdr.magic[1]!=MAGIC1 || hdr.magic[2]!=MAGIC2 || hdr.magic[3]!=MAGIC3) {
            ESP_LOGW(TAG, "bad magic");
            uart_write_str("ERR bad magic\n");
            continue;
        }
        if (hdr.ver != 1 || hdr.fmt != 0) {
            ESP_LOGW(TAG, "bad ver/fmt");
            uart_write_str("ERR bad ver/fmt\n");
            continue;
        }
        // 期待する解像度はサンプルに合わせて 1200x1600
        if (hdr.w != 1200 || hdr.h != 1600) {
            ESP_LOGW(TAG, "bad size %ux%u", hdr.w, hdr.h);
            uart_write_str("ERR bad size\n");
            continue;
        }
        if (hdr.payload_len != (uint32_t)(1200u*1600u/2u)) {
            ESP_LOGW(TAG, "bad len %u", (unsigned)hdr.payload_len);
            uart_write_str("ERR bad len\n");
            continue;
        }

        // PSRAMに確保（960KB）
        uint8_t *buf = (uint8_t *)heap_caps_malloc(hdr.payload_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) {
            ESP_LOGE(TAG, "no mem");
            uart_write_str("ERR no mem\n");
            continue;
        }

        err = uart_read_exact(buf, hdr.payload_len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "payload timeout");
            heap_caps_free(buf);
            uart_write_str("ERR payload timeout\n");
            continue;
        }

        uint32_t crc = esp_crc32_le(0, buf, hdr.payload_len);
        if (crc != hdr.crc32) {
            ESP_LOGW(TAG, "crc mismatch got=%08" PRIx32 " exp=%08" PRIx32, crc, hdr.crc32);
            heap_caps_free(buf);
            uart_write_str("ERR crc\n");
            continue;
        }

        ESP_LOGI(TAG, "frame ok, drawing...");
        uart_write_str("OK recv\n");

        err = epd_draw_4bpp(buf, hdr.payload_len, hdr.w, hdr.h);
        heap_caps_free(buf);

        if (err == ESP_OK) uart_write_str("OK draw\n");
        else uart_write_str("ERR draw\n");
    }
}

void uart_receiver_start(void)
{
    uart_config_t cfg = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, UART_RX_BUF, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));

    // UART0を使う場合：ログが混ざるのが嫌なら menuconfig でログ出力先をUSB-CDC等へ逃がすのがベター
    xTaskCreatePinnedToCore(receiver_task, "receiver_task", 8192, NULL, 5, NULL, 1);
}

