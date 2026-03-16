#include "dns_server.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_log.h"

#define TAG "DNS"

#define DNS_PORT       53
#define DNS_BUF_SIZE   512
#define RESOLVE_IP     "192.168.4.1"

/* DNS header (12 bytes) */
typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_header_t;

static int s_sock = -1;
static TaskHandle_t s_task = NULL;

static void dns_task(void *arg)
{
    uint8_t buf[DNS_BUF_SIZE];
    struct sockaddr_in client;
    socklen_t client_len;

    /* resolve_ipをバイナリに変換 */
    uint32_t ip_addr;
    inet_pton(AF_INET, RESOLVE_IP, &ip_addr);

    while (1) {
        client_len = sizeof(client);
        int len = recvfrom(s_sock, buf, sizeof(buf), 0,
                           (struct sockaddr *)&client, &client_len);
        if (len < (int)sizeof(dns_header_t)) continue;

        dns_header_t *hdr = (dns_header_t *)buf;

        /* QDCOUNTが0なら無視 */
        if (ntohs(hdr->qdcount) == 0) continue;

        /* Questionセクションの末尾を探す（QNAME + QTYPE(2) + QCLASS(2)） */
        int qname_start = sizeof(dns_header_t);
        int pos = qname_start;
        while (pos < len && buf[pos] != 0) {
            pos += 1 + buf[pos];  /* label length + label */
        }
        if (pos >= len) continue;
        pos++;           /* null terminator */
        pos += 4;        /* QTYPE(2) + QCLASS(2) */
        if (pos > len) continue;

        /* レスポンスを構築: ヘッダのフラグを応答に変更 */
        hdr->flags = htons(0x8000 | 0x0400);  /* QR=1, AA=1 */
        hdr->ancount = hdr->qdcount;           /* 各質問に1つの回答 */

        /* Answerセクションを追加（ポインタ圧縮でQNAMEを参照） */
        uint8_t answer[16];
        answer[0] = 0xC0;          /* ポインタ: 上位2ビット=11 */
        answer[1] = (uint8_t)qname_start;  /* オフセット: QNAMEの位置 */
        answer[2] = 0x00; answer[3] = 0x01;   /* TYPE A */
        answer[4] = 0x00; answer[5] = 0x01;   /* CLASS IN */
        answer[6] = 0x00; answer[7] = 0x00;
        answer[8] = 0x00; answer[9] = 0x0A;   /* TTL = 10秒 */
        answer[10] = 0x00; answer[11] = 0x04;  /* RDLENGTH = 4 */
        memcpy(&answer[12], &ip_addr, 4);      /* RDATA = IP */

        /* Question + Answer を結合して送信 */
        if (pos + 16 <= (int)sizeof(buf)) {
            memcpy(buf + pos, answer, 16);
            sendto(s_sock, buf, pos + 16, 0,
                   (struct sockaddr *)&client, client_len);
        }
    }
}

esp_err_t dns_server_start(void)
{
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        return ESP_FAIL;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind port %d", DNS_PORT);
        close(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }

    xTaskCreate(dns_task, "dns_server", 4096, NULL, 3, &s_task);
    ESP_LOGI(TAG, "Captive portal DNS server started");
    return ESP_OK;
}

void dns_server_stop(void)
{
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
    ESP_LOGI(TAG, "DNS server stopped");
}
