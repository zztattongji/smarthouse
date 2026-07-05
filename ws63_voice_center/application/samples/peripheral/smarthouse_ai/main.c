/**
 * @file    main.c
 * @brief   4 号板 — AI 语音管家 (WS63V100 通信侧)
 *
 *   架构:
 *     CI1302 (AI芯片) ←─UART─→ WS63V100 (通信芯片) ←─SLE Client─→ 1号板 Server
 *
 *   WS63 职责:
 *     1. SLE Client 连 1号板, 收传感器数据 + 执行器状态
 *     2. 通过 UART 转发给 CI1302 做 AI 分析
 *     3. 从 CI1302 收 AI 命令 → SLE 发给 1号板 → 1号板转发 3号板执行
 *     4. 从 CI1302 收 TTS 文本 → (保留, 由 CI1302 直接播报)
 */
#include "sle_uart_client.h"
#include "sle_bus.h"
#include "msg_builder.h"
#include "pinctrl.h"
#include "uart.h"
#include "soc_osal.h"
#include "app_init.h"
#include "common_def.h"
#include "cJSON.h"
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#ifndef CONFIG_SMARTHOUSE_AI_UART_BUS
#define CONFIG_SMARTHOUSE_AI_UART_BUS 1
#endif
#ifndef CONFIG_SMARTHOUSE_AI_UART_TXD_PIN
#define CONFIG_SMARTHOUSE_AI_UART_TXD_PIN 15
#endif
#ifndef CONFIG_SMARTHOUSE_AI_UART_RXD_PIN
#define CONFIG_SMARTHOUSE_AI_UART_RXD_PIN 16
#endif
#ifndef CONFIG_SMARTHOUSE_AI_UART_BAUD
#define CONFIG_SMARTHOUSE_AI_UART_BAUD 1000000   // 1M波特率，匹配CI1302的UART_BaudRate1M
#endif

/* ═══ CI1302 UART 协议 ═══ */
#define CI1302_UART_BUS     CONFIG_SMARTHOUSE_AI_UART_BUS
#define CI1302_BAUD         CONFIG_SMARTHOUSE_AI_UART_BAUD
#define CI1302_TXD_PIN      CONFIG_SMARTHOUSE_AI_UART_TXD_PIN
#define CI1302_RXD_PIN      CONFIG_SMARTHOUSE_AI_UART_RXD_PIN

/* ── SLE 连接状态 ── */
static uint16_t g_gw_conn_id = 0;
static uint8_t  g_sle_connected = 0;

/* ── 传感器/执行器缓存 (最新一帧) ── */
static char g_last_sensor_json[SLE_BUS_PAYLOAD_MAX + 1];
static char g_last_actuator_json[SLE_BUS_PAYLOAD_MAX + 1];

/* ── UART 接收环形缓冲 (收 CI1302 的 AI 命令) ── */
#define UART_RX_BUF_SIZE 512
static uint8_t  g_uart_rx_buf[UART_RX_BUF_SIZE];
static volatile uint16_t g_uart_rx_head = 0;  /* 写入位置 (中断上下文) */
static volatile uint16_t g_uart_rx_tail = 0;  /* 读取位置 (任务上下文, 临界区内写) */

/* ═══════════════ UART 收发 ═══════════════ */

static void uart_rx_callback(const void *data, uint16_t len, bool error)
{
    if (error || data == NULL) {
        return;
    }

    const uint8_t *b = (const uint8_t *)data;
    for (uint16_t i = 0; i < len; i++) {
        uint16_t next = (g_uart_rx_head + 1) % UART_RX_BUF_SIZE;
        if (next == g_uart_rx_tail) {
            g_uart_rx_tail = (g_uart_rx_tail + 1) % UART_RX_BUF_SIZE; /* 覆盖最旧 */
        }
        g_uart_rx_buf[g_uart_rx_head] = b[i];
        g_uart_rx_head = next;
    }
}

static void uart_send_string(const char *str)
{
    if (str == NULL) return;
    uapi_uart_write(CI1302_UART_BUS, (const uint8_t *)str,
                    (uint32_t)strlen(str), 100);
}

static int json_payload_len_valid(int len)
{
    return (len > 0 && len < SLE_BUS_PAYLOAD_MAX);
}

typedef struct {
    const char *device;
    int value;
    const char *extra;
} ci1302_device_cmd_t;

static int ci1302_send_device_cmd(const char *device, int value, const char *extra)
{
    char json[SLE_BUS_PAYLOAD_MAX];
    int jl = build_device_cmd_json(json, sizeof(json), BOARD_AI, device, value, extra);
    if (!json_payload_len_valid(jl)) {
        return -1;
    }

    sle_bus_packet_t pkt;
    uint16_t pl = sle_bus_pack(&pkt, BOARD_AI, MSG_DEVICE_CMD, json, (uint16_t)jl);
    if (pl == 0 || !g_sle_connected) {
        return -1;
    }

    ssapc_write_param_t *wp = get_g_sle_uart_send_param();
    if (wp == NULL) {
        return -1;
    }
    wp->data_len = pl;
    wp->data = (uint8_t *)&pkt;
    ssapc_write_req(0, g_gw_conn_id, wp);
    return 0;
}

static int in_range_u32(uint32_t value, uint32_t start, uint32_t end)
{
    return (value >= start && value <= end);
}

static int ci1302_map_v1_cmd(uint16_t cmd_id, ci1302_device_cmd_t *cmd)
{
    if (cmd == NULL) {
        return 0;
    }

    switch (cmd_id) {
    case 0x0002: /* Standard demo: open air conditioner. */
        cmd->device = "fan";
        cmd->value = 1;
        cmd->extra = "ci1302_v1_air_on";
        return 1;
    case 0x0003: /* Standard demo: close air conditioner. */
        cmd->device = "fan";
        cmd->value = 0;
        cmd->extra = "ci1302_v1_air_off";
        return 1;
    default:
        return 0;
    }
}

static int ci1302_map_v2_semantic(uint32_t semantic_id, ci1302_device_cmd_t *cmd)
{
    if (cmd == NULL) {
        return 0;
    }

    if (semantic_id == 0x19421C8 || semantic_id == 0x19421CA ||
        semantic_id == 0x19491C7 ||
        in_range_u32(semantic_id, 0x19492C1, 0x19492C2)) {
        cmd->device = "fan";
        cmd->value = 1;
        cmd->extra = "ci1302_v2_fan_on";
        return 1;
    }
    if (in_range_u32(semantic_id, 0x1949301, 0x1949302) ||
        in_range_u32(semantic_id, 0x1949341, 0x1949342)) {
        cmd->device = "fan";
        cmd->value = 0;
        cmd->extra = "ci1302_v2_fan_off";
        return 1;
    }
    if (in_range_u32(semantic_id, 0x1949381, 0x1949382)) {
        cmd->device = "fan";
        cmd->value = 1;
        cmd->extra = "ci1302_v2_fan_speed";
        return 1;
    }
    if (in_range_u32(semantic_id, 0x19493C1, 0x19493C3)) {
        cmd->device = "fan";
        cmd->value = 2;
        cmd->extra = "ci1302_v2_fan_speed";
        return 1;
    }
    if (in_range_u32(semantic_id, 0x1949401, 0x1949403)) {
        cmd->device = "fan";
        cmd->value = 3;
        cmd->extra = "ci1302_v2_fan_speed";
        return 1;
    }

    if (in_range_u32(semantic_id, 0x2341941, 0x2341946) ||
        in_range_u32(semantic_id, 0x23419C1, 0x23419C3) ||
        in_range_u32(semantic_id, 0x2341A41, 0x2341A43) ||
        in_range_u32(semantic_id, 0x2341AC1, 0x2341AC6) ||
        in_range_u32(semantic_id, 0x2341B41, 0x2341B44) ||
        in_range_u32(semantic_id, 0x2341BC1, 0x2341BC8)) {
        cmd->device = "led";
        cmd->value = 100;
        cmd->extra = "ci1302_v2_light_on";
        return 1;
    }
    if (in_range_u32(semantic_id, 0x2341981, 0x2341986) ||
        in_range_u32(semantic_id, 0x2341A01, 0x2341A03) ||
        in_range_u32(semantic_id, 0x2341A81, 0x2341A83) ||
        in_range_u32(semantic_id, 0x2341B01, 0x2341B08) ||
        in_range_u32(semantic_id, 0x2341B81, 0x2341B84) ||
        in_range_u32(semantic_id, 0x2341C01, 0x2341C08)) {
        cmd->device = "led";
        cmd->value = 0;
        cmd->extra = "ci1302_v2_light_off";
        return 1;
    }

    if (in_range_u32(semantic_id, 0x2042001, 0x2042002) ||
        in_range_u32(semantic_id, 0x2042101, 0x2042102)) {
        cmd->device = "relay";
        cmd->value = 1;
        cmd->extra = "ci1302_v2_socket_on";
        return 1;
    }
    if (in_range_u32(semantic_id, 0x2042041, 0x2042042) ||
        in_range_u32(semantic_id, 0x2042141, 0x2042142)) {
        cmd->device = "relay";
        cmd->value = 0;
        cmd->extra = "ci1302_v2_socket_off";
        return 1;
    }

    return 0;
}

static uint8_t ci1302_checksum_v1(const uint8_t *frame)
{
    uint16_t sum = 0;
    for (uint8_t i = 0; i < 6; i++) {
        sum += frame[i];
    }
    return (uint8_t)sum;
}

static uint16_t ci1302_checksum_v2(const uint8_t *frame, uint16_t checksum_pos)
{
    uint16_t sum = 0;
    for (uint16_t i = 0; i < checksum_pos; i++) {
        sum += frame[i];
    }
    return sum;
}

static void ci1302_handle_v1_frame(const uint8_t *frame, uint16_t len)
{
    if (len != 8 || frame[0] != 0xA5 || frame[1] != 0xFA || frame[7] != 0xFB) {
        return;
    }
    if (frame[6] != ci1302_checksum_v1(frame)) {
        return;
    }
    if (frame[3] != 0x81) {
        return;
    }

    uint16_t cmd_id = (uint16_t)frame[4] | ((uint16_t)frame[5] << 8);
    ci1302_device_cmd_t cmd;
    if (ci1302_map_v1_cmd(cmd_id, &cmd)) {
        (void)ci1302_send_device_cmd(cmd.device, cmd.value, cmd.extra);
    }
}

static void ci1302_handle_v2_frame(const uint8_t *frame, uint16_t len)
{
    if (len < 10 || frame[0] != 0xA5 || frame[1] != 0xFC || frame[len - 1] != 0xFB) {
        return;
    }

    uint16_t payload_len = (uint16_t)frame[2] | ((uint16_t)frame[3] << 8);
    if ((uint16_t)(payload_len + 10) != len) {
        return;
    }

    uint16_t checksum_pos = (uint16_t)(len - 3);
    uint16_t expected = (uint16_t)frame[checksum_pos] | ((uint16_t)frame[checksum_pos + 1] << 8);
    if (expected != ci1302_checksum_v2(frame, checksum_pos)) {
        return;
    }

    if (frame[4] != 0xA0 || frame[5] != 0x91 || payload_len < 6) {
        return;
    }

    const uint8_t *data = &frame[7];
    uint32_t semantic_id = (uint32_t)data[0] |
                           ((uint32_t)data[1] << 8) |
                           ((uint32_t)data[2] << 16) |
                           ((uint32_t)data[3] << 24);

    ci1302_device_cmd_t cmd;
    if (ci1302_map_v2_semantic(semantic_id, &cmd)) {
        (void)ci1302_send_device_cmd(cmd.device, cmd.value, cmd.extra);
    }
}

/* ═══════════════ 涂鸦协议处理 (55 AA) ═══════════════ */

/**
 * @brief 涂鸦协议帧格式:
 *   55 AA 03 92 [data_len_H] [data_len_L] [data...] [checksum]
 *
 *   示例: 55 AA 03 92 00 05 08 [4字节语义ID(大端序)] [checksum]
 *                            ↑  ↑___ 语义ID ___↑
 *                            sub_cmd = 0x08
 */
static void ci1302_handle_tuya_frame(const uint8_t *frame, uint16_t len)
{
    // 验证帧头和最小长度
    if (len < 7 || frame[0] != 0x55 || frame[1] != 0xAA) {
        return;
    }

    // 验证命令类型 (03 92 表示语音识别结果)
    if (frame[2] != 0x03 || frame[3] != 0x92) {
        return;
    }

    // 提取数据长度 (大端序)
    uint16_t data_len = ((uint16_t)frame[4] << 8) | (uint16_t)frame[5];
    if (len != (uint16_t)(6 + data_len + 1)) {
        return;  // 长度不匹配
    }

    // 验证校验和 (累加和)
    uint8_t checksum = 0;
    for (uint16_t i = 0; i < len - 1; i++) {
        checksum += frame[i];
    }
    if (checksum != frame[len - 1]) {
        return;  // 校验失败
    }

    // 数据格式: [sub_cmd=0x08] [语义ID 4字节(大端序)]
    if (data_len >= 5) {
        const uint8_t *data = &frame[6];

        // 验证子命令 = 0x08
        if (data[0] != 0x08) {
            return;
        }

        // 提取语义ID (大端序)
        uint32_t semantic_id = ((uint32_t)data[1] << 24) |
                               ((uint32_t)data[2] << 16) |
                               ((uint32_t)data[3] << 8)  |
                               (uint32_t)data[4];

        // 使用V2协议的语义ID映射表
        ci1302_device_cmd_t cmd;
        if (ci1302_map_v2_semantic(semantic_id, &cmd)) {
            (void)ci1302_send_device_cmd(cmd.device, cmd.value, cmd.extra);
        }
    }
}

static int uart_buffer_used(uint16_t head, uint16_t tail)
{
    return (head >= tail) ? (head - tail) : (UART_RX_BUF_SIZE - tail + head);
}

static uint8_t uart_buffer_peek(uint16_t tail, uint16_t offset)
{
    return g_uart_rx_buf[(tail + offset) % UART_RX_BUF_SIZE];
}

static void uart_buffer_drop(uint16_t count)
{
    g_uart_rx_tail = (g_uart_rx_tail + count) % UART_RX_BUF_SIZE;
}

static uint16_t uart_buffer_copy(uint8_t *dst, uint16_t max_len)
{
    uint16_t head = g_uart_rx_head;
    uint16_t copied = 0;
    while (g_uart_rx_tail != head && copied < max_len) {
        dst[copied++] = g_uart_rx_buf[g_uart_rx_tail];
        g_uart_rx_tail = (g_uart_rx_tail + 1) % UART_RX_BUF_SIZE;
    }
    return copied;
}

static int uart_try_take_json_line(char *line_buf, uint16_t line_size)
{
    uint16_t head = g_uart_rx_head;
    uint16_t pos = g_uart_rx_tail;
    uint16_t offset = 0;
    while (pos != head) {
        if (g_uart_rx_buf[pos] == '\n') {
            uint16_t copied = 0;
            while (copied < offset && copied < line_size - 1) {
                line_buf[copied] = (char)uart_buffer_peek(g_uart_rx_tail, copied);
                copied++;
            }
            line_buf[copied] = '\0';
            uart_buffer_drop((uint16_t)(offset + 1));
            return copied > 0;
        }
        pos = (pos + 1) % UART_RX_BUF_SIZE;
        offset++;
    }
    return 0;
}

static int uart_try_take_binary_frame(uint8_t *frame_buf, uint16_t frame_size, uint16_t *frame_len)
{
    uint16_t head = g_uart_rx_head;
    int used = uart_buffer_used(head, g_uart_rx_tail);
    if (used < 2) {
        return 0;
    }

    uint8_t b0 = uart_buffer_peek(g_uart_rx_tail, 0);
    uint8_t b1 = uart_buffer_peek(g_uart_rx_tail, 1);
    if (b0 == 0xA5 && b1 == 0xFA) {
        if (used < 8) {
            return 0;
        }
        if (frame_size < 8) {
            uart_buffer_drop(8);
            return 0;
        }
        *frame_len = 8;
        (void)uart_buffer_copy(frame_buf, 8);
        return 1;
    }

    if (b0 == 0xA5 && b1 == 0xFC) {
        if (used < 4) {
            return 0;
        }
        uint16_t payload_len = (uint16_t)uart_buffer_peek(g_uart_rx_tail, 2) |
                               ((uint16_t)uart_buffer_peek(g_uart_rx_tail, 3) << 8);
        uint16_t total_len = (uint16_t)(payload_len + 10);
        if (total_len > frame_size || total_len > UART_RX_BUF_SIZE - 1) {
            uart_buffer_drop(1);
            return 0;
        }
        if (used < total_len) {
            return 0;
        }
        *frame_len = total_len;
        (void)uart_buffer_copy(frame_buf, total_len);
        return 1;
    }

    // 涂鸦协议: 55 AA 03 92 [data_len(2)] [data...] [checksum]
    if (b0 == 0x55 && b1 == 0xAA) {
        if (used < 6) {
            return 0;  // 至少需要头部6字节
        }
        // 数据长度在第4-5字节（大端序）
        uint16_t data_len = ((uint16_t)uart_buffer_peek(g_uart_rx_tail, 4) << 8) |
                            (uint16_t)uart_buffer_peek(g_uart_rx_tail, 5);
        uint16_t total_len = (uint16_t)(6 + data_len + 1);  // header(6) + data + checksum(1)
        if (total_len > frame_size || total_len > UART_RX_BUF_SIZE - 1) {
            uart_buffer_drop(1);
            return 0;
        }
        if (used < total_len) {
            return 0;  // 数据不够
        }
        *frame_len = total_len;
        (void)uart_buffer_copy(frame_buf, total_len);
        return 1;
    }

    if (b0 == 0xA5) {
        uart_buffer_drop(1);
    }
    if (b0 == 0x55) {
        uart_buffer_drop(1);
    }
    return 0;
}

static void handle_ci1302_json_line(const char *line_buf)
{
    cJSON *root = cJSON_Parse(line_buf);
    if (root == NULL) return;

    cJSON *type_item = cJSON_GetObjectItem(root, "type");
    cJSON *intent_item = cJSON_GetObjectItem(root, "intent");
    const char *msg_type = NULL;
    if (type_item && cJSON_IsString(type_item)) {
        msg_type = type_item->valuestring;
    } else if (intent_item && cJSON_IsString(intent_item)) {
        if (strcmp(intent_item->valuestring, "device.set") == 0) {
            msg_type = "device_cmd";
        } else if (strcmp(intent_item->valuestring, "ai.suggestion") == 0) {
            msg_type = "ai_suggestion";
        }
    }
    if (msg_type == NULL) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(msg_type, "device_cmd") == 0) {
        cJSON *device_item = cJSON_GetObjectItem(root, "device");
        cJSON *value_item  = cJSON_GetObjectItem(root, "value");
        cJSON *extra_item  = cJSON_GetObjectItem(root, "extra");
        if (device_item && cJSON_IsString(device_item) && value_item) {
            int val = cJSON_IsNumber(value_item) ? value_item->valueint : 0;
            const char *extra = (extra_item && cJSON_IsString(extra_item))
                                ? extra_item->valuestring : "";
            (void)ci1302_send_device_cmd(device_item->valuestring, val, extra);
        }
    } else if (strcmp(msg_type, "ai_suggestion") == 0) {
        cJSON *text_item = cJSON_GetObjectItem(root, "text");
        if (text_item && cJSON_IsString(text_item)) {
            char json[SLE_BUS_PAYLOAD_MAX];
            int jl = build_ai_suggestion_json(json, sizeof(json), text_item->valuestring);
            if (json_payload_len_valid(jl)) {
                sle_bus_packet_t pkt;
                uint16_t pl = sle_bus_pack(&pkt, BOARD_AI, MSG_AI_SUGGESTION, json, (uint16_t)jl);
                if (pl && g_sle_connected) {
                    ssapc_write_param_t *wp = get_g_sle_uart_send_param();
                    if (wp) { wp->data_len = pl; wp->data = (uint8_t *)&pkt;
                              ssapc_write_req(0, g_gw_conn_id, wp); }
                }
            }
        }
    }
    cJSON_Delete(root);
}

static int process_ci1302_stream_once(void)
{
    char line_buf[600];
    uint8_t frame_buf[128];
    uint16_t frame_len = 0;
    uint8_t got_json = 0;
    uint8_t got_frame = 0;
    uint8_t frame_head = 0;

    uint32_t irq = osal_irq_lock();
    uint16_t head_snap = g_uart_rx_head;
    int used = uart_buffer_used(head_snap, g_uart_rx_tail);
    if (used == 0) {
        osal_irq_restore(irq);
        return 0;
    }

    uint8_t first = uart_buffer_peek(g_uart_rx_tail, 0);
    if (first == '{') {
        got_json = (uint8_t)uart_try_take_json_line(line_buf, sizeof(line_buf));
        if (!got_json && used > 400) {
            uart_buffer_drop(1);
            osal_irq_restore(irq);
            return 1;
        }
    } else if (first == 0xA5) {
        got_frame = (uint8_t)uart_try_take_binary_frame(frame_buf, sizeof(frame_buf), &frame_len);
        if (got_frame) {
            frame_head = frame_buf[1];
        } else if (used > 260) {
            uart_buffer_drop(1);
            osal_irq_restore(irq);
            return 1;
        }
    } else if (first == 0x55) {
        // 涂鸦协议: 55 AA ...
        got_frame = (uint8_t)uart_try_take_binary_frame(frame_buf, sizeof(frame_buf), &frame_len);
        if (got_frame) {
            frame_head = 0x55;  // 标记为涂鸦协议
        } else if (used > 260) {
            uart_buffer_drop(1);
            osal_irq_restore(irq);
            return 1;
        }
    } else {
        uart_buffer_drop(1);
        osal_irq_restore(irq);
        return 1;
    }
    osal_irq_restore(irq);

    if (got_json) {
        handle_ci1302_json_line(line_buf);
        return 1;
    }
    if (got_frame) {
        if (frame_head == 0xFA) {
            ci1302_handle_v1_frame(frame_buf, frame_len);
        } else if (frame_head == 0xFC) {
            ci1302_handle_v2_frame(frame_buf, frame_len);
        } else if (frame_head == 0x55) {
            ci1302_handle_tuya_frame(frame_buf, frame_len);  // 涂鸦协议
        }
        return 1;
    }
    return 0;
}

static void uart_init_ci1302(void)
{
    uapi_pin_set_mode(CI1302_TXD_PIN, 1);
    uapi_pin_set_mode(CI1302_RXD_PIN, 1);

    uart_pin_config_t pin = { .tx_pin = CI1302_TXD_PIN,
                              .rx_pin = CI1302_RXD_PIN,
                              .cts_pin = PIN_NONE, .rts_pin = PIN_NONE };
    uart_attr_t attr = { .baud_rate = CI1302_BAUD,
                         .data_bits = UART_DATA_BIT_8,
                         .stop_bits = UART_STOP_BIT_1,
                         .parity = UART_PARITY_NONE };
    uapi_uart_init(CI1302_UART_BUS, &pin, &attr, NULL, NULL);
    uapi_uart_register_rx_callback(CI1302_UART_BUS,
        UART_RX_CONDITION_FULL_OR_IDLE, 1, uart_rx_callback);
}

/* ═══════════════ 处理 CI1302 发来的数据 ═══════════════ */
static void process_ci1302_data(void)
{
    while (process_ci1302_stream_once()) {
    }
}
/* ═══════════════ SLE Client 回调 ═══════════════ */

/* notification: Server 推送数据过来 (传感器 / 执行器状态 / 健康评分 / ...) */
static void sle_notify_cb(uint8_t client_id, uint16_t conn_id,
                           ssapc_handle_value_t *data, errcode_t status)
{
    (void)client_id; (void)conn_id;
    if (status != ERRCODE_SUCC || data == NULL) return;
    if (data->data == NULL || data->data_len < 8) return;

    sle_bus_packet_t *p = (sle_bus_packet_t *)data->data;
    if (sle_bus_validate(p, data->data_len) != 0) return;

    uint16_t pl = p->payload_len;
    if (pl > SLE_BUS_PAYLOAD_MAX) pl = SLE_BUS_PAYLOAD_MAX;

    switch ((sle_msg_type_t)p->msg_type) {

    case MSG_SENSOR_DATA:
        memcpy(g_last_sensor_json, p->payload, pl);
        g_last_sensor_json[pl] = '\0';
        {
            char buf[600];
            /* payload 本身是 {"temperature":23.5,...}, 包装成合法 JSON */
            snprintf(buf, sizeof(buf), "{\"type\":\"sensor_data\",\"data\":%.*s}\n",
                     (int)pl, p->payload);
            uart_send_string(buf);
        }
        break;

    case MSG_ACTUATOR_STATE:
        memcpy(g_last_actuator_json, p->payload, pl);
        g_last_actuator_json[pl] = '\0';
        {
            char buf[600];
            snprintf(buf, sizeof(buf), "{\"type\":\"actuator_state\",\"data\":%.*s}\n",
                     (int)pl, p->payload);
            uart_send_string(buf);
        }
        break;

    case MSG_HEALTH_SCORE:
        {
            char buf[600];
            snprintf(buf, sizeof(buf), "{\"type\":\"health_score\",\"data\":%.*s}\n",
                     (int)pl, p->payload);
            uart_send_string(buf);
        }
        break;

    case MSG_AI_SUGGESTION:
        {
            char buf[600];
            snprintf(buf, sizeof(buf), "{\"type\":\"tts_play\",\"data\":%.*s}\n",
                     (int)pl, p->payload);
            uart_send_string(buf);
        }
        break;

    case MSG_DEVICE_ACK:
        {
            char buf[600];
            snprintf(buf, sizeof(buf), "{\"type\":\"device_ack\",\"data\":%.*s}\n",
                     (int)pl, p->payload);
            uart_send_string(buf);
        }
        break;

    default:
        break;
    }
}

static void sle_indication_cb(uint8_t client_id, uint16_t conn_id,
                                ssapc_handle_value_t *data, errcode_t status)
{
    (void)client_id; (void)conn_id; (void)data; (void)status;
}

/* ═══════════════ 主任务 ═══════════════ */

static void ai_check_conn(void)
{
    uint16_t cur_id = get_g_sle_uart_conn_id();
    if (cur_id == 0 || cur_id != g_gw_conn_id) {
        g_sle_connected = 0;
        g_gw_conn_id = 0;
    }
}

static void ai_board_reconnect(void)
{
    ai_check_conn();
    if (g_sle_connected) return;
    /* 主循环 100ms, 每 20 轮 (2s) 重试一次 */
    static uint16_t retry_div = 0;
    if (++retry_div % 20 != 0) return;
    sle_uart_start_scan();
    osal_msleep(50);
    uint16_t conn_id = get_g_sle_uart_conn_id();
    if (conn_id != 0) {
        g_gw_conn_id = conn_id;
        g_sle_connected = 1;
    }
}

static void ai_task(void)
{
    for (;;) {
        ai_board_reconnect();  /* 断线自动重连 */
        /* 处理 CI1302 通过 UART 发来的 AI 命令 */
        process_ci1302_data();

        /* 定期发送心跳到 1号板 */
        static uint32_t tick = 0;
        if (++tick % 10 == 0 && g_sle_connected) {
            char json[SLE_BUS_PAYLOAD_MAX];
            int jl = build_heartbeat_json(json, sizeof(json), BOARD_AI);
            if (!json_payload_len_valid(jl)) {
                osal_msleep(100);
                continue;
            }
            sle_bus_packet_t pkt;
            uint16_t pl = sle_bus_pack(&pkt, BOARD_AI, MSG_HEARTBEAT, json, (uint16_t)jl);
            if (pl) {
                ssapc_write_param_t *wp = get_g_sle_uart_send_param();
                if (wp) { wp->data_len = pl; wp->data = (uint8_t *)&pkt;
                          ssapc_write_req(0, g_gw_conn_id, wp); }
            }
        }
        osal_msleep(100);
    }
}

/* ═══════════════ 入口 ═══════════════ */

static void ai_board_entry(void)
{
    /* 1. 初始化 UART 连接 CI1302 */
    uart_init_ci1302();

    /* 2. SLE Client 连 1号板 Server */
    sle_uart_client_init(sle_notify_cb, sle_indication_cb);
    sle_uart_start_scan();

    /* 3. 等待连接完成 */
    osal_msleep(3000);
    g_gw_conn_id = get_g_sle_uart_conn_id();
    if (g_gw_conn_id != 0) {
        g_sle_connected = 1;
        uart_send_string("{\"type\":\"status\",\"msg\":\"SLE connected\"}\n");
    }

    /* 4. 启动主任务 */
    osal_kthread_create((osal_kthread_handler)ai_task, 0, "ai_board_task", 4096);
}

static void run_ai(void) { ai_board_entry(); }
app_run(run_ai);
