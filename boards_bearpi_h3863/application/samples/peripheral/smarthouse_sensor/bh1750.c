/** @file bh1750.c — uapi I2C */
#include "bh1750.h"
#include "i2c.h"

int bh1750_init(void)
{
    uint8_t cmd[1] = { 0x10 };
    i2c_data_t data = { .send_buf = cmd, .send_len = 1 };
    return uapi_i2c_master_write(CONFIG_SMARTHOUSE_SENSOR_I2C_BUS, BH1750_I2C_ADDR, &data);
}

int bh1750_read(uint16_t *lux)
{
    uint8_t buf[2] = { 0 };
    for (volatile int i = 0; i < 60000; i++) { __asm__ volatile ("nop"); }
    i2c_data_t data = { .send_buf = buf, .send_len = 2 };
    if (uapi_i2c_master_read(CONFIG_SMARTHOUSE_SENSOR_I2C_BUS, BH1750_I2C_ADDR, &data) != 0)
        return -1;
    uint16_t raw = ((uint16_t)buf[0] << 8) | buf[1];
    *lux = (uint16_t)((float)raw / 1.2f);
    return 0;
}
