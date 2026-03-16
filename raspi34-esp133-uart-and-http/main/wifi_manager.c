#include "wifi_manager.h"
#include "dns_server.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"

#define TAG "WIFI"

#define NVS_NAMESPACE  "wifi_cfg"
#define NVS_KEY_SSID   "ssid"
#define NVS_KEY_PASS   "pass"

#define WIFI_MAX_RETRY     10
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

#define SOFTAP_SSID        "EPD-Setup"
#define SOFTAP_MAX_CONN    2
#define SOFTAP_CHANNEL     1

/* ---------- NVS helpers ---------- */

static esp_err_t nvs_load_wifi_creds(char *ssid, size_t ssid_sz,
                                     char *pass, size_t pass_sz)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    err = nvs_get_str(h, NVS_KEY_SSID, ssid, &ssid_sz);
    if (err != ESP_OK) { nvs_close(h); return err; }

    err = nvs_get_str(h, NVS_KEY_PASS, pass, &pass_sz);
    if (err != ESP_OK) { nvs_close(h); return err; }

    nvs_close(h);
    return ESP_OK;
}

static esp_err_t nvs_save_wifi_creds(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_str(h, NVS_KEY_SSID, ssid);
    if (err != ESP_OK) { nvs_close(h); return err; }

    err = nvs_set_str(h, NVS_KEY_PASS, pass);
    if (err != ESP_OK) { nvs_close(h); return err; }

    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* ---------- URL decode ---------- */

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *dst, const char *src, size_t dst_sz)
{
    size_t di = 0;
    for (size_t si = 0; src[si] && di < dst_sz - 1; si++) {
        if (src[si] == '%' && src[si + 1] && src[si + 2]) {
            int hi = hex_val(src[si + 1]);
            int lo = hex_val(src[si + 2]);
            if (hi >= 0 && lo >= 0) {
                dst[di++] = (char)(hi << 4 | lo);
                si += 2;
                continue;
            }
        }
        if (src[si] == '+') {
            dst[di++] = ' ';
        } else {
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
}

/* ---------- STA connection ---------- */

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num;

static void sta_event_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_retry_num++;
        if (s_retry_num <= WIFI_MAX_RETRY) {
            ESP_LOGI(TAG, "Retry %d/%d", s_retry_num, WIFI_MAX_RETRY);
            esp_wifi_connect();
        }
        if (s_retry_num == WIFI_MAX_RETRY) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_try_sta_connect(const char *ssid, const char *pass)
{
    s_retry_num = 0;
    s_wifi_event_group = xEventGroupCreate();

    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t h_any, h_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &sta_event_handler, NULL, &h_any));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &sta_event_handler, NULL, &h_ip));

    wifi_config_t wcfg = { 0 };
    strncpy((char *)wcfg.sta.ssid, ssid, sizeof(wcfg.sta.ssid) - 1);
    strncpy((char *)wcfg.sta.password, pass, sizeof(wcfg.sta.password) - 1);
    wcfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to \"%s\"...", ssid);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);

    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, h_any);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, h_ip);
    vEventGroupDelete(s_wifi_event_group);
    s_wifi_event_group = NULL;

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to \"%s\"", ssid);
        return true;
    }

    ESP_LOGW(TAG, "Failed to connect to \"%s\"", ssid);
    /* クリーンアップ — APモードで再初期化するため */
    esp_wifi_stop();
    esp_wifi_deinit();
    esp_netif_destroy(sta_netif);
    return false;
}

/* ---------- SoftAP provisioning ---------- */

static const char PROV_HTML[] =
    "<!DOCTYPE html><html><head>"
    "<meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>EPD-Setup</title>"
    "<style>"
    "body{font-family:sans-serif;max-width:360px;margin:40px auto;padding:0 16px}"
    "h2{text-align:center}"
    "label{display:block;margin:12px 0 4px}"
    "input{width:100%;padding:8px;box-sizing:border-box}"
    "button{margin-top:16px;width:100%;padding:10px;background:#0078d4;color:#fff;"
    "border:none;border-radius:4px;font-size:16px;cursor:pointer}"
    "</style></head><body>"
    "<h2>WiFi Setup</h2>"
    "<form method=\"POST\" action=\"/save\">"
    "<label>SSID</label><input name=\"ssid\" required maxlength=\"32\">"
    "<label>Password</label><input name=\"pass\" type=\"password\" maxlength=\"64\">"
    "<button type=\"submit\">Save &amp; Restart</button>"
    "</form></body></html>";

static esp_err_t prov_root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PROV_HTML, sizeof(PROV_HTML) - 1);
    return ESP_OK;
}

static esp_err_t prov_save_handler(httpd_req_t *req)
{
    char body[256];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[len] = '\0';

    /* Parse "ssid=...&pass=..." */
    char raw_ssid[128] = {0}, raw_pass[128] = {0};
    char ssid[33] = {0}, pass[65] = {0};

    char *sp = strstr(body, "ssid=");
    char *pp = strstr(body, "pass=");

    if (sp) {
        sp += 5;
        char *end = strchr(sp, '&');
        size_t n = end ? (size_t)(end - sp) : strlen(sp);
        if (n >= sizeof(raw_ssid)) n = sizeof(raw_ssid) - 1;
        memcpy(raw_ssid, sp, n);
        raw_ssid[n] = '\0';
    }
    if (pp) {
        pp += 5;
        char *end = strchr(pp, '&');
        size_t n = end ? (size_t)(end - pp) : strlen(pp);
        if (n >= sizeof(raw_pass)) n = sizeof(raw_pass) - 1;
        memcpy(raw_pass, pp, n);
        raw_pass[n] = '\0';
    }

    url_decode(ssid, raw_ssid, sizeof(ssid));
    url_decode(pass, raw_pass, sizeof(pass));

    if (strlen(ssid) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Saving credentials for \"%s\"", ssid);

    esp_err_t err = nvs_save_wifi_creds(ssid, pass);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS save failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "NVS write failed — please try again");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<!DOCTYPE html><html><body style=\"font-family:sans-serif;"
        "text-align:center;margin-top:60px\">"
        "<h2>Saved! Restarting...</h2></body></html>");

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    /* NOTREACHED */
    return ESP_OK;
}

/* キャプティブポータル: 未知のパスにも設定ページを直接返す */
static esp_err_t prov_catchall_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PROV_HTML, sizeof(PROV_HTML) - 1);
    return ESP_OK;
}

static void wifi_start_provisioning(void)
{
    ESP_LOGI(TAG, "Starting SoftAP provisioning (SSID: %s)", SOFTAP_SSID);

    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = SOFTAP_SSID,
            .ssid_len = sizeof(SOFTAP_SSID) - 1,
            .channel = SOFTAP_CHANNEL,
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = SOFTAP_MAX_CONN,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* キャプティブポータル用DNSサーバー起動（全ドメインを192.168.4.1に解決） */
    dns_server_start();

    /* Provisioning HTTP server */
    httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
    hcfg.uri_match_fn = httpd_uri_match_wildcard;
    httpd_handle_t srv = NULL;
    ESP_ERROR_CHECK(httpd_start(&srv, &hcfg));

    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = prov_root_handler,
    };
    httpd_register_uri_handler(srv, &root);

    const httpd_uri_t save = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = prov_save_handler,
    };
    httpd_register_uri_handler(srv, &save);

    /* キャプティブポータル: 他の全てのGETリクエストにも設定ページを返す */
    const httpd_uri_t catchall = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = prov_catchall_handler,
    };
    httpd_register_uri_handler(srv, &catchall);

    ESP_LOGI(TAG, "Provisioning ready — connect to \"%s\" and open http://192.168.4.1",
             SOFTAP_SSID);

    /* ブロック: prov_save_handler が esp_restart() を呼ぶまで待機 */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ---------- Public API ---------- */

esp_err_t wifi_init_sta(void)
{
    /* NVS初期化 */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* NVSから認証情報読み込み */
    char ssid[33] = {0};
    char pass[65] = {0};
    ret = nvs_load_wifi_creds(ssid, sizeof(ssid), pass, sizeof(pass));

    if (ret == ESP_OK && strlen(ssid) > 0) {
        ESP_LOGI(TAG, "NVS credentials found for \"%s\"", ssid);
        if (wifi_try_sta_connect(ssid, pass)) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "STA connection failed, entering provisioning mode");
    } else {
        ESP_LOGI(TAG, "No WiFi credentials in NVS");
    }

    /* STA失敗 or 認証情報なし → プロビジョニング（戻らない） */
    wifi_start_provisioning();
    /* NOTREACHED */
    return ESP_FAIL;
}

esp_err_t wifi_clear_credentials(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    nvs_erase_key(h, NVS_KEY_SSID);
    nvs_erase_key(h, NVS_KEY_PASS);
    err = nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "WiFi credentials cleared");
    return err;
}
