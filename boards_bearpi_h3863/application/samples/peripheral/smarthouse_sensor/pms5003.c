/** @file pms5003.c — uapi UART 中断接收 + 帧解析 */
#include "pms5003.h"
#include "uart.h"
#include "pinctrl.h"
#include <string.h>

#define PMS_RING 128
static uint8_t  pms_buf[PMS_RING];
static volatile uint16_t pms_head, pms_tail;
static pms5003_frame_t g_frame;

static uint16_t swap16(uint16_t v) { return (v >> 8) | (v << 8); }

void pms5003_uart_rx_callback(const void *data, uint16_t len, bool error)
{
    if (error) return;
    const uint8_t *b = (const uint8_t *)data;
    for (uint16_t i = 0; i < len; i++) {
        uint16_t n = (pms_head + 1) % PMS_RING;
        if (n == pms_tail) { pms_tail = (pms_tail + 1) % PMS_RING; }
        pms_buf[pms_head] = b[i];
        pms_head = n;
    }
}

int pms5003_init(void)
{
    pms_head = pms_tail = 0;
    memset(&g_frame, 0, sizeof(g_frame));

    uapi_pin_set_mode(CONFIG_SMARTHOUSE_SENSOR_UART_TXD_PIN, PIN_MODE_1);
    uapi_pin_set_mode(CONFIG_SMARTHOUSE_SENSOR_UART_RXD_PIN, PIN_MODE_1);

    uart_pin_config_t pin = { .tx_pin = CONFIG_SMARTHOUSE_SENSOR_UART_TXD_PIN,
                              .rx_pin = CONFIG_SMARTHOUSE_SENSOR_UART_RXD_PIN,
                              .cts_pin = PIN_NONE, .rts_pin = PIN_NONE };
    uart_attr_t attr = { .baud_rate = 9600, .data_bits = UART_DATA_BIT_8,
                         .stop_bits = UART_STOP_BIT_1, .parity = UART_PARITY_NONE };
    uart_buffer_config_t buf_cfg = { .rx_buffer = NULL, .rx_buffer_size = 512 };

    if (uapi_uart_init(CONFIG_SMARTHOUSE_SENSOR_UART_BUS, &pin, &attr, NULL, &buf_cfg) != 0) return -1;
    return uapi_uart_register_rx_callback(CONFIG_SMARTHOUSE_SENSOR_UART_BUS,
            UART_RX_CONDITION_FULL_OR_IDLE, 1, pms5003_uart_rx_callback);
}

int pms5003_read(uint16_t *pm1_0, uint16_t *pm2_5, uint16_t *pm10)
{
    while (pms_tail != pms_head) {
        if (pms_buf[pms_tail] != 0x42) { pms_tail = (pms_tail + 1) % PMS_RING; continue; }
        uint16_t n = (pms_tail + 1) % PMS_RING;
        if (n == pms_head || pms_buf[n] != 0x4D) { pms_tail = (pms_tail + 1) % PMS_RING; continue; }

        uint8_t raw[32];
        for (uint8_t i = 0; i < 32; i++) {
            uint16_t pos = (pms_tail + i) % PMS_RING;
            if (pos == pms_head) return -1;
            raw[i] = pms_buf[pos];
        }
        uint16_t sum = 0;
        for (uint8_t i = 0; i < 30; i++) sum += raw[i];
        if (sum != ((uint16_t)raw[30] << 8 | raw[31])) { pms_tail = (pms_tail + 1) % PMS_RING; continue; }

        memcpy(&g_frame, raw, 32);
        if (pm1_0) *pm1_0 = swap16(g_frame.pm1_0_atm);
        if (pm2_5) *pm2_5 = swap16(g_frame.pm2_5_atm);
        if (pm10)  *pm10  = swap16(g_frame.pm10_atm);
        pms_tail = (pms_tail + 32) % PMS_RING;
        return 0;
    }
    return -1;
}
