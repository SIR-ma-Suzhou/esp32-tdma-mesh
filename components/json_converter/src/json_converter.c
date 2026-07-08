#include "json_converter.h"
#include <stdio.h>   // 提供 snprintf()

int sensor_data_to_json(const sensor_data_t *data, char *json_buf, size_t buf_size)
{
    // 参数检查：如果传入空指针，直接返回失败
    if (data == NULL || json_buf == NULL) {
        return -1;
    }

    // 参数检查：如果缓冲区大小为 0，也无法写入
    if (buf_size == 0) {
        return -1;
    }

    int len = snprintf(
        json_buf,
        buf_size,
        "{\"node_id\":\"%s\","
        "\"mac\":\"%s\","
        "\"temperature\":%.2f,"
        "\"humidity\":%.2f,"
        "\"motion\":%d,"
        "\"battery_mv\":%d,"
        "\"wake_reason\":%d,"
        "\"seq\":%u,"
        "\"hop\":%u,"
        "\"slot\":%u,"
        "\"offset_us\":%lld,"
        "\"rssi\":%d,"
        "\"timestamp\":%lu}",
        data->node_id,
        data->mac,
        data->temperature,
        data->humidity,
        data->motion,
        data->battery_mv,
        data->wake_reason,
        (unsigned int)data->seq,
        (unsigned int)data->hop,
        (unsigned int)data->slot,
        (long long)data->offset_us,
        (int)data->rssi,
        (unsigned long)data->timestamp
    );

    if (len < 0 || len >= (int)buf_size) {
        return -1;
    }

    return len;
}
