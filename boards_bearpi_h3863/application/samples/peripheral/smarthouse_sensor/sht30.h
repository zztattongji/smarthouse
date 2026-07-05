/**
 * @file    sht30.h
 * @brief   GY-SHT30 温湿度传感器驱动 (I2C, 0x44)
 */
#ifndef SHT30_H
#define SHT30_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHT30_I2C_ADDR  0x44

/**
 * @brief 初始化 SHT30
 * @return 0=成功
 */
int sht30_init(void);

/**
 * @brief 读取温湿度（单次测量，高重复性）
 * @param temp [out] 温度 °C
 * @param hum  [out] 相对湿度 %
 * @return 0=成功
 */
int sht30_read(float *temp, float *hum);

#ifdef __cplusplus
}
#endif

#endif
