/**
 * @file    msg_builder.h
 * @brief   构造各类型 SLE 总线消息的 JSON 负载
 *
 * 所有函数将传感器/执行器/命令数据序列化为 JSON 字符串，
 * 填入 buf，返回写入字节数（不含 '\0'）。
 */
#ifndef MSG_BUILDER_H
#define MSG_BUILDER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 传感器数据 (2号→广播) ── */
int build_sensor_json(char *buf, uint16_t buf_size,
    float temperature, float humidity, float pressure,
    uint16_t light, uint16_t voc, uint16_t eco2, uint16_t pm25);

/* ── 执行器状态 (3号→广播) ── */
int build_actuator_state_json(char *buf, uint16_t buf_size,
    uint8_t led, uint8_t fan, const char *motor,
    uint8_t relay, uint8_t servo, uint8_t buzzer, uint8_t pir);

/* ── 设备控制命令 (1/4→3号) ── */
int build_device_cmd_json(char *buf, uint16_t buf_size,
    uint8_t src_board, const char *device, int value, const char *extra);

int build_device_ack_json(char *buf, uint16_t buf_size, uint32_t msg_id,
    uint8_t target_board, const char *device, uint8_t ok, const char *state);

/* ── 健康评分 (1号→广播) ── */
int build_health_score_json(char *buf, uint16_t buf_size,
    uint8_t score, const char *reason);

/* ── 模式切换 (1号→广播) ── */
int build_mode_change_json(char *buf, uint16_t buf_size,
    uint8_t mode);   /* 0=HOME, 1=AWAY */

/* ── 告警消息 (1号→广播) ── */
int build_alert_json(char *buf, uint16_t buf_size,
    const char *title, const char *detail);

/* ── AI 建议 (4号→广播) ── */
int build_ai_suggestion_json(char *buf, uint16_t buf_size,
    const char *text);

/* ── 心跳 (各板→广播) ── */
int build_heartbeat_json(char *buf, uint16_t buf_size,
    uint8_t board_id);

#ifdef __cplusplus
}
#endif

#endif /* MSG_BUILDER_H */
