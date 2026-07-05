/** @file bmp280.c — uapi I2C + Bosch 补偿算法 */
#include "bmp280.h"
#include "i2c.h"

static uint16_t dig_T1, dig_P1;
static int16_t  dig_T2, dig_T3, dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;

static int read_regs(uint8_t reg, uint8_t *buf, uint8_t len)
{
    i2c_data_t data;
    data.send_buf = &reg; data.send_len = 1;
    if (uapi_i2c_master_write(CONFIG_SMARTHOUSE_SENSOR_I2C_BUS, BMP280_I2C_ADDR, &data) != 0)
        return -1;
    data.send_buf = buf; data.send_len = len;
    return uapi_i2c_master_read(CONFIG_SMARTHOUSE_SENSOR_I2C_BUS, BMP280_I2C_ADDR, &data);
}

static int write_reg(uint8_t reg, uint8_t val)
{
    uint8_t d[2] = { reg, val };
    i2c_data_t data = { .send_buf = d, .send_len = 2 };
    return uapi_i2c_master_write(CONFIG_SMARTHOUSE_SENSOR_I2C_BUS, BMP280_I2C_ADDR, &data);
}

int bmp280_init(void)
{
    uint8_t calib[24];
    if (read_regs(0x88, calib, 24) != 0) return -1;
    dig_T1 = (uint16_t)(calib[0]  | (calib[1]  << 8));
    dig_T2 = (int16_t) (calib[2]  | (calib[3]  << 8));
    dig_T3 = (int16_t) (calib[4]  | (calib[5]  << 8));
    dig_P1 = (uint16_t)(calib[6]  | (calib[7]  << 8));
    dig_P2 = (int16_t) (calib[8]  | (calib[9]  << 8));
    dig_P3 = (int16_t) (calib[10] | (calib[11] << 8));
    dig_P4 = (int16_t) (calib[12] | (calib[13] << 8));
    dig_P5 = (int16_t) (calib[14] | (calib[15] << 8));
    dig_P6 = (int16_t) (calib[16] | (calib[17] << 8));
    dig_P7 = (int16_t) (calib[18] | (calib[19] << 8));
    dig_P8 = (int16_t) (calib[20] | (calib[21] << 8));
    dig_P9 = (int16_t) (calib[22] | (calib[23] << 8));
    write_reg(0xF4, 0x27);
    write_reg(0xF5, 0xA0);
    return 0;
}

static int32_t bmp280_compensate_T(int32_t adc_T)
{
    int32_t var1 = (((adc_T >> 3) - ((int32_t)dig_T1 << 1)) * ((int32_t)dig_T2)) >> 11;
    int32_t var2 = (((((adc_T >> 4) - ((int32_t)dig_T1)) * ((adc_T >> 4) - ((int32_t)dig_T1))) >> 12) * ((int32_t)dig_T3)) >> 14;
    return (var1 + var2) / 100;  /* °C*100 */
}

static uint32_t bmp280_compensate_P(int32_t adc_P, int32_t t_fine)
{
    int64_t var1, var2, p;
    var1 = ((int64_t)t_fine) - 128000;
    var2 = var1 * var1 * (int64_t)dig_P6;
    var2 += ((var1 * (int64_t)dig_P5) << 17);
    var2 += (((int64_t)dig_P4) << 35);
    var1 = ((var1 * var1 * (int64_t)dig_P3) >> 8) + ((var1 * (int64_t)dig_P2) << 12);
    var1 = (((((int64_t)1) << 47) + var1)) * ((int64_t)dig_P1) >> 33;
    if (var1 == 0) return 0;
    p = 1048576 - adc_P;
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = (((int64_t)dig_P9) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (((int64_t)dig_P8) * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (((int64_t)dig_P7) << 4);
    return (uint32_t)p;
}

int bmp280_read(float *pressure, float *temperature)
{
    uint8_t data[6];
    if (read_regs(0xF7, data, 6) != 0) return -1;
    int32_t adc_P = ((int32_t)data[0] << 12) | ((int32_t)data[1] << 4) | (data[2] >> 4);
    int32_t adc_T = ((int32_t)data[3] << 12) | ((int32_t)data[4] << 4) | (data[5] >> 4);
    int32_t t = bmp280_compensate_T(adc_T);
    if (temperature) *temperature = t / 100.0f;
    if (pressure)    *pressure = bmp280_compensate_P(adc_P, t) / 100.0f;  /* hPa */
    return 0;
}
