#ifndef EPD_DRIVER_H
#define EPD_DRIVER_H

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

/**
 * @brief EPDドライバを初期化する
 *
 * GPIO、SPI、電源を初期化し、EPDに初期化コマンドを送信する
 *
 * @return ESP_OK 成功時, その他 エラー時
 */
esp_err_t epd_init(void);

/**
 * @brief EPDドライバを終了する
 *
 * SPIバスを解放し、電源をオフにする
 */
void epd_deinit(void);

/**
 * @brief 画像データをEPDに書き込む
 *
 * @param data 4bpp画像データ（1200x1600 = 960000バイト）
 * @param len データ長
 * @return ESP_OK 成功時, その他 エラー時
 */
esp_err_t epd_write_image(const uint8_t *data, size_t len);

/**
 * @brief EPD表示を更新（リフレッシュ）する
 *
 * 書き込んだ画像を実際に表示する。数十秒かかる場合がある。
 *
 * @return ESP_OK 成功時, その他 エラー時
 */
esp_err_t epd_refresh(void);

#endif // EPD_DRIVER_H
