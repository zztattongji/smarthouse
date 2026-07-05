/** @file sgp30.c — uapi I2C + CRC8 */
#include "sgp30.h"
#include "i2c.h"

static uint8_t crc8(const uint8_t *data, uint8_t len)
{
    uint8_t crc = 0xFF;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++)
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
    }
    return crc;
}

static int sgp30_cmd(uint16_t cmd, uint8_t *resp, uint8_t resp_len)
{
    uint8_t cmd_buf[2] = { (cmd >> 8) & 0xFF, cmd & 0xFF };
    i2c_data_t data = { .send_buf = cmd_buf, .send_len = 2 };
    if (uapi_i2c_master_write(CONFIG_SMARTHOUSE_SENSOR_I2C_BUS, SGP30_I2C_ADDR, &data) != 0)
        return -1;
    for (volatile int i = 0; i < 15000; i++) { __asm__ volatile ("nop"); }
    if (resp == NULL || resp_len == 0) return 0;
    data.send_buf = resp; data.send_len = resp_len;
    return uapi_i2c_master_read(CONFIG_SMARTHOUSE_SENSOR_I2C_BUS, SGP30_I2C_ADDR, &data);
}

int sgp30_init(void)
{
    uint8_t resp[6];
    if (sgp30_cmd(0x2003, NULL, 0) != 0) return -1;
    for (volatile int i = 0; i < 20000; i++) { __asm__ volatile ("nop"); }
    return sgp30_cmd(0x2008, resp, 6);
}

int sgp30_read(uint16_t *voc, uint16_t *eco2)
{
    uint8_t resp[6];
    if (sgp30_cmd(0x2008, resp, 6) != 0) return -1;
    if (crc8(&resp[0], 2) != resp[2]) return -2;
    if (crc8(&resp[3], 2) != resp[5]) return -2;
    if (eco2) *eco2 = ((uint16_t)resp[0] << 8) | resp[1];
    if (voc)  *voc  = ((uint16_t)resp[3] << 8) | resp[4];
    return 0;
}
