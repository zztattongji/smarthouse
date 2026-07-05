/**
 * @file    bh1750.h
 * @brief   GY-302/BH1750 光照传感器 (I2C, 0x23)
 */
#ifndef BH1750_H
#define BH1750_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BH1750_I2C_ADDR 0x23

int  bh1750_init(void);
int  bh1750_read(uint16_t *lux);

#ifdef __cplusplus
}
#endif
#endif
