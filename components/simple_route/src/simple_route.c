#include "simple_route.h"
#include "espnow_core.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_mac.h"

static const char *TAG = "ROUTE";

static bool g_is_gateway = false;
static uint8_t g_my_mac[6];
static uint8_t g_parent_mac[6];
static uint8_t g_hop_count = 0xFF;
static bool g_parent_valid = false;
static neighbor_info_t g_neighbors[ROUTE_MAX_NEIGHBORS];
static int g_neighbor_count = 0;
static route_data_cb_t g_data_cb = NULL;
static SemaphoreHandle_t g_route_mutex = NULL;

static uint32_t g_last_heartbeat_time = 0;
static uint32_t g_heartbeat_seq = 0;
static int g_heartbeat_unack_count = 0;

// 子节点跟踪（用于心跳 ACK 只回复子节点）
#define ROUTE_MAX_CHILDREN_TRACK  8
static uint8_t g_child_macs[ROUTE_MAX_CHILDREN_TRACK][6];
static uint32_t g_child_last_hb_ms[ROUTE_MAX_CHILDREN_TRACK];
static int g_child_count = 0;

static uint32_t g_last_hello_time = 0;

#ifndef MACSTR
#define MACSTR "%02X:%02X:%02X:%02X:%02X:%02X"
#endif
#ifndef MAC2STR
#define MAC2STR(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]
#endif

static void route_recv_callback(const espnow_core_packet_info_t *pkt);
static void select_parent(void);
static void forward_data(const route_data_packet_t *pkt, const espnow_core_packet_info_t *info);
static bool send_hello(void);
static void update_avg_rssi(neighbor_info_t *nei, int8_t new_rssi);
static bool is_child_mac(const uint8_t mac[6]);
static void track_child_mac(const uint8_t mac[6]);
static void route_check_heartbeat(void);

static uint32_t route_get_elapsed_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void route_init(void)
{
    g_route_mutex = xSemaphoreCreateMutex();
    memset(g_parent_mac, 0, 6);
    g_parent_valid = false;
    g_hop_count = 0xFF;
    g_neighbor_count = 0;
    espnow_core_get_local_mac(g_my_mac);

    espnow_core_register_recv_cb(route_recv_callback);
    ESP_LOGI(TAG, "Route module initialized");
}

void route_set_gateway(bool is_gateway)
{
    g_is_gateway = is_gateway;
    if (g_is_gateway) {
        g_hop_count = 0;
        g_parent_valid = false;
        memset(g_parent_mac, 0, 6);
        ESP_LOGI(TAG, "Role: GATEWAY (root)");
    } else {
        ESP_LOGI(TAG, "Role: TERMINAL");
    }
}

void route_register_data_callback(route_data_cb_t cb)
{
    g_data_cb = cb;
}

const uint8_t* route_get_parent_mac(void)
{
    if (g_parent_valid)
        return g_parent_mac;
    return NULL;
}

uint8_t route_get_hop_count(void)
{
    return g_hop_count;
}

bool route_send_to_gateway(const uint8_t *data, size_t len, const uint8_t gateway_mac[6])
{
    if (len > ESP_NOW_MAX_DATA_LEN - sizeof(route_data_packet_t)) {
        ESP_LOGE(TAG, "Data too long for routing");
        return false;
    }

    if (g_is_gateway) {
        ESP_LOGW(TAG, "Gateway should not call route_send_to_gateway");
        return false;
    }

    if (gateway_mac == NULL) {
        ESP_LOGE(TAG, "Gateway MAC is NULL");
        return false;
    }

    // 构建路由数据包（TTL 按跳数 × 2，留足余量）
    size_t total_len = sizeof(route_data_packet_t) + len;
    uint8_t *buf = (uint8_t *)malloc(total_len);
    if (!buf) return false;
    route_data_packet_t *pkt = (route_data_packet_t *)buf;
    pkt->type = ROUTE_TYPE_DATA;
    memcpy(pkt->src_origin, g_my_mac, 6);
    memcpy(pkt->dst_final, gateway_mac, 6);
    pkt->ttl = ROUTE_MAX_TTL;
    memcpy(pkt->payload, data, len);

    const uint8_t *next_hop = route_get_parent_mac();
    if (!next_hop) {
        free(buf);
        ESP_LOGE(TAG, "No parent, cannot send");
        return false;
    }

    bool ret = espnow_core_send_data(next_hop, buf, total_len, 0);
    free(buf);
    return ret;
}

bool route_send_hello_if_contention(void)
{
    uint32_t now = route_get_elapsed_ms();
    if (now - g_last_hello_time >= ROUTE_HELLO_INTERVAL_MS) {
        if (send_hello()) {
            g_last_hello_time = now;
            return true;
        }
    }
    return false;
}

void route_send_heartbeat(void)
{
    if (!g_parent_valid) return;
    uint32_t now = route_get_elapsed_ms();
    if (now - g_last_heartbeat_time < ROUTE_HEARTBEAT_INTERVAL_MS) return;
    g_last_heartbeat_time = now;

    route_heartbeat_t hb;
    hb.type = ROUTE_TYPE_HEARTBEAT;
    memcpy(hb.sender_mac, g_my_mac, 6);
    hb.seq = g_heartbeat_seq++;

    espnow_core_send_data(g_parent_mac, (uint8_t *)&hb, sizeof(hb), 0);

    if (xSemaphoreTake(g_route_mutex, pdMS_TO_TICKS(10))) {
        g_heartbeat_unack_count++;
        xSemaphoreGive(g_route_mutex);
    }
}

static bool send_hello(void)
{
    route_hello_t hello;
    hello.type = ROUTE_TYPE_HELLO;
    memcpy(hello.sender_mac, g_my_mac, 6);
    hello.hop_count = g_hop_count;
    hello.flags = (g_child_count > 0) ? 0x01 : 0x00;  // 宣告自己是否已是中继
    return espnow_core_send_broadcast((uint8_t *)&hello, sizeof(hello));
}

// 路由主处理
void route_process(void)
{
    uint32_t now = route_get_elapsed_ms();
    if (xSemaphoreTake(g_route_mutex, pdMS_TO_TICKS(10))) {
        // 邻居老化
        for (int i = 0; i < g_neighbor_count; ) {
            if (now - g_neighbors[i].last_seen_ms > ROUTE_NEIGHBOR_TIMEOUT_MS) {
                ESP_LOGI(TAG, "Neighbor " MACSTR " timed out", MAC2STR(g_neighbors[i].mac));
                memmove(&g_neighbors[i], &g_neighbors[i+1], (g_neighbor_count - i - 1) * sizeof(neighbor_info_t));
                g_neighbor_count--;
            } else {
                i++;
            }
        }

        // 检测父节点是否丢失
        if (g_parent_valid) {
            bool parent_exists = false;
            for (int i = 0; i < g_neighbor_count; i++) {
                if (memcmp(g_neighbors[i].mac, g_parent_mac, 6) == 0) {
                    parent_exists = true;
                    break;
                }
            }
            if (!parent_exists) {
                g_parent_valid = false;
                ESP_LOGW(TAG, "Parent lost, re-selecting...");
                g_heartbeat_unack_count = 0;
            }
        }

        // 心跳超时检测
        route_check_heartbeat();

        // 如果没有父节点且不是网关，尝试选择父节点
        if (!g_is_gateway && !g_parent_valid) {
            select_parent();
        }
        xSemaphoreGive(g_route_mutex);
    }
}

// 滑动窗口更新平均 RSSI
static void update_avg_rssi(neighbor_info_t *nei, int8_t new_rssi)
{
    nei->rssi_history[nei->rssi_idx % ROUTE_RSSI_WINDOW_SIZE] = new_rssi;
    nei->rssi_idx++;
    int sum = 0;
    int count = (nei->rssi_idx >= ROUTE_RSSI_WINDOW_SIZE) ? ROUTE_RSSI_WINDOW_SIZE : nei->rssi_idx;
    for (int i = 0; i < count; i++) {
        sum += nei->rssi_history[i];
    }
    nei->avg_rssi = sum / count;
}

// 选择父节点：最小跳数 + 最强平均 RSSI + 中继节点偏好
static void select_parent(void)
{
    uint8_t best_hop = 255;
    int8_t best_rssi = -128;
    bool best_is_relay = false;
    int best_idx = -1;

    for (int i = 0; i < g_neighbor_count; i++) {
        if (g_neighbors[i].hop_count == 0xFF) continue; // 未同步节点忽略
        // 不能选择跳数大于等于自己的节点（避免环路，且保证跳数严格减小）
        if (g_neighbors[i].hop_count >= g_hop_count) continue;

        bool is_relay = (g_neighbors[i].flags & 0x01);

        if (g_neighbors[i].hop_count < best_hop) {
            // 跳数更少：总是优先
            best_hop = g_neighbors[i].hop_count;
            best_rssi = g_neighbors[i].avg_rssi;
            best_is_relay = is_relay;
            best_idx = i;
        } else if (g_neighbors[i].hop_count == best_hop) {
            // 跳数相同：已是中继 > 不是中继；同为(非)中继比 RSSI
            bool upgrade = false;
            if (is_relay && !best_is_relay) {
                upgrade = true;  // 偏好已是中继的节点
            } else if (is_relay == best_is_relay &&
                       g_neighbors[i].avg_rssi > best_rssi) {
                upgrade = true;  // 同类型比 RSSI
            }
            if (upgrade) {
                best_hop = g_neighbors[i].hop_count;
                best_rssi = g_neighbors[i].avg_rssi;
                best_is_relay = is_relay;
                best_idx = i;
            }
        }
    }

    if (best_idx >= 0) {
        memcpy(g_parent_mac, g_neighbors[best_idx].mac, 6);
        g_hop_count = best_hop + 1;
        g_parent_valid = true;
        g_heartbeat_unack_count = 0;

        // 将选中的父节点添加为 ESP-NOW peer（多跳转发的关键）
        uint8_t ch;
        wifi_second_chan_t sc;
        esp_wifi_get_channel(&ch, &sc);
        espnow_core_add_peer(g_parent_mac, ch);

        ESP_LOGI(TAG, "Selected parent " MACSTR " hop=%d avg_rssi=%d",
                 MAC2STR(g_parent_mac), g_hop_count, best_rssi);
    }
}

static void route_recv_callback(const espnow_core_packet_info_t *pkt)
{
    if (pkt->data_len < 1) return;
    uint8_t type = pkt->data[0];
    uint32_t now = route_get_elapsed_ms();

    switch (type) {
        case ROUTE_TYPE_HELLO: {
            if (pkt->data_len < sizeof(route_hello_t)) break;
            const route_hello_t *hello = (const route_hello_t *)pkt->data;
            if (memcmp(hello->sender_mac, g_my_mac, 6) == 0) break;

            if (xSemaphoreTake(g_route_mutex, pdMS_TO_TICKS(20))) {
                bool found = false;
                for (int i = 0; i < g_neighbor_count; i++) {
                    if (memcmp(g_neighbors[i].mac, hello->sender_mac, 6) == 0) {
                        update_avg_rssi(&g_neighbors[i], pkt->rssi);
                        g_neighbors[i].hop_count = hello->hop_count;
                        g_neighbors[i].flags     = hello->flags;
                        g_neighbors[i].last_seen_ms = now;
                        found = true;
                        break;
                    }
                }
                if (!found && g_neighbor_count < ROUTE_MAX_NEIGHBORS) {
                    memset(&g_neighbors[g_neighbor_count], 0, sizeof(neighbor_info_t));
                    memcpy(g_neighbors[g_neighbor_count].mac, hello->sender_mac, 6);
                    update_avg_rssi(&g_neighbors[g_neighbor_count], pkt->rssi);
                    g_neighbors[g_neighbor_count].hop_count = hello->hop_count;
                    g_neighbors[g_neighbor_count].flags     = hello->flags;
                    g_neighbors[g_neighbor_count].last_seen_ms = now;
                    g_neighbor_count++;
                }
                xSemaphoreGive(g_route_mutex);
            }
            break;
        }

        case ROUTE_TYPE_HEARTBEAT: {
            if (pkt->data_len < sizeof(route_heartbeat_t)) break;
            const route_heartbeat_t *hb = (const route_heartbeat_t *)pkt->data;
            if (memcmp(hb->sender_mac, g_my_mac, 6) == 0) break;

            // 跟踪子节点：收到心跳的节点可能是我的子节点
            track_child_mac(hb->sender_mac);

            // 只回复已知子节点的心跳
            if (is_child_mac(hb->sender_mac)) {
                route_heartbeat_ack_t ack;
                ack.type = ROUTE_TYPE_HEARTBEAT_ACK;
                memcpy(ack.sender_mac, g_my_mac, 6);
                ack.seq = hb->seq;
                espnow_core_send_data(hb->sender_mac, (uint8_t *)&ack, sizeof(ack), 0);
            }
            break;
        }

        case ROUTE_TYPE_HEARTBEAT_ACK: {
            if (pkt->data_len < sizeof(route_heartbeat_ack_t)) break;
            const route_heartbeat_ack_t *ack = (const route_heartbeat_ack_t *)pkt->data;
            if (memcmp(ack->sender_mac, g_parent_mac, 6) == 0) {
                // 收到父节点的 ACK，重置计数器（加锁保护，防止与主循环竞态）
                if (xSemaphoreTake(g_route_mutex, pdMS_TO_TICKS(10))) {
                    g_heartbeat_unack_count = 0;
                    xSemaphoreGive(g_route_mutex);
                }
            }
            break;
        }

        case ROUTE_TYPE_DATA: {
            if (pkt->data_len < sizeof(route_data_packet_t)) break;
            const route_data_packet_t *rd = (const route_data_packet_t *)pkt->data;

            if (memcmp(rd->dst_final, g_my_mac, 6) == 0) {
                size_t payload_len = pkt->data_len - sizeof(route_data_packet_t);
                if (g_data_cb) {
                    g_data_cb(rd->src_origin, rd->payload, payload_len);
                } else {
                    ESP_LOGI(TAG, "Data from " MACSTR " arrived, no callback registered",
                             MAC2STR(rd->src_origin));
                }
            } else {
                // 转发
                if (!g_is_gateway && rd->ttl > 0) {
                    forward_data(rd, pkt);
                } else {
                    ESP_LOGW(TAG, "TTL expired or gateway cannot forward");
                }
            }
            break;
        }

        default:
            break;
    }
}

// 转发数据包（TTL递减）
static void forward_data(const route_data_packet_t *pkt, const espnow_core_packet_info_t *info)
{
    if (!g_parent_valid) {
        ESP_LOGW(TAG, "No parent, cannot forward");
        return;
    }

    // 复制数据包，TTL减1
    size_t total_len = info->data_len;
    uint8_t *buf = (uint8_t *)malloc(total_len);
    if (!buf) return;
    memcpy(buf, info->data, total_len);
    route_data_packet_t *new_pkt = (route_data_packet_t *)buf;
    new_pkt->ttl--;

    bool ret = espnow_core_send_data(g_parent_mac, buf, total_len, 0);
    free(buf);
    if (!ret) {
        ESP_LOGE(TAG, "Forward failed");
    }
}

int route_get_neighbor_count(void)
{
    return g_neighbor_count;
}

bool route_has_children(void)
{
    uint32_t now = route_get_elapsed_ms();
    for (int i = 0; i < g_child_count; i++) {
        if (now - g_child_last_hb_ms[i] < 30000) {  // 30秒内有心跳
            return true;
        }
    }
    return false;
}

// 心跳检查：应该在主循环中调用，或在独立定时器中
void route_check_heartbeat(void)
{
    if (!g_parent_valid || g_is_gateway) return;
    // 未经确认的计数超过阈值，重新选父
    if (g_heartbeat_unack_count >= ROUTE_HEARTBEAT_MAX_RETRIES) {
        ESP_LOGW(TAG, "Heartbeat lost, re-selecting parent");
        g_parent_valid = false;
        g_heartbeat_unack_count = 0;
    }
}

// 子节点跟踪（心跳 ACK 只回复子节点）

static bool is_child_mac(const uint8_t mac[6])
{
    uint32_t now = route_get_elapsed_ms();
    for (int i = 0; i < g_child_count; i++) {
        if (memcmp(g_child_macs[i], mac, 6) == 0) {
            // 超时清理：30秒无心跳则移除
            if (now - g_child_last_hb_ms[i] > 30000) {
                memmove(&g_child_macs[i], &g_child_macs[i + 1],
                        (g_child_count - i - 1) * 6);
                memmove(&g_child_last_hb_ms[i], &g_child_last_hb_ms[i + 1],
                        (g_child_count - i - 1) * sizeof(uint32_t));
                g_child_count--;
                return false;
            }
            return true;
        }
    }
    return false;
}

static void track_child_mac(const uint8_t mac[6])
{
    uint32_t now = route_get_elapsed_ms();
    for (int i = 0; i < g_child_count; i++) {
        if (memcmp(g_child_macs[i], mac, 6) == 0) {
            g_child_last_hb_ms[i] = now;
            return;
        }
    }
    if (g_child_count < ROUTE_MAX_CHILDREN_TRACK) {
        memcpy(g_child_macs[g_child_count], mac, 6);
        g_child_last_hb_ms[g_child_count] = now;
        g_child_count++;
    }
}

// 拓扑信息导出（供 Web 可视化使用）

static void mac_to_json_str(const uint8_t mac[6], char *out)
{
    snprintf(out, 18, MACSTR, MAC2STR(mac));
}

cJSON* simple_route_get_topology_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    char mac_str[18];

    if (xSemaphoreTake(g_route_mutex, pdMS_TO_TICKS(100))) {
        // self_mac
        mac_to_json_str(g_my_mac, mac_str);
        cJSON_AddStringToObject(root, "self_mac", mac_str);

        // parent_mac
        if (g_parent_valid) {
            mac_to_json_str(g_parent_mac, mac_str);
            cJSON_AddStringToObject(root, "parent_mac", mac_str);
        } else {
            cJSON_AddNullToObject(root, "parent_mac");
        }

        // hop_count
        cJSON_AddNumberToObject(root, "hop_count", g_hop_count);

        // neighbors[]
        cJSON *neighbors = cJSON_AddArrayToObject(root, "neighbors");
        if (neighbors) {
            for (int i = 0; i < g_neighbor_count; i++) {
                cJSON *nei = cJSON_CreateObject();
                if (!nei) continue;

                mac_to_json_str(g_neighbors[i].mac, mac_str);
                cJSON_AddStringToObject(nei, "mac", mac_str);
                cJSON_AddNumberToObject(nei, "rssi", g_neighbors[i].avg_rssi);
                cJSON_AddNumberToObject(nei, "hop_count", g_neighbors[i].hop_count);

                bool is_parent = g_parent_valid &&
                    (memcmp(g_neighbors[i].mac, g_parent_mac, 6) == 0);
                cJSON_AddBoolToObject(nei, "is_parent", is_parent);

                cJSON_AddItemToArray(neighbors, nei);
            }
        }

        xSemaphoreGive(g_route_mutex);
    }

    return root;
}

char* simple_route_get_topology_string(void)
{
    cJSON *root = simple_route_get_topology_json();
    if (!root) return NULL;
    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return str;
}
