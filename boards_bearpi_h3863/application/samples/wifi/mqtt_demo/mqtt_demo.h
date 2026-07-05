#ifndef MQTT_DEMO_H
#define MQTT_DEMO_H

#include <stdbool.h>

/* ====== 消息来源 ====== */
typedef enum {
    MSG_FROM_BOARD2 = 2,  /* 2号 环境检测板 */
    MSG_FROM_BOARD3 = 3,  /* 3号 执行板 */
    MSG_FROM_CLOUD  = 99, /* 华为云端下发 */
} msg_source_t;

/* ====== 消息类型 ====== */
typedef enum {
    MSG_TYPE_SENSOR_REPORT   = 1,  /* 环境数据上报（2号→1号→云） */
    MSG_TYPE_ACTUATOR_CMD    = 2,  /* 执行命令（云→1号→3号） */
    MSG_TYPE_ACTUATOR_STATE  = 3,  /* 执行器状态回传（3号→1号→云） */
} msg_type_t;

/* ====== 7个执行命令（与华为云 actuator 服务命令一一对应）====== */
typedef enum {
    CMD_SET_LED    = 1,  /* set_led    : brightness 0-100 */
    CMD_SET_FAN    = 2,  /* set_fan    : speed 0-100 */
    CMD_SET_MOTOR  = 3,  /* set_motor  : "stop"/"forward"/"reverse" */
    CMD_SET_RELAY  = 4,  /* set_relay  : true/false */
    CMD_SET_SERVO  = 5,  /* set_servo  : angle 0-180 */
    CMD_SET_BUZZER = 6,  /* set_buzzer : true/false */
    CMD_SEND_IR    = 7,  /* send_ir    : ir_code string */
} cmd_type_t;

/* ====== 传感器数据（与华为云 environment 服务属性一一对应）====== */
typedef struct {
    float temperature;  /* SHT30  温度 ℃ */
    float humidity;     /* SHT30  湿度 %RH */
    float pressure;     /* BMP280 气压 hPa */
    int   light;        /* BH1750 光照 lux */
    int   voc;          /* SGP30  VOC ppb */
    int   eco2;         /* SGP30  eCO2 ppm */
    int   pm25;         /* PMS5003 PM2.5 μg/m³ */
} sensor_data_t;

/* ====== 执行命令参数（union 按命令类型选用）====== */
typedef struct {
    cmd_type_t cmd;
    union {
        int   brightness;   /* CMD_SET_LED */
        int   speed;        /* CMD_SET_FAN */
        char  direction[16];/* CMD_SET_MOTOR: "stop"/"forward"/"reverse" */
        bool  relay_state;  /* CMD_SET_RELAY */
        int   angle;        /* CMD_SET_SERVO: 0-180 */
        bool  buzzer_state; /* CMD_SET_BUZZER */
        char  ir_code[64];  /* CMD_SEND_IR: NEC 红外码 */
    } params;
} actuator_cmd_t;

/* ====== 执行器状态回传 ====== */
typedef struct {
    int  led_brightness;   /* 0-100 */
    int  fan_speed;        /* 0-100 */
    char motor_state[16];  /* "stop"/"forward"/"reverse" */
    bool relay_state;
    int  servo_angle;      /* 0-180 */
    bool buzzer_state;
    bool pir_triggered;    /* 人体红外触发 */
} actuator_state_t;

/* ====== 统一消息体 ====== */
typedef struct {
    msg_source_t source;
    msg_type_t   type;
    union {
        sensor_data_t   sensor;          /* MSG_TYPE_SENSOR_REPORT */
        actuator_cmd_t  cmd;             /* MSG_TYPE_ACTUATOR_CMD */
        actuator_state_t state;          /* MSG_TYPE_ACTUATOR_STATE */
        char            raw_json[512];   /* 兜底：未解析的原始JSON */
    } data;
    char request_id[64];  /* 华为云命令的 request_id，回复命令响应时用 */
} mqtt_msg_t;

#endif
