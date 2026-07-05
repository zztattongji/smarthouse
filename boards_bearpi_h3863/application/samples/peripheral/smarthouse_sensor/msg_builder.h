/**
 * @file    msg_builder.h
 * @brief   鏋勯€犲悇绫诲瀷 SLE 鎬荤嚎娑堟伅鐨?JSON 璐熻浇
 *
 * 鎵€鏈夊嚱鏁板皢浼犳劅鍣?鎵ц鍣?鍛戒护鏁版嵁搴忓垪鍖栦负 JSON 瀛楃涓诧紝
 * 濉叆 buf锛岃繑鍥炲啓鍏ュ瓧鑺傛暟锛堜笉鍚?'\0'锛夈€? */
#ifndef MSG_BUILDER_H
#define MSG_BUILDER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 鈹€鈹€ 浼犳劅鍣ㄦ暟鎹?(2鍙封啋骞挎挱) 鈹€鈹€ */
int build_sensor_json(char *buf, uint16_t buf_size,
    float temperature, float humidity, float pressure,
    uint16_t voc, uint16_t eco2, uint16_t pm25);

/* 鈹€鈹€ 鎵ц鍣ㄧ姸鎬?(3鍙封啋骞挎挱) 鈹€鈹€ */
int build_actuator_state_json(char *buf, uint16_t buf_size,
    uint8_t led, uint8_t fan, const char *motor,
    uint8_t relay, uint8_t servo, uint8_t buzzer, uint8_t pir);

/* 鈹€鈹€ 璁惧鎺у埗鍛戒护 (1/4鈫?鍙? 鈹€鈹€ */
int build_device_cmd_json(char *buf, uint16_t buf_size,
    uint8_t src_board, const char *device, int value, const char *extra);

/* 鈹€鈹€ 鍋ュ悍璇勫垎 (1鍙封啋骞挎挱) 鈹€鈹€ */
int build_health_score_json(char *buf, uint16_t buf_size,
    uint8_t score, const char *reason);

/* 鈹€鈹€ 妯″紡鍒囨崲 (1鍙封啋骞挎挱) 鈹€鈹€ */
int build_mode_change_json(char *buf, uint16_t buf_size,
    uint8_t mode);   /* 0=HOME, 1=AWAY */

/* 鈹€鈹€ 鍛婅娑堟伅 (1鍙封啋骞挎挱) 鈹€鈹€ */
int build_alert_json(char *buf, uint16_t buf_size,
    const char *title, const char *detail);

/* 鈹€鈹€ AI 寤鸿 (4鍙封啋骞挎挱) 鈹€鈹€ */
int build_ai_suggestion_json(char *buf, uint16_t buf_size,
    const char *text);

/* 鈹€鈹€ 蹇冭烦 (鍚勬澘鈫掑箍鎾? 鈹€鈹€ */
int build_heartbeat_json(char *buf, uint16_t buf_size,
    uint8_t board_id);

#ifdef __cplusplus
}
#endif

#endif /* MSG_BUILDER_H */


