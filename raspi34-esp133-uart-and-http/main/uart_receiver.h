#pragma once

/**
 * @brief UART受信を開始する
 *
 * UART1を初期化し、E6UPプロトコルで画像データを受信するタスクを開始する
 * 受信完了後は自動的にEPDに描画する
 */
void uart_receiver_start(void);
