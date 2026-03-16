#pragma once

#include "esp_err.h"

/**
 * キャプティブポータル用DNSサーバーを開始する。
 * 全てのDNSクエリに対してresolve_ipを返す。
 */
esp_err_t dns_server_start(void);

/**
 * DNSサーバーを停止する。
 */
void dns_server_stop(void);
