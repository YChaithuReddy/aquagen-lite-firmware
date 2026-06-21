#include "modbus_meter.h"
#include "iot_configs.h"
#include <string.h>
#include <stdio.h>
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "modbus";
static uint32_t s_baud = 0;
static char s_parity[6] = "";

static modbus_stats_t s_stats = { 0 };
const modbus_stats_t *modbus_meter_stats(void) { return &s_stats; }

static uart_parity_t parity_from_str(const char *p)
{
    if (strcmp(p, "even") == 0) return UART_PARITY_EVEN;
    if (strcmp(p, "odd") == 0)  return UART_PARITY_ODD;
    return UART_PARITY_DISABLE;
}

void modbus_meter_init(uint32_t baud, const char *parity)
{
    // Skip re-init if line settings unchanged (avoids churn on every read).
    if (baud == s_baud && strcmp(parity, s_parity) == 0) return;

    if (uart_is_driver_installed(MODBUS_UART_NUM)) {
        uart_driver_delete(MODBUS_UART_NUM);
    }

    uart_config_t uc = {
        .baud_rate = baud,
        .data_bits = UART_DATA_8_BITS,
        .parity    = parity_from_str(parity),
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(MODBUS_UART_NUM, 256, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(MODBUS_UART_NUM, &uc));
    ESP_ERROR_CHECK(uart_set_pin(MODBUS_UART_NUM, PIN_RS485_TX, PIN_RS485_RX,
                                 PIN_RS485_RTS, UART_PIN_NO_CHANGE));
    if (PIN_RS485_RTS >= 0) {
        uart_set_mode(MODBUS_UART_NUM, UART_MODE_RS485_HALF_DUPLEX);
    }
    s_baud = baud;
    strlcpy(s_parity, parity, sizeof(s_parity));
    ESP_LOGI(TAG, "UART2 @ %lu %s parity", (unsigned long)baud, parity);
}

void modbus_meter_reinit(void)
{
    if (s_baud == 0) return;   // never initialized → nothing to recover
    // Stash the live settings, then clear the cache so modbus_meter_init() does NOT skip and
    // actually tears down + reinstalls the driver (the whole point of a recovery reinit).
    uint32_t baud = s_baud;
    char parity[sizeof(s_parity)];
    strlcpy(parity, s_parity, sizeof(parity));
    s_baud = 0;
    s_parity[0] = '\0';
    modbus_meter_init(baud, parity);
    s_stats.recoveries++;
    ESP_LOGW(TAG, "RS485 driver re-initialized (self-heal recovery #%lu)",
             (unsigned long)s_stats.recoveries);
}

// Modbus RTU CRC16 (poly 0xA001).
static uint16_t crc16(const uint8_t *buf, int len)
{
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 1) crc = (crc >> 1) ^ 0xA001;
            else         crc >>= 1;
        }
    }
    return crc;
}

// One read attempt: function 0x03 (read holding registers). Returns true + fills regs[qty] on success.
static bool read_holding(uint8_t slave, uint16_t addr, uint16_t qty, uint16_t *regs)
{
    uint8_t req[8];
    req[0] = slave;
    req[1] = 0x03;
    req[2] = addr >> 8;   req[3] = addr & 0xFF;
    req[4] = qty >> 8;    req[5] = qty & 0xFF;
    uint16_t c = crc16(req, 6);
    req[6] = c & 0xFF;    req[7] = c >> 8;   // CRC is little-endian on the wire

    uart_flush_input(MODBUS_UART_NUM);
    uart_write_bytes(MODBUS_UART_NUM, (const char *)req, sizeof(req));
    uart_wait_tx_done(MODBUS_UART_NUM, pdMS_TO_TICKS(100));   // half-duplex: finish TX before RX
    ESP_LOGI(TAG, "TX: %02X %02X %02X %02X %02X %02X %02X %02X",
             req[0], req[1], req[2], req[3], req[4], req[5], req[6], req[7]);

    // Expected response: slave, func, bytecount(=2*qty), data..., crc_lo, crc_hi
    int expected = 5 + 2 * qty;
    uint8_t resp[64];
    if (expected > (int)sizeof(resp)) return false;
    int got = uart_read_bytes(MODBUS_UART_NUM, resp, expected, pdMS_TO_TICKS(500));
    // Verbose RX dump so the raw meter reply is visible (like the gateway logs).
    if (got > 0) {
        char hex[3 * 32 + 1]; int n = 0;
        for (int i = 0; i < got && i < 32; i++) n += snprintf(hex + n, sizeof(hex) - n, "%02X ", resp[i]);
        ESP_LOGI(TAG, "RX (%d bytes): %s", got, hex);
    } else {
        ESP_LOGW(TAG, "RX: 0 bytes (no reply from slave %u)", slave);
    }
    // Modbus exception reply: func | 0x80, then a 1-byte exception code (3-byte payload + CRC).
    if (got >= 3 && resp[0] == slave && (resp[1] & 0x80)) {
        s_stats.last_exception = resp[2];
        ESP_LOGW(TAG, "Modbus exception 0x%02X (func 0x%02X)", resp[2], resp[1] & 0x7F);
        return false;   // distinguishes "wrong register/func" from "no wire"
    }
    if (got < expected) {
        ESP_LOGW(TAG, "short read: got %d / %d", got, expected);
        s_stats.timeouts++;
        return false;
    }
    if (resp[0] != slave || resp[1] != 0x03 || resp[2] != 2 * qty) {
        ESP_LOGW(TAG, "bad header %02X %02X %02X", resp[0], resp[1], resp[2]);
        return false;
    }
    uint16_t rx_crc = resp[expected - 2] | (resp[expected - 1] << 8);
    if (rx_crc != crc16(resp, expected - 2)) {
        ESP_LOGW(TAG, "CRC mismatch");
        s_stats.crc_errors++;
        return false;
    }
    for (int i = 0; i < qty; i++) {
        regs[i] = (resp[3 + 2 * i] << 8) | resp[4 + 2 * i];
    }
    return true;
}

meter_reading_t modbus_meter_read(const meter_cfg_t *meter, uint8_t retry_count, uint16_t retry_delay_ms)
{
    meter_reading_t r = { 0 };
    modbus_meter_init(meter->baud, meter->parity);
    s_stats.total++;

    uint16_t regs[MODBUS_FLOW_QTY];
    int attempts = retry_count + 1;
    for (int a = 0; a < attempts; a++) {
        if (read_holding(meter->slave_id, MODBUS_FLOW_REG, MODBUS_FLOW_QTY, regs)) {
            // CDAB (mid-little-endian) -> int32, matching the legacy parse.
            uint16_t reg0 = regs[0];  // CD
            uint16_t reg1 = regs[1];  // AB
            uint8_t C = (reg0 >> 8) & 0xFF, D = reg0 & 0xFF;
            uint8_t A = (reg1 >> 8) & 0xFF, B = reg1 & 0xFF;
            uint32_t raw = ((uint32_t)A << 24) | ((uint32_t)B << 16) | ((uint32_t)C << 8) | D;
            int32_t v;
            memcpy(&v, &raw, sizeof(v));
            r.consumption = (double)v * MODBUS_SCALE;
            r.ok = true;
            s_stats.ok++;
            ESP_LOGI(TAG, "slave %u -> %.2f", meter->slave_id, r.consumption);
            // TODO(datasheet): read alarm/status register and set r.alarm_* flags.
            return r;
        }
        if (a < attempts - 1) vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));
    }
    s_stats.failed++;
    ESP_LOGE(TAG, "slave %u read failed after %d attempts", meter->slave_id, attempts);
    return r;
}
