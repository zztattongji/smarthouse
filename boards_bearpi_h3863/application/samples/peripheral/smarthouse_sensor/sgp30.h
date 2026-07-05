/**
 * @file    sgp30.h
 * @brief   SGP30 空气质量传感器驱动 (I2C, 0x58)
 *          上电需预热 15s，init 需读基准值
 */
#ifndef SGP30_H
#define SGP30_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SGP30_I2C_ADDR 0x58

int  sgp30_init(void);          /* init_air_quality + 读基准值 */
int  sgp30_read(uint16_t *voc, uint16_t *eco2);
int  sgp30_get_baseline(uint16_t *voc_base, uint16_t *eco2_base);
int  sgp30_set_baseline(uint16_t voc_base, uint16_t eco2_base);

#ifdef __cplusplus
}
#endif
#endif
