#ifndef __SIMPLE_ROUTE_H__
#define __SIMPLE_ROUTE_H__

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ROUTE_TYPE_HELLO         0x20
#define ROUTE_TYPE_HEARTBEAT     0x24
#define ROUTE_TYPE_HEARTBEAT_ACK 0x25
#define ROUTE_TYPE_DATA          0x23

#define ROUTE_MAX_NEIGHBORS      10
#define ROUTE_MAX_CHILDREN       5
#define ROUTE_HELLO_INTERVAL_MS  200
#define ROUTE_NEIGHBOR_TIMEOUT_MS 15000
#define ROUTE_HEARTBEAT_INTERVAL_MS 3000
#define ROUTE_HEARTBEAT_MAX_RETRIES   3
#define ROUTE_RSSI_WINDOW_SIZE   5
#define ROUTE_MAX_TTL            5

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t sender_mac[6];
    uint8_t hop_count;
    uint8_t flags;              // bit0: has_children
} route_hello_t;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t sender_mac[6];
    uint32_t seq;
} route_heartbeat_t;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t sender_mac[6];
    uint32_t seq;
} route_heartbeat_ack_t;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t src_origin[6];
    uint8_t dst_final[6];
    uint8_t ttl;                // 每跳递减，为0丢弃
    uint8_t payload[];
} route_data_packet_t;

typedef struct {
    uint8_t mac[6];
    int8_t  rssi_history[ROUTE_RSSI_WINDOW_SIZE];
    uint8_t rssi_idx;
    int8_t  avg_rssi;
    uint8_t hop_count;
    uint8_t flags;              // bit0: has_children
    uint32_t last_seen_ms;
} neighbor_info_t;

typedef void (*route_data_cb_t)(const uint8_t *src_mac, const uint8_t *data, size_t len);

void route_init(void);
void route_set_gateway(bool is_gateway);
void route_register_data_callback(route_data_cb_t cb);
const uint8_t* route_get_parent_mac(void);
uint8_t route_get_hop_count(void);
bool route_send_to_gateway(const uint8_t *data, size_t len, const uint8_t gateway_mac[6]);
bool route_send_hello_if_contention(void);
void route_send_heartbeat(void);
void route_process(void);
int route_get_neighbor_count(void);

// 是否有活跃子节点（30s内有心跳），用于判定为中继/叶子
bool route_has_children(void);

cJSON* simple_route_get_topology_json(void);

char* simple_route_get_topology_string(void);

#ifdef __cplusplus
}
#endif

#endif // __SIMPLE_ROUTE_H__
