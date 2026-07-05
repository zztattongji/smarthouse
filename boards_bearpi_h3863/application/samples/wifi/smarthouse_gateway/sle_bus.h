/**
 * @file    sle_bus.h
 * @brief   SLE 广播总线协议定义 — 四块板子统一消息格式
 */
#ifndef SLE_BUS_H
#define SLE_BUS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 常量 ── */
#define SLE_BUS_MAGIC           0xAA
#define SLE_BUS_VERSION         0x01
#define SLE_BUS_PAYLOAD_MAX     256
#define SLE_BUS_CHANNEL         "smarthouse"

/* ── 板号 ── */
#define BOARD_GATEWAY    1
#define BOARD_SENSOR     2
#define BOARD_ACTUATOR   3
#define BOARD_AI         4

/* ── 消息类型 ── */
typedef enum {
    MSG_SENSOR_DATA      = 0x01,   /* 传感器数据    (2→广播)         */
    MSG_ACTUATOR_STATE   = 0x02,   /* 执行器状态    (3→广播)         */
    MSG_DEVICE_CMD       = 0x03,   /* 设备控制命令  (1/4→3)           */
    MSG_HEALTH_SCORE     = 0x04,   /* 健康评分      (1→广播)         */
    MSG_AI_SUGGESTION    = 0x05,   /* AI 建议       (4→广播)         */
    MSG_MODE_CHANGE      = 0x06,   /* 模式切换      (1→广播)         */
    MSG_ALERT            = 0x07,   /* 故障告警      (1→广播)         */
    MSG_DEVICE_ACK       = 0x08,   /* 执行确认      (3→广播)         */
    MSG_HEARTBEAT        = 0xFE,   /* 心跳保活      (各板→广播)       */
} sle_msg_type_t;

/* ── 设备 ID（用于 DEVICE_CMD） ── */
typedef enum {
    DEV_LED,
    DEV_FAN,
    DEV_MOTOR,
    DEV_RELAY,
    DEV_SERVO,
    DEV_BUZZER,
    DEV_IR,
    DEV_REFRESH_SENSORS,    /* 触发传感器刷新 */
} sle_device_t;

/* ── 模式 ── */
typedef enum {
    MODE_HOME,
    MODE_AWAY,
} home_mode_t;

/* ── 统一消息包 ── */
#pragma pack(1)
typedef struct {
    uint8_t  magic;                         /* 帧头 0xAA              */
    uint8_t  version;                       /* 协议版本               */
    uint8_t  src_board;                     /* 来源板号 1-4           */
    uint8_t  msg_type;                      /* 消息类型 (sle_msg_type_t) */
    uint16_t payload_len;                   /* 负载长度               */
    uint32_t timestamp;                     /* 发送时间戳 (ms)         */
    uint8_t  payload[SLE_BUS_PAYLOAD_MAX];  /* 负载（JSON 文本）       */
    uint16_t crc16;                         /* CRC16 校验             */
} sle_bus_packet_t;
#pragma pack()

/* ── CRC16 计算 ── */
uint16_t sle_bus_crc16(const uint8_t *data, uint16_t len);

/* ── 打包/解包 ── */

/**
 * @brief 构造一个 SLE 总线消息包
 * @param pkt     [out] 输出包
 * @param src     来源板号
 * @param type    消息类型
 * @param payload JSON 负载字符串
 * @param len     负载长度（不含 '\0'）
 * @return        实际包大小（字节），0 表示失败
 */
uint16_t sle_bus_pack(sle_bus_packet_t *pkt, uint8_t src, sle_msg_type_t type,
                      const char *payload, uint16_t len);

/**
 * @brief 验证一个 SLE 总线消息包
 * @param pkt  输入包
 * @param size 包大小
 * @return     0=有效, -1=magic错误, -2=CRC错误
 */
int sle_bus_validate(const sle_bus_packet_t *pkt, uint16_t size);

#ifdef __cplusplus
}
#endif

#endif /* SLE_BUS_H */
