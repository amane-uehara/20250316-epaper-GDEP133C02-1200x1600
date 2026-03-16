// main/usb_receiver.c
// USB Host CDC-ACM版の画像受信モジュール
// Raspberry Pi PicoからE6UPプロトコルで画像データを受信する

#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_crc.h"
#include "esp_heap_caps.h"
#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"

#define TAG "USB_RX"

// E6UPプロトコル定義（uart_receiver.cと同一）
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

// USB受信バッファ
#define USB_RX_BUF_SIZE (64 * 1024)
static uint8_t *usb_rx_buf = NULL;
static size_t usb_rx_buf_head = 0;
static size_t usb_rx_buf_tail = 0;
static SemaphoreHandle_t usb_rx_mutex = NULL;
static SemaphoreHandle_t usb_rx_data_sem = NULL;

// CDC-ACMデバイスハンドル
static cdc_acm_dev_hdl_t cdc_dev = NULL;

// イベントグループ
static EventGroupHandle_t usb_events = NULL;
#define USB_DEVICE_CONNECTED_BIT BIT0
#define USB_DEVICE_DISCONNECTED_BIT BIT1

// USB受信コールバック
static bool usb_rx_callback(const uint8_t *data, size_t data_len, void *arg)
{
    if (usb_rx_mutex == NULL || usb_rx_buf == NULL) {
        return true;
    }

    if (xSemaphoreTake(usb_rx_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        // リングバッファにデータを追加
        for (size_t i = 0; i < data_len; i++) {
            size_t next_head = (usb_rx_buf_head + 1) % USB_RX_BUF_SIZE;
            if (next_head != usb_rx_buf_tail) {
                usb_rx_buf[usb_rx_buf_head] = data[i];
                usb_rx_buf_head = next_head;
            } else {
                // バッファフル - 古いデータを破棄
                ESP_LOGW(TAG, "RX buffer overflow");
                break;
            }
        }
        xSemaphoreGive(usb_rx_mutex);
        xSemaphoreGive(usb_rx_data_sem);
    }
    return true;
}

// CDC-ACMイベントコールバック
static void cdc_acm_event_callback(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    switch (event->type) {
        case CDC_ACM_HOST_ERROR:
            ESP_LOGE(TAG, "CDC-ACM error: %d", event->data.error);
            break;
        case CDC_ACM_HOST_DEVICE_DISCONNECTED:
            ESP_LOGW(TAG, "CDC-ACM device disconnected");
            if (usb_events) {
                xEventGroupSetBits(usb_events, USB_DEVICE_DISCONNECTED_BIT);
            }
            cdc_dev = NULL;
            break;
        case CDC_ACM_HOST_SERIAL_STATE:
            ESP_LOGI(TAG, "Serial state: 0x%04x", event->data.serial_state.val);
            break;
        case CDC_ACM_HOST_NETWORK_CONNECTION:
        default:
            break;
    }
}

// USB Hostライブラリタスク
static void usb_host_lib_task(void *arg)
{
    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_LOGI(TAG, "No more USB clients");
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "All USB devices freed");
        }
    }
}

// バッファからデータを読み取る
static size_t usb_read_available(void)
{
    size_t available = 0;
    if (xSemaphoreTake(usb_rx_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (usb_rx_buf_head >= usb_rx_buf_tail) {
            available = usb_rx_buf_head - usb_rx_buf_tail;
        } else {
            available = USB_RX_BUF_SIZE - usb_rx_buf_tail + usb_rx_buf_head;
        }
        xSemaphoreGive(usb_rx_mutex);
    }
    return available;
}

// バッファからデータを読み取る（正確なサイズを読み取るまでブロック）
static esp_err_t usb_read_exact(uint8_t *dst, size_t len, uint32_t timeout_ms)
{
    size_t got = 0;
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    while (got < len) {
        // タイムアウトチェック
        TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= timeout_ticks) {
            return ESP_ERR_TIMEOUT;
        }

        // データ待ち
        TickType_t remaining = timeout_ticks - elapsed;
        if (usb_read_available() == 0) {
            if (xSemaphoreTake(usb_rx_data_sem, remaining) != pdTRUE) {
                return ESP_ERR_TIMEOUT;
            }
        }

        // バッファからデータを読み取る
        if (xSemaphoreTake(usb_rx_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            while (got < len && usb_rx_buf_tail != usb_rx_buf_head) {
                dst[got++] = usb_rx_buf[usb_rx_buf_tail];
                usb_rx_buf_tail = (usb_rx_buf_tail + 1) % USB_RX_BUF_SIZE;
            }
            xSemaphoreGive(usb_rx_mutex);
        }
    }
    return ESP_OK;
}

// デバイスにデータを送信
static esp_err_t usb_write_str(const char *s)
{
    if (cdc_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return cdc_acm_host_data_tx_blocking(cdc_dev, (const uint8_t *)s, strlen(s), pdMS_TO_TICKS(1000));
}

// CDC-ACMデバイスを開く
static esp_err_t open_cdc_device(void)
{
    ESP_LOGI(TAG, "Opening CDC-ACM device...");

    const cdc_acm_host_device_config_t dev_config = {
        .connection_timeout_ms = 5000,
        .out_buffer_size = 512,
        .in_buffer_size = 4096,
        .event_cb = cdc_acm_event_callback,
        .data_cb = usb_rx_callback,
        .user_arg = NULL,
    };

    // VID/PIDを指定せず、任意のCDC-ACMデバイスを開く
    // Raspberry Pi Pico: VID=0x2E8A, PID=0x000A (CDC)
    esp_err_t err = cdc_acm_host_open(0x2E8A, 0x000A, 0, &dev_config, &cdc_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open Pico CDC device (0x2E8A:0x000A): %s", esp_err_to_name(err));
        // 汎用CDC-ACMデバイスとして再試行
        err = cdc_acm_host_open_vendor_specific(0x2E8A, 0x000A, 0, &dev_config, &cdc_dev);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to open as vendor specific: %s", esp_err_to_name(err));
            return err;
        }
    }

    ESP_LOGI(TAG, "CDC-ACM device opened successfully");

    // ライン設定（115200bps, 8N1）- Picoは無視するが念のため
    cdc_acm_line_coding_t line_coding = {
        .dwDTERate = 115200,
        .bCharFormat = 0,  // 1 stop bit
        .bParityType = 0,  // No parity
        .bDataBits = 8,
    };
    err = cdc_acm_host_line_coding_set(cdc_dev, &line_coding);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set line coding (may be ignored): %s", esp_err_to_name(err));
    }

    // DTR/RTS設定
    err = cdc_acm_host_set_control_line_state(cdc_dev, true, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set control line state (may be ignored): %s", esp_err_to_name(err));
    }

    xEventGroupSetBits(usb_events, USB_DEVICE_CONNECTED_BIT);
    return ESP_OK;
}

// 受信タスク
static void receiver_task(void *arg)
{
    while (1) {
        // デバイス接続待ち
        if (cdc_dev == NULL) {
            ESP_LOGI(TAG, "Waiting for CDC-ACM device...");
            while (open_cdc_device() != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }

        // バッファクリア
        if (xSemaphoreTake(usb_rx_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            usb_rx_buf_head = 0;
            usb_rx_buf_tail = 0;
            xSemaphoreGive(usb_rx_mutex);
        }

        // READY送信
        usb_write_str("READY\n");
        ESP_LOGI(TAG, "Sent READY, waiting for data...");

        while (cdc_dev != NULL) {
            // ヘッダ受信
            frame_hdr_t hdr;
            esp_err_t err = usb_read_exact((uint8_t *)&hdr, sizeof(hdr), 30000);
            if (err != ESP_OK) {
                if (cdc_dev == NULL) break;  // 切断された
                continue;  // タイムアウト - 再試行
            }

            // マジック確認
            if (hdr.magic[0] != MAGIC0 || hdr.magic[1] != MAGIC1 ||
                hdr.magic[2] != MAGIC2 || hdr.magic[3] != MAGIC3) {
                ESP_LOGW(TAG, "bad magic: %c%c%c%c", hdr.magic[0], hdr.magic[1], hdr.magic[2], hdr.magic[3]);
                usb_write_str("ERR bad magic\n");
                continue;
            }

            // バージョン/フォーマット確認
            if (hdr.ver != 1 || hdr.fmt != 0) {
                ESP_LOGW(TAG, "bad ver/fmt: ver=%u fmt=%u", hdr.ver, hdr.fmt);
                usb_write_str("ERR bad ver/fmt\n");
                continue;
            }

            // 解像度確認（1200x1600）
            if (hdr.w != 1200 || hdr.h != 1600) {
                ESP_LOGW(TAG, "bad size: %ux%u", hdr.w, hdr.h);
                usb_write_str("ERR bad size\n");
                continue;
            }

            // ペイロード長確認
            uint32_t expected_len = (uint32_t)(1200u * 1600u / 2u);
            if (hdr.payload_len != expected_len) {
                ESP_LOGW(TAG, "bad len: %u (expected %u)", (unsigned)hdr.payload_len, (unsigned)expected_len);
                usb_write_str("ERR bad len\n");
                continue;
            }

            ESP_LOGI(TAG, "Header OK: %ux%u, %u bytes", hdr.w, hdr.h, (unsigned)hdr.payload_len);

            // PSRAMにバッファ確保（960KB）
            uint8_t *buf = (uint8_t *)heap_caps_malloc(hdr.payload_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!buf) {
                ESP_LOGE(TAG, "Failed to allocate %u bytes", (unsigned)hdr.payload_len);
                usb_write_str("ERR no mem\n");
                continue;
            }

            // ペイロード受信（タイムアウト60秒）
            err = usb_read_exact(buf, hdr.payload_len, 60000);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Payload timeout");
                heap_caps_free(buf);
                usb_write_str("ERR payload timeout\n");
                continue;
            }

            // CRC32検証
            uint32_t crc = esp_crc32_le(0, buf, hdr.payload_len);
            if (crc != hdr.crc32) {
                ESP_LOGW(TAG, "CRC mismatch: got=%08" PRIx32 " exp=%08" PRIx32, crc, hdr.crc32);
                heap_caps_free(buf);
                usb_write_str("ERR crc\n");
                continue;
            }

            ESP_LOGI(TAG, "Frame OK, drawing...");
            usb_write_str("OK recv\n");

            // EPD描画
            err = epd_draw_4bpp(buf, hdr.payload_len, hdr.w, hdr.h);
            heap_caps_free(buf);

            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Draw complete");
                usb_write_str("OK draw\n");
            } else {
                ESP_LOGE(TAG, "Draw failed: %d", err);
                usb_write_str("ERR draw\n");
            }
        }

        // 切断された場合は再接続待ち
        ESP_LOGW(TAG, "Device disconnected, will reconnect...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void usb_receiver_start(void)
{
    ESP_LOGI(TAG, "Initializing USB Host...");

    // イベントグループ作成
    usb_events = xEventGroupCreate();
    if (!usb_events) {
        ESP_LOGE(TAG, "Failed to create event group");
        return;
    }

    // 受信バッファ作成（PSRAM使用）
    usb_rx_buf = (uint8_t *)heap_caps_malloc(USB_RX_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!usb_rx_buf) {
        ESP_LOGE(TAG, "Failed to allocate RX buffer");
        return;
    }
    usb_rx_buf_head = 0;
    usb_rx_buf_tail = 0;

    // Mutex/セマフォ作成
    usb_rx_mutex = xSemaphoreCreateMutex();
    usb_rx_data_sem = xSemaphoreCreateBinary();
    if (!usb_rx_mutex || !usb_rx_data_sem) {
        ESP_LOGE(TAG, "Failed to create semaphores");
        return;
    }

    // USB Hostライブラリ初期化
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB Host install failed: %s", esp_err_to_name(err));
        return;
    }

    // USB Hostライブラリタスク開始
    xTaskCreatePinnedToCore(usb_host_lib_task, "usb_host_lib", 4096, NULL, 2, NULL, 0);

    // CDC-ACMドライバ初期化
    const cdc_acm_host_driver_config_t cdc_config = {
        .driver_task_stack_size = 4096,
        .driver_task_priority = 5,
        .xCoreID = 0,
    };
    err = cdc_acm_host_install(&cdc_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CDC-ACM Host install failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "USB Host initialized");

    // 受信タスク開始
    xTaskCreatePinnedToCore(receiver_task, "usb_receiver", 8192, NULL, 5, NULL, 1);
}
