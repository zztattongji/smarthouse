/**
 * @file    actuator.h
 * @brief   执行器板硬件控制接口声明 - Board 3
 * 7 路输出: LED灯带(NMOS PWM), 风扇(NMOS PWM), 电机(L9110S H桥),
 *           继电器(S8050+SRD-05VDC), SG90舵机(50Hz PWM), 红外(38kHz PWM), 蜂鸣器(GPIO)
 * 3 路输入: PIR(AM312), 按键x2
 */
#ifndef ACTUATOR_H
#define ACTUATOR_H
#include <stdint.h>

typedef struct {
    uint8_t led_brightness;   /* 0-100%   */
    uint8_t fan_speed;        /* 0-100%   */
    char    motor_state[8];   /* fwd/rev/stop/brake */
    uint8_t relay_state;      /* 0/1      */
    uint8_t servo_angle;      /* 0-180    */
    uint8_t buzzer_state;     /* 0/1      */
    uint8_t pir_triggered;    /* PIR      */
    uint8_t key1_pressed;
    uint8_t key2_pressed;
    uint8_t armed;            /* 安防布防 */
} actuator_state_t;

int  actuator_init_all(void);
void led_set(uint8_t brightness);
void fan_set(uint8_t speed);
void motor_set(const char *state);
void relay_set(uint8_t state);
void servo_set(uint8_t angle);
void buzzer_set(uint8_t state);
void ir_send_code(const char *code);
uint8_t pir_read(void);
uint8_t key1_read(void);
uint8_t key2_read(void);
void actuator_get_state(actuator_state_t *state);
void actuator_set_armed(uint8_t armed);
uint8_t actuator_get_armed(void);

#endif
