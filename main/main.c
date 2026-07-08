#include "espnow_core.h"
#include "time_sync.h"
#include "tdma_scheduler.h"
#include "simple_route.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_sleep.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_wifi.h"
#include "sensor_dht.h"
#include "sensor_pir.h"
#include "sensor_batt.h"
#include "hw_crypto.h"
#include "json_converter.h"
#include "mqtt_handler.h"
#include "topology_aggregation.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "cJSON.h"

static const char *TAG = "APP_MAIN";

#ifndef MACSTR
#define MACSTR "%02X:%02X:%02X:%02X:%02X:%02X"
#endif
#ifndef MAC2STR
#define MAC2STR(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]
#endif

#define NODE_IS_ROOT  1          // 1=根节点(网关), 0=子节点(终端)
#define ENABLE_WIFI   1          // 1=启用WiFi(根节点), 0=禁用(子节点)
#define ROOT_NUM      6          // 根节点默认ESP-NOW信道

#define BLACK_BOARD_MAC  {0xE8, 0xF6, 0x0A, 0xA6, 0xF8, 0x8C}  // 网关
#define WHITE_BOARD_MAC  {0xE8, 0xF6, 0x0A, 0xA6, 0xF1, 0xD0}  // 终端1
#define BROADCAST_MAC    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}  // 广播地址

#define TERMINAL_01_MAC  {0xE8, 0xF6, 0x0A, 0xA6, 0xF1, 0xD0}  // 终端1
#define TERMINAL_02_MAC  {0xE8, 0xF6, 0x0A, 0xA6, 0xF8, 0xA0}  // 终端2
#define TERMINAL_03_MAC  {0xE8, 0xF6, 0x0A, 0xA7, 0x29, 0x8C}  // 终端3
#define TERMINAL_MAC_LIST { TERMINAL_01_MAC, TERMINAL_02_MAC, TERMINAL_03_MAC }

#define TEST_MODE          0
#define TEST_PACKET_COUNT  1000
#define TEST_CSMA_MODE     0

#if TEST_MODE == 1
static uint32_t g_send_count = 0;
static uint32_t g_recv_count = 0;
static uint32_t g_test_start_time = 0;
static bool g_test_running = false;
#endif

#define RTC_MAGIC_VALID  0xA5A5A5A5
typedef struct {
    uint32_t magic;           // 有效性校验
    uint8_t  slot_id;         // 已分配的 TDMA 时隙
    uint8_t  gateway_mac[6];  // 网关 MAC
    bool     is_joined;       // 是否已入网
} rtc_preserved_t;

static RTC_NOINIT_ATTR rtc_preserved_t g_rtc_state;

#define DHT_GPIO        GPIO_NUM_4
#define PIR_GPIO        GPIO_NUM_5
#define BATT_GPIO       GPIO_NUM_3   // ADC1_CH2, 电池分压后接入
#define BATT_SCALE      2.0f         // 分压系数 (100k+100k → 2.0, 直连 → 1.0)
#define NODE_ID         0x01
#define WAKEUP_GPIO     GPIO_NUM_5

// 并通过安全渠道分发到各节点，或通过 ECDH 等协议动态协商。
static const uint8_t AES_KEY[16] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
};

#define WIFI_SSID       "iPhone"
#define WIFI_PASSWORD   "3315181687"
#define MQTT_BROKER_URI "mqtts://d4c11111.ala.cn-hangzhou.emqxsl.cn:8883"

// 目标 MAC（用于单播测试）
#define TEST_PEER_MAC  WHITE_BOARD_MAC

static void ble_broadcast(uint8_t wake_reason)
{
    esp_ble_adv_data_t adv_data = {
        .set_scan_rsp = false,
        .include_name = false,
        .include_txpower = false,
        .min_interval = 0x100,
        .max_interval = 0x160,
        .appearance = 0x00,
        .manufacturer_len = 3,
        .p_manufacturer_data = (uint8_t[]){NODE_ID, wake_reason, 0x00},
        .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
    };
    esp_ble_gap_config_adv_data(&adv_data);
    esp_ble_adv_params_t params = {
        .adv_int_min = 0x100, .adv_int_max = 0x160, .adv_type = ADV_TYPE_NONCONN_IND,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC, .channel_map = ADV_CHNL_ALL,
        .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
    };
    esp_ble_gap_start_advertising(&params);
    ESP_LOGI(TAG, "BLE broadcasting: Node=%d Wake=%d", NODE_ID, wake_reason);
}

static void ble_scan_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        esp_ble_gap_cb_param_t *scan = (esp_ble_gap_cb_param_t *)param;
        switch (scan->scan_rst.search_evt) {
        case ESP_GAP_SEARCH_INQ_RES_EVT: {
            uint8_t *adv_data = scan->scan_rst.ble_adv;
            uint8_t adv_len = scan->scan_rst.adv_data_len;
            // 解析厂商自定义数据（ad_type=0xFF，即 Manufacturer Specific Data）
            // BLE adv 格式: [len][type][data...]
            for (int i = 0; i < adv_len; ) {
                uint8_t field_len = adv_data[i];
                if (field_len == 0 || i + field_len >= adv_len) break;
                uint8_t ad_type = adv_data[i + 1];
                if (ad_type == 0xFF && field_len >= 5) {
                    uint8_t node_id = adv_data[i + 2];
                    uint8_t wake_reason = adv_data[i + 3];
                    // 只上报本网络节点的广播（NODE_ID=0x01），屏蔽环境BLE噪声
                    if (node_id == NODE_ID) {
                        ESP_LOGI(TAG, "BLE scan: Node=%d Wake=%d RSSI=%d",
                                 node_id, wake_reason, scan->scan_rst.rssi);
                    }
                }
                i += (field_len + 1);
            }
            break;
        }
        default: break;
        }
        break;
    }
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        ESP_LOGI(TAG, "BLE scan started");
        break;
    default: break;
    }
}

// 网关启动 BLE 扫描
static void gateway_ble_scan_start(void)
{
    esp_ble_scan_params_t scan_params = {
        .scan_type = BLE_SCAN_TYPE_PASSIVE,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval = 0x50,
        .scan_window = 0x30,
        .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE
    };
    esp_ble_gap_set_scan_params(&scan_params);
    esp_ble_gap_start_scanning(0);  // 0 = 持续扫描
    ESP_LOGI(TAG, "Gateway BLE scan started");
}

// 中继节点(有子节点依赖我)用 light sleep 保持 WiFi 在线
// 叶子节点(无子节点依赖我)用 deep sleep 极致省电
static void terminal_enter_sleep(void)
{
    #define WAKE_BEFORE_LIGHT_US   2000UL   // light sleep唤醒只需1ms，提前2ms足够
    #define WAKE_BEFORE_DEEP_US  500000UL   // deep sleep需要ROM boot+WiFi重建，提前500ms
    uint64_t net_time = time_sync_get_network_time_us();
    uint32_t t_in_frame = (uint32_t)(net_time % TDMA_SUPERFRAME_PERIOD_US);
    uint32_t sleep_us;
    bool is_relay = route_has_children();
    uint32_t wake_before = is_relay ? WAKE_BEFORE_LIGHT_US : WAKE_BEFORE_DEEP_US;

    if (t_in_frame < TDMA_BEACON_SLOT_US) {
        uint32_t time_to_slot = tdma_scheduler_get_time_to_next_slot_us();
        if (time_to_slot <= wake_before) return;
        sleep_us = time_to_slot - wake_before;
    } else {
        sleep_us = TDMA_SUPERFRAME_PERIOD_US - t_in_frame;
        if (sleep_us <= wake_before) return;
        sleep_us -= wake_before;
    }

    if (sleep_us < 10000UL || sleep_us > TDMA_SUPERFRAME_PERIOD_US) return;

    esp_sleep_enable_timer_wakeup(sleep_us);

    if (is_relay) {
        // PIR：light sleep 也支持 EXT0
        if (gpio_get_level(PIR_GPIO) == 0) {
            esp_sleep_enable_ext0_wakeup(PIR_GPIO, 1);
        }
        ESP_LOGI(TAG, "Relay: light sleep %lu us", (unsigned long)sleep_us);
        esp_light_sleep_start();
        // 醒来后所有状态保持，直接继续执行
        return;
    }

    g_rtc_state.magic = RTC_MAGIC_VALID;
    g_rtc_state.slot_id = tdma_scheduler_get_my_slot_id();
    g_rtc_state.is_joined = tdma_scheduler_is_joined();
    const uint8_t black_mac[6] = BLACK_BOARD_MAC;
    memcpy(g_rtc_state.gateway_mac, black_mac, 6);

    if (gpio_get_level(PIR_GPIO) == 0) {
        esp_sleep_enable_ext0_wakeup(PIR_GPIO, 1);
    }

    ESP_LOGI(TAG, "Leaf: deep sleep %lu us", (unsigned long)sleep_us);
    esp_deep_sleep_start();
}

static void my_recv_cb(const espnow_core_packet_info_t *pkt)
{
    if (pkt->data_len > 0 && pkt->data[0] == 0x10) return;
    ESP_LOGI(TAG, "Data from " MACSTR ", len=%d", MAC2STR(pkt->src_mac), pkt->data_len);
}

// 网关数据回调：收到路由解封装后的应用数据
static void gateway_data_cb(const uint8_t *src_mac, const uint8_t *data, size_t len)
{
    if (len > 1 && data[0] == 0x7E) {
        size_t json_len = len - 1;
        char *topo_json = (char *)malloc(json_len + 1);
        if (!topo_json) {
            ESP_LOGE(TAG, "Failed to malloc topo_json");
            return;
        }

        memcpy(topo_json, data + 1, json_len);
        topo_json[json_len] = '\0';

        ESP_LOGI(TAG, "Topology report from " MACSTR ": %s", MAC2STR(src_mac), topo_json);
        topo_table_upsert(topo_json);

        free(topo_json);
        return;
    }

#if TEST_MODE == 1
    // 测试包（TEST:前缀）: 计数 + 跳过解密
    if (len >= 5 && memcmp(data, "TEST:", 5) == 0) {
        g_recv_count++;
        if (g_recv_count >= TEST_PACKET_COUNT) {
            uint32_t recv = g_recv_count;
            int32_t lost = (int32_t)TEST_PACKET_COUNT - (int32_t)recv;
            float loss_rate = (lost > 0) ? (float)lost / TEST_PACKET_COUNT * 100 : 0.0f;
            ESP_LOGI(TAG, "========== Gateway Test Result ==========");
            ESP_LOGI(TAG, "Expected: %d, Received: %lu, Loss: %ld, Loss Rate: %.2f%%",
                     TEST_PACKET_COUNT, recv, (long)lost, loss_rate);
            g_recv_count = 0;
        } else if (g_recv_count % 100 == 0) {
            ESP_LOGI(TAG, "GW received: %lu / %d", g_recv_count, TEST_PACKET_COUNT);
        }
        ESP_LOGI(TAG, "GW received from " MACSTR ": %.*s", MAC2STR(src_mac), (int)len, data);
        return;
    }
#endif
    ESP_LOGI(TAG, "GW received from " MACSTR ": %.*s", MAC2STR(src_mac), (int)len, data);

    uint8_t *plaintext = NULL;
    size_t plaintext_len = 0;
    uint64_t now_us = time_sync_get_network_time_us();

    esp_err_t ret = hw_crypto_decrypt_inplace_with_node_mac(
        (uint8_t *)data, len, src_mac, now_us,
        HW_CRYPTO_DEFAULT_TIME_WINDOW_US,
        &plaintext, &plaintext_len);

    if (ret == ESP_OK && plaintext != NULL && plaintext_len > 0) {
        ESP_LOGI(TAG, "Decrypted: %.*s", (int)plaintext_len, (char *)plaintext);

        sensor_data_t sensor_data = {0};

        // 从 MAC 地址自动生成 node_id："node_XX"（XX=MAC末字节）
        snprintf(sensor_data.node_id, sizeof(sensor_data.node_id),
                 "node_%02X", src_mac[5]);
        snprintf(sensor_data.mac, sizeof(sensor_data.mac),
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 src_mac[0], src_mac[1], src_mac[2],
                 src_mac[3], src_mac[4], src_mac[5]);

        float t_val = 0, h_val = 0;
        int w_val = 0, p_val = 0, b_val = 0;
        unsigned int seq_val = 0, hop_val = 0, slot_val = 0;
        long long offset_val = 0;
        int rssi_val = 0;
        int parsed = sscanf((char *)plaintext,
            "temp:%f,humid:%f,wake:%d,pir:%d,batt:%d,seq:%u,hop:%u,slot:%u,offset:%lld,rssi:%d",
            &t_val, &h_val, &w_val, &p_val, &b_val,
            &seq_val, &hop_val, &slot_val, &offset_val, &rssi_val);
        if (parsed >= 5) {
            sensor_data.temperature = t_val;
            sensor_data.humidity    = h_val;
            sensor_data.wake_reason = w_val;
            sensor_data.motion      = p_val;
            sensor_data.battery_mv  = b_val;
            sensor_data.seq         = (parsed >= 6)  ? (uint16_t)seq_val : 0;
            sensor_data.hop         = (parsed >= 7)  ? (uint8_t)hop_val  : 0;
            sensor_data.slot        = (parsed >= 8)  ? (uint8_t)slot_val : 0;
            sensor_data.offset_us   = (parsed >= 9)  ? offset_val : 0;
            sensor_data.rssi        = (parsed >= 10) ? (int8_t)rssi_val : 0;
        }
        sensor_data.timestamp = (uint32_t)(now_us / 1000000);

        char json_buf[JSON_CONVERTER_MAX_LEN];
        int json_len = sensor_data_to_json(&sensor_data, json_buf, sizeof(json_buf));
        if (json_len > 0) {
            mqtt_test_send_json(json_buf);
        }
    } else {
        ESP_LOGW(TAG, "Decrypt failed or not encrypted: %s", esp_err_to_name(ret));
    }
}

static void terminal_encrypt_and_send(float temp, float humi, uint8_t wake_reason,
                                       bool pir_triggered, uint32_t batt_mv,
                                       const uint8_t *gateway_mac)
{
    static uint16_t g_seq = 0;
    uint16_t seq = g_seq++;

    uint8_t hop = route_get_hop_count();
    uint8_t slot = tdma_scheduler_get_my_slot_id();
    int64_t offset = time_sync_get_offset_us();
    int8_t rssi = time_sync_get_last_beacon_rssi();

    char plaintext[128];
    int plain_len = snprintf(plaintext, sizeof(plaintext),
        "temp:%.1f,humid:%.1f,wake:%d,pir:%d,batt:%d,seq:%u,hop:%u,slot:%u,offset:%lld,rssi:%d",
        temp, humi, wake_reason,
        pir_triggered ? 1 : 0, (int)batt_mv,
        (unsigned int)seq, (unsigned int)hop, (unsigned int)slot,
        (long long)offset, (int)rssi);

    uint8_t tx_buf[250] = {0};
    uint8_t *plain_area = tx_buf + sizeof(hw_crypto_packet_header_t);
    memcpy(plain_area, plaintext, plain_len);

    uint64_t now_us = time_sync_get_network_time_us();
    size_t packet_len = 0;

    esp_err_t ret = hw_crypto_encrypt_inplace(tx_buf, sizeof(tx_buf), plain_len,
                                               0x01, now_us, &packet_len);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Encrypted: plain=%d bytes -> packet=%d bytes", plain_len, (int)packet_len);
        if (route_send_to_gateway(tx_buf, packet_len, gateway_mac)) {
            ESP_LOGI(TAG, "Encrypted data sent to gateway");
        }
    } else {
        ESP_LOGE(TAG, "Encrypt failed: %s", esp_err_to_name(ret));
    }
}

static uint8_t get_current_channel(void)
{
    uint8_t ch;
    wifi_second_chan_t sc;
    esp_wifi_get_channel(&ch, &sc);
    return ch;
}

void app_main(void)
{
	float temp = 0, humi = 0;
	uint8_t wake_reason = 0;
    const uint8_t gateway_mac[6] = BLACK_BOARD_MAC;

	esp_log_level_set("TIME_SYNC", ESP_LOG_DEBUG);
    esp_log_level_set("TDMA_SCHED", ESP_LOG_DEBUG);
    esp_log_level_set("ROUTE", ESP_LOG_DEBUG);

    hw_crypto_init(AES_KEY);
    ESP_ERROR_CHECK(espnow_core_init());

    uint8_t local_mac[6];
    espnow_core_get_local_mac(local_mac);
    ESP_LOGI(TAG, "Local MAC: " MACSTR, MAC2STR(local_mac));

    espnow_core_register_recv_cb(my_recv_cb);
    time_sync_init();
    tdma_scheduler_init();
    route_init();
    topology_aggregation_init();

    if (NODE_IS_ROOT) {
        time_sync_set_root(true);
        tdma_scheduler_set_role(true);
        route_set_gateway(true);
        route_register_data_callback(gateway_data_cb);

	#if ENABLE_WIFI
    	mqtt_test_start(WIFI_SSID, WIFI_PASSWORD, MQTT_BROKER_URI, "admin", "abc123456789");

    	uint8_t current_channel = get_current_channel();
    	ESP_LOGI(TAG, "Current Wi-Fi channel: %d", current_channel);

    	// 1. 添加广播 peer
    	const uint8_t broadcast_mac[6] = BROADCAST_MAC;
    	espnow_core_add_peer(broadcast_mac, current_channel);

    	// 2. 批量添加所有终端节点
    	const uint8_t terminal_list[][6] = TERMINAL_MAC_LIST;
    	int terminal_count = sizeof(terminal_list) / sizeof(terminal_list[0]);
  	    for (int i = 0; i < terminal_count; i++) {
        	espnow_core_add_peer(terminal_list[i], current_channel);
        	ESP_LOGI(TAG, "Terminal peer added: " MACSTR, MAC2STR(terminal_list[i]));
    	}
    	ESP_LOGI(TAG, ">>> ROOT MODE: %d terminal peers configured <<<", terminal_count);
	#else
        // 无 WiFi：使用默认信道
        uint8_t current_channel = ROOT_NUM;
        ESP_LOGW(TAG, "WiFi disabled, using channel %d", current_channel);
	#endif

        time_sync_start_beacon(200);

        esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        esp_bt_controller_init(&bt_cfg);
        esp_bt_controller_enable(ESP_BT_MODE_BLE);
        esp_bluedroid_init();
        esp_bluedroid_enable();
        esp_ble_gap_register_callback(ble_scan_gap_cb);
        gateway_ble_scan_start();

        ESP_LOGI(TAG, ">>> ROOT MODE: Beacon + TDMA Gateway + Routing Root + MQTT + BLE Scan <<<");
    } else {
        time_sync_set_root(false);
        tdma_scheduler_set_role(false);
        route_set_gateway(false);

        pir_sensor_init(PIR_GPIO, false);

        batt_sensor_init(BATT_GPIO, BATT_SCALE);

        esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        esp_bt_controller_init(&bt_cfg);
        esp_bt_controller_enable(ESP_BT_MODE_BLE);
        esp_bluedroid_init();
        esp_bluedroid_enable();

        esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
        if (cause == ESP_SLEEP_WAKEUP_EXT0) {
            wake_reason = 0x02;  // GPIO (PIR) 唤醒
        } else if (cause == ESP_SLEEP_WAKEUP_TIMER) {
            wake_reason = 0x01;  // 定时器唤醒
        } else {
            wake_reason = 0x00;  // 冷启动
        }
        ESP_LOGI(TAG, "%s wakeup",
                 wake_reason == 0x02 ? "GPIO(PIR)" :
                 wake_reason == 0x01 ? "Timer" : "Cold boot");

        static uint32_t last_dht_read_ms = 0;
        uint32_t now_sensor_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (last_dht_read_ms == 0 || now_sensor_ms - last_dht_read_ms >= 2000) {
            esp_err_t dht_ret = dht_read_float_data(DHT_TYPE_AM2301, DHT_GPIO, &humi, &temp);
            if (dht_ret == ESP_OK) {
                ESP_LOGI(TAG, "Temp: %.1fC  Humi: %.1f%%", temp, humi);
                last_dht_read_ms = now_sensor_ms;
            } else {
                ESP_LOGE(TAG, "DHT22 fail: %s", esp_err_to_name(dht_ret));
            }
        } else {
            ESP_LOGI(TAG, "DHT22 skip (last read %lu ms ago)",
                     now_sensor_ms - last_dht_read_ms);
        }

        bool pir_triggered = pir_sensor_is_triggered();
        if (pir_triggered) {
            ESP_LOGI(TAG, "PIR motion detected!");
            wake_reason = 0x02;
        }

        ble_broadcast(wake_reason);

        bool restored_from_deep = (g_rtc_state.magic == RTC_MAGIC_VALID && g_rtc_state.is_joined);
        if (restored_from_deep) {
            ESP_LOGI(TAG, "Restored from deep sleep: slot=%d", g_rtc_state.slot_id);
            g_rtc_state.magic = 0;  // 清除magic，防止异常复位时误判为深睡恢复
        }

        ESP_LOGI(TAG, ">>> CHILD MODE: Waiting for sync & joining network <<<");

        // 等待时间同步 — 跳频扫描找到网关信标
        uint8_t scan_ch = 1;
        while (!time_sync_is_synchronized()) {
            esp_wifi_set_channel(scan_ch, WIFI_SECOND_CHAN_NONE);
            vTaskDelay(pdMS_TO_TICKS(100));
            scan_ch = (scan_ch % 13) + 1;  // 循环 1-13 信道
        }
        ESP_LOGI(TAG, "Time synced! Requesting join...");

        uint8_t current_channel = get_current_channel();
        ESP_LOGI(TAG, "Current ESP-NOW channel: %d", current_channel);

        // 添加 peer（使用当前信道）
        const uint8_t broadcast_mac[6] = BROADCAST_MAC;
        espnow_core_add_peer(gateway_mac, current_channel);
        espnow_core_add_peer(broadcast_mac, current_channel);

        // 从RTC恢复旧时隙（本地立即生效，后续发入网请求更新网关侧时隙表）
        if (restored_from_deep) {
            tdma_scheduler_restore_slot(g_rtc_state.slot_id);
        }

        for (int retry = 0; retry < 3; retry++) {
            tdma_scheduler_join_request(local_mac, gateway_mac);
            ESP_LOGI(TAG, "Join request sent%s (attempt %d)",
                     restored_from_deep ? " (RTC restored)" : "", retry + 1);
            // 等应答：每30ms检查一次，最多等300ms
            for (int w = 0; w < 10; w++) {
                if (tdma_scheduler_is_joined()) break;
                vTaskDelay(pdMS_TO_TICKS(30));
            }
            if (tdma_scheduler_is_joined()) break;
        }
        if (tdma_scheduler_is_joined()) {
            ESP_LOGI(TAG, "Join confirmed, slot=%u", tdma_scheduler_get_my_slot_id());
        } else {
            ESP_LOGW(TAG, "Join not confirmed, will retry next cycle");
        }
    }

    ESP_LOGI(TAG, "System ready.");

    while (1) {
        route_process();
        if (!NODE_IS_ROOT) {
            route_send_heartbeat();
        }

        if (NODE_IS_ROOT) {
            route_send_hello_if_contention();   // 不限于竞争时隙，每2秒发一次Hello

            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

            static uint32_t last_topo_ms = 0;
            if (now_ms - last_topo_ms >= 5000 && mqtt_test_is_connected()) {
                last_topo_ms = now_ms;
                // 使用全网聚合拓扑（终端上报的持久数据），而非本地路由快照
                cJSON *full_topo = topo_table_build_full_json();
                if (full_topo) {
                    char *topo = cJSON_PrintUnformatted(full_topo);
                    if (topo) {
                        mqtt_test_publish_to_topic("esp32/topology", topo);
                        free(topo);
                    }
                    cJSON_Delete(full_topo);
                }
            }

            static uint32_t last_tdma_ms = 0;
            if (now_ms - last_tdma_ms >= 3000 && mqtt_test_is_connected()) {
                last_tdma_ms = now_ms;
                char *slots = tdma_scheduler_get_slot_string();
                if (slots) {
                    mqtt_test_publish_to_topic("esp32/tdma_status", slots);
                    free(slots);
                }
            }

            static uint32_t last_status_ms = 0;
            if (now_ms - last_status_ms >= 5000 && mqtt_test_is_connected()) {
                last_status_ms = now_ms;

                // 从 TDMA 时隙表获取真实的已入网终端数
                int terminal_count = 0;
                int tdma_used = 0;
                char *tdma_str = tdma_scheduler_get_slot_string();
                if (tdma_str) {
                    cJSON *tdma_json = cJSON_Parse(tdma_str);
                    if (tdma_json) {
                        cJSON *used = cJSON_GetObjectItem(tdma_json, "used_slots");
                        if (used) tdma_used = used->valueint;
                        cJSON_Delete(tdma_json);
                    }
                    free(tdma_str);
                }
                terminal_count = tdma_used;  // TDMA used_slots = 真实在线终端数

                // 这里仅根据当前已知的拓扑数据构建节点列表
                cJSON *status = cJSON_CreateObject();
                cJSON_AddStringToObject(status, "gateway_id", "gw_001");
                cJSON_AddNumberToObject(status, "terminal_count", terminal_count);
                cJSON_AddNumberToObject(status, "tdma_total_slots", TDMA_MAX_DATA_SLOTS);
                cJSON_AddNumberToObject(status, "tdma_used_slots", tdma_used);
                cJSON_AddNumberToObject(status, "sync_accuracy_ms", 0);  // 网关自身精度为0

                // 构建节点列表
                cJSON *nodes_arr = cJSON_AddArrayToObject(status, "nodes");
                if (nodes_arr) {
                    for (int ni = 0; ni < terminal_count; ni++) {
                        char *node_json = topo_table_get_json(ni);
                        if (node_json) {
                            cJSON *node_obj = cJSON_Parse(node_json);
                            if (node_obj) {
                                cJSON_AddItemToArray(nodes_arr, node_obj);
                            }
                            free(node_json);
                        }
                    }
                }

                char *status_str = cJSON_PrintUnformatted(status);
                if (status_str) {
                    mqtt_test_publish_to_topic("esp32/status", status_str);
                    free(status_str);
                }
                cJSON_Delete(status);
            }

            static uint32_t last_test_msg = 0;
            if (now_ms - last_test_msg >= 3000) {
                const uint8_t test_peer[6] = TEST_PEER_MAC;
                const char *msg = "Hello from ROOT!";
                espnow_core_send_data(test_peer, (uint8_t *)msg, strlen(msg), 0);
                ESP_LOGI(TAG, "[ROOT] Gateway running, test message sent");
                last_test_msg = now_ms;
            }

        } else {
		#if TEST_MODE == 1
            if (!g_test_running) {
                ESP_LOGI(TAG, "========== %s Drop Rate Test Started ==========",
                         TEST_CSMA_MODE ? "CSMA" : "TDMA");
                ESP_LOGI(TAG, "Target: %d packets", TEST_PACKET_COUNT);
                g_test_running = true;
                g_send_count = 0;
                g_test_start_time = esp_timer_get_time() / 1000;
            }

            if (g_test_running && g_send_count < TEST_PACKET_COUNT) {
			#if TEST_CSMA_MODE == 1
                char msg_buf[32];
                snprintf(msg_buf, sizeof(msg_buf), "TEST:%lu", g_send_count);
                if (route_send_to_gateway((uint8_t *)msg_buf, strlen(msg_buf), gateway_mac)) {
                    g_send_count++;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
			#else
                if (tdma_scheduler_is_my_slot()) {
                    char msg_buf[32];
                    snprintf(msg_buf, sizeof(msg_buf), "TEST:%lu", g_send_count);
                    if (route_send_to_gateway((uint8_t *)msg_buf, strlen(msg_buf), gateway_mac)) {
                        g_send_count++;
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(5));
			#endif
                continue;
            }

            if (g_send_count >= TEST_PACKET_COUNT && g_test_running) {
                uint32_t elapsed = (esp_timer_get_time() / 1000) - g_test_start_time;
                ESP_LOGI(TAG, "========== %s Test Completed ==========",
                         TEST_CSMA_MODE ? "CSMA" : "TDMA");
                ESP_LOGI(TAG, "Sent: %lu packets in %lu ms", g_send_count, elapsed);
                g_test_running = false;
            }
		#else
            // Hello 不再限制在竞争时隙，确保终端任何时间醒来都能发现路由
            route_send_hello_if_contention();

            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

            static uint32_t last_emergency_send = 0;
            if (pir_sensor_is_triggered() && tdma_scheduler_is_emergency_slot()
                && route_get_parent_mac() != NULL
                && (now_ms - last_emergency_send > 3000)) {
                uint32_t batt_em_raw = batt_sensor_read_mv();
                uint32_t batt_em = (batt_em_raw > 500 && batt_em_raw < 4500) ? batt_em_raw : 3700;
                terminal_encrypt_and_send(temp, humi, 0x02,
                                          true, batt_em, gateway_mac);
                last_emergency_send = now_ms;
                ESP_LOGI(TAG, "Emergency slot: PIR data sent");
            }

            static uint32_t last_send_time = 0;
            // 深睡唤醒后 last_send_time=0，首包立即发送
            // 路由未就绪时不能发送 — RTC恢复时隙后路由需时间重建
            if (tdma_scheduler_is_my_slot()
                && route_get_parent_mac() != NULL
                && (last_send_time == 0 || now_ms - last_send_time > 5000)) {
                bool pir_now = pir_sensor_is_triggered();
                uint32_t batt_raw = batt_sensor_read_mv();
                // GPIO3 浮空时 ADC 噪声可 >5V，钳位到合理范围用于演示
                uint32_t batt_now = (batt_raw > 500 && batt_raw < 4500) ? batt_raw : 3700;
                if (batt_now != batt_raw) {
                    ESP_LOGW(TAG, "Battery reading %lumV out of range, using %lumV", batt_raw, batt_now);
                }

                terminal_encrypt_and_send(temp, humi, wake_reason,
                                          pir_now, batt_now, gateway_mac);
                last_send_time = now_ms;

                // 再次读取传感器（为下次发送准备，含 2 秒间隔保护）
                {
                    static uint32_t last_dht_slot_ms = 0;
                    if (now_ms - last_dht_slot_ms >= 2000) {
                        if (dht_read_float_data(DHT_TYPE_AM2301, DHT_GPIO, &humi, &temp) == ESP_OK) {
                            last_dht_slot_ms = now_ms;
                        }
                    }
                }

                static uint32_t last_topo_report_ms = 0;
                if (last_topo_report_ms == 0 || now_ms - last_topo_report_ms > 10000) {
                    last_topo_report_ms = now_ms;
                    char *topo_str = simple_route_get_topology_string();
                    if (topo_str) {
                        size_t topo_len = strlen(topo_str);
                        uint8_t *buf = malloc(1 + topo_len + 1);
                        if (buf) {
                            buf[0] = 0x7E;  // 拓扑报告类型标记

                            memcpy(buf + 1, topo_str, topo_len + 1);

                            size_t send_len = 1 + topo_len + 1;
                            if (route_send_to_gateway(buf, send_len, gateway_mac)) {
                                ESP_LOGI(TAG, "Topology report sent, len=%d, json=%s",
                                         (int)send_len, topo_str);
                            } else {
                                ESP_LOGW(TAG, "Topology report send failed, len=%d", (int)send_len);
                            }

                            free(buf);
                        }
                        free(topo_str);
                    }
                }
            }

            static uint32_t last_print = 0;
            if (now_ms - last_print > 1000) {
                last_print = now_ms;
                const uint8_t *parent = route_get_parent_mac();
                if (parent) {
                    ESP_LOGI(TAG, "[CHILD] Parent: " MACSTR ", hop=%d, slot=%u, offset=%lld us, drift=%.3f ppm",
                             MAC2STR(parent), route_get_hop_count(),
                             tdma_scheduler_get_my_slot_id(),
                             (long long)time_sync_get_offset_us(),
                             time_sync_get_drift_ppm());
                } else {
                    ESP_LOGI(TAG, "[CHILD] No parent yet, waiting...");
                }
            }

            if (tdma_scheduler_get_phase() == TDMA_PHASE_IDLE && tdma_scheduler_is_joined()) {
                terminal_enter_sleep();
            }

		#endif
        }

        vTaskDelay(pdMS_TO_TICKS(10));   // 统一高频循环
    }
}
