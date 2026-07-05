/**
 * @file    main.c
 * @brief   1 号板 — 网关 (WiFi + MQTT + SLE Server + dispatch + 健康评分 + 诊断 + 模式切换)
 *   基于 mqtt_demo.c 架构 + Smarthouse SLE Bus 协议
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "MQTTClient.h"
#include "app_init.h"
#include "soc_osal.h"
#include "cmsis_os2.h"
#include "osal_debug.h"
#include "wifi_connect.h"
#include "sle_uart_server.h"
#include "sle_uart_server_adv.h"
#include "sle_device_discovery.h"
#include "sle_errcode.h"
#include "sle_connection_manager.h"
#include "systick.h"
#include "cJSON.h"
#include "sle_bus.h"
#include "msg_builder.h"

/* ═══ 用户配置 — 来自华为云 IoTDA + 后端 D:\MQTT\server.py ═══ */
#define WIFI_SSID       "YOUR_WIFI_SSID"
#define WIFI_PWD        "YOUR_WIFI_PASSWORD"
#define DEVICE_ID       "YOUR_DEVICE_ID"
#define MQTT_ADDRESS    "tcp://YOUR_IOTDA_MQTT_ENDPOINT:1883"
#define MQTT_CLIENTID   "YOUR_MQTT_CLIENT_ID"
#define MQTT_USERNAME   "YOUR_MQTT_USERNAME"
#define MQTT_PASSWORD   "YOUR_MQTT_PASSWORD"

#define QOS            1
#define KEEPALIVE      120
#define MSG_QUEUE_LEN  16
#define MSG_QUEUE_SIZE sizeof(dispatch_msg_t)

/* ═══ dispatch 消息 ═══ */
typedef struct {
    uint8_t  src_board, msg_type;
    uint32_t recv_tick;       /* 1号板收到此消息时的本地 uapi_systick_get_ms() */
    uint8_t  payload[SLE_BUS_PAYLOAD_MAX + 1];
    uint16_t payload_len;
} dispatch_msg_t;

/* ═══ 全局变量 ═══ */
static MQTTClient       g_client;
static unsigned long    g_msg_queue;
static char g_cmd_topic[128], g_report_topic[128], g_cmd_resp_fmt[128];

/* 传感器缓存 */
static float    s_temp, s_hum, s_pressure;
static uint16_t s_light, s_voc, s_eco2, s_pm25;

/* 执行器缓存 */
static uint8_t a_led, a_fan, a_relay, a_servo, a_buzzer, a_pir;
static char    a_motor[8];

/* 模式状态机 */
static uint8_t  g_mode = MODE_HOME;
static uint32_t last_pir_time;

/* 诊断历史窗口缓存 */
static float    temp_5min_ago;        /* 5 分钟前的温度 */
static float    voc_24h_ago;          /* 24 小时前的 VOC */
static float    eco2_24h_ago;         /* 24 小时前的 eCO2 */
static uint32_t last_temp_snapshot;   /* 上次温度快照时间戳 */
static uint32_t last_voc_snapshot;    /* 上次 VOC 快照时间戳 */
static uint32_t last_pir_alert;      /* 上次 PIR 告警时间戳 */

/* ═══ 命令仲裁: 1号板本地接收时间 + 500ms 窗口 ═══ */
static uint32_t g_arb_last_ms  = 0;    /* 上条命令的 recv_tick */
static uint8_t  g_arb_last_src = 0;
#define ARB_WINDOW_MS  500

/**
 * @brief 基于 1号板本地接收时间做优先级仲裁
 *   规则: 500ms 窗口内, Gateway(用户App) > AI(4号板)
 *   @return 1=放行, 0=丢弃
 */
static int gateway_arbitrate(uint8_t src, uint32_t recv_tick)
{
    uint32_t delta = (recv_tick > g_arb_last_ms)
                     ? (recv_tick - g_arb_last_ms) : 0;

    if (delta < ARB_WINDOW_MS) {
        /* 500ms 窗口内: Gateway 覆盖 AI, 同源后到覆盖先到 */
        if (g_arb_last_src == BOARD_GATEWAY && src == BOARD_AI) {
            /* 已有用户命令待执行, 丢弃 AI 命令 */
            return 0;
        }
        if (g_arb_last_src == BOARD_AI && src == BOARD_GATEWAY) {
            /* 用户命令到达, 覆盖之前的 AI 命令 (继续执行新命令) */
            ;
        }
        /* 同源: 后到覆盖先到 (自然放行) */
    }
    /* 超窗口: 直接放行, 更新基准 */
    g_arb_last_ms  = recv_tick;
    g_arb_last_src = src;
    return 1;
}

extern int MQTTClient_init(void);
/* 使用 SDK 真实声明的 const 签名, 与 sle_uart_server.h 一致 */
extern errcode_t sle_uart_server_send_report_by_handle(const uint8_t *data, uint16_t len);

static int json_payload_len_valid(int len)
{
    return (len > 0 && len < SLE_BUS_PAYLOAD_MAX);
}

/* ═══════════════ 健康评分 ═══════════════ */
static uint8_t score_temp(float t) {
    if (t >= 20 && t <= 26) return 100;
    float d = (t < 20) ? (20 - t) : (t - 26);
    int s = (int)(100 - d * 5);
    return (uint8_t)(s < 0 ? 0 : s);
}
static uint8_t score_hum(float h) {
    if (h >= 40 && h <= 60) return 100;
    float d = (h < 40) ? (40 - h) : (h - 60);
    return (uint8_t)(d > 50 ? 0 : 100 - (int)(d * 0.8f));
}
static uint8_t score_pm25(uint16_t p) {
    if (p <= 35) return 100;
    if (p >= 75) return 0;
    return (uint8_t)(100 - (p - 35) * 60 / 40);
}
static uint8_t score_voc(uint16_t v) {
    if (v <= 130) return 100;
    if (v >= 260) return 10;
    return (uint8_t)(100 - (v - 130) * 60 / 130);
}
static uint8_t score_eco2(uint16_t c) {
    if (c <= 800) return 100;
    if (c >= 1500) return 30;
    return (uint8_t)(100 - (c - 800) * 70 / 700);
}

static uint8_t score_light(uint16_t l) {
    if (l >= 100 && l <= 800) return 100;
    if (l < 50) return 100;  /* 夜间不扣分 */
    return 60;
}

static void compute_health(void)
{
    /* 权重: T=0.20 H=0.15 P=0.25 V=0.15 C=0.15 L=0.10 — 总计 1.0 */
    uint8_t score = (uint8_t)(
        0.20f * score_temp(s_temp) +
        0.15f * score_hum(s_hum) +
        0.25f * score_pm25(s_pm25) +
        0.15f * score_voc(s_voc) +
        0.15f * score_eco2(s_eco2) +
        0.10f * score_light(s_light));
    char json[SLE_BUS_PAYLOAD_MAX];
    int jl = build_health_score_json(json, sizeof(json), score, "");
    if (!json_payload_len_valid(jl)) return;
    sle_bus_packet_t p; uint16_t pl = sle_bus_pack(&p, BOARD_GATEWAY, MSG_HEALTH_SCORE, json, (uint16_t)jl);
    if (pl) { errcode_t sr = sle_uart_server_send_report_by_handle((const uint8_t *)&p, pl); (void)sr; }
}

/* ═══════════════ 故障诊断 ═══════════════ */
static void run_diag(void)
{
    static uint32_t diag_tick = 0;
    diag_tick++;

    /* 1. 风扇效率检测: 每 5 分钟对比一次 (diag_task 每 60s 跑一次, 5min=5ticks) */
    if (diag_tick - last_temp_snapshot >= 5) {
        if (a_fan > 60 && fabsf(s_temp - temp_5min_ago) < 0.3f && s_temp > 30.0f) {
            char j[SLE_BUS_PAYLOAD_MAX]; int jl = build_alert_json(j, sizeof(j), "风扇效率异常", "高转速5分钟温度不降");
            if (!json_payload_len_valid(jl)) return;
            sle_bus_packet_t p; uint16_t pl = sle_bus_pack(&p, BOARD_GATEWAY, MSG_ALERT, j, (uint16_t)jl);
            if (pl) { errcode_t sr = sle_uart_server_send_report_by_handle((const uint8_t *)&p, pl); (void)sr; }
        }
        temp_5min_ago = s_temp;
        last_temp_snapshot = diag_tick;
    }

    /* 2. 传感器冻结检测: 每 24 小时对比一次 (24h/60s=1440ticks) */
    if (diag_tick - last_voc_snapshot >= 1440) {
        if (fabsf(s_voc - voc_24h_ago) < 0.5f && fabsf(s_eco2 - eco2_24h_ago) < 1.0f) {
            char j[SLE_BUS_PAYLOAD_MAX]; int jl = build_alert_json(j, sizeof(j), "sensor_freeze", "SGP30 no change in 24h");
            if (!json_payload_len_valid(jl)) return;
            sle_bus_packet_t p; uint16_t pl = sle_bus_pack(&p, BOARD_GATEWAY, MSG_ALERT, j, (uint16_t)jl);
            if (pl) { errcode_t sr = sle_uart_server_send_report_by_handle((const uint8_t *)&p, pl); (void)sr; }
        }
        voc_24h_ago = s_voc;
        eco2_24h_ago = s_eco2;
        last_voc_snapshot = diag_tick;
    }

    /* 3. PIR 静默检测: 48 小时无触发告警 (48h/60s=2880ticks) */
    if (g_mode == MODE_AWAY && diag_tick - last_pir_alert >= 2880) {
        char j[SLE_BUS_PAYLOAD_MAX]; int jl = build_alert_json(j, sizeof(j), "PIR 静默", "离家模式超48h，如非预期请检查PIR");
        if (!json_payload_len_valid(jl)) return;
        sle_bus_packet_t p; uint16_t pl = sle_bus_pack(&p, BOARD_GATEWAY, MSG_ALERT, j, (uint16_t)jl);
        if (pl) { errcode_t sr = sle_uart_server_send_report_by_handle((const uint8_t *)&p, pl); (void)sr; }
        last_pir_alert = diag_tick;
    }
}

/* ═══════════════ 模式切换 ═══════════════ */
static void mode_broadcast(uint8_t mode)
{
    char j[SLE_BUS_PAYLOAD_MAX]; int jl = build_mode_change_json(j, sizeof(j), mode);
    if (!json_payload_len_valid(jl)) return;
    sle_bus_packet_t p; uint16_t pl = sle_bus_pack(&p, BOARD_GATEWAY, MSG_MODE_CHANGE, j, (uint16_t)jl);
    if (pl) { errcode_t sr = sle_uart_server_send_report_by_handle((const uint8_t *)&p, pl); (void)sr; }
}

static void apply_away(void) {
    const char *devs[] = {"led","fan","motor","relay"}; int vals[] = {0,0,0,0};
    for (int i = 0; i < 4; i++) {
        char j[SLE_BUS_PAYLOAD_MAX]; int jl = build_device_cmd_json(j, sizeof(j), BOARD_GATEWAY, devs[i], vals[i], i==2?"stop":"");
        if (!json_payload_len_valid(jl)) continue;
        sle_bus_packet_t p; uint16_t pl = sle_bus_pack(&p, BOARD_GATEWAY, MSG_DEVICE_CMD, j, (uint16_t)jl);
        if (pl) { errcode_t sr = sle_uart_server_send_report_by_handle((const uint8_t *)&p, pl); (void)sr; }
    }
    /* 布防 */
    char j[SLE_BUS_PAYLOAD_MAX]; int jl = build_device_cmd_json(j, sizeof(j), BOARD_GATEWAY, "buzzer_arm", 1, "");
    if (!json_payload_len_valid(jl)) return;
    sle_bus_packet_t p; uint16_t pl = sle_bus_pack(&p, BOARD_GATEWAY, MSG_DEVICE_CMD, j, (uint16_t)jl);
    if (pl) { errcode_t sr = sle_uart_server_send_report_by_handle((const uint8_t *)&p, pl); (void)sr; }
    mode_broadcast(MODE_AWAY);
}

static void apply_home(void) {
    int led = s_light < 300 ? 70 : 0, fan = s_temp > 26 ? 60 : 0;
    char j[SLE_BUS_PAYLOAD_MAX];
    int jl = build_device_cmd_json(j, sizeof(j), BOARD_GATEWAY, "led", led, "");
    if (!json_payload_len_valid(jl)) return;
    sle_bus_packet_t p; uint16_t pl = sle_bus_pack(&p, BOARD_GATEWAY, MSG_DEVICE_CMD, j, (uint16_t)jl);
    if (pl) { errcode_t sr = sle_uart_server_send_report_by_handle((const uint8_t *)&p, pl); (void)sr; }
    jl = build_device_cmd_json(j, sizeof(j), BOARD_GATEWAY, "fan", fan, "");
    if (!json_payload_len_valid(jl)) return;
    pl = sle_bus_pack(&p, BOARD_GATEWAY, MSG_DEVICE_CMD, j, (uint16_t)jl);
    if (pl) { errcode_t sr = sle_uart_server_send_report_by_handle((const uint8_t *)&p, pl); (void)sr; }
    jl = build_device_cmd_json(j, sizeof(j), BOARD_GATEWAY, "buzzer_arm", 0, "");
    if (!json_payload_len_valid(jl)) return;
    pl = sle_bus_pack(&p, BOARD_GATEWAY, MSG_DEVICE_CMD, j, (uint16_t)jl);
    if (pl) { errcode_t sr = sle_uart_server_send_report_by_handle((const uint8_t *)&p, pl); (void)sr; }
    mode_broadcast(MODE_HOME);
}

static void mode_fsm(uint8_t pir)
{
    uint32_t now = (uint32_t)uapi_systick_get_ms();
    if (pir) { last_pir_time = now; if (g_mode == MODE_AWAY) { g_mode = MODE_HOME; apply_home(); } }
    if (g_mode == MODE_HOME && now - last_pir_time > 30UL * 60UL * 1000UL) { g_mode = MODE_AWAY; apply_away(); }
}

/* ═══════════════ JSON 解析 ═══════════════ */
static void parse_sensor(const char *j) {
    cJSON *r = cJSON_Parse(j); if (!r) return;
    cJSON *values = cJSON_GetObjectItem(r, "values");
    cJSON *obj = cJSON_IsObject(values) ? values : r;
    cJSON *it; if ((it=cJSON_GetObjectItem(obj,"temperature"))) s_temp=(float)it->valuedouble;
    if ((it=cJSON_GetObjectItem(obj,"humidity"))) s_hum=(float)it->valuedouble;
    if ((it=cJSON_GetObjectItem(obj,"pressure"))) s_pressure=(float)it->valuedouble;
    if ((it=cJSON_GetObjectItem(obj,"light"))) s_light=(uint16_t)it->valueint;
    if ((it=cJSON_GetObjectItem(obj,"voc"))) s_voc=(uint16_t)it->valueint;
    if ((it=cJSON_GetObjectItem(obj,"eco2"))) s_eco2=(uint16_t)it->valueint;
    if ((it=cJSON_GetObjectItem(obj,"pm25"))) s_pm25=(uint16_t)it->valueint;
    cJSON_Delete(r);
}

static cJSON *json_get_alias(cJSON *obj, const char *new_key, const char *old_key)
{
    cJSON *it = cJSON_GetObjectItem(obj, new_key);
    return it ? it : cJSON_GetObjectItem(obj, old_key);
}

static uint8_t json_bool_value(cJSON *it)
{
    if (it == NULL) return 0;
    return (uint8_t)((it->type == cJSON_True || it->valueint != 0) ? 1 : 0);
}

static void parse_actuator(const char *j) {
    cJSON *r = cJSON_Parse(j); if (!r) return;
    cJSON *values = cJSON_GetObjectItem(r, "values");
    cJSON *obj = cJSON_IsObject(values) ? values : r;
    cJSON *it; if ((it=json_get_alias(obj,"led","led_brightness"))) a_led=(uint8_t)it->valueint;
    if ((it=json_get_alias(obj,"fan","fan_speed"))) a_fan=(uint8_t)it->valueint;
    if ((it=json_get_alias(obj,"relay","relay_state"))) a_relay=json_bool_value(it);
    if ((it=json_get_alias(obj,"servo","servo_angle"))) a_servo=(uint8_t)it->valueint;
    if ((it=json_get_alias(obj,"buzzer","buzzer_state"))) a_buzzer=json_bool_value(it);
    if ((it=json_get_alias(obj,"pir","pir_triggered"))) a_pir=json_bool_value(it);
    if ((it=json_get_alias(obj,"motor","motor_state")) && cJSON_IsString(it)) {
        strncpy(a_motor, it->valuestring, sizeof(a_motor) - 1);
        a_motor[sizeof(a_motor) - 1] = '\0';
    }
    cJSON_Delete(r);
}

static void build_mqtt_properties(const char *payload, char *out, uint16_t out_len)
{
    cJSON *root = cJSON_Parse(payload);
    if (root == NULL) {
        (void)snprintf(out, out_len, "%s", payload);
        return;
    }

    cJSON *values = cJSON_GetObjectItem(root, "values");
    cJSON *props = cJSON_IsObject(values) ? values : root;
    char *printed = cJSON_PrintUnformatted(props);
    if (printed != NULL) {
        (void)snprintf(out, out_len, "%s", printed);
        cJSON_free(printed);
    } else {
        (void)snprintf(out, out_len, "%s", payload);
    }
    cJSON_Delete(root);
}

/* ═══════════════ MQTT 回调 ═══════════════ */
static void normalize_device_cmd_payload(uint8_t *payload, uint16_t *payload_len, uint8_t src)
{
    cJSON *root = cJSON_Parse((const char *)payload);
    if (root == NULL) return;

    cJSON *intent = cJSON_GetObjectItem(root, "intent");
    if (intent && cJSON_IsString(intent) && strcmp(intent->valuestring, "device.set") == 0) {
        cJSON_Delete(root);
        return;
    }

    cJSON *cmd = root;
    cJSON *paras = cJSON_GetObjectItem(root, "paras");
    if (cJSON_IsObject(paras)) {
        cmd = paras;
    }

    cJSON *device = cJSON_GetObjectItem(cmd, "device");
    cJSON *value = cJSON_GetObjectItem(cmd, "value");
    cJSON *extra = cJSON_GetObjectItem(cmd, "extra");
    if (device && cJSON_IsString(device) && value) {
        char unified[SLE_BUS_PAYLOAD_MAX];
        int val = cJSON_IsNumber(value) ? value->valueint : 0;
        const char *ext = (extra && cJSON_IsString(extra)) ? extra->valuestring : "";
        int len = build_device_cmd_json(unified, sizeof(unified), src,
            device->valuestring, val, ext);
        if (json_payload_len_valid(len)) {
            (void)memcpy(payload, unified, (uint16_t)len);
            payload[len] = '\0';
            *payload_len = (uint16_t)len;
        }
    }
    cJSON_Delete(root);
}

static void on_conn_lost(void *ctx, char *cause) { (void)ctx; (void)cause; }
static void on_delivered(void *ctx, MQTTClient_deliveryToken tk) { (void)ctx; (void)tk; }

static int on_message(void *ctx, char *topic, int tlen, MQTTClient_message *msg)
{
    (void)ctx; (void)tlen; (void)topic;
    char *payload = (char *)msg->payload;
    int plen = msg->payloadlen;
    if (plen > 0) {
        /* MQTT 命令也进消息队列, 统一走 dispatch 仲裁 */
        dispatch_msg_t m;
        m.src_board   = BOARD_GATEWAY;
        m.msg_type    = MSG_DEVICE_CMD;
        m.recv_tick   = (uint32_t)uapi_systick_get_ms();
        m.payload_len = (uint16_t)(plen > SLE_BUS_PAYLOAD_MAX ? SLE_BUS_PAYLOAD_MAX : plen);
        (void)memcpy(m.payload, payload, m.payload_len);
        m.payload[m.payload_len] = '\0';
        normalize_device_cmd_payload(m.payload, &m.payload_len, BOARD_GATEWAY);
        (void)osal_msg_queue_write_copy(g_msg_queue, &m, sizeof(m), 0);
    }
    MQTTClient_freeMessage(&msg);
    MQTTClient_free(topic);
    return 1;
}

/* ═══════════════ SLE 回调 (SSAP 真实签名) ═══════════════ */
#include "sle_ssap_server.h"

static void sle_read_cb(uint8_t sid, uint16_t cid,
                        ssaps_req_read_cb_t *rcb, errcode_t st) {
    (void)sid; (void)cid; (void)rcb; (void)st;
}

static void sle_write_cb(uint8_t sid, uint16_t cid,
                         ssaps_req_write_cb_t *wcb, errcode_t st)
{
    (void)sid; (void)cid;
    if (st != ERRCODE_SUCC || wcb == NULL) return;
    if (wcb->value == NULL || wcb->length < 8) return;

    dispatch_msg_t m;
    sle_bus_packet_t *p = (sle_bus_packet_t *)wcb->value;
    if (sle_bus_validate(p, wcb->length) != 0) return;
    m.src_board = p->src_board; m.msg_type = p->msg_type;
    m.recv_tick = (uint32_t)uapi_systick_get_ms();  /* 1号板本地接收时间 */
    m.payload_len = p->payload_len > SLE_BUS_PAYLOAD_MAX
                    ? SLE_BUS_PAYLOAD_MAX : p->payload_len;
    memcpy(m.payload, p->payload, m.payload_len);
    m.payload[m.payload_len] = '\0';
    if (osal_msg_queue_write_copy(g_msg_queue, &m, sizeof(m), 0) != 0) {
        return;
    }
}

/* ═══════════════ Dispatch 任务 ═══════════════ */
static void dispatch_task(void)
{
    dispatch_msg_t m;
    for (;;) {
        unsigned int read_size = sizeof(m);
        if (osal_msg_queue_read_copy(g_msg_queue, &m, &read_size, OSAL_WAIT_FOREVER) != 0 ||
            read_size != sizeof(m)) {
            continue;
        }
        switch (m.msg_type) {
        case MSG_SENSOR_DATA:
            parse_sensor((const char *)m.payload);
            {
                char props[384];
                build_mqtt_properties((const char *)m.payload, props, sizeof(props));
                char rep[512]; snprintf(rep, sizeof(rep),
                    "{\"services\":[{\"service_id\":\"env_sensor\",\"properties\":%s}]}", props);
                MQTTClient_message msg = { .qos=QOS, .retained=0, .payload=rep, .payloadlen=(int)strlen(rep) };
                MQTTClient_deliveryToken tk;
                { int pr = MQTTClient_publishMessage(g_client, g_report_topic, &msg, &tk); (void)pr; }
            }
            compute_health();
            break;
        case MSG_ACTUATOR_STATE:
            parse_actuator((const char *)m.payload);
            {
                char props[384];
                build_mqtt_properties((const char *)m.payload, props, sizeof(props));
                char rep[512]; snprintf(rep, sizeof(rep),
                    "{\"services\":[{\"service_id\":\"actuator\",\"properties\":%s}]}", props);
                MQTTClient_message msg = { .qos=QOS, .retained=0, .payload=rep, .payloadlen=(int)strlen(rep) };
                MQTTClient_deliveryToken tk;
                { int pr = MQTTClient_publishMessage(g_client, g_report_topic, &msg, &tk); (void)pr; }
            }
            mode_fsm(a_pir);
            break;

        case MSG_DEVICE_CMD:
            /* 统一仲裁: 1号板本地接收时间 + 500ms 窗口 */
            if (!gateway_arbitrate(m.src_board, m.recv_tick)) break;
            /* 广播到所有 Client (3号板执行) */
            {
                sle_bus_packet_t p;
                uint16_t pl = sle_bus_pack(&p, m.src_board, MSG_DEVICE_CMD,
                    (const char *)m.payload, m.payload_len);
                if (pl) {
                    errcode_t sr = sle_uart_server_send_report_by_handle(
                        (const uint8_t *)&p, pl); (void)sr;
                }
            }
            /* 同时上云记录 */
            {
                char rep[512]; snprintf(rep, sizeof(rep),
                    "{\"services\":[{\"service_id\":\"command_log\",\"properties\":%s}]}",
                    m.payload);
                MQTTClient_message msg = { .qos=QOS, .retained=0,
                    .payload=rep, .payloadlen=(int)strlen(rep) };
                MQTTClient_deliveryToken tk;
                { int pr = MQTTClient_publishMessage(g_client, g_report_topic, &msg, &tk); (void)pr; }
            }
            break;

        case MSG_AI_SUGGESTION:
            /* AI 建议: 广播到所有 Client (4号板收 → CI1302 TTS 播报) */
            {
                sle_bus_packet_t p;
                uint16_t pl = sle_bus_pack(&p, m.src_board, MSG_AI_SUGGESTION,
                    (const char *)m.payload, m.payload_len);
                if (pl) {
                    errcode_t sr = sle_uart_server_send_report_by_handle(
                        (const uint8_t *)&p, pl); (void)sr;
                }
            }
            break;

        case MSG_ALERT:
            {
                char props[384];
                build_mqtt_properties((const char *)m.payload, props, sizeof(props));
                char rep[512]; snprintf(rep, sizeof(rep),
                    "{\"services\":[{\"service_id\":\"alert\",\"properties\":%s}]}", props);
                MQTTClient_message msg = { .qos=QOS, .retained=0, .payload=rep, .payloadlen=(int)strlen(rep) };
                MQTTClient_deliveryToken tk;
                { int pr = MQTTClient_publishMessage(g_client, g_report_topic, &msg, &tk); (void)pr; }
            }
            break;

        case MSG_DEVICE_ACK:
            {
                sle_bus_packet_t p;
                uint16_t pl = sle_bus_pack(&p, m.src_board, MSG_DEVICE_ACK,
                    (const char *)m.payload, m.payload_len);
                if (pl) {
                    errcode_t sr = sle_uart_server_send_report_by_handle(
                        (const uint8_t *)&p, pl); (void)sr;
                }
            }
            {
                char props[384];
                build_mqtt_properties((const char *)m.payload, props, sizeof(props));
                char rep[512]; snprintf(rep, sizeof(rep),
                    "{\"services\":[{\"service_id\":\"device_ack\",\"properties\":%s}]}", props);
                MQTTClient_message msg = { .qos=QOS, .retained=0, .payload=rep, .payloadlen=(int)strlen(rep) };
                MQTTClient_deliveryToken tk;
                { int pr = MQTTClient_publishMessage(g_client, g_report_topic, &msg, &tk); (void)pr; }
            }
            break;
        }
    }
}

/* ═══════════════ 诊断定时任务 ═══════════════ */
static void diag_task(void) { osal_msleep(30000); for (;;) { run_diag(); osal_msleep(60000); } }

/* ═══════════════ 入口 ═══════════════ */
static void gateway_entry(void)
{
    /* WiFi */
    wifi_connect(WIFI_SSID, WIFI_PWD);

    /* MQTT */ {
        MQTTClient_init();
        MQTTClient_create(&g_client, MQTT_ADDRESS, MQTT_CLIENTID, MQTTCLIENT_PERSISTENCE_NONE, NULL);
        MQTTClient_setCallbacks(g_client, NULL, on_conn_lost, on_message, on_delivered);
        MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
        opts.keepAliveInterval = KEEPALIVE; opts.cleansession = 1;
        opts.username = MQTT_USERNAME; opts.password = MQTT_PASSWORD;
        MQTTClient_connect(g_client, &opts);
        snprintf(g_cmd_topic, sizeof(g_cmd_topic),
            "$oc/devices/%s/sys/commands/#", DEVICE_ID);
        MQTTClient_subscribe(g_client, g_cmd_topic, QOS);
        snprintf(g_report_topic, sizeof(g_report_topic),
            "$oc/devices/%s/sys/properties/report", DEVICE_ID);
    }

    /* 消息队列 */
    if (osal_msg_queue_create("gw_queue", MSG_QUEUE_LEN, &g_msg_queue, 0, MSG_QUEUE_SIZE) != 0) {
        return;
    }

    /* SLE Server */ {
        sle_uart_server_init(sle_read_cb, sle_write_cb);
        sle_uart_server_adv_init();
    }

    /* 任务 */
    osal_kthread_create((osal_kthread_handler)dispatch_task, 0, "dispatch", 8192);
    osal_kthread_create((osal_kthread_handler)diag_task, 0, "diag", 4096);
}

static void run_gateway(void) { gateway_entry(); }
app_run(run_gateway);
