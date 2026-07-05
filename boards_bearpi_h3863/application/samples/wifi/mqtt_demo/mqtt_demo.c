#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "MQTTClient.h"
#include "app_init.h"
#include "soc_osal.h"
#include "cmsis_os2.h"
#include "osal_debug.h"
#include "wifi_connect.h"
#include "mqtt_demo.h"

/* SLE Server 头文件 */
#include "sle_uart_server.h"
#include "sle_uart_server_adv.h"
#include "sle_device_discovery.h"
#include "sle_errcode.h"
#include "sle_connection_manager.h"

/* ====== 用户配置（修改为你的华为云IoT设备信息）====== */
#define WIFI_SSID       "YOUR_WIFI_SSID"
#define WIFI_PWD        "YOUR_WIFI_PASSWORD"

#define MQTT_ADDRESS    "tcp://YOUR_IOTDA_MQTT_ENDPOINT:1883"
#define MQTT_CLIENTID   "YOUR_MQTT_CLIENT_ID"
#define MQTT_USERNAME   "YOUR_MQTT_USERNAME"
#define MQTT_PASSWORD   "YOUR_MQTT_PASSWORD"

#define DEVICE_ID       "YOUR_DEVICE_ID"
/* ====== 用户配置结束 ====== */

#define QOS                 1
#define KEEPALIVE           120
#define MQTT_TASK_PRIO      24
#define MQTT_TASK_STACK     0x2000
#define DISPATCH_TASK_PRIO  23
#define DISPATCH_TASK_STACK 0x2000
#define SLE_TASK_PRIO       26
#define SLE_TASK_STACK      0x2000

#define MSG_QUEUE_LEN       16
#define MSG_QUEUE_MAX_SIZE  sizeof(mqtt_msg_t)

extern int MQTTClient_init(void);

/* ── 全局变量 ── */
static MQTTClient    g_client;
static unsigned long g_msg_queue;
static char g_cmd_topic[128];
static char g_cmd_resp_topic_fmt[128];
static char g_report_topic[128];

/* SLE 状态 */
static volatile bool g_sle_ready = false;
static volatile int  g_sle_connected_board = 0; /* 0=无连接, 2=2号板, 3=3号板 */

/* ================================================================
 *  SLE 回调 — 接收检测板/执行板发来的数据
 * ================================================================ */

static void ssaps_server_read_request_cbk(uint8_t server_id, uint16_t conn_id,
    ssaps_req_read_cb_t *read_cb_para, errcode_t status)
{
    printf("[SLE] read request server:%x conn:%x handle:%x status:%x\r\n",
           server_id, conn_id, read_cb_para->handle, status);
}

/*
 * SLE 客户端写入回调 — 2号/3号通过 SLE 发送数据到这里
 * 在这里解析数据 → 构造 mqtt_msg_t → 写入消息队列
 */
static void ssaps_server_write_request_cbk(uint8_t server_id, uint16_t conn_id,
    ssaps_req_write_cb_t *write_cb_para, errcode_t status)
{
    unused(server_id);
    unused(status);

    if (write_cb_para == NULL || write_cb_para->value == NULL ||
        write_cb_para->length == 0) {
        return;
    }

    char *json = (char *)write_cb_para->value;
    printf("[SLE] recv from conn:%x, len:%d\r\n", conn_id, write_cb_para->length);
    printf("[SLE] data: %s\r\n", json);

    mqtt_msg_t *msg = osal_kmalloc(sizeof(mqtt_msg_t), 0);
    if (msg == NULL) {
        return;
    }
    memset_s(msg, sizeof(mqtt_msg_t), 0, sizeof(mqtt_msg_t));

    /* 尝试解析 board 字段判断来源 */
    char *board_ptr = strstr(json, "\"board\"");
    if (board_ptr != NULL) {
        int board_id = 0;
        if (sscanf_s(board_ptr, "\"board\":%d", &board_id) == 1) {
            msg->source = (board_id == 2) ? MSG_FROM_BOARD2 :
                          (board_id == 3) ? MSG_FROM_BOARD3 : 0;
            g_sle_connected_board = board_id;
        }
    }

    /* 尝试解析 type 字段判断消息类型 */
    char *type_ptr = strstr(json, "\"type\"");
    if (type_ptr != NULL && strstr(type_ptr, "\"state\"") != NULL) {
        /* 3号执行板状态回传 */
        msg->type = MSG_TYPE_ACTUATOR_STATE;
        actuator_state_t *s = &msg->data.state;
        sscanf_s(json,
            "{\"board\":%*d,\"type\":\"state\","
            "\"led\":%d,\"fan\":%d,\"motor\":\"%[^\"]\","
            "\"relay\":%d,\"servo\":%d,\"buzzer\":%d,\"pir\":%d}",
            &s->led_brightness, &s->fan_speed, s->motor_state,
            (int *)&s->relay_state, &s->servo_angle,
            (int *)&s->buzzer_state, (int *)&s->pir_triggered);
        printf("[SLE] -> actuator state: led=%d fan=%d motor=%s relay=%d\r\n",
               s->led_brightness, s->fan_speed, s->motor_state, s->relay_state);
    } else {
        /* 默认当作传感器数据（2号检测板） */
        msg->type = MSG_TYPE_SENSOR_REPORT;
        sensor_data_t *d = &msg->data.sensor;
        int ret = sscanf_s(json,
            "{\"board\":%*d,\"temp\":%f,\"humi\":%f,"
            "\"press\":%f,\"light\":%d,\"voc\":%d,\"eco2\":%d,\"pm25\":%d}",
            &d->temperature, &d->humidity, &d->pressure,
            &d->light, &d->voc, &d->eco2, &d->pm25);
        if (ret >= 2) {
            printf("[SLE] -> sensor: temp=%.1f humi=%.1f press=%.1f "
                   "light=%d voc=%d eco2=%d pm25=%d\r\n",
                   d->temperature, d->humidity, d->pressure,
                   d->light, d->voc, d->eco2, d->pm25);
        } else {
            /* 未识别的格式，存原始 JSON */
            msg->type = MSG_TYPE_SENSOR_REPORT;
            strncpy_s(msg->data.raw_json, sizeof(msg->data.raw_json), json,
                      sizeof(msg->data.raw_json) - 1);
            printf("[SLE] -> raw json saved\r\n");
        }
    }

    if (g_sle_connected_board == 0) {
        /* 无法识别来源，丢弃 */
        osal_kfree(msg);
        return;
    }

    uint32_t ret = osal_msg_queue_write_copy(g_msg_queue, msg,
                                              sizeof(mqtt_msg_t), 0);
    if (ret != OSAL_SUCCESS) {
        printf("[SLE] enqueue failed: 0x%x\r\n", ret);
        osal_kfree(msg);
    }
}

/* ================================================================
 *  MQTT 回调
 * ================================================================ */

static void on_delivered(void *context, MQTTClient_deliveryToken dt)
{
    unused(context);
    printf("[MQTT] delivered, token=%d\r\n", dt);
}

/*
 * 解析华为云 command_name → cmd_type_t 枚举
 */
static cmd_type_t parse_command_name(const char *name)
{
    if (strcmp(name, "set_led")    == 0) return CMD_SET_LED;
    if (strcmp(name, "set_fan")    == 0) return CMD_SET_FAN;
    if (strcmp(name, "set_motor")  == 0) return CMD_SET_MOTOR;
    if (strcmp(name, "set_relay")  == 0) return CMD_SET_RELAY;
    if (strcmp(name, "set_servo")  == 0) return CMD_SET_SERVO;
    if (strcmp(name, "set_buzzer") == 0) return CMD_SET_BUZZER;
    if (strcmp(name, "send_ir")    == 0) return CMD_SEND_IR;
    return (cmd_type_t)0;
}

/*
 * 解析华为云命令的 paras JSON → actuator_cmd_t.params
 */
static void parse_command_paras(cmd_type_t cmd, const char *paras_json,
                                actuator_cmd_t *out)
{
    out->cmd = cmd;
    switch (cmd) {
    case CMD_SET_LED:
        sscanf_s(paras_json, "\"brightness\":%d", &out->params.brightness);
        break;
    case CMD_SET_FAN:
        sscanf_s(paras_json, "\"speed\":%d", &out->params.speed);
        break;
    case CMD_SET_MOTOR:
        sscanf_s(paras_json, "\"direction\":\"%[^\"]\"",
                 out->params.direction, (unsigned int)sizeof(out->params.direction));
        break;
    case CMD_SET_RELAY:
        out->params.relay_state =
            (strstr(paras_json, "\"state\":true") != NULL);
        break;
    case CMD_SET_SERVO:
        sscanf_s(paras_json, "\"angle\":%d", &out->params.angle);
        break;
    case CMD_SET_BUZZER:
        out->params.buzzer_state =
            (strstr(paras_json, "\"state\":true") != NULL);
        break;
    case CMD_SEND_IR:
        sscanf_s(paras_json, "\"ir_code\":\"%[^\"]\"",
                 out->params.ir_code, (unsigned int)sizeof(out->params.ir_code));
        break;
    default:
        break;
    }
}

/*
 * 回复华为云命令响应（必须回复，否则云端超时）
 */
static void send_command_response(const char *request_id)
{
    char topic[256];
    int ret;

    ret = snprintf_s(topic, sizeof(topic), sizeof(topic) - 1,
                     g_cmd_resp_topic_fmt, request_id);
    if (ret <= 0) {
        return;
    }

    const char *payload = "{\"result_code\":0,"
                          "\"response_name\":\"COMMAND_RESPONSE\","
                          "\"paras\":{\"result\":\"success\"}}";

    MQTTClient_message msg = MQTTClient_message_initializer;
    MQTTClient_deliveryToken token;
    msg.payload    = (void *)payload;
    msg.payloadlen = (int)strlen(payload);
    msg.qos        = QOS;
    msg.retained   = 0;

    MQTTClient_publishMessage(g_client, topic, &msg, &token);
    printf("[MQTT] cmd response sent -> %s\r\n", topic);
}

/*
 * 收到华为云下发的命令
 */
static int on_message(void *context, char *topic, int topic_len,
                      MQTTClient_message *msg)
{
    unused(context);

    char *payload = (char *)osal_kmalloc(msg->payloadlen + 1, 0);
    if (payload == NULL) {
        MQTTClient_freeMessage(&msg);
        MQTTClient_free(topic);
        return -1;
    }
    memcpy_s(payload, msg->payloadlen + 1, msg->payload, msg->payloadlen);
    payload[msg->payloadlen] = '\0';

    printf("\r\n========== [MQTT] 云端命令 ==========\r\n");
    printf("  topic:   %.*s\r\n", topic_len, topic);
    printf("  payload: %s\r\n", payload);
    printf("======================================\r\n\r\n");

    /* 提取 request_id（从 topic 中） */
    char request_id[64] = {0};
    char *req_pos = strstr(topic, "request_id=");
    if (req_pos != NULL) {
        strncpy_s(request_id, sizeof(request_id), req_pos + strlen("request_id="),
                  sizeof(request_id) - 1);
    }

    /* 提取 command_name */
    char cmd_name[32] = {0};
    char *cmd_pos = strstr(payload, "\"command_name\"");
    if (cmd_pos != NULL) {
        sscanf_s(cmd_pos, "\"command_name\":\"%[^\"]\"",
                 cmd_name, (unsigned int)sizeof(cmd_name));
    }

    cmd_type_t cmd = parse_command_name(cmd_name);
    if (cmd == 0) {
        printf("[MQTT] unknown command: %s\r\n", cmd_name);
        /* 仍然回复失败响应 */
        goto cleanup;
    }

    /* 提取 paras */
    char *paras_start = strstr(payload, "\"paras\"");
    if (paras_start == NULL) {
        printf("[MQTT] no paras found\r\n");
        goto cleanup;
    }

    /* 构造消息入队 */
    mqtt_msg_t *mqtt_msg = osal_kmalloc(sizeof(mqtt_msg_t), 0);
    if (mqtt_msg == NULL) {
        goto cleanup;
    }
    memset_s(mqtt_msg, sizeof(mqtt_msg_t), 0, sizeof(mqtt_msg_t));

    mqtt_msg->source = MSG_FROM_CLOUD;
    mqtt_msg->type   = MSG_TYPE_ACTUATOR_CMD;
    strncpy_s(mqtt_msg->request_id, sizeof(mqtt_msg->request_id), request_id,
              sizeof(mqtt_msg->request_id) - 1);
    parse_command_paras(cmd, paras_start, &mqtt_msg->data.cmd);

    printf("[MQTT] enqueue: cmd=%d request_id=%s\r\n", cmd, request_id);

    uint32_t ret = osal_msg_queue_write_copy(g_msg_queue, mqtt_msg,
                                              sizeof(mqtt_msg_t), 0);
    if (ret != OSAL_SUCCESS) {
        printf("[MQTT] enqueue failed: 0x%x\r\n", ret);
        osal_kfree(mqtt_msg);
    }

cleanup:
    /* 回复华为云命令响应 */
    send_command_response(request_id);
    osal_kfree(payload);
    MQTTClient_freeMessage(&msg);
    MQTTClient_free(topic);
    return 1;
}

static void on_lost(void *context, char *cause)
{
    unused(context);
    printf("[MQTT] 连接断开: %s\r\n", cause);
}

/* ================================================================
 *  MQTT 操作函数
 * ================================================================ */

static int mqtt_connect(void)
{
    MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
    int rc;

    MQTTClient_init();

    rc = MQTTClient_create(&g_client, MQTT_ADDRESS, MQTT_CLIENTID,
                           MQTTCLIENT_PERSISTENCE_NONE, NULL);
    if (rc != MQTTCLIENT_SUCCESS) {
        printf("[MQTT] create failed, rc=%d\r\n", rc);
        return -1;
    }

    MQTTClient_setCallbacks(g_client, NULL, on_lost, on_message, on_delivered);

    opts.keepAliveInterval = KEEPALIVE;
    opts.cleansession = 1;
    opts.username = MQTT_USERNAME;
    opts.password = MQTT_PASSWORD;

    rc = MQTTClient_connect(g_client, &opts);
    if (rc != MQTTCLIENT_SUCCESS) {
        printf("[MQTT] connect failed, rc=%d\r\n", rc);
        return -1;
    }
    printf("[MQTT] 华为云IoT连接成功!\r\n");
    return 0;
}

static int mqtt_subscribe(const char *topic)
{
    int rc = MQTTClient_subscribe(g_client, topic, QOS);
    if (rc != MQTTCLIENT_SUCCESS) {
        printf("[MQTT] subscribe failed: %s, rc=%d\r\n", topic, rc);
        return -1;
    }
    printf("[MQTT] 已订阅: %s\r\n", topic);
    return 0;
}

static int mqtt_publish(const char *topic, const char *payload)
{
    MQTTClient_message pub_msg = MQTTClient_message_initializer;
    MQTTClient_deliveryToken token;
    int rc;

    pub_msg.payload    = (void *)payload;
    pub_msg.payloadlen = (int)strlen(payload);
    pub_msg.qos        = QOS;
    pub_msg.retained   = 0;

    rc = MQTTClient_publishMessage(g_client, topic, &pub_msg, &token);
    if (rc != MQTTCLIENT_SUCCESS) {
        printf("[MQTT] publish failed, rc=%d\r\n", rc);
        return -1;
    }
    printf("[MQTT] >>> %s\r\n", topic);
    printf("[MQTT] >>> %s\r\n", payload);
    return 0;
}

/* ================================================================
 *  属性上报函数
 * ================================================================ */

/*
 * 上报环境数据到华为云（environment 服务）
 */
static int report_sensor_data(const sensor_data_t *data)
{
    char payload[512];
    int ret;

    ret = snprintf_s(payload, sizeof(payload), sizeof(payload) - 1,
        "{\"services\":[{\"service_id\":\"environment\","
        "\"properties\":{"
        "\"temperature\":%.1f,\"humidity\":%.1f,"
        "\"pressure\":%.1f,\"light\":%d,"
        "\"voc\":%d,\"eco2\":%d,\"pm25\":%d}}]}",
        data->temperature, data->humidity,
        data->pressure, data->light,
        data->voc, data->eco2, data->pm25);
    if (ret <= 0) {
        return -1;
    }

    return mqtt_publish(g_report_topic, payload);
}

/*
 * 上报执行器状态到华为云（actuator 服务）
 */
static int report_actuator_state(const actuator_state_t *state)
{
    char payload[512];
    int ret;

    ret = snprintf_s(payload, sizeof(payload), sizeof(payload) - 1,
        "{\"services\":[{\"service_id\":\"actuator\","
        "\"properties\":{"
        "\"led_brightness\":%d,\"fan_speed\":%d,"
        "\"motor_state\":\"%s\",\"relay_state\":%s,"
        "\"servo_angle\":%d,\"buzzer_state\":%s,"
        "\"pir_triggered\":%s}}]}",
        state->led_brightness, state->fan_speed,
        state->motor_state,
        state->relay_state ? "true" : "false",
        state->servo_angle,
        state->buzzer_state ? "true" : "false",
        state->pir_triggered ? "true" : "false");
    if (ret <= 0) {
        return -1;
    }

    return mqtt_publish(g_report_topic, payload);
}

/* ================================================================
 *  SLE 发送 — 将命令转发给 3号执行板
 * ================================================================ */

static void send_to_board3(const actuator_cmd_t *cmd)
{
    if (!g_sle_ready || g_sle_connected_board != 3) {
        printf("[SLE] 3号板未连接, 命令丢弃 (connected=%d)\r\n",
               g_sle_connected_board);
        return;
    }

    char json[256];
    int len;

    switch (cmd->cmd) {
    case CMD_SET_LED:
        len = snprintf_s(json, sizeof(json), sizeof(json) - 1,
            "{\"type\":\"cmd\",\"cmd\":\"set_led\",\"brightness\":%d}",
            cmd->params.brightness);
        break;
    case CMD_SET_FAN:
        len = snprintf_s(json, sizeof(json), sizeof(json) - 1,
            "{\"type\":\"cmd\",\"cmd\":\"set_fan\",\"speed\":%d}",
            cmd->params.speed);
        break;
    case CMD_SET_MOTOR:
        len = snprintf_s(json, sizeof(json), sizeof(json) - 1,
            "{\"type\":\"cmd\",\"cmd\":\"set_motor\",\"direction\":\"%s\"}",
            cmd->params.direction);
        break;
    case CMD_SET_RELAY:
        len = snprintf_s(json, sizeof(json), sizeof(json) - 1,
            "{\"type\":\"cmd\",\"cmd\":\"set_relay\",\"state\":%s}",
            cmd->params.relay_state ? "true" : "false");
        break;
    case CMD_SET_SERVO:
        len = snprintf_s(json, sizeof(json), sizeof(json) - 1,
            "{\"type\":\"cmd\",\"cmd\":\"set_servo\",\"angle\":%d}",
            cmd->params.angle);
        break;
    case CMD_SET_BUZZER:
        len = snprintf_s(json, sizeof(json), sizeof(json) - 1,
            "{\"type\":\"cmd\",\"cmd\":\"set_buzzer\",\"state\":%s}",
            cmd->params.buzzer_state ? "true" : "false");
        break;
    case CMD_SEND_IR:
        len = snprintf_s(json, sizeof(json), sizeof(json) - 1,
            "{\"type\":\"cmd\",\"cmd\":\"send_ir\",\"ir_code\":\"%s\"}",
            cmd->params.ir_code);
        break;
    default:
        printf("[SLE] unknown cmd: %d\r\n", cmd->cmd);
        return;
    }

    if (len <= 0 || len >= (int)sizeof(json)) {
        return;
    }

    errcode_t ret = sle_uart_server_send_report_by_handle((uint8_t *)json,
                                                          (uint16_t)len);
    if (ret != ERRCODE_SLE_SUCCESS) {
        printf("[SLE] send to board3 failed: 0x%x\r\n", ret);
    } else {
        printf("[SLE] >>> to board3: %s\r\n", json);
    }
}

/* ================================================================
 *  消息分发任务 — 从队列取消息，按类型路由
 * ================================================================ */

static void *dispatch_task(const char *param)
{
    unused(param);

    mqtt_msg_t *msg = osal_kmalloc(sizeof(mqtt_msg_t), 0);
    if (msg == NULL) {
        printf("[DISPATCH] malloc failed\r\n");
        return;
    }

    while (1) {
        uint32_t size = sizeof(mqtt_msg_t);
        uint32_t ret = osal_msg_queue_read_copy(g_msg_queue, msg, &size,
                                                 OSAL_WAIT_FOREVER);
        if (ret != OSAL_SUCCESS) {
            printf("[DISPATCH] queue read failed: 0x%x\r\n", ret);
            osDelay(100);
            continue;
        }

        printf("[DISPATCH] msg: source=%d type=%d\r\n", msg->source, msg->type);

        switch (msg->type) {
        case MSG_TYPE_SENSOR_REPORT:
            /* 2号检测板的环境数据 → 上报到华为云 */
            printf("[DISPATCH] -> report sensor data to cloud\r\n");
            report_sensor_data(&msg->data.sensor);
            break;

        case MSG_TYPE_ACTUATOR_CMD:
            /* 华为云下发的命令 → 转发给3号执行板 */
            printf("[DISPATCH] -> forward cmd to board3\r\n");
            send_to_board3(&msg->data.cmd);
            break;

        case MSG_TYPE_ACTUATOR_STATE:
            /* 3号执行板状态回传 → 上报到华为云 */
            printf("[DISPATCH] -> report actuator state to cloud\r\n");
            report_actuator_state(&msg->data.state);
            break;

        default:
            printf("[DISPATCH] unknown msg type: %d\r\n", msg->type);
            break;
        }

        osal_kfree(msg);
        msg = osal_kmalloc(sizeof(mqtt_msg_t), 0);
        if (msg == NULL) {
            printf("[DISPATCH] re-malloc failed, exit\r\n");
            break;
        }
    }
    return NULL;
}

/* ================================================================
 *  SLE 初始化任务
 * ================================================================ */

static void *sle_init_task(const char *param)
{
    unused(param);

    printf("[SLE] initializing SLE server...\r\n");

    errcode_t ret = sle_uart_server_init(ssaps_server_read_request_cbk,
                                          ssaps_server_write_request_cbk);
    if (ret != ERRCODE_SLE_SUCCESS) {
        printf("[SLE] init failed: 0x%x\r\n", ret);
        return NULL;
    }

    g_sle_ready = true;
    printf("[SLE] server ready, waiting for client connections...\r\n");

    /* 保持任务存活，检测连接状态变化 */
    while (1) {
        if (!sle_uart_client_is_connected()) {
            g_sle_connected_board = 0;
        }
        osDelay(500);
    }
    return NULL;
}

/* ================================================================
 *  MQTT 主任务
 * ================================================================ */

static void *mqtt_demo_task(const char *param)
{
    unused(param);

    /* 1. 连接 Wi-Fi */
    printf("[MQTT] 正在连接 Wi-Fi: %s ...\r\n", WIFI_SSID);
    if (wifi_connect(WIFI_SSID, WIFI_PWD) != 0) {
        printf("[MQTT] Wi-Fi 连接失败!\r\n");
        return;
    }
    printf("[MQTT] Wi-Fi 连接成功, 已获取 IP\r\n");

    /* 2. 连接华为云 IoT MQTT */
    if (mqtt_connect() != 0) {
        printf("[MQTT] MQTT 连接失败!\r\n");
        return;
    }

    /* 3. 构建 Topic */
    snprintf_s(g_cmd_topic, sizeof(g_cmd_topic), sizeof(g_cmd_topic) - 1,
               "$oc/devices/%s/sys/commands/#", DEVICE_ID);
    snprintf_s(g_cmd_resp_topic_fmt, sizeof(g_cmd_resp_topic_fmt),
               sizeof(g_cmd_resp_topic_fmt) - 1,
               "$oc/devices/%s/sys/commands/response/request_id=%%s", DEVICE_ID);
    snprintf_s(g_report_topic, sizeof(g_report_topic), sizeof(g_report_topic) - 1,
               "$oc/devices/%s/sys/properties/report", DEVICE_ID);

    /* 4. 订阅命令下发 topic */
    mqtt_subscribe(g_cmd_topic);

    /* 5. 上报设备在线 */
    {
        const char *online = "{\"services\":[{\"service_id\":\"actuator\","
                             "\"properties\":{\"relay_state\":false}}]}";
        mqtt_publish(g_report_topic, online);
    }

    /* 6. 主循环 */
    int heartbeat = 0;
    while (1) {
        osDelay(500);  /* 5秒 */
        heartbeat++;

        /* 每 60 秒打印一次心跳 */
        if (heartbeat % 12 == 0) {
            printf("[MQTT] heartbeat, sle_ready=%d sle_board=%d\r\n",
                   g_sle_ready, g_sle_connected_board);
        }
    }
    return NULL;
}

/* ================================================================
 *  入口
 * ================================================================ */

static void mqtt_demo_entry(void)
{
    uint32_t ret;

    /* 创建消息队列 */
    ret = osal_msg_queue_create("mqtt_msg_queue", MSG_QUEUE_LEN,
                                &g_msg_queue, 0, MSG_QUEUE_MAX_SIZE);
    if (ret != OSAL_SUCCESS) {
        printf("[GATEWAY] create msg queue failed: 0x%x\r\n", ret);
        return;
    }
    printf("[GATEWAY] msg queue created, id=%lu\r\n", g_msg_queue);

    osal_kthread_lock();

    /* MQTT 主任务 */
    osal_task *mqtt_handle = osal_kthread_create(
        (osal_kthread_handler)mqtt_demo_task, 0, "MqttTask",
        MQTT_TASK_STACK);
    if (mqtt_handle != NULL) {
        osal_kthread_set_priority(mqtt_handle, MQTT_TASK_PRIO);
        osal_kfree(mqtt_handle);
    }

    /* 消息分发任务 */
    osal_task *dispatch_handle = osal_kthread_create(
        (osal_kthread_handler)dispatch_task, 0, "DispatchTask",
        DISPATCH_TASK_STACK);
    if (dispatch_handle != NULL) {
        osal_kthread_set_priority(dispatch_handle, DISPATCH_TASK_PRIO);
        osal_kfree(dispatch_handle);
    }

    /* SLE 初始化任务 */
    osal_task *sle_handle = osal_kthread_create(
        (osal_kthread_handler)sle_init_task, 0, "SleInitTask",
        SLE_TASK_STACK);
    if (sle_handle != NULL) {
        osal_kthread_set_priority(sle_handle, SLE_TASK_PRIO);
        osal_kfree(sle_handle);
    }

    osal_kthread_unlock();

    printf("[GATEWAY] all tasks created\r\n");
}

app_run(mqtt_demo_entry);
