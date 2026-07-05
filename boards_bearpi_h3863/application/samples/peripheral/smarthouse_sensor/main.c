/**
 * @file    main.c
 * @brief   2 号板 — 传感器采集板 (app_run 入口)
 *   每 2s 轮询 6 传感器 → JSON → SLE 广播 → OLED 更新
 */
/* 当前先调试 2 号板本地检测能力：关闭到 1 号板网关的 SLE 连接与上报，并关闭 PMS5003 UART。 */
#define SMARTHOUSE_SENSOR_ENABLE_SLE 0
#define SMARTHOUSE_SENSOR_ENABLE_PMS5003 0

#include "sht30.h"
#include "bmp280.h"
#include "sgp30.h"
#if SMARTHOUSE_SENSOR_ENABLE_PMS5003
#include "pms5003.h"
#endif
#include "sle_bus.h"
#include "msg_builder.h"

#include "pinctrl.h"
#include "i2c.h"
#include "uart.h"
#include "soc_osal.h"
#include "app_init.h"
#include "common_def.h"
#include "ssd1306.h"
#include "ssd1306_fonts.h"

#include <stdio.h>
#include <string.h>


/* ── 传感器缓存 ── */
static float    g_temp, g_hum, g_pressure;
static uint16_t g_voc, g_eco2, g_pm25, g_pm10;
static volatile uint8_t g_refresh_requested = 0;

#if SMARTHOUSE_SENSOR_ENABLE_SLE
/* ── SLE Client 模式: 连接到 1号板 Server ── */
#include "sle_uart_client.h"

static uint16_t g_gw_conn_id = 0;
static uint8_t  g_sle_connected = 0;

static int json_payload_len_valid(int len)
{
    return (len > 0 && len < SLE_BUS_PAYLOAD_MAX);
}

/* Client notification 回调: 收到 Server 下发的命令 */
static void sle_client_notify_cb(uint8_t client_id, uint16_t conn_id,
                                  ssapc_handle_value_t *data, errcode_t status)
{
    (void)client_id; (void)conn_id;
    if (status != ERRCODE_SUCC || data == NULL) return;
    if (data->data == NULL || data->data_len < 8) return;
    sle_bus_packet_t *p = (sle_bus_packet_t *)data->data;
    if (sle_bus_validate(p, data->data_len) != 0) return;
    if (p->msg_type == (uint8_t)MSG_DEVICE_CMD) {
        uint16_t pl = p->payload_len;
        if (pl > SLE_BUS_PAYLOAD_MAX) pl = SLE_BUS_PAYLOAD_MAX;
        const char *needle = "refresh_sensors";
        uint16_t needle_len = (uint16_t)strlen(needle);
        for (int i = 0; i <= (int)pl - (int)needle_len; i++) {
            if (memcmp(&p->payload[i], needle, needle_len) == 0) {
                g_refresh_requested = 1;
                break;
            }
        }
    }
}

static void sle_client_indication_cb(uint8_t client_id, uint16_t conn_id,
                                      ssapc_handle_value_t *data, errcode_t status)
{
    (void)client_id; (void)conn_id; (void)data; (void)status;
}

/* 检查 SLE 连接是否有效, 失效则清零 — 解决掉线不重连 */
static void sensor_check_conn(void)
{
    uint16_t cur_id = get_g_sle_uart_conn_id();
    if (cur_id == 0 || cur_id != g_gw_conn_id) {
        g_sle_connected = 0;
        g_gw_conn_id = 0;
    }
}

static void sensor_reconnect(void)
{
    sensor_check_conn();
    if (g_sle_connected) return;
    sle_uart_start_scan();
    osal_msleep(50);
    uint16_t conn_id = get_g_sle_uart_conn_id();
    if (conn_id != 0) {
        g_gw_conn_id = conn_id;
        g_sle_connected = 1;
    }
}

static void sensor_broadcast(void)
{
    sensor_reconnect();
    if (!g_sle_connected) return;
    char json[SLE_BUS_PAYLOAD_MAX];
    int jlen = build_sensor_json(json, sizeof(json), g_temp, g_hum, g_pressure,
                                 g_voc, g_eco2, g_pm25);
    if (!json_payload_len_valid(jlen)) return;
    sle_bus_packet_t pkt;
    uint16_t plen = sle_bus_pack(&pkt, BOARD_SENSOR, MSG_SENSOR_DATA, json, (uint16_t)jlen);
    if (plen) {
        ssapc_write_param_t *wp = get_g_sle_uart_send_param();
        if (wp) { wp->data_len = plen; wp->data = (uint8_t *)&pkt;
                  ssapc_write_req(0, g_gw_conn_id, wp); }
    }
}
#else
static void sensor_broadcast(void)
{
    /* Local sensor/OLED test mode: do not connect or send to other boards. */
}
#endif
static void oled_update(void)
{
    ssd1306_Fill(Black);
    ssd1306_SetCursor(0, 0);  ssd1306_printf("T:%.1fC H:%.0f%%", g_temp, g_hum);
    ssd1306_SetCursor(0, 16); ssd1306_printf("P:%.0fhPa", g_pressure);
    ssd1306_SetCursor(0, 32); ssd1306_printf("PM2.5:%u VOC:%u", g_pm25, g_voc);
    ssd1306_SetCursor(0, 48); ssd1306_printf("CO2:%u", g_eco2);
    ssd1306_UpdateScreen();
}

static void poll_all(void)
{
    sht30_read(&g_temp, &g_hum);
    float bt; bmp280_read(&g_pressure, &bt);
    sgp30_read(&g_voc, &g_eco2);
#if SMARTHOUSE_SENSOR_ENABLE_PMS5003
    pms5003_read(NULL, &g_pm25, &g_pm10);
#endif
}

static void i2c_pins_init(void)
{
    uapi_pin_set_mode(CONFIG_SMARTHOUSE_SENSOR_I2C_SCL_PIN, PIN_MODE_2);
    uapi_pin_set_pull(CONFIG_SMARTHOUSE_SENSOR_I2C_SCL_PIN, PIN_PULL_TYPE_UP);
    uapi_pin_set_mode(CONFIG_SMARTHOUSE_SENSOR_I2C_SDA_PIN, PIN_MODE_2);
    uapi_pin_set_pull(CONFIG_SMARTHOUSE_SENSOR_I2C_SDA_PIN, PIN_PULL_TYPE_UP);
}

static void sensor_task(void)
{
    printf("[sensor] task start\r\n");

    i2c_pins_init();
    printf("[sensor] i2c pins ready: bus=%d scl=%d sda=%d\r\n",
           CONFIG_SMARTHOUSE_SENSOR_I2C_BUS,
           CONFIG_SMARTHOUSE_SENSOR_I2C_SCL_PIN,
           CONFIG_SMARTHOUSE_SENSOR_I2C_SDA_PIN);

    int ret = (int)uapi_i2c_master_init(CONFIG_SMARTHOUSE_SENSOR_I2C_BUS, 400000, 0);
    printf("[sensor] i2c init ret=%d\r\n", ret);

    printf("[sensor] oled init begin\r\n");
    ssd1306_Init();
    ssd1306_Fill(Black);
    ssd1306_SetCursor(0, 20);
    ssd1306_DrawString("Sensor Booting", Font_7x10, White);
    ssd1306_UpdateScreen();
    printf("[sensor] oled boot screen done\r\n");

    ret = sht30_init();
    printf("[sensor] sht30 init ret=%d\r\n", ret);
    ret = bmp280_init();
    printf("[sensor] bmp280 init ret=%d\r\n", ret);
    ret = sgp30_init();
    printf("[sensor] sgp30 init ret=%d\r\n", ret);
#if SMARTHOUSE_SENSOR_ENABLE_PMS5003
    ret = pms5003_init();
    printf("[sensor] pms5003 init ret=%d\r\n", ret);
#else
    printf("[sensor] pms5003 disabled\r\n");
#endif
    ssd1306_Fill(Black);
    ssd1306_SetCursor(0, 20);
    ssd1306_DrawString("Sensor Board OK", Font_7x10, White);
    ssd1306_UpdateScreen();

    printf("[sensor] warmup begin\r\n");
    osal_msleep(15000);  /* SGP30 预热 */
    printf("[sensor] warmup done\r\n");

    while (1) {
        if (g_refresh_requested) {
            g_refresh_requested = 0;
        }
        poll_all();
        sensor_broadcast();
        oled_update();
        printf("[sensor] data t=%d h=%d p=%d voc=%u eco2=%u pm25=%u\r\n",
               (int)g_temp, (int)g_hum, (int)g_pressure, g_voc, g_eco2, g_pm25);
        osal_msleep(2000);
    }
}

static void sensor_entry(void)
{
    printf("[sensor] entry local test mode\r\n");
    osal_kthread_create((osal_kthread_handler)sensor_task, 0, "sensor_task", 8192);

#if SMARTHOUSE_SENSOR_ENABLE_SLE
    osal_msleep(200);
    printf("[sensor] sle client init begin\r\n");
    sle_uart_client_init(sle_client_notify_cb, sle_client_indication_cb);
    sle_uart_start_scan();

    osal_msleep(3000);
    g_gw_conn_id = get_g_sle_uart_conn_id();
    if (g_gw_conn_id != 0) g_sle_connected = 1;
    printf("[sensor] sle conn_id=%u connected=%u\r\n", g_gw_conn_id, g_sle_connected);
#else
    printf("[sensor] sle disabled, run OLED and sensors only\r\n");
#endif
}

/* BearPi SDK 标准入口 */
static void run_sensor(void) { sensor_entry(); }
app_run(run_sensor);








