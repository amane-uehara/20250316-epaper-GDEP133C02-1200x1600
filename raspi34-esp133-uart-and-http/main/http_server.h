#pragma once

#include "esp_err.h"

/**
 * @brief HTTPサーバーを起動する
 *
 * POST /image  - E6UPフォーマットの画像データを受信してEPDに表示
 * GET  /status - 現在の状態をJSON形式で返す
 *
 * @return ESP_OK 成功, その他 エラー
 */
esp_err_t http_server_start(void);

/**
 * @brief HTTPサーバーを停止する
 */
void http_server_stop(void);
