// main/epd_driver.c
// GDEP133C02 (13.3inch 1200x1600) EPD Driver for ESP32-S3
// Based on official sample code

#include "epd_driver.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#define TAG "EPD"

// GPIO Pin Definitions
#define PIN_SPI_CLK     9
#define PIN_SPI_DATA0   41  // MOSI
#define PIN_SPI_DATA1   40  // MISO
#define PIN_CS0         18
#define PIN_CS1         17
#define PIN_RESET       6
#define PIN_BUSY        7
#define PIN_LOAD_SW     45

// SPI Settings
#define EPD_SPI_HOST    SPI3_HOST
#define SPI_MAX_BUFFER_SIZE  32768

// EPD Commands
#define CMD_PSR         0x00
#define CMD_PWR         0x01
#define CMD_POF         0x02
#define CMD_PON         0x04
#define CMD_BTST_N      0x05
#define CMD_BTST_P      0x06
#define CMD_DTM         0x10
#define CMD_DRF         0x12
#define CMD_CDI         0x50
#define CMD_TCON        0x60
#define CMD_TRES        0x61
#define CMD_AN_TM       0x74
#define CMD_AGID        0x86
#define CMD_BUCK_BOOST_VDDN 0xB0
#define CMD_TFT_VCOM_POWER  0xB1
#define CMD_EN_BUF      0xB6
#define CMD_BOOST_VDDP_EN   0xB7
#define CMD_CCSET       0xE0
#define CMD_PWS         0xE3
#define CMD_CMD66       0xF0

// Initialization Data (from official sample)
static const uint8_t PSR_V[2]    = {0xDF, 0x69};
static const uint8_t PWR_V[6]    = {0x0F, 0x00, 0x28, 0x2C, 0x28, 0x38};
static const uint8_t POF_V[1]    = {0x00};
static const uint8_t DRF_V[1]    = {0x01};
static const uint8_t CDI_V[1]    = {0xF7};
static const uint8_t TCON_V[2]   = {0x03, 0x03};
static const uint8_t TRES_V[4]   = {0x04, 0xB0, 0x03, 0x20};  // 1200x800 per driver IC
static const uint8_t CMD66_V[6]  = {0x49, 0x55, 0x13, 0x5D, 0x05, 0x10};
static const uint8_t EN_BUF_V[1] = {0x07};
static const uint8_t CCSET_V[1]  = {0x01};
static const uint8_t PWS_V[1]    = {0x22};
static const uint8_t AN_TM_V[9]  = {0xC0, 0x1C, 0x1C, 0xCC, 0xCC, 0xCC, 0x15, 0x15, 0x55};
static const uint8_t AGID_V[1]   = {0x10};
static const uint8_t BTST_P_V[2] = {0xE8, 0x28};
static const uint8_t BOOST_VDDP_EN_V[1] = {0x01};
static const uint8_t BTST_N_V[2] = {0xE8, 0x28};
static const uint8_t BUCK_BOOST_VDDN_V[1] = {0x01};
static const uint8_t TFT_VCOM_POWER_V[1] = {0x02};

static const uint8_t spi_cs_pins[2] = {PIN_CS0, PIN_CS1};

static spi_device_handle_t spi_handle = NULL;
static bool epd_initialized = false;

//--------------------------------------------------------------------
// GPIO Functions
//--------------------------------------------------------------------

static void set_pin_cs(uint8_t cs_num, int level)
{
    gpio_set_level(spi_cs_pins[cs_num], level);
}

static void set_pin_cs_all(int level)
{
    gpio_set_level(PIN_CS0, level);
    gpio_set_level(PIN_CS1, level);
}

static void reset_pin(int level)
{
    gpio_set_level(PIN_RESET, level);
}

static int busy_read(void)
{
    return gpio_get_level(PIN_BUSY);
}

static void check_busy_high(void)
{
    // Wait for BUSY to become HIGH
    uint32_t waited = 0;
    while (!busy_read()) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
        if (waited > 120000) {
            ESP_LOGW(TAG, "BUSY wait timeout");
            break;
        }
    }
}

static void delayms(uint32_t ms)
{
    vTaskDelay(ms / portTICK_PERIOD_MS);
}

static esp_err_t gpio_init(void)
{
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << PIN_CS0) | (1ULL << PIN_CS1) |
                        (1ULL << PIN_RESET) | (1ULL << PIN_LOAD_SW),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&out_cfg);
    if (err != ESP_OK) return err;

    gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << PIN_BUSY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    err = gpio_config(&in_cfg);
    if (err != ESP_OK) return err;

    // Initial states
    set_pin_cs_all(1);
    reset_pin(1);
    gpio_set_level(PIN_LOAD_SW, 0);

    return ESP_OK;
}

//--------------------------------------------------------------------
// SPI Functions
//--------------------------------------------------------------------

static esp_err_t spi_init(void)
{
    spi_bus_config_t bus_cfg = {
        .data0_io_num = PIN_SPI_DATA0,
        .data1_io_num = PIN_SPI_DATA1,
        .sclk_io_num = PIN_SPI_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .flags = SPICOMMON_BUSFLAG_MASTER,
        .max_transfer_sz = SPI_MAX_BUFFER_SIZE,
    };
    esp_err_t err = spi_bus_initialize(EPD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) return err;

    spi_device_interface_config_t dev_cfg = {
        .command_bits = 8,
        .clock_speed_hz = SPI_MASTER_FREQ_10M,
        .duty_cycle_pos = 128,
        .queue_size = 7,
        .cs_ena_posttrans = 3,
        .spics_io_num = -1,  // Manual CS control
    };
    err = spi_bus_add_device(EPD_SPI_HOST, &dev_cfg, &spi_handle);
    return err;
}

static void spi_deinit(void)
{
    if (spi_handle) {
        spi_bus_remove_device(spi_handle);
        spi_handle = NULL;
    }
    spi_bus_free(EPD_SPI_HOST);
}

//--------------------------------------------------------------------
// EPD SPI Communication Functions
//--------------------------------------------------------------------

static esp_err_t spi_transmit_command(uint8_t cmd)
{
    spi_transaction_t trans = {0};
    trans.cmd = cmd;
    trans.length = 0;
    trans.tx_buffer = NULL;
    return spi_device_transmit(spi_handle, &trans);
}

static esp_err_t spi_transmit_data(const uint8_t *data, size_t len)
{
    esp_err_t err = ESP_OK;
    spi_transaction_ext_t trans_ext;

    while (len >= SPI_MAX_BUFFER_SIZE) {
        memset(&trans_ext, 0, sizeof(trans_ext));
        trans_ext.command_bits = 0;
        trans_ext.base.length = SPI_MAX_BUFFER_SIZE * 8;
        trans_ext.base.tx_buffer = data;
        trans_ext.base.flags = SPI_TRANS_VARIABLE_CMD;
        err = spi_device_transmit(spi_handle, (spi_transaction_t *)&trans_ext);
        if (err != ESP_OK) return err;
        len -= SPI_MAX_BUFFER_SIZE;
        data += SPI_MAX_BUFFER_SIZE;
    }

    if (len > 0) {
        memset(&trans_ext, 0, sizeof(trans_ext));
        trans_ext.command_bits = 0;
        trans_ext.base.length = len * 8;
        trans_ext.base.tx_buffer = data;
        trans_ext.base.flags = SPI_TRANS_VARIABLE_CMD;
        err = spi_device_transmit(spi_handle, (spi_transaction_t *)&trans_ext);
    }

    return err;
}

static esp_err_t spi_transmit(uint8_t cmd, const uint8_t *data, size_t len)
{
    spi_transaction_t trans = {0};
    trans.cmd = cmd;
    trans.length = len * 8;
    trans.tx_buffer = data;
    trans.rx_buffer = NULL;
    return spi_device_transmit(spi_handle, &trans);
}

static esp_err_t spi_transmit_large_data(uint8_t cmd, const uint8_t *data, size_t len)
{
    esp_err_t err = ESP_OK;
    spi_transaction_t trans;
    spi_transaction_ext_t trans_ext;
    bool first_packet = true;

    while (len > SPI_MAX_BUFFER_SIZE) {
        if (first_packet) {
            memset(&trans, 0, sizeof(trans));
            trans.cmd = cmd;
            trans.length = SPI_MAX_BUFFER_SIZE * 8;
            trans.tx_buffer = data;
            trans.rx_buffer = NULL;
            err = spi_device_transmit(spi_handle, &trans);
            first_packet = false;
        } else {
            memset(&trans_ext, 0, sizeof(trans_ext));
            trans_ext.command_bits = 0;
            trans_ext.base.length = SPI_MAX_BUFFER_SIZE * 8;
            trans_ext.base.tx_buffer = data;
            trans_ext.base.flags = SPI_TRANS_VARIABLE_CMD;
            err = spi_device_transmit(spi_handle, (spi_transaction_t *)&trans_ext);
        }
        if (err != ESP_OK) return err;
        len -= SPI_MAX_BUFFER_SIZE;
        data += SPI_MAX_BUFFER_SIZE;
    }

    if (len > 0) {
        if (first_packet) {
            memset(&trans, 0, sizeof(trans));
            trans.cmd = cmd;
            trans.length = len * 8;
            trans.tx_buffer = data;
            trans.rx_buffer = NULL;
            err = spi_device_transmit(spi_handle, &trans);
        } else {
            memset(&trans_ext, 0, sizeof(trans_ext));
            trans_ext.command_bits = 0;
            trans_ext.base.length = len * 8;
            trans_ext.base.tx_buffer = data;
            trans_ext.base.flags = SPI_TRANS_VARIABLE_CMD;
            err = spi_device_transmit(spi_handle, (spi_transaction_t *)&trans_ext);
        }
    }

    return err;
}

//--------------------------------------------------------------------
// EPD Command Functions
//--------------------------------------------------------------------

static void write_epd(uint8_t cmd, const uint8_t *data, size_t len)
{
    spi_transmit(cmd, data, len);
}

static void write_epd_command(uint8_t cmd)
{
    spi_transmit_command(cmd);
}

static void write_epd_data(const uint8_t *data, size_t len)
{
    spi_transmit_data(data, len);
}

static void epd_hardware_reset(void)
{
    reset_pin(0);
    delayms(20);
    reset_pin(1);
    delayms(20);
}

//--------------------------------------------------------------------
// EPD Public Functions
//--------------------------------------------------------------------

esp_err_t epd_init(void)
{
    if (epd_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing EPD driver...");

    // 1. GPIO init
    esp_err_t err = gpio_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO init failed: %d", err);
        return err;
    }

    // 2. Load Switch ON + wait
    gpio_set_level(PIN_LOAD_SW, 1);
    delayms(100);
    ESP_LOGI(TAG, "Load switch ON");

    // 3. SPI init
    err = spi_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI init failed: %d", err);
        return err;
    }
    ESP_LOGI(TAG, "SPI initialized");

    // 4. Hardware reset
    epd_hardware_reset();
    ESP_LOGI(TAG, "Hardware reset done");

    // 5. Wait for BUSY high
    check_busy_high();
    ESP_LOGI(TAG, "BUSY released");

    // 6. Send initialization commands (matching official sample)
    ESP_LOGI(TAG, "Sending init commands...");

    // AN_TM - CS0 only
    set_pin_cs(0, 0);
    write_epd(CMD_AN_TM, AN_TM_V, sizeof(AN_TM_V));
    set_pin_cs_all(1);

    // CMD66 - All CS
    set_pin_cs_all(0);
    write_epd(CMD_CMD66, CMD66_V, sizeof(CMD66_V));
    set_pin_cs_all(1);

    // PSR - All CS
    set_pin_cs_all(0);
    write_epd(CMD_PSR, PSR_V, sizeof(PSR_V));
    set_pin_cs_all(1);

    // CDI - All CS
    set_pin_cs_all(0);
    write_epd(CMD_CDI, CDI_V, sizeof(CDI_V));
    set_pin_cs_all(1);

    // TCON - All CS
    set_pin_cs_all(0);
    write_epd(CMD_TCON, TCON_V, sizeof(TCON_V));
    set_pin_cs_all(1);

    // AGID - All CS
    set_pin_cs_all(0);
    write_epd(CMD_AGID, AGID_V, sizeof(AGID_V));
    set_pin_cs_all(1);

    // PWS - All CS
    set_pin_cs_all(0);
    write_epd(CMD_PWS, PWS_V, sizeof(PWS_V));
    set_pin_cs_all(1);

    // CCSET - All CS
    set_pin_cs_all(0);
    write_epd(CMD_CCSET, CCSET_V, sizeof(CCSET_V));
    set_pin_cs_all(1);

    // TRES - All CS
    set_pin_cs_all(0);
    write_epd(CMD_TRES, TRES_V, sizeof(TRES_V));
    set_pin_cs_all(1);

    // PWR - CS0 only
    set_pin_cs(0, 0);
    write_epd(CMD_PWR, PWR_V, sizeof(PWR_V));
    set_pin_cs_all(1);

    // EN_BUF - CS0 only
    set_pin_cs(0, 0);
    write_epd(CMD_EN_BUF, EN_BUF_V, sizeof(EN_BUF_V));
    set_pin_cs_all(1);

    // BTST_P - CS0 only
    set_pin_cs(0, 0);
    write_epd(CMD_BTST_P, BTST_P_V, sizeof(BTST_P_V));
    set_pin_cs_all(1);

    // BOOST_VDDP_EN - CS0 only
    set_pin_cs(0, 0);
    write_epd(CMD_BOOST_VDDP_EN, BOOST_VDDP_EN_V, sizeof(BOOST_VDDP_EN_V));
    set_pin_cs_all(1);

    // BTST_N - CS0 only
    set_pin_cs(0, 0);
    write_epd(CMD_BTST_N, BTST_N_V, sizeof(BTST_N_V));
    set_pin_cs_all(1);

    // BUCK_BOOST_VDDN - CS0 only
    set_pin_cs(0, 0);
    write_epd(CMD_BUCK_BOOST_VDDN, BUCK_BOOST_VDDN_V, sizeof(BUCK_BOOST_VDDN_V));
    set_pin_cs_all(1);

    // TFT_VCOM_POWER - CS0 only
    set_pin_cs(0, 0);
    write_epd(CMD_TFT_VCOM_POWER, TFT_VCOM_POWER_V, sizeof(TFT_VCOM_POWER_V));
    set_pin_cs_all(1);

    epd_initialized = true;
    ESP_LOGI(TAG, "EPD initialized successfully");
    return ESP_OK;
}

void epd_deinit(void)
{
    if (!epd_initialized) return;

    spi_deinit();
    gpio_set_level(PIN_LOAD_SW, 0);
    epd_initialized = false;
    ESP_LOGI(TAG, "EPD deinitialized");
}

esp_err_t epd_write_image(const uint8_t *data, size_t len)
{
    if (!epd_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Writing image data (%u bytes)...", (unsigned)len);

    // Image data format: 1200x1600, 4bpp = 960000 bytes
    // Split into two driver ICs: each gets 1200x800 = 480000 bytes
    // Data layout: each row has 1200 pixels = 600 bytes
    // First half of each row goes to CS0, second half to CS1

    const uint32_t width = 1200;
    const uint32_t height = 1600;
    const uint32_t bytes_per_row = width / 2;  // 600 bytes (4bpp)
    const uint32_t half_width_bytes = bytes_per_row / 2;  // 300 bytes per driver IC

    // Write to CS0 (first half of each row)
    set_pin_cs_all(1);
    set_pin_cs(0, 0);
    write_epd_command(CMD_DTM);
    for (uint32_t row = 0; row < height; row++) {
        write_epd_data(data + row * bytes_per_row, half_width_bytes);
    }
    set_pin_cs_all(1);

    // Write to CS1 (second half of each row)
    set_pin_cs(1, 0);
    write_epd_command(CMD_DTM);
    for (uint32_t row = 0; row < height; row++) {
        write_epd_data(data + row * bytes_per_row + half_width_bytes, half_width_bytes);
    }
    set_pin_cs_all(1);

    ESP_LOGI(TAG, "Image data written");
    return ESP_OK;
}

esp_err_t epd_refresh(void)
{
    if (!epd_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Refreshing display...");

    // PON (Power ON)
    set_pin_cs_all(0);
    write_epd_command(CMD_PON);
    check_busy_high();
    set_pin_cs_all(1);

    // Wait 30ms
    delayms(30);

    // DRF (Display Refresh) with data
    set_pin_cs_all(0);
    write_epd(CMD_DRF, DRF_V, sizeof(DRF_V));
    ESP_LOGI(TAG, "Waiting for refresh (this may take a while)...");
    check_busy_high();
    set_pin_cs_all(1);

    // POF (Power OFF) with data
    set_pin_cs_all(0);
    write_epd(CMD_POF, POF_V, sizeof(POF_V));
    check_busy_high();
    set_pin_cs_all(1);

    ESP_LOGI(TAG, "Display refresh complete");
    return ESP_OK;
}
