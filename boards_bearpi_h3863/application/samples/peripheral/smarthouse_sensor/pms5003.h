/**
 * @file    pms5003.h
 * @brief   攀藤 PMS5003 PM2.5 传感器驱动 (UART, 9600 8N1)
 *          主动上报模式 — 每秒 32 字节数据帧
 */
#ifndef PMS5003_H
#define PMS5003_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PMS5003 一帧的原始数据结构（32 字节） */
#pragma pack(1)
typedef struct {
    uint16_t start1;       /* 0x42 */
    uint16_t start2;       /* 0x4D */
    uint16_t frame_len;
    uint16_t pm1_0_cf1;
    uint16_t pm2_5_cf1;
    uint16_t pm10_cf1;
    uint16_t pm1_0_atm;
    uint16_t pm2_5_atm;
    uint16_t pm10_atm;
    uint16_t cnt_0_3um;
    uint16_t cnt_0_5um;
    uint16_t cnt_1_0um;
    uint16_t cnt_2_5um;
    uint16_t cnt_5_0um;
    uint16_t cnt_10_0um;
    uint16_t reserved;
    uint16_t checksum;
} pms5003_frame_t;
#pragma pack()

int   pms5003_init(void);
int   pms5003_read(uint16_t *pm1_0, uint16_t *pm2_5, uint16_t *pm10);

/* UART 批量接收回调 — 由 uapi_uart_register_rx_callback 注册 */
void  pms5003_uart_rx_callback(const void *data, uint16_t len, bool error);

#ifdef __cplusplus
}
#endif
#endif
