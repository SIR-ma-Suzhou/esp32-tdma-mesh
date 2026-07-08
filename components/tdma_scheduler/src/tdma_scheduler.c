#include "tdma_scheduler.h"
#include "time_sync.h"
#include "espnow_core.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "TDMA_SCHED";

#define TDMA_SYNC_GUARD_US      1000UL
#define TIME_DIFF_US(newer, older)  ((uint32_t)((newer) - (older)))

typedef struct {
    uint32_t superframe_period_us;
    uint32_t beacon_slot_us;
    uint32_t contention_slot_us;
    uint32_t emergency_slot_us;
    uint32_t data_slot_us;
    uint32_t guard_time_us;
    uint8_t  data_slot_count;
} tdma_config_t;

static tdma_config_t g_config = {
    .superframe_period_us  = TDMA_SUPERFRAME_PERIOD_US,
    .beacon_slot_us        = TDMA_BEACON_SLOT_US,
    .contention_slot_us    = TDMA_CONTENTION_SLOT_US,
    .emergency_slot_us     = TDMA_EMERGENCY_SLOT_US,
    .data_slot_us          = TDMA_DEFAULT_SLOT_US,
    .guard_time_us         = TDMA_GUARD_TIME_US,
    .data_slot_count       = TDMA_MAX_DATA_SLOTS,
};

static bool                     g_is_gateway    = false;
static bool                     g_is_joined     = false;
static uint8_t                  g_my_slot_id    = 0xFF;
static uint8_t                  g_my_mac[6];
static tdma_slot_info_t         g_slot_table[TDMA_MAX_NODES];
static SemaphoreHandle_t        g_table_mutex   = NULL;

static uint8_t                  g_gateway_mac[6] = {0};

static void tdma_recv_callback(const espnow_core_packet_info_t *pkt);
static void tdma_gateway_process_join_request(const tdma_join_req_t *req,
                                               const uint8_t *src_mac);

void tdma_scheduler_init(void)
{
    g_table_mutex = xSemaphoreCreateMutex();
    memset(g_slot_table, 0, sizeof(g_slot_table));
    espnow_core_get_local_mac(g_my_mac);

    espnow_core_register_recv_cb(tdma_recv_callback);

    ESP_LOGI(TAG, "TDMA scheduler initialized");
}

void tdma_scheduler_set_role(bool is_gateway)
{
    g_is_gateway = is_gateway;
    if (g_is_gateway) {
        g_is_joined = true;
        g_my_slot_id = 0;
        ESP_LOGI(TAG, "Role: GATEWAY");
    } else {
        ESP_LOGI(TAG, "Role: TERMINAL");
    }
}

void tdma_scheduler_set_params(uint32_t superframe_us,
                               uint32_t beacon_us,
                               uint32_t contention_us,
                               uint32_t emergency_us,
                               uint32_t data_slot_us,
                               uint32_t guard_us,
                               uint8_t  data_slot_cnt)
{
    if (xSemaphoreTake(g_table_mutex, pdMS_TO_TICKS(100))) {
        g_config.superframe_period_us = superframe_us;
        g_config.beacon_slot_us       = beacon_us;
        g_config.contention_slot_us   = contention_us;
        g_config.emergency_slot_us    = emergency_us;
        g_config.data_slot_us         = data_slot_us;
        g_config.guard_time_us        = guard_us;
        g_config.data_slot_count      = data_slot_cnt;
        xSemaphoreGive(g_table_mutex);
        ESP_LOGI(TAG, "Config updated: SF=%ums, DS=%ums, Guard=%uus",
                 (unsigned int)(superframe_us/1000),
                 (unsigned int)(data_slot_us/1000),
                 (unsigned int)guard_us);
    }
}

tdma_phase_t tdma_scheduler_get_phase(void)
{
    uint64_t net_time = time_sync_get_network_time_us();
    uint32_t t = (uint32_t)(net_time % g_config.superframe_period_us);

    uint32_t beacon_end      = g_config.beacon_slot_us;
    uint32_t contention_end  = beacon_end + g_config.contention_slot_us;
    uint32_t data_start      = contention_end + g_config.guard_time_us;
    // 每个数据时隙占用 data_slot_us + guard_time_us
    uint32_t slot_period     = g_config.data_slot_us + g_config.guard_time_us;
    uint32_t data_end        = data_start +
                               (uint32_t)g_config.data_slot_count * slot_period;
    uint32_t emergency_start = data_end + g_config.guard_time_us;
    uint32_t emergency_end   = emergency_start + g_config.emergency_slot_us;

    if (t < beacon_end)                   return TDMA_PHASE_BEACON;
    if (t < contention_end)              return TDMA_PHASE_CONTENTION;
    if (t >= data_start && t < data_end)  return TDMA_PHASE_DATA;
    if (t >= emergency_start && t < emergency_end) return TDMA_PHASE_EMERGENCY;
    return TDMA_PHASE_IDLE;
}

bool tdma_scheduler_is_my_slot(void)
{
    if (!g_is_joined || g_my_slot_id >= g_config.data_slot_count)
        return false;

    uint64_t net_time = time_sync_get_network_time_us();
    uint32_t t = (uint32_t)(net_time % g_config.superframe_period_us);

    uint32_t data_start = g_config.beacon_slot_us
                        + g_config.contention_slot_us
                        + g_config.guard_time_us;

    // 每个数据时隙包含 guard_time_us 的保护间隔
    uint32_t slot_period = g_config.data_slot_us + g_config.guard_time_us;
    uint32_t slot_start  = data_start + (uint32_t)g_my_slot_id * slot_period;
    uint32_t slot_end    = slot_start + g_config.data_slot_us;  // 有效发送窗口不含尾随guard

    return (t >= slot_start && t < slot_end);
}

bool tdma_scheduler_is_emergency_slot(void)
{
    return (tdma_scheduler_get_phase() == TDMA_PHASE_EMERGENCY);
}

bool tdma_scheduler_is_contention_slot(void)
{
    return (tdma_scheduler_get_phase() == TDMA_PHASE_CONTENTION);
}

const tdma_slot_info_t* tdma_scheduler_get_my_slot_info(void)
{
    if (g_my_slot_id >= g_config.data_slot_count) return NULL;
    return &g_slot_table[g_my_slot_id];
}

esp_err_t tdma_scheduler_join_request(const uint8_t node_mac[6],
                                       const uint8_t gateway_mac[6])
{
    if (g_is_gateway) {
        if (xSemaphoreTake(g_table_mutex, pdMS_TO_TICKS(2000))) {
            // 先释放该 MAC 之前占用的旧时隙（终端复位后重新入网会分到新时隙）
            for (uint8_t j = 0; j < g_config.data_slot_count; j++) {
                if (g_slot_table[j].status == TDMA_SLOT_ALLOCATED
                    && memcmp(g_slot_table[j].node_mac, node_mac, 6) == 0) {
                    g_slot_table[j].status = TDMA_SLOT_FREE;
                    memset(g_slot_table[j].node_mac, 0, 6);
                }
            }

            for (uint8_t i = 1; i < g_config.data_slot_count; i++) {
                if (g_slot_table[i].status == TDMA_SLOT_FREE) {
                    g_slot_table[i].slot_id   = i;
                    g_slot_table[i].status    = TDMA_SLOT_ALLOCATED;
                    memcpy(g_slot_table[i].node_mac, node_mac, 6);
                    uint32_t slot_period = g_config.data_slot_us + g_config.guard_time_us;
                g_slot_table[i].slot_start_us = g_config.beacon_slot_us
                                                   + g_config.contention_slot_us
                                                   + g_config.guard_time_us
                                                   + (uint32_t)i * slot_period;
                    g_slot_table[i].slot_duration_us = g_config.data_slot_us;
                    xSemaphoreGive(g_table_mutex);

                    tdma_join_reply_t reply = {
                        .type = TDMA_TYPE_JOIN_REPLY,
                        .assigned_slot = i,
                        .status = 0  // 成功
                    };
                    memcpy(reply.node_mac, node_mac, 6);
                    espnow_core_send_data(node_mac, (uint8_t *)&reply,
                                          sizeof(reply), 1);  // 高优先级

                    ESP_LOGI(TAG, "Assigned slot %u to node %02X:%02X:%02X:%02X:%02X:%02X",
                             i, node_mac[0], node_mac[1], node_mac[2],
                             node_mac[3], node_mac[4], node_mac[5]);
                    return ESP_OK;
                }
            }
            xSemaphoreGive(g_table_mutex);
        }
        // 无空闲时隙
        ESP_LOGW(TAG, "No free slot for joining node");
        return ESP_ERR_NO_MEM;
    } else {
        if (gateway_mac == NULL) return ESP_ERR_INVALID_ARG;

        memcpy(g_gateway_mac, gateway_mac, 6);

        tdma_join_req_t req = {
            .type = TDMA_TYPE_JOIN_REQ,
            .requested_slots = 1
        };
        memcpy(req.node_mac, node_mac, 6);

        esp_err_t ret = espnow_core_send_data(gateway_mac, (uint8_t *)&req,
                                               sizeof(req), 1);
        if (ret) {
            ESP_LOGI(TAG, "Join request sent to gateway");
            return ESP_OK;
        } else {
            ESP_LOGE(TAG, "Failed to send join request");
            return ESP_FAIL;
        }
    }
}

esp_err_t tdma_scheduler_leave(void)
{
    if (g_is_gateway && g_my_slot_id < g_config.data_slot_count) {
        if (xSemaphoreTake(g_table_mutex, pdMS_TO_TICKS(100))) {
            g_slot_table[g_my_slot_id].status = TDMA_SLOT_FREE;
            memset(g_slot_table[g_my_slot_id].node_mac, 0, 6);
            xSemaphoreGive(g_table_mutex);
        }
    }
    g_is_joined  = false;
    g_my_slot_id = 0xFF;
    return ESP_OK;
}

uint32_t tdma_scheduler_get_slot_remaining_us(void)
{
    uint64_t net_time = time_sync_get_network_time_us();
    uint32_t t = (uint32_t)(net_time % g_config.superframe_period_us);
    uint32_t data_start = g_config.beacon_slot_us
                        + g_config.contention_slot_us
                        + g_config.guard_time_us;
    uint32_t slot_period = g_config.data_slot_us + g_config.guard_time_us;
    uint32_t slot_end = data_start
                      + (uint32_t)(g_my_slot_id + 1) * slot_period
                      - g_config.guard_time_us;  // 减去尾随 guard，返回有效发送窗口结束时间
    return (t < slot_end) ? (slot_end - t) : 0;
}

uint32_t tdma_scheduler_get_time_to_next_slot_us(void)
{
    if (!g_is_joined || g_my_slot_id >= g_config.data_slot_count)
        return g_config.superframe_period_us;  // 未入网，返回整个超帧周期

    uint64_t net_time = time_sync_get_network_time_us();
    uint32_t t = (uint32_t)(net_time % g_config.superframe_period_us);
    uint32_t data_start = g_config.beacon_slot_us
                        + g_config.contention_slot_us
                        + g_config.guard_time_us;
    uint32_t slot_period = g_config.data_slot_us + g_config.guard_time_us;
    uint32_t slot_start = data_start
                        + (uint32_t)g_my_slot_id * slot_period;
    if (t < slot_start)
        return slot_start - t;
    else
        return g_config.superframe_period_us - t + slot_start;
}

void tdma_scheduler_poll(void) { /* 保留接口兼容性 */ }

uint8_t tdma_scheduler_get_my_slot_id(void) { return g_my_slot_id; }
bool tdma_scheduler_is_joined(void) { return g_is_joined; }

esp_err_t tdma_scheduler_restore_slot(uint8_t slot_id)
{
    if (g_is_gateway) return ESP_ERR_INVALID_STATE;
    if (slot_id >= g_config.data_slot_count) return ESP_ERR_INVALID_ARG;

    g_my_slot_id = slot_id;
    g_is_joined = true;
    ESP_LOGI(TAG, "Slot restored from RTC: %u", slot_id);
    return ESP_OK;
}

// TDMA 接收回调（处理入网请求/应答）

static void tdma_recv_callback(const espnow_core_packet_info_t *pkt)
{
    if (pkt->data_len < 1) return;
    uint8_t type = pkt->data[0];

    switch (type) {
        case TDMA_TYPE_JOIN_REQ: {
            // 网关侧：处理入网请求
            if (!g_is_gateway) break;
            if (pkt->data_len < sizeof(tdma_join_req_t)) break;
            const tdma_join_req_t *req = (const tdma_join_req_t *)pkt->data;
            tdma_gateway_process_join_request(req, req->node_mac);
            break;
        }

        case TDMA_TYPE_JOIN_REPLY: {
            // 终端侧：处理入网应答
            if (g_is_gateway) break;
            if (pkt->data_len < sizeof(tdma_join_reply_t)) break;
            const tdma_join_reply_t *reply = (const tdma_join_reply_t *)pkt->data;

            // 确认是发给自己的
            if (memcmp(reply->node_mac, g_my_mac, 6) != 0) break;

            if (reply->status == 0) {
                g_my_slot_id = reply->assigned_slot;
                g_is_joined = true;
                ESP_LOGI(TAG, "✅ Joined network! Assigned slot: %u", g_my_slot_id);
            } else {
                ESP_LOGE(TAG, "Join rejected, status=%u", reply->status);
            }
            break;
        }

        default:
            break;
    }
}

// 网关处理入网请求的具体逻辑
static void tdma_gateway_process_join_request(const tdma_join_req_t *req,
                                               const uint8_t *src_mac)
{
    ESP_LOGI(TAG, "Join request from %02X:%02X:%02X:%02X:%02X:%02X",
             src_mac[0], src_mac[1], src_mac[2],
             src_mac[3], src_mac[4], src_mac[5]);

    // 直接调用统一的入网函数（网关侧逻辑）
    tdma_scheduler_join_request(src_mac, NULL);
}

// 时隙表 JSON 导出（供 Web 可视化使用）

static const char* slot_status_to_str(tdma_slot_status_t s)
{
    switch (s) {
        case TDMA_SLOT_FREE:       return "free";
        case TDMA_SLOT_ALLOCATED:  return "allocated";
        case TDMA_SLOT_EMERGENCY:  return "emergency";
        default:                   return "unknown";
    }
}

cJSON* tdma_scheduler_get_slot_json(void)
{
    if (!g_is_gateway) return NULL;

    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    char mac_str[18];
    uint8_t used = 0;

    cJSON *slots_array = cJSON_AddArrayToObject(root, "slots");
    if (!slots_array) {
        cJSON_Delete(root);
        return NULL;
    }

    if (xSemaphoreTake(g_table_mutex, pdMS_TO_TICKS(100))) {
        for (uint8_t i = 0; i < g_config.data_slot_count; i++) {
            cJSON *slot = cJSON_CreateObject();
            if (!slot) continue;

            cJSON_AddNumberToObject(slot, "slot_id", g_slot_table[i].slot_id);

            if (g_slot_table[i].status == TDMA_SLOT_FREE) {
                cJSON_AddNullToObject(slot, "node_mac");
            } else {
                snprintf(mac_str, sizeof(mac_str),
                         "%02X:%02X:%02X:%02X:%02X:%02X",
                         g_slot_table[i].node_mac[0], g_slot_table[i].node_mac[1],
                         g_slot_table[i].node_mac[2], g_slot_table[i].node_mac[3],
                         g_slot_table[i].node_mac[4], g_slot_table[i].node_mac[5]);
                cJSON_AddStringToObject(slot, "node_mac", mac_str);
                used++;
            }

            cJSON_AddStringToObject(slot, "status",
                                    slot_status_to_str(g_slot_table[i].status));
            cJSON_AddNumberToObject(slot, "slot_start_us",
                                    g_slot_table[i].slot_start_us);
            cJSON_AddNumberToObject(slot, "slot_duration_us",
                                    g_slot_table[i].slot_duration_us);

            cJSON_AddItemToArray(slots_array, slot);
        }

        xSemaphoreGive(g_table_mutex);
    }

    cJSON_AddNumberToObject(root, "total_slots", g_config.data_slot_count);
    cJSON_AddNumberToObject(root, "used_slots", used);
    cJSON_AddNumberToObject(root, "superframe_us", g_config.superframe_period_us);

    // 附加超帧阶段时间信息，方便前端定位时隙
    cJSON_AddNumberToObject(root, "beacon_us", g_config.beacon_slot_us);
    cJSON_AddNumberToObject(root, "contention_us", g_config.contention_slot_us);
    cJSON_AddNumberToObject(root, "emergency_us", g_config.emergency_slot_us);
    cJSON_AddNumberToObject(root, "guard_us", g_config.guard_time_us);

    return root;
}

char* tdma_scheduler_get_slot_string(void)
{
    cJSON *root = tdma_scheduler_get_slot_json();
    if (!root) return NULL;
    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return str;
}
