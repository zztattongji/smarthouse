#ifndef LED_CTRL_H
#define LED_CTRL_H

#include <stdint.h>

typedef enum {
    LED_MODE_OFF = 0,
    LED_MODE_ON  = 1,
    LED_MODE_BLINK = 2
} led_mode_t;

/* 初始化LED控制模块 */
void led_ctrl_init(void);

/* 设置LED模式 */
void led_ctrl_set_mode(led_mode_t mode);

/* 获取当前LED模式 */
led_mode_t led_ctrl_get_mode(void);

#endif