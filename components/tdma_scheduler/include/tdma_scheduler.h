#ifndef __TDMA_SCHEDULER_H__
#define __TDMA_SCHEDULER_H__

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

// 超帧与调度参数

#define TDMA_SUPERFRAME_PERIOD_US      500000UL   // 默认超帧总周期: 500ms
#define TDMA_BEACON_SLOT_US             10000UL   // 信标时隙: 10ms
#define TDMA_CONTENTION_SLOT_US         20000UL   // 竞争入网时隙: 20ms
#define TDMA_EMERGENCY_SLOT_US          30000UL   // 紧急时隙: 30ms
#define TDMA_DEFAULT_SLOT_US            50000UL   // 普通数据时隙: 50ms
#define TDMA_GUARD_TIME_US              1000UL    // 时隙间保护间隔: 1ms
#define TDMA_MAX_DATA_SLOTS             8         // 最大数据时隙数量
#define TDMA_MAX_NODES                  TDMA_MAX_DATA_SLOTS

#define TDMA_TYPE_JOIN_REQ      0x30    // 入网请求
#define TDMA_TYPE_JOIN_REPLY    0x31    // 入网应答

// 结构与枚举

typedef enum {
    TDMA_SLOT_FREE = 0,
    TDMA_SLOT_ALLOCATED,
    TDMA_SLOT_EMERGENCY,
} tdma_slot_status_t;

typedef enum {
    TDMA_PHASE_BEACON,       // 信标阶段
    TDMA_PHASE_CONTENTION,   // 竞争入网阶段
    TDMA_PHASE_DATA,         // 数据传输阶段
    TDMA_PHASE_EMERGENCY,    // 紧急传输阶段
    TDMA_PHASE_IDLE,         // 空闲阶段
} tdma_phase_t;

typedef struct {
    uint8_t  slot_id;
    uint8_t  node_mac[6];
    uint32_t slot_start_us;
    uint32_t slot_duration_us;
    tdma_slot_status_t status;
} tdma_slot_info_t;

typedef struct __attribute__((packed)) {
    uint8_t type;               // TDMA_TYPE_JOIN_REQ
    uint8_t node_mac[6];        // 请求节点 MAC
    uint8_t requested_slots;    // 请求时隙数量（通常为 1）
} tdma_join_req_t;

typedef struct __attribute__((packed)) {
    uint8_t type;               // TDMA_TYPE_JOIN_REPLY
    uint8_t node_mac[6];        // 分配节点 MAC
    uint8_t assigned_slot;      // 分配的时隙号
    uint8_t status;             // 0: 成功, 1: 无空闲时隙, 2: 拒绝
} tdma_join_reply_t;

// 公共 API

void tdma_scheduler_init(void);
void tdma_scheduler_set_role(bool is_gateway);
void tdma_scheduler_set_params(uint32_t superframe_us,
                               uint32_t beacon_us,
                               uint32_t contention_us,
                               uint32_t emergency_us,
                               uint32_t data_slot_us,
                               uint32_t guard_us,
                               uint8_t  data_slot_cnt);
tdma_phase_t tdma_scheduler_get_phase(void);
bool tdma_scheduler_is_my_slot(void);
bool tdma_scheduler_is_emergency_slot(void);
bool tdma_scheduler_is_contention_slot(void);
const tdma_slot_info_t* tdma_scheduler_get_my_slot_info(void);

esp_err_t tdma_scheduler_join_request(const uint8_t node_mac[6],
                                       const uint8_t gateway_mac[6]);

esp_err_t tdma_scheduler_leave(void);
uint32_t tdma_scheduler_get_slot_remaining_us(void);
uint32_t tdma_scheduler_get_time_to_next_slot_us(void);
void tdma_scheduler_poll(void);

uint8_t tdma_scheduler_get_my_slot_id(void);

bool tdma_scheduler_is_joined(void);

esp_err_t tdma_scheduler_restore_slot(uint8_t slot_id);

cJSON* tdma_scheduler_get_slot_json(void);

char* tdma_scheduler_get_slot_string(void);

#ifdef __cplusplus
}
#endif

#endif // __TDMA_SCHEDULER_H__
