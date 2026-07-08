// components/mqtt_handler/mqtt_handler.c

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "mqtt_client.h"

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

#include "mqtt_handler.h"

static const char *TAG = "MQTT_TEST";
static EventGroupHandle_t s_wifi_event_group = NULL;
#define WIFI_CONNECTED_BIT BIT0
static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static bool s_mqtt_connected = false;

// Wi-Fi 事件处理函数
static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi started, connecting...");
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, reconnecting...");
        esp_wifi_connect();
        if (s_wifi_event_group != NULL) {
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
        s_mqtt_connected = false;
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        if (s_wifi_event_group != NULL) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
    }
}

// 【关键修复】初始化并连接 Wi-Fi
static esp_err_t wifi_init_sta(const char *ssid, const char *password)
{
    if (ssid == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create WiFi event group");
        return ESP_ERR_NO_MEM;
    }

    // 【修复点】不再调用 esp_netif_init, esp_event_loop_create_default 和 esp_netif_create_default_wifi_sta
    // 改为验证并复用 espnow_core 已经创建好的接口

    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif == NULL) {
        ESP_LOGE(TAG, "Default STA netif not found. Please ensure espnow_core is initialized first.");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "Reusing existing default WiFi STA interface from espnow_core");

    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(WIFI_EVENT,
                                            ESP_EVENT_ANY_ID,
                                            &wifi_event_handler,
                                            NULL,
                                            NULL)
    );
    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(IP_EVENT,
                                            IP_EVENT_STA_GOT_IP,
                                            &wifi_event_handler,
                                            NULL,
                                            NULL)
    );

    // 配置 Wi-Fi 名称和密码
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    // 写入 Wi-Fi 配置，不重复调用 esp_wifi_set_mode
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

	esp_wifi_connect();

    ESP_LOGI(TAG, "Waiting for WiFi connection...");
    xEventGroupWaitBits(s_wifi_event_group,
                        WIFI_CONNECTED_BIT,
                        pdFALSE,
                        pdTRUE,
                        portMAX_DELAY);
    ESP_LOGI(TAG, "WiFi connected successfully");
    return ESP_OK;
}

// MQTT 错误打印辅助函数
static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

// MQTT 事件处理函数
static void mqtt_event_handler(void *handler_args,
                               esp_event_base_t base,
                               int32_t event_id,
                               void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    switch ((esp_mqtt_event_id_t) event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        s_mqtt_connected = true;
        esp_mqtt_client_subscribe(client, MQTT_TEST_TOPIC_CMD, 0);
        esp_mqtt_client_publish(client,
                                MQTT_TEST_TOPIC_STATUS,
                                "{\"gateway_id\":\"gw_001\",\"status\":\"online\",\"mqtt\":\"connected\"}",
                                0, 0, 0);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        s_mqtt_connected = false;
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT subscribed, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT unsubscribed, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT published, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT data: topic=%.*s, payload=%.*s",
                 event->topic_len, event->topic,
                 event->data_len, event->data);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport socket errno", event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
        }
        break;
    default:
        ESP_LOGI(TAG, "Other MQTT event id:%d", event->event_id);
        break;
    }
}

static esp_err_t mqtt_client_start(const char *broker_uri,
                                   const char *mqtt_username,
                                   const char *mqtt_password)
{
    if (broker_uri == NULL) return ESP_ERR_INVALID_ARG;
    if (s_mqtt_client != NULL) {
        ESP_LOGW(TAG, "MQTT client already started");
        return ESP_OK;
    }
    esp_mqtt_client_config_t mqtt_cfg = { .broker.address.uri = broker_uri };
    if (mqtt_username != NULL && strlen(mqtt_username) > 0) {
        mqtt_cfg.credentials.username = mqtt_username;
    }
    if (mqtt_password != NULL && strlen(mqtt_password) > 0) {
        mqtt_cfg.credentials.authentication.password = mqtt_password;
    }
    if (strncmp(broker_uri, "mqtts://", 8) == 0) {
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
        ESP_LOGI(TAG, "TLS enabled, using ESP certificate bundle");
#else
        ESP_LOGW(TAG, "TLS broker detected, but certificate bundle is not enabled");
        ESP_LOGW(TAG, "Please enable CONFIG_MBEDTLS_CERTIFICATE_BUNDLE in menuconfig");
#endif
    }
    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_mqtt_client == NULL) {
        ESP_LOGE(TAG, "Failed to init MQTT client");
        return ESP_FAIL;
    }
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_mqtt_client));
    ESP_LOGI(TAG, "MQTT client start requested, broker=%s", broker_uri);
    return ESP_OK;
}

// 对外函数：一键启动 Wi-Fi + MQTT
esp_err_t mqtt_test_start(const char *wifi_ssid, const char *wifi_password,
                          const char *broker_uri, const char *mqtt_username, const char *mqtt_password)
{
    if (wifi_ssid == NULL || wifi_password == NULL || broker_uri == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(wifi_init_sta(wifi_ssid, wifi_password));
    ESP_ERROR_CHECK(mqtt_client_start(broker_uri, mqtt_username, mqtt_password));
    return ESP_OK;
}

// 对外函数：发送外部已经生成好的 JSON 字符串
esp_err_t mqtt_test_send_json(const char *json)
{
    if (json == NULL) return ESP_ERR_INVALID_ARG;
    if (s_mqtt_client == NULL || !s_mqtt_connected) {
        ESP_LOGW(TAG, "MQTT not connected, cannot send json");
        return ESP_ERR_INVALID_STATE;
    }
    int msg_id = esp_mqtt_client_publish(s_mqtt_client, MQTT_TEST_TOPIC_DATA, json, 0, 1, 0);
    ESP_LOGI(TAG, "Send JSON msg_id=%d, topic=%s, json=%s", msg_id, MQTT_TEST_TOPIC_DATA, json);
    return msg_id >= 0 ? ESP_OK : ESP_FAIL;
}

// 对外函数：发送状态 JSON
esp_err_t mqtt_test_publish_status(const char *json)
{
    if (json == NULL) return ESP_ERR_INVALID_ARG;
    if (s_mqtt_client == NULL || !s_mqtt_connected) {
        ESP_LOGW(TAG, "MQTT not connected, cannot publish status");
        return ESP_ERR_INVALID_STATE;
    }
    int msg_id = esp_mqtt_client_publish(s_mqtt_client, MQTT_TEST_TOPIC_STATUS, json, 0, 1, 0);
    ESP_LOGI(TAG, "Publish status msg_id=%d, topic=%s, json=%s", msg_id, MQTT_TEST_TOPIC_STATUS, json);
    return msg_id >= 0 ? ESP_OK : ESP_FAIL;
}

// 对外函数：判断 MQTT 是否连接成功
bool mqtt_test_is_connected(void) { return s_mqtt_connected; }

// 对外函数：发布 JSON 到任意 topic
esp_err_t mqtt_test_publish_to_topic(const char *topic, const char *json)
{
    if (topic == NULL || json == NULL) return ESP_ERR_INVALID_ARG;
    if (s_mqtt_client == NULL || !s_mqtt_connected) {
        ESP_LOGW(TAG, "MQTT not connected, cannot publish to %s", topic);
        return ESP_ERR_INVALID_STATE;
    }
    int msg_id = esp_mqtt_client_publish(s_mqtt_client, topic, json, 0, 1, 0);
    ESP_LOGI(TAG, "Publish to %s, msg_id=%d, len=%d", topic, msg_id, (int)strlen(json));
    return msg_id >= 0 ? ESP_OK : ESP_FAIL;
}
