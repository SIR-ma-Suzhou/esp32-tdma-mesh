#include "topology_aggregation.h"
#include "tdma_scheduler.h"
#include "espnow_core.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>
#include "cJSON.h"

static const char *TAG = "TOPO_AGG";

typedef struct {
    char mac[18];
    char json_str[512];
    uint32_t last_update_ms;
    bool valid;
} topo_entry_t;

static topo_entry_t s_topo_table[TOPO_MAX_NODES];

void topology_aggregation_init(void)
{
    memset(s_topo_table, 0, sizeof(s_topo_table));
    ESP_LOGI(TAG, "Initialized");
}

void topo_table_upsert(const char *json_str)
{
    if (!json_str) return;

    const char *p = strstr(json_str, "\"self_mac\":\"");
    if (!p) return;
    p += 12;
    char mac[18] = {0};
    for (int i = 0; i < 17 && *p && *p != '"'; i++, p++) mac[i] = *p;

    int slot = -1;
    for (int i = 0; i < TOPO_MAX_NODES; i++) {
        if (s_topo_table[i].valid && strcmp(s_topo_table[i].mac, mac) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < TOPO_MAX_NODES; i++) {
            if (!s_topo_table[i].valid) { slot = i; break; }
        }
    }
    if (slot < 0) return;

    s_topo_table[slot].valid = true;
    strncpy(s_topo_table[slot].mac, mac, 17);
    strncpy(s_topo_table[slot].json_str, json_str, 511);
    s_topo_table[slot].last_update_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

int topo_table_get_count(void)
{
    int count = 0;
    for (int i = 0; i < TOPO_MAX_NODES; i++) {
        if (s_topo_table[i].valid) count++;
    }
    return count;
}

char* topo_table_get_json(int index)
{
    int found = 0;
    for (int i = 0; i < TOPO_MAX_NODES; i++) {
        if (!s_topo_table[i].valid) continue;
        if (found == index) return strdup(s_topo_table[i].json_str);
        found++;
    }
    return NULL;
}

cJSON* topo_table_build_full_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    cJSON_AddNumberToObject(root, "timestamp", (double)now_ms);

    cJSON *nodes_arr = cJSON_AddArrayToObject(root, "nodes");
    if (!nodes_arr) {
        cJSON_Delete(root);
        return NULL;
    }

    // 1. 先添加网关自身节点（确保拓扑图始终至少显示网关）
    uint8_t gw_mac[6];
    if (espnow_core_get_local_mac(gw_mac) == ESP_OK) {
        cJSON *gw_node = cJSON_CreateObject();
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 gw_mac[0], gw_mac[1], gw_mac[2], gw_mac[3], gw_mac[4], gw_mac[5]);
        cJSON_AddStringToObject(gw_node, "self_mac", mac_str);
        cJSON_AddNullToObject(gw_node, "parent_mac");
        cJSON_AddNumberToObject(gw_node, "hop_count", 0);
        cJSON_AddArrayToObject(gw_node, "neighbors");
        cJSON_AddItemToArray(nodes_arr, gw_node);
    }

    // 2. 遍历拓扑报告（终端0x7E上报的详细数据）
    for (int i = 0; i < TOPO_MAX_NODES; i++) {
        if (!s_topo_table[i].valid) continue;
        if (now_ms - s_topo_table[i].last_update_ms > 30000) {
            s_topo_table[i].valid = false;
            continue;
        }
        cJSON *node = cJSON_Parse(s_topo_table[i].json_str);
        if (node) {
            cJSON_AddItemToArray(nodes_arr, node);
        }
    }

    // 3. 从 TDMA 时隙表补全：已入网（含RTC恢复使用旧时隙）的终端也能展示
    cJSON *tdma = tdma_scheduler_get_slot_json();
    if (tdma) {
        cJSON *tdma_slots = cJSON_GetObjectItem(tdma, "slots");
        if (tdma_slots && cJSON_IsArray(tdma_slots)) {
            cJSON *s;
            cJSON_ArrayForEach(s, tdma_slots) {
                cJSON *status = cJSON_GetObjectItem(s, "status");
                if (!status || !cJSON_IsString(status)) continue;
                bool is_allocated = (strcmp(status->valuestring, "allocated") == 0);
                bool is_free      = (strcmp(status->valuestring, "free") == 0);
                if (!is_allocated && !is_free) continue;

                cJSON *mac_item = cJSON_GetObjectItem(s, "node_mac");
                // free 时隙可能有残留 MAC（终端RTC恢复后在用），只要MAC非空就展示
                if (!mac_item || !cJSON_IsString(mac_item)) continue;
                const char *mac_str = mac_item->valuestring;
                if (strlen(mac_str) < 10) continue;  // 跳过空/无效MAC

                bool already_in = false;
                cJSON *existing;
                cJSON_ArrayForEach(existing, nodes_arr) {
                    cJSON *em = cJSON_GetObjectItem(existing, "self_mac");
                    if (em && cJSON_IsString(em) && strcmp(em->valuestring, mac_str) == 0) {
                        already_in = true;
                        break;
                    }
                }
                if (already_in) continue;

                // 构造最小拓扑节点（无详细邻居，但标注位置）
                cJSON *node = cJSON_CreateObject();
                cJSON_AddStringToObject(node, "self_mac", mac_str);
                cJSON_AddNullToObject(node, "parent_mac");
                cJSON_AddNumberToObject(node, "hop_count", 1);
                cJSON *empty_nei = cJSON_AddArrayToObject(node, "neighbors");
                (void)empty_nei;
                cJSON_AddItemToArray(nodes_arr, node);
            }
        }
        cJSON_Delete(tdma);
    }

    return root;
}
