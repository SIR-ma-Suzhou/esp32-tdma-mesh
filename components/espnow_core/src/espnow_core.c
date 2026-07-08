
#include "espnow_core.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ESPNOW_CORE";

#define ESPNOW_MAX_CALLBACKS 6

static espnow_core_recv_cb_t s_recv_cbs[ESPNOW_MAX_CALLBACKS];
static int s_recv_cb_count = 0;

static espnow_core_send_cb_t s_send_cbs[ESPNOW_MAX_CALLBACKS];
static int s_send_cb_count = 0;

typedef struct {
    uint8_t  dest_mac[6];
    uint8_t *data;
    size_t   len;
    uint8_t  priority;   /* 0=普通, 1=高优先级 */
} send_item_t;

#define SEND_QUEUE_LEN 32
static QueueHandle_t s_send_queue = NULL;
static SemaphoreHandle_t s_peer_mutex = NULL;
static uint8_t s_local_mac[6];
static bool s_initialized = false;

static void unified_espnow_recv_cb(const esp_now_recv_info_t *recv_info,
                                   const uint8_t *data, int data_len)
{
    if (data == NULL || data_len <= 0) return;
    if (recv_info == NULL || recv_info->src_addr == NULL) return;

    espnow_core_packet_info_t pkt;
    memcpy(pkt.src_mac, recv_info->src_addr, 6);
    memcpy(pkt.dst_mac, recv_info->des_addr, 6);
    pkt.data         = (uint8_t *)data;
    pkt.data_len     = data_len;
    pkt.rssi         = recv_info->rx_ctrl ? recv_info->rx_ctrl->rssi : 0;
    pkt.timestamp_us = recv_info->rx_ctrl ? (uint64_t)recv_info->rx_ctrl->timestamp : 0;

    for (int i = 0; i < s_recv_cb_count; i++) {
        if (s_recv_cbs[i]) {
            s_recv_cbs[i](&pkt);
        }
    }
}

static void unified_espnow_send_cb(const uint8_t *mac_addr,
                                   esp_now_send_status_t status)
{
    for (int i = 0; i < s_send_cb_count; i++) {
        if (s_send_cbs[i]) {
            s_send_cbs[i](mac_addr, status);
        }
    }
}

static void send_task(void *arg)
{
    send_item_t item;

    while (1) {
        if (xQueueReceive(s_send_queue, &item, portMAX_DELAY) == pdTRUE) {
            esp_err_t ret = esp_now_send(item.dest_mac, item.data, item.len);

            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "esp_now_send failed: %s, retrying once...",
                         esp_err_to_name(ret));
                vTaskDelay(pdMS_TO_TICKS(5));
                ret = esp_now_send(item.dest_mac, item.data, item.len);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Retry also failed: %s", esp_err_to_name(ret));
                }
            }

            free(item.data);
        }
    }
}

esp_err_t espnow_core_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    memset(s_recv_cbs, 0, sizeof(s_recv_cbs));
    s_recv_cb_count = 0;
    memset(s_send_cbs, 0, sizeof(s_send_cbs));
    s_send_cb_count = 0;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi set mode STA failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_now_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ESP-NOW init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_now_register_recv_cb(unified_espnow_recv_cb);
    esp_now_register_send_cb(unified_espnow_send_cb);

    ret = esp_wifi_get_mac(WIFI_IF_STA, s_local_mac);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WiFi get MAC failed, trying efuse fallback");
        ret = esp_efuse_mac_get_default(s_local_mac);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Cannot get local MAC");
        }
    }
    ESP_LOGI(TAG, "Local MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             s_local_mac[0], s_local_mac[1], s_local_mac[2],
             s_local_mac[3], s_local_mac[4], s_local_mac[5]);

    s_peer_mutex = xSemaphoreCreateMutex();

    s_send_queue = xQueueCreate(SEND_QUEUE_LEN, sizeof(send_item_t));
    if (s_send_queue == NULL) {
        ESP_LOGE(TAG, "Send queue create failed");
        return ESP_ERR_NO_MEM;
    }
    xTaskCreate(send_task, "espnow_send", 3072, NULL, 3, NULL);

    s_initialized = true;
    ESP_LOGI(TAG, "ESP-NOW core initialized");
    return ESP_OK;
}

esp_err_t espnow_core_get_local_mac(uint8_t mac_out[6])
{
    if (mac_out == NULL) return ESP_ERR_INVALID_ARG;
    memcpy(mac_out, s_local_mac, 6);
    return ESP_OK;
}

void espnow_core_register_recv_cb(espnow_core_recv_cb_t cb)
{
    if (cb == NULL) return;
    if (s_recv_cb_count >= ESPNOW_MAX_CALLBACKS) {
        ESP_LOGE(TAG, "Recv callback list full (max %d)", ESPNOW_MAX_CALLBACKS);
        return;
    }
    for (int i = 0; i < s_recv_cb_count; i++) {
        if (s_recv_cbs[i] == cb) return;
    }
    s_recv_cbs[s_recv_cb_count++] = cb;
    ESP_LOGI(TAG, "Registered recv cb (%d/%d)", s_recv_cb_count, ESPNOW_MAX_CALLBACKS);
}

void espnow_core_register_send_cb(espnow_core_send_cb_t cb)
{
    if (cb == NULL) return;
    if (s_send_cb_count >= ESPNOW_MAX_CALLBACKS) {
        ESP_LOGE(TAG, "Send callback list full");
        return;
    }
    for (int i = 0; i < s_send_cb_count; i++) {
        if (s_send_cbs[i] == cb) return;
    }
    s_send_cbs[s_send_cb_count++] = cb;
}

bool espnow_core_add_peer(const uint8_t peer_mac[6], uint8_t channel)
{
    if (peer_mac == NULL) return false;
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized, cannot add peer");
        return false;
    }

    static const uint8_t bc[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    if (memcmp(peer_mac, bc, 6) == 0) {
        return true;  /* 广播地址无需注册 */
    }

    if (memcmp(peer_mac, s_local_mac, 6) == 0) {
        return true;
    }

    if (xSemaphoreTake(s_peer_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGE(TAG, "Peer mutex timeout");
        return false;
    }

    if (esp_now_is_peer_exist(peer_mac)) {
        esp_err_t ret = esp_now_del_peer(peer_mac);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Del existing peer failed: %s", esp_err_to_name(ret));
        }
    }

    esp_now_peer_info_t peer_info = {
        .channel = channel,
        .ifidx   = WIFI_IF_STA,
        .encrypt = false,
    };
    memcpy(peer_info.peer_addr, peer_mac, 6);

    esp_err_t ret = esp_now_add_peer(&peer_info);
    xSemaphoreGive(s_peer_mutex);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Peer added: %02X:%02X:%02X:%02X:%02X:%02X ch=%d",
                 peer_mac[0], peer_mac[1], peer_mac[2],
                 peer_mac[3], peer_mac[4], peer_mac[5], channel);
        return true;
    } else {
        ESP_LOGW(TAG, "Add peer failed: %s", esp_err_to_name(ret));
        return false;
    }
}

bool espnow_core_del_peer(const uint8_t peer_mac[6])
{
    if (peer_mac == NULL) return false;
    if (!s_initialized) return false;

    if (xSemaphoreTake(s_peer_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    esp_err_t ret = esp_now_del_peer(peer_mac);
    xSemaphoreGive(s_peer_mutex);

    return (ret == ESP_OK);
}

bool espnow_core_send_data(const uint8_t *dest_mac, const uint8_t *data, size_t len, uint8_t priority)
{
    if (dest_mac == NULL || data == NULL || len == 0) return false;
    if (!s_initialized) return false;
    if (len > ESPNOW_CORE_MAX_DATA_LEN) {
        ESP_LOGE(TAG, "Data too long: %d > %d", (int)len, ESPNOW_CORE_MAX_DATA_LEN);
        return false;
    }

    send_item_t item;
    memcpy(item.dest_mac, dest_mac, 6);
    item.data = (uint8_t *)malloc(len);
    if (item.data == NULL) {
        ESP_LOGE(TAG, "Malloc send buffer failed");
        return false;
    }
    memcpy(item.data, data, len);
    item.len      = len;
    item.priority = priority;

    if (xQueueSend(s_send_queue, &item, priority ? 0 : pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "Send queue full, dropping packet");
        free(item.data);
        return false;
    }
    return true;
}

bool espnow_core_send_broadcast(const uint8_t *data, size_t len)
{
    static const uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    return espnow_core_send_data(broadcast_mac, data, len, 1);
}
