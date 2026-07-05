/**
 * @file    actuator.c
 * @brief   3号板执行器驱�?�?GPIO/PWM (uapi SDK API)
 *   使用者需根据原理图修�?CONFIG_PIN_XX 引脚�? */
#include "actuator.h"
#include "pinctrl.h"
#include "gpio.h"
#include "pwm.h"
#include "soc_osal.h"
#include "common_def.h"
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

/* ══�?引脚定义 (需按原理图修改) ══�?*/
#define PWM_CH_LED     CONFIG_SMARTHOUSE_ACTUATOR_PWM_CH_LED
#define PWM_CH_FAN     CONFIG_SMARTHOUSE_ACTUATOR_PWM_CH_FAN
#define PWM_CH_SERVO   CONFIG_SMARTHOUSE_ACTUATOR_PWM_CH_SERVO
#define PWM_CH_IR      CONFIG_SMARTHOUSE_ACTUATOR_PWM_CH_IR
#define ACTUATOR_PWM_PIN_MODE 1

/* GPIO 引脚 �?来自原理�?BearPi 模块 Pin 编号 */
#define CONFIG_PIN_LED       CONFIG_SMARTHOUSE_ACTUATOR_PIN_LED
#define CONFIG_PIN_FAN       CONFIG_SMARTHOUSE_ACTUATOR_PIN_FAN
#define CONFIG_PIN_MOTOR_IA  CONFIG_SMARTHOUSE_ACTUATOR_PIN_MOTOR_IA
#define CONFIG_PIN_MOTOR_IB  CONFIG_SMARTHOUSE_ACTUATOR_PIN_MOTOR_IB
#define CONFIG_PIN_RELAY     CONFIG_SMARTHOUSE_ACTUATOR_PIN_RELAY
#define CONFIG_PIN_SERVO     CONFIG_SMARTHOUSE_ACTUATOR_PIN_SERVO
#define CONFIG_PIN_BUZZER    CONFIG_SMARTHOUSE_ACTUATOR_PIN_BUZZER
#define CONFIG_PIN_IR_LED    CONFIG_SMARTHOUSE_ACTUATOR_PIN_IR
#define CONFIG_PIN_PIR       CONFIG_SMARTHOUSE_ACTUATOR_PIN_PIR
#define CONFIG_PIN_KEY1      CONFIG_SMARTHOUSE_ACTUATOR_PIN_KEY1
#define CONFIG_PIN_KEY2      CONFIG_SMARTHOUSE_ACTUATOR_PIN_KEY2

/* PWM 参数: 假设 1MHz 时钟 */
#define PWM_FREQ_LED_FAN 1000     /* 1kHz */
#define PWM_PERIOD_LF    1000     /* us at 1MHz */
#define PWM_FREQ_SERVO   50       /* 50Hz */
#define PWM_PERIOD_SERVO 20000    /* us */
#define PWM_FREQ_IR      38000    /* 38kHz */
#define PWM_PERIOD_IR    26       /* us (~38.46kHz) */

static actuator_state_t g_state;

/* ── PWM 辅助 ── */
static void pwm_setup(uint32_t pin, uint8_t ch, uint32_t period, uint32_t low)
{
    pwm_config_t cfg = { .low_time = low, .high_time = period - low,
                         .offset_time = 0, .cycles = 0, .repeat = true };
    uapi_pin_set_mode(pin, ACTUATOR_PWM_PIN_MODE);
    uapi_pwm_open(ch, &cfg);
    uapi_pwm_start(ch);
}

static void pwm_update(uint8_t ch, uint32_t period, uint32_t low)
{
    pwm_config_t cfg = { .low_time = low, .high_time = period - low,
                         .offset_time = 0, .cycles = 0, .repeat = true };
    uapi_pwm_close(ch);
    uapi_pwm_open(ch, &cfg);
    uapi_pwm_start(ch);
}

/* ── GPIO 辅助 ── */
static void gpio_out(uint32_t pin, uint8_t val)
{
    uapi_pin_set_mode(pin, PIN_MODE_0);
    uapi_gpio_set_dir(pin, GPIO_DIRECTION_OUTPUT);
    uapi_gpio_set_val(pin, val ? GPIO_LEVEL_HIGH : GPIO_LEVEL_LOW);
}

static void gpio_in_setup(uint32_t pin)
{
    uapi_pin_set_mode(pin, PIN_MODE_0);
    uapi_gpio_set_dir(pin, GPIO_DIRECTION_INPUT);
}

static uint8_t gpio_read(uint32_t pin)
{
    gpio_level_t val = uapi_gpio_get_val(pin);
    return (val == GPIO_LEVEL_HIGH) ? 1 : 0;
}

/* ══�?公开 API ══�?*/

int actuator_init_all(void)
{
    memset(&g_state, 0, sizeof(g_state));
    strcpy(g_state.motor_state, "stop");
    uapi_pwm_init();

    /* PWM 输出: 初始关闭 */
    pwm_setup(CONFIG_PIN_LED,      PWM_CH_LED,   PWM_PERIOD_LF, PWM_PERIOD_LF);
    pwm_setup(CONFIG_PIN_FAN,      PWM_CH_FAN,   PWM_PERIOD_LF, PWM_PERIOD_LF);
    pwm_setup(CONFIG_PIN_SERVO,    PWM_CH_SERVO, PWM_PERIOD_SERVO, 1500);  /* 90° */
    pwm_setup(CONFIG_PIN_IR_LED,   PWM_CH_IR,    PWM_PERIOD_IR, PWM_PERIOD_IR);

    /* GPIO 输出 */
    gpio_out(CONFIG_PIN_MOTOR_IA, 0);
    gpio_out(CONFIG_PIN_MOTOR_IB, 0);
    gpio_out(CONFIG_PIN_RELAY, 0);
    gpio_out(CONFIG_PIN_BUZZER, 0);

    /* GPIO 输入 */
    gpio_in_setup(CONFIG_PIN_PIR);
    gpio_in_setup(CONFIG_PIN_KEY1);
    gpio_in_setup(CONFIG_PIN_KEY2);

    g_state.servo_angle = 90;
    return 0;
}

void led_set(uint8_t v) {
    if (v > 100) v = 100;
    g_state.led_brightness = v;
    pwm_update(PWM_CH_LED, PWM_PERIOD_LF, PWM_PERIOD_LF * (100 - v) / 100);
}

void fan_set(uint8_t v) {
    if (v > 100) v = 100;
    g_state.fan_speed = v;
    pwm_update(PWM_CH_FAN, PWM_PERIOD_LF, PWM_PERIOD_LF * (100 - v) / 100);
}

void motor_set(const char *dir) {
    if (!dir) return;
    if (strcmp(dir, "fwd") == 0 || strcmp(dir, "forward") == 0) {
        uapi_gpio_set_val(CONFIG_PIN_MOTOR_IA, GPIO_LEVEL_HIGH);
        uapi_gpio_set_val(CONFIG_PIN_MOTOR_IB, GPIO_LEVEL_LOW);
        strcpy(g_state.motor_state, "fwd");
    } else if (strcmp(dir, "rev") == 0 || strcmp(dir, "reverse") == 0) {
        uapi_gpio_set_val(CONFIG_PIN_MOTOR_IA, GPIO_LEVEL_LOW);
        uapi_gpio_set_val(CONFIG_PIN_MOTOR_IB, GPIO_LEVEL_HIGH);
        strcpy(g_state.motor_state, "rev");
    } else if (strcmp(dir, "brake") == 0) {
        uapi_gpio_set_val(CONFIG_PIN_MOTOR_IA, GPIO_LEVEL_HIGH);
        uapi_gpio_set_val(CONFIG_PIN_MOTOR_IB, GPIO_LEVEL_HIGH);
        strcpy(g_state.motor_state, "brake");
    } else {
        uapi_gpio_set_val(CONFIG_PIN_MOTOR_IA, GPIO_LEVEL_LOW);
        uapi_gpio_set_val(CONFIG_PIN_MOTOR_IB, GPIO_LEVEL_LOW);
        strcpy(g_state.motor_state, "stop");
    }
}

void relay_set(uint8_t v) { g_state.relay_state = v ? 1 : 0;
    uapi_gpio_set_val(CONFIG_PIN_RELAY, v ? GPIO_LEVEL_HIGH : GPIO_LEVEL_LOW); }

void servo_set(uint8_t angle) {
    if (angle > 180) angle = 180;
    g_state.servo_angle = angle;
    uint32_t pulse = 500 + (uint32_t)angle * 2000 / 180;
    pwm_update(PWM_CH_SERVO, PWM_PERIOD_SERVO, pulse);
}

void buzzer_set(uint8_t v) { g_state.buzzer_state = v ? 1 : 0;
    uapi_gpio_set_val(CONFIG_PIN_BUZZER, v ? GPIO_LEVEL_HIGH : GPIO_LEVEL_LOW); }

/* ── IR NEC 38kHz ── */
static void ir_carrier(uint8_t on) {
    if (on) pwm_update(PWM_CH_IR, PWM_PERIOD_IR, PWM_PERIOD_IR / 2);
    else    pwm_update(PWM_CH_IR, PWM_PERIOD_IR, PWM_PERIOD_IR);
}
static void delay_us(volatile uint32_t us) { while (us--) { __asm__ volatile ("nop"); } }

static void ir_nec_byte(uint8_t b) {
    for (uint8_t i = 0; i < 8; i++) {
        ir_carrier(1); delay_us(560);
        ir_carrier(0); delay_us(b & 1 ? 1690 : 560);
        b >>= 1;
    }
}

void ir_send_code(const char *code) {
    if (!code) return;
    const char *p = code;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    uint8_t bytes[4] = {0};
    for (int i = 0; i < 4 && *p; i++) {
        unsigned int v;
        char h[3] = {p[0], p[1] ? p[1] : '0', '\0'};
        if (sscanf(h, "%2x", &v) == 1) { bytes[i] = (uint8_t)v; p += 2; } else break;
    }
    ir_carrier(1); delay_us(9000);
    ir_carrier(0); delay_us(4500);
    ir_nec_byte(bytes[0]); ir_nec_byte(~bytes[0]);
    ir_nec_byte(bytes[1]); ir_nec_byte(~bytes[1]);
    ir_carrier(1); delay_us(560);
    ir_carrier(0);
}

/* ── 输入 ── */
uint8_t pir_read(void)  { uint8_t v = gpio_read(CONFIG_PIN_PIR);  g_state.pir_triggered = v; return v; }
uint8_t key1_read(void) { uint8_t v = gpio_read(CONFIG_PIN_KEY1); g_state.key1_pressed = !v; return !v; }
uint8_t key2_read(void) { uint8_t v = gpio_read(CONFIG_PIN_KEY2); g_state.key2_pressed = !v; return !v; }

void actuator_get_state(actuator_state_t *s) {
    if (!s) return;
    s->led_brightness = g_state.led_brightness;
    s->fan_speed      = g_state.fan_speed;
    memcpy(s->motor_state, g_state.motor_state, sizeof(g_state.motor_state));
    s->relay_state    = g_state.relay_state;
    s->servo_angle    = g_state.servo_angle;
    s->buzzer_state   = g_state.buzzer_state;
    s->pir_triggered  = pir_read();
    s->key1_pressed   = key1_read();
    s->key2_pressed   = key2_read();
    s->armed          = g_state.armed;
}

void actuator_set_armed(uint8_t v)  { g_state.armed = v ? 1 : 0; }
uint8_t actuator_get_armed(void)    { return g_state.armed; }
