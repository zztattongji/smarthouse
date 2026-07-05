/**
 * @file    main.c
 * @brief   3 号板 — 执行器控制 (app_run 入口)
 *   收 MSG_DEVICE_CMD → 执行 → 广播 MSG_ACTUATOR_STATE
 */
#include "actuator.h"
#include "sle_bus.h"
#include "msg_builder.h"
#include "pinctrl.h"
#include "soc_osal.h"
#include "app_init.h"
#include "common_def.h"
#include "cJSON.h"
#include "sle_uart_client.h"
#include <string.h>

static uint8_t  g_armed, g_pir_prev;
static uint16_t g_gw_conn_id = 0;
static uint8_t  g_sle_connected = 0;

static int json_payload_len_valid(int len)
{
    return (len > 0 && len < SLE_BUS_PAYLOAD_MAX);
}

/* ── 命令队列: 双槽 + 互斥 + 仲裁 (Gateway > AI within same cycle) ── */
#define CMD_QUEUE_SIZE 2
typedef struct {
    char payload[SLE_BUS_PAYLOAD_MAX + 1];
    uint32_t seq;
    uint8_t src;
    uint8_t valid;
} cmd_slot_t;

static cmd_slot_t g_cmd_queue[CMD_QUEUE_SIZE];
static uint32_t g_cmd_seq;

static uint8_t cmd_priority(uint8_t src)
{
    return (src == BOARD_GATEWAY) ? 2 : 1;
}

static void actuator_check_conn(void)
{
    uint16_t cur_id = get_g_sle_uart_conn_id();
    if (cur_id == 0 || cur_id != g_gw_conn_id) {
        g_sle_connected = 0;
        g_gw_conn_id = 0;
    }
}

static void actuator_reconnect(void)
{
    actuator_check_conn();
    if (g_sle_connected) return;
    /* 主循环 1s, 每轮重试即可 */
    sle_uart_start_scan();
    osal_msleep(50);
    uint16_t conn_id = get_g_sle_uart_conn_id();
    if (conn_id != 0) {
        g_gw_conn_id = conn_id;
        g_sle_connected = 1;
    }
}

static void broadcast_state(void)
{
    actuator_reconnect();  /* 断线自动重连 */
    if (!g_sle_connected) return;
    actuator_state_t s; actuator_get_state(&s);
    char json[SLE_BUS_PAYLOAD_MAX];
    int jl = build_actuator_state_json(json, sizeof(json), s.led_brightness,
        s.fan_speed, s.motor_state, s.relay_state, s.servo_angle, s.buzzer_state, s.pir_triggered);
    if (!json_payload_len_valid(jl)) return;
    sle_bus_packet_t pkt;
    uint16_t pl = sle_bus_pack(&pkt, BOARD_ACTUATOR, MSG_ACTUATOR_STATE, json, (uint16_t)jl);
    if (pl) {
        ssapc_write_param_t *wp = get_g_sle_uart_send_param();
        if (wp) { wp->data_len = pl; wp->data = (uint8_t *)&pkt;
                  ssapc_write_req(0, g_gw_conn_id, wp); }
    }
}

static void send_device_ack(uint32_t msg_id, uint8_t target_board,
    const char *device, uint8_t ok, const char *state)
{
    actuator_reconnect();
    if (!g_sle_connected) return;

    char json[SLE_BUS_PAYLOAD_MAX];
    int jl = build_device_ack_json(json, sizeof(json), msg_id, target_board, device, ok, state);
    if (!json_payload_len_valid(jl)) return;

    sle_bus_packet_t pkt;
    uint16_t pl = sle_bus_pack(&pkt, BOARD_ACTUATOR, MSG_DEVICE_ACK, json, (uint16_t)jl);
    if (pl) {
        ssapc_write_param_t *wp = get_g_sle_uart_send_param();
        if (wp) {
            wp->data_len = pl;
            wp->data = (uint8_t *)&pkt;
            ssapc_write_req(0, g_gw_conn_id, wp);
        }
    }
}

static void execute(const char *json)
{
    cJSON *r = cJSON_Parse(json); if (!r) return;
    cJSON *intent = cJSON_GetObjectItem(r, "intent");
    if (intent && cJSON_IsString(intent) && strcmp(intent->valuestring, "device.set") != 0) {
        cJSON_Delete(r);
        return;
    }
    cJSON *target = cJSON_GetObjectItem(r, "target");
    if (target && cJSON_IsNumber(target) &&
        target->valueint != 0 && target->valueint != BOARD_ACTUATOR) {
        cJSON_Delete(r);
        return;
    }
    cJSON *d = cJSON_GetObjectItem(r, "device");
    cJSON *v = cJSON_GetObjectItem(r, "value");
    cJSON *e = cJSON_GetObjectItem(r, "extra");
    if (!d || !cJSON_IsString(d)) { cJSON_Delete(r); return; }
    const char *dev = d->valuestring;
    int val = (v && cJSON_IsNumber(v)) ? v->valueint : 0;
    const char *ext = (e && cJSON_IsString(e)) ? e->valuestring : "";
    cJSON *mid = cJSON_GetObjectItem(r, "msg_id");
    cJSON *src = cJSON_GetObjectItem(r, "source");
    uint32_t msg_id = (mid && cJSON_IsNumber(mid)) ? (uint32_t)mid->valuedouble : 0;
    uint8_t target_board = (src && cJSON_IsNumber(src)) ? (uint8_t)src->valueint : BOARD_GATEWAY;
    uint8_t ok = 1;
    const char *state = "ok";

    if      (!strcmp(dev, "led"))         led_set((uint8_t)val);
    else if (!strcmp(dev, "fan"))         fan_set((uint8_t)val);
    else if (!strcmp(dev, "motor"))       motor_set(ext);
    else if (!strcmp(dev, "relay"))       relay_set((uint8_t)(val != 0));
    else if (!strcmp(dev, "servo"))       servo_set((uint8_t)val);
    else if (!strcmp(dev, "buzzer"))      buzzer_set((uint8_t)(val != 0));
    else if (!strcmp(dev, "ir"))          ir_send_code(ext);
    else if (!strcmp(dev, "buzzer_arm"))  { g_armed = val ? 1 : 0; actuator_set_armed(g_armed); }
    else { ok = 0; state = "unknown_device"; }
    send_device_ack(msg_id, target_board, dev, ok, state);
    cJSON_Delete(r);
    broadcast_state();
}

/* ── SLE Client notification 回调: 收到 Server (1号板) 下发的命令 ── */
static void sle_client_notify_cb(uint8_t client_id, uint16_t conn_id,
                                  ssapc_handle_value_t *data, errcode_t status)
{
    (void)client_id; (void)conn_id;
    if (status != ERRCODE_SUCC || data == NULL) return;
    if (data->data == NULL || data->data_len < 8) return;

    sle_bus_packet_t *p = (sle_bus_packet_t *)data->data;
    if (sle_bus_validate(p, data->data_len) != 0) return;
    if (p->msg_type != MSG_DEVICE_CMD) return;
    if (p->src_board != BOARD_GATEWAY && p->src_board != BOARD_AI) return;

    uint16_t pl = p->payload_len;
    if (pl > SLE_BUS_PAYLOAD_MAX) pl = SLE_BUS_PAYLOAD_MAX;

    uint32_t irq = osal_irq_lock();
    uint8_t idx = 0xFF;
    for (uint8_t i = 0; i < CMD_QUEUE_SIZE; i++) {
        if (!g_cmd_queue[i].valid) { idx = i; break; }
    }
    if (idx == 0xFF) idx = (g_cmd_queue[0].seq <= g_cmd_queue[1].seq) ? 0 : 1;
    (void)memcpy(g_cmd_queue[idx].payload, p->payload, pl);
    g_cmd_queue[idx].payload[pl] = '\0';
    g_cmd_queue[idx].seq = ++g_cmd_seq;
    g_cmd_queue[idx].src = p->src_board;
    g_cmd_queue[idx].valid = 1;
    osal_irq_restore(irq);
}

static void sle_client_indication_cb(uint8_t client_id, uint16_t conn_id,
                                      ssapc_handle_value_t *data, errcode_t status)
{
    (void)client_id; (void)conn_id; (void)data; (void)status;
}

/* ── 主循环消费队列, 同一周期内 Gateway 优先于 AI ── */
static void process_cmd_queue(void)
{
    cmd_slot_t pending[CMD_QUEUE_SIZE];
    uint32_t irq = osal_irq_lock();
    (void)memcpy(pending, g_cmd_queue, sizeof(pending));
    for (uint8_t i = 0; i < CMD_QUEUE_SIZE; i++) {
        g_cmd_queue[i].valid = 0;
    }
    osal_irq_restore(irq);

    uint8_t locked_best_idx = 0xFF;
    for (uint8_t i = 0; i < CMD_QUEUE_SIZE; i++) {
        if (!pending[i].valid) continue;
        if (locked_best_idx == 0xFF) {
            locked_best_idx = i;
            continue;
        }

        uint8_t cur_prio = cmd_priority(pending[i].src);
        uint8_t best_prio = cmd_priority(pending[locked_best_idx].src);
        if (cur_prio > best_prio ||
            (cur_prio == best_prio && pending[i].seq > pending[locked_best_idx].seq)) {
            locked_best_idx = i;
        }
    }

    if (locked_best_idx != 0xFF) {
        execute(pending[locked_best_idx].payload);
    }
}

static void actuator_task(void)
{
    actuator_init_all();
    broadcast_state();
    for (;;) {
        process_cmd_queue();

        uint8_t pir = pir_read();
        if (g_armed && pir && !g_pir_prev) { buzzer_set(1); osal_msleep(300); buzzer_set(0); }
        g_pir_prev = pir;

        static uint8_t k1p, k2p;
        uint8_t k1 = key1_read(), k2 = key2_read();
        if (k1 && !k1p) {
            actuator_state_t s; actuator_get_state(&s);
            relay_set(s.relay_state ? 0 : 1);
            broadcast_state();
        }
        if (k2 && !k2p) { g_armed = g_armed ? 0 : 1; actuator_set_armed(g_armed); }
        k1p = k1; k2p = k2;

        broadcast_state();
        osal_msleep(1000);
    }
}

static void actuator_entry(void)
{
    /* 作为 Client 连到 1号板 Server */
    sle_uart_client_init(sle_client_notify_cb, sle_client_indication_cb);
    sle_uart_start_scan();

    /* 等待扫描连接完成 */
    osal_msleep(3000);
    g_gw_conn_id = get_g_sle_uart_conn_id();
    if (g_gw_conn_id != 0) g_sle_connected = 1;

    osal_kthread_create((osal_kthread_handler)actuator_task, 0, "actuator_task", 4096);
}

static void run_actuator(void) { actuator_entry(); }
app_run(run_actuator);

