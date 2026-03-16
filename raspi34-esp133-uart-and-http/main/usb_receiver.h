// main/usb_receiver.h
#pragma once

#include "esp_err.h"

/**
 * @brief USB Host CDC-ACM受信を開始
 *
 * USB Hostスタックを初期化し、CDC-ACMデバイス（Pico）からの
 * E6UPプロトコルデータを受信するタスクを開始する
 */
void usb_receiver_start(void);
