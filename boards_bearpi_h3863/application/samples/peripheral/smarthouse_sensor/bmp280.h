/**
 * @file    bmp280.h
 * @brief   GY-BMP280 气压传感器驱动 (I2C, 0x76)
 */
#ifndef BMP280_H
#define BMP280_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BMP280_I2C_ADDR 0x76

int bmp280_init(void);
int bmp280_read(float *pressure, float *temperature);

#ifdef __cplusplus
}
#endif
#endif
