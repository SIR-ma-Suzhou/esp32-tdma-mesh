#ifndef MQTT_TEST_H
#define MQTT_TEST_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include "esp_err.h"

#define MQTT_TEST_TOPIC_DATA    "esp32/data"

#define MQTT_TEST_TOPIC_STATUS  "esp32/status"

#define MQTT_TEST_TOPIC_CMD     "esp32/cmd"

#define MQTT_TEST_TOPIC_TOPOLOGY  "esp32/topology"

#define MQTT_TEST_TOPIC_TDMA      "esp32/tdma_status"

esp_err_t mqtt_test_start(const char *wifi_ssid,
                          const char *wifi_password,
                          const char *broker_uri,
                          const char *mqtt_username,
                          const char *mqtt_password);

esp_err_t mqtt_test_send_json(const char *json);

esp_err_t mqtt_test_publish_status(const char *json);

bool mqtt_test_is_connected(void);

esp_err_t mqtt_test_publish_to_topic(const char *topic, const char *json);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_TEST_H */
