#include "led_ctrl.h"
#include "pinctrl.h"
#include "gpio.h"
#include "soc_osal.h"
#include "cmsis_os2.h"
#include "common_def.h"

#ifndef CONFIG_BLINKY_PIN
#define CONFIG_BLINKY_PIN 2   /* 如果board.json里已有定义，就用你的定义 */
#endif

#define LED_TASK_STACK_SIZE    0x1000
#define LED_TASK_PRIO          (osPriority_t)(17)
#define LED_BLINK_INTERVAL_MS  500

static volatile led_mode_t g_led_mode = LED_MODE_OFF;
static volatile uint8_t g_led_inited = 0;

static void led_hw_init(void)
{
    uapi_pin_set_mode(CONFIG_BLINKY_PIN, PIN_MODE_0);
    uapi_gpio_set_dir(CONFIG_BLINKY_PIN, GPIO_DIRECTION_OUTPUT);
    uapi_gpio_set_val(CONFIG_BLINKY_PIN, GPIO_LEVEL_LOW);
}

static void led_set_off(void)
{
    uapi_gpio_set_val(CONFIG_BLINKY_PIN, GPIO_LEVEL_LOW);
}

static void led_set_on(void)
{
    uapi_gpio_set_val(CONFIG_BLINKY_PIN, GPIO_LEVEL_HIGH);
}

static void *led_ctrl_task(const char *arg)
{
    unused(arg);

    led_hw_init();

    while (1) {
        switch (g_led_mode) {
            case LED_MODE_OFF:
                led_set_off();
                osal_msleep(100);
                break;

            case LED_MODE_ON:
                led_set_on();
                osal_msleep(100);
                break;

            case LED_MODE_BLINK:
                uapi_gpio_toggle(CONFIG_BLINKY_PIN);
                osal_msleep(LED_BLINK_INTERVAL_MS);
                break;

            default:
                led_set_off();
                osal_msleep(100);
                break;
        }
    }

    return NULL;
}

void led_ctrl_init(void)
{
    if (g_led_inited) {
        return;
    }

    osThreadAttr_t attr;
    attr.name = "LedCtrlTask";
    attr.attr_bits = 0U;
    attr.cb_mem = NULL;
    attr.cb_size = 0U;
    attr.stack_mem = NULL;
    attr.stack_size = LED_TASK_STACK_SIZE;
    attr.priority = LED_TASK_PRIO;

    if (osThreadNew((osThreadFunc_t)led_ctrl_task, NULL, &attr) != NULL) {
        g_led_inited = 1;
    }
}

void led_ctrl_set_mode(led_mode_t mode)
{
    g_led_mode = mode;
}

led_mode_t led_ctrl_get_mode(void)
{
    return g_led_mode;
}