#ifndef __ESPNOW_CORE_H__
#define __ESPNOW_CORE_H__

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_now.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESPNOW_CORE_CHANNEL          1
#define ESPNOW_CORE_MAX_DATA_LEN     250
#define ESPNOW_CORE_BROADCAST_ADDR   {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}

typedef struct {
    uint8_t src_mac[6];         /**< 源 MAC 地址 */
    uint8_t dst_mac[6];         /**< 目标 MAC 地址 */
    uint8_t *data;              /**< 数据指针（仅在回调内有效） */
    int data_len;               /**< 数据长度 */
    int8_t rssi;                /**< RSSI 信号强度 */
    uint64_t timestamp_us;      /**< 接收时刻的微秒级时间戳 */
} espnow_core_packet_info_t;

typedef void (*espnow_core_recv_cb_t)(const espnow_core_packet_info_t *pkt_info);

typedef void (*espnow_core_send_cb_t)(const uint8_t *mac_addr, esp_now_send_status_t status);

esp_err_t espnow_core_init(void);

esp_err_t espnow_core_get_local_mac(uint8_t mac_out[6]);

void espnow_core_register_recv_cb(espnow_core_recv_cb_t cb);

void espnow_core_register_send_cb(espnow_core_send_cb_t cb);

bool espnow_core_add_peer(const uint8_t peer_mac[6], uint8_t channel);

bool espnow_core_del_peer(const uint8_t peer_mac[6]);

bool espnow_core_send_data(const uint8_t *dest_mac, const uint8_t *data, size_t len, uint8_t priority);

bool espnow_core_send_broadcast(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif // __ESPNOW_CORE_H__
