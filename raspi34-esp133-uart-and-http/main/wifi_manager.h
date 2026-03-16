#pragma once

#include "esp_err.h"

/**
 * @brief WiFi STAモードで初期化し、APに接続する
 *
 * NVSに保存済みの認証情報でSTA接続を試行する。
 * 認証情報が無い、またはSTA接続に失敗した場合は
 * SoftAPプロビジョニングモードに移行し、設定完了後に
 * esp_restart()するため呼び出し元には戻らない。
 *
 * @return ESP_OK STA接続成功（プロビジョニング時は戻らない）
 */
esp_err_t wifi_init_sta(void);

/**
 * @brief NVSに保存されたWiFi認証情報を消去する
 *
 * 次回起動時にSoftAPプロビジョニングモードに移行する。
 *
 * @return ESP_OK 消去成功, その他 NVSエラー
 */
esp_err_t wifi_clear_credentials(void);
