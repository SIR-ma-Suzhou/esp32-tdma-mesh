#include "time_sync.h"
#include "espnow_core.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <inttypes.h>

static const char *TAG = "TIME_SYNC";

#define TIME_SYNC_BEACON_TYPE  0x10
#define MAX_SYNC_HOP            3     // 信标最大转发跳数

// 同步信标帧格式
typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t root_mac[6];
    uint8_t hop_count;
	uint8_t channel;			 // 发送时网关的Wi-Fi信道
    uint64_t timestamp_us;       // 发送时刻的网络时间（微秒，64 位）
} time_sync_beacon_t;

typedef struct {
    int64_t  offset_us;          // 时间偏移（网络时间 = 本地时间 + offset）
    double   drift_ppm;          // 时钟漂移率（ppm，微秒/微秒）
    uint64_t last_sync_local_us; // 上次同步时的本地时间（微秒）
    int      sync_count;         // 已完成的有效同步次数
} time_sync_state_t;

static int8_t g_last_beacon_rssi = 0;

static time_sync_state_t g_state = {
    .offset_us = 0,
    .drift_ppm = 0.0,
    .last_sync_local_us = 0,
    .sync_count = 0
};

static bool is_root = false;
static bool is_synchronized = false;
static TaskHandle_t beacon_task_handle = NULL;

#ifndef MACSTR
#define MACSTR "%02X:%02X:%02X:%02X:%02X:%02X"
#endif
#ifndef MAC2STR
#define MAC2STR(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]
#endif

static void time_sync_recv_cb(const espnow_core_packet_info_t *pkt);
static void beacon_task(void *arg);

void time_sync_init(void)
{
    espnow_core_register_recv_cb(time_sync_recv_cb);
    ESP_LOGI(TAG, "Time sync module initialized");
}

void time_sync_set_root(bool root)
{
    is_root = root;
    if (is_root) {
        is_synchronized = true;
        g_state.offset_us = 0;
        g_state.drift_ppm = 0.0;
        ESP_LOGI(TAG, "This node is ROOT");
    } else {
        ESP_LOGI(TAG, "This node is CHILD");
    }
}

void time_sync_start_beacon(uint32_t interval_ms)
{
    if (!is_root) {
        ESP_LOGW(TAG, "Only root node can start beacon");
        return;
    }
    if (beacon_task_handle != NULL) {
        ESP_LOGW(TAG, "Beacon task already running");
        return;
    }
    xTaskCreate(beacon_task, "time_sync_beacon", 4096,
                (void *)(uintptr_t)interval_ms, 5, &beacon_task_handle);
    ESP_LOGI(TAG, "Beacon task started, interval=%lu ms", interval_ms);
}

uint64_t time_sync_get_network_time_us(void)
{
    uint64_t local_now = esp_timer_get_time();
    double elapsed_sec = (double)(local_now - g_state.last_sync_local_us) / 1000000.0;

    // 补偿公式：校正量 = 偏移 + 漂移率 × 经过时间
    int64_t correction = g_state.offset_us +
                         (int64_t)(g_state.drift_ppm * elapsed_sec * 1000000.0);

    return (uint64_t)((int64_t)local_now + correction);
}

int64_t time_sync_get_offset_us(void)
{
    return g_state.offset_us;
}

double time_sync_get_drift_ppm(void)
{
    return g_state.drift_ppm;
}

bool time_sync_is_synchronized(void)
{
    return is_synchronized;
}

int8_t time_sync_get_last_beacon_rssi(void)
{
    return g_last_beacon_rssi;
}

static void time_sync_recv_cb(const espnow_core_packet_info_t *pkt)
{
    if (pkt->data_len < sizeof(time_sync_beacon_t)) return;

    const time_sync_beacon_t *beacon = (const time_sync_beacon_t *)pkt->data;
    if (beacon->type != TIME_SYNC_BEACON_TYPE) return;
    if (is_root) return;

    // 记录信标 RSSI
    g_last_beacon_rssi = pkt->rssi;

    // 原代码：int64_t new_offset = (int64_t)(network_time - local_rx_time);
    //   → network_time 和 local_rx_time 都是 uint64_t，相减后无法得到负值
    // 修复后：先各自转换为 int64_t，再相减
    int64_t network_time_s64 = (int64_t)beacon->timestamp_us;
    int64_t local_rx_time_s64 = (int64_t)pkt->timestamp_us;
    int64_t new_offset = network_time_s64 - local_rx_time_s64;

    if (!is_synchronized) {
        // 首次同步：直接取偏移
		esp_wifi_set_channel(beacon->channel, WIFI_SECOND_CHAN_NONE);
		ESP_LOGI(TAG, "Channel set to %d from beacon", beacon->channel);

        g_state.offset_us = new_offset;
        g_state.last_sync_local_us = pkt->timestamp_us;
        g_state.sync_count = 1;
        is_synchronized = true;
        ESP_LOGI(TAG, "✅ First sync! offset=%lld us", (long long)g_state.offset_us);
    } else {
        // 后续同步：更新漂移率
        int64_t dt_us = (int64_t)(pkt->timestamp_us - g_state.last_sync_local_us);
        if (dt_us > 80000) { // 至少间隔 80ms（信标200ms，允许抖动）
            // 原始漂移率（分数，非ppm；×1e6 = 真实ppm）
            double raw_drift = (double)(new_offset - g_state.offset_us) / (double)dt_us;

            // EMA: 70% 旧 + 30% 新（比原来90/10更快收敛）
            if (g_state.sync_count > 2) {
                g_state.drift_ppm = g_state.drift_ppm * 0.7 + raw_drift * 0.3;
            } else {
                g_state.drift_ppm = raw_drift;
            }

            // 偏移：50% 旧 + 50% 新（减少层层衰减）
            g_state.offset_us = (g_state.offset_us + new_offset) / 2;
            g_state.last_sync_local_us = pkt->timestamp_us;
            g_state.sync_count++;

            ESP_LOGI(TAG, "Sync update: offset=%lld us, drift_raw=%.1f ppm",
                     (long long)g_state.offset_us, raw_drift * 1e6);
        }
    }

    if (is_synchronized && beacon->hop_count < MAX_SYNC_HOP) {
        time_sync_beacon_t relay = *beacon;
        relay.hop_count = beacon->hop_count + 1;
        // 随机延迟避免多个节点同时转发产生碰撞
        vTaskDelay(pdMS_TO_TICKS(2 + (esp_random() % 15)));
        espnow_core_send_broadcast((uint8_t *)&relay, sizeof(relay));
    }
}

static void beacon_task(void *arg)
{
    uint32_t interval_ms = (uint32_t)(uintptr_t)arg;
    uint8_t root_mac[6];
    espnow_core_get_local_mac(root_mac);

    TickType_t last_wake = xTaskGetTickCount();

    while (1) {
        time_sync_beacon_t beacon = {
            .type = TIME_SYNC_BEACON_TYPE,
            .hop_count = 0,
            .timestamp_us = (uint64_t)esp_timer_get_time()
        };
        memcpy(beacon.root_mac, root_mac, 6);

		uint8_t ch;
		wifi_second_chan_t sc;
		esp_wifi_get_channel(&ch, &sc);
		beacon.channel = ch;

        // 修复架构破坏：不再绕过 espnow_core 封装层
        bool sent = espnow_core_send_broadcast((uint8_t *)&beacon, sizeof(beacon));

        if (!sent) {
            vTaskDelay(pdMS_TO_TICKS(10));
            sent = espnow_core_send_broadcast((uint8_t *)&beacon, sizeof(beacon));
            if (!sent) {
                ESP_LOGE(TAG, "Beacon send failed after retry");
            }
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(interval_ms));
    }
}
