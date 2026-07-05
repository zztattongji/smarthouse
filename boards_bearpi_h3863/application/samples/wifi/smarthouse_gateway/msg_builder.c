/**
 * @file    msg_builder.c
 * @brief   消息 JSON 构造实现
 */
#include "msg_builder.h"
#include "sle_bus.h"
#include <stdio.h>
#include <string.h>

#define JSON_SCHEMA "smarthome.v1"

static uint32_t g_msg_id;

static const char *cmd_property(const char *device)
{
    if (device == NULL) return "value";
    if (strcmp(device, "fan") == 0) return "speed";
    if (strcmp(device, "led") == 0) return "brightness";
    if (strcmp(device, "motor") == 0) return "state";
    if (strcmp(device, "servo") == 0) return "angle";
    if (strcmp(device, "relay") == 0 ||
        strcmp(device, "buzzer") == 0 ||
        strcmp(device, "buzzer_arm") == 0) return "power";
    if (strcmp(device, "ir") == 0) return "code";
    if (strcmp(device, "refresh_sensors") == 0) return "refresh";
    return "value";
}

static uint8_t cmd_target(const char *device)
{
    if (device != NULL && strcmp(device, "refresh_sensors") == 0) {
        return BOARD_SENSOR;
    }
    return BOARD_ACTUATOR;
}

int build_sensor_json(char *buf, uint16_t buf_size,
    float temperature, float humidity, float pressure,
    uint16_t light, uint16_t voc, uint16_t eco2, uint16_t pm25)
{
    return snprintf(buf, buf_size,
        "{\"schema\":\"%s\",\"source\":%u,\"target\":%u,\"intent\":\"sensor.report\","
        "\"values\":{\"temperature\":%.1f,\"humidity\":%.1f,\"pressure\":%.1f,"
        "\"light\":%u,\"voc\":%u,\"eco2\":%u,\"pm25\":%u}}",
        JSON_SCHEMA, BOARD_SENSOR, BOARD_GATEWAY,
        temperature, humidity, pressure,
        light, voc, eco2, pm25);
}

int build_actuator_state_json(char *buf, uint16_t buf_size,
    uint8_t led, uint8_t fan, const char *motor,
    uint8_t relay, uint8_t servo, uint8_t buzzer, uint8_t pir)
{
    return snprintf(buf, buf_size,
        "{\"schema\":\"%s\",\"source\":%u,\"target\":%u,\"intent\":\"actuator.report\","
        "\"values\":{\"led\":%u,\"fan\":%u,\"motor\":\"%s\","
        "\"relay\":%s,\"servo\":%u,\"buzzer\":%s,\"pir\":%s}}",
        JSON_SCHEMA, BOARD_ACTUATOR, BOARD_GATEWAY,
        led, fan, motor,
        relay ? "true" : "false", servo,
        buzzer ? "true" : "false",
        pir ? "true" : "false");
}

int build_device_cmd_json(char *buf, uint16_t buf_size,
    uint8_t src_board, const char *device, int value, const char *extra)
{
    const char *dev = (device != NULL) ? device : "";
    const char *prop = cmd_property(dev);
    uint8_t target = cmd_target(dev);
    if (extra != NULL && extra[0] != '\0') {
        return snprintf(buf, buf_size,
            "{\"schema\":\"%s\",\"msg_id\":%lu,\"source\":%u,\"target\":%u,\"intent\":\"device.set\","
            "\"device\":\"%s\",\"property\":\"%s\",\"value\":%d,\"extra\":\"%s\"}",
            JSON_SCHEMA, (unsigned long)++g_msg_id, src_board, target, dev, prop, value, extra);
    }
    return snprintf(buf, buf_size,
        "{\"schema\":\"%s\",\"msg_id\":%lu,\"source\":%u,\"target\":%u,\"intent\":\"device.set\","
        "\"device\":\"%s\",\"property\":\"%s\",\"value\":%d}",
        JSON_SCHEMA, (unsigned long)++g_msg_id, src_board, target, dev, prop, value);
}

int build_device_ack_json(char *buf, uint16_t buf_size, uint32_t msg_id,
    uint8_t target_board, const char *device, uint8_t ok, const char *state)
{
    return snprintf(buf, buf_size,
        "{\"schema\":\"%s\",\"msg_id\":%lu,\"source\":%u,\"target\":%u,"
        "\"intent\":\"device.ack\",\"values\":{\"msg_id\":%lu,\"device\":\"%s\",\"ok\":%s,\"state\":\"%s\"}}",
        JSON_SCHEMA, (unsigned long)msg_id, BOARD_ACTUATOR, target_board,
        (unsigned long)msg_id, device != NULL ? device : "", ok ? "true" : "false",
        state != NULL ? state : "");
}

int build_health_score_json(char *buf, uint16_t buf_size,
    uint8_t score, const char *reason)
{
    return snprintf(buf, buf_size,
        "{\"schema\":\"%s\",\"source\":%u,\"target\":0,\"intent\":\"health.report\","
        "\"values\":{\"score\":%u,\"reason\":\"%s\"}}",
        JSON_SCHEMA, BOARD_GATEWAY, score, reason);
}

int build_mode_change_json(char *buf, uint16_t buf_size, uint8_t mode)
{
    return snprintf(buf, buf_size,
        "{\"schema\":\"%s\",\"source\":%u,\"target\":0,\"intent\":\"mode.change\","
        "\"values\":{\"mode\":\"%s\"}}",
        JSON_SCHEMA, BOARD_GATEWAY, mode == 0 ? "HOME" : "AWAY");
}

int build_alert_json(char *buf, uint16_t buf_size,
    const char *title, const char *detail)
{
    return snprintf(buf, buf_size,
        "{\"schema\":\"%s\",\"source\":%u,\"target\":0,\"intent\":\"alert.report\","
        "\"values\":{\"title\":\"%s\",\"detail\":\"%s\"}}",
        JSON_SCHEMA, BOARD_GATEWAY, title, detail);
}

int build_ai_suggestion_json(char *buf, uint16_t buf_size, const char *text)
{
    return snprintf(buf, buf_size,
        "{\"schema\":\"%s\",\"source\":%u,\"target\":%u,\"intent\":\"ai.suggestion\","
        "\"text\":\"%s\"}",
        JSON_SCHEMA, BOARD_AI, BOARD_GATEWAY, text);
}

int build_heartbeat_json(char *buf, uint16_t buf_size, uint8_t board_id)
{
    return snprintf(buf, buf_size,
        "{\"schema\":\"%s\",\"source\":%u,\"target\":%u,\"intent\":\"heartbeat\","
        "\"status\":\"alive\"}",
        JSON_SCHEMA, board_id, BOARD_GATEWAY);
}
