/**
 * @file    sht30.c
 * @brief   SHT30 驱动 — uapi_i2c_master_write/read
 */
#include "sht30.h"
#include "i2c.h"

static uint8_t sht30_crc8(const uint8_t *data, uint8_t len)
{
    uint8_t crc = 0xFF;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
        }
    }
    return crc;
}

int sht30_init(void) { return 0; }

int sht30_read(float *temp, float *hum)
{
    uint8_t cmd[2] = { 0x2C, 0x06 };
    uint8_t buf[6] = { 0 };
    i2c_data_t data;

    data.send_buf = cmd;
    data.send_len = 2;
    if (uapi_i2c_master_write(CONFIG_SMARTHOUSE_SENSOR_I2C_BUS, SHT30_I2C_ADDR, &data) != 0)
        return -1;

    for (volatile int i = 0; i < 20000; i++) { __asm__ volatile ("nop"); }

    data.send_buf = buf;
    data.send_len = 6;
    if (uapi_i2c_master_read(CONFIG_SMARTHOUSE_SENSOR_I2C_BUS, SHT30_I2C_ADDR, &data) != 0)
        return -2;

    if (sht30_crc8(&buf[0], 2) != buf[2]) return -3;
    if (sht30_crc8(&buf[3], 2) != buf[5]) return -3;

    uint16_t raw_t = ((uint16_t)buf[0] << 8) | buf[1];
    uint16_t raw_h = ((uint16_t)buf[3] << 8) | buf[4];
    *temp = -45.0f + 175.0f * ((float)raw_t / 65535.0f);
    *hum  = 100.0f * ((float)raw_h / 65535.0f);
    if (*hum > 100.0f) *hum = 100.0f;
    return 0;
}
