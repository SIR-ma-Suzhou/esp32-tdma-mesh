#ifndef JSON_CONVERTER_H
#define JSON_CONVERTER_H

#include <stdint.h>   // 提供 uint32_t 等固定宽度整数类型
#include <stddef.h>   // 提供 size_t 类型，用于表示缓冲区大小

// JSON 字符串最大长度
// 这里设置为 256 字节，足够存放一条简单传感器数据
// 后续如果字段增加，可以改大，例如 384 或 512
#define JSON_CONVERTER_MAX_LEN 384

typedef struct {
    char node_id[16];        // 节点编号，从 MAC 推导
    char mac[18];            // 完整 MAC 地址 "XX:XX:XX:XX:XX:XX"
    float temperature;       // 温度数据
    float humidity;          // 湿度数据
    int motion;              // 人体红外检测结果：0=无人, 1=有人
    int battery_mv;          // 电池电压 (mV)
    int wake_reason;         // 唤醒原因：0=冷启, 1=定时器, 2=GPIO/PIR
    uint16_t seq;            // 数据包序列号 (0-65535 回绕)
    uint8_t hop;             // 到网关跳数
    uint8_t slot;            // TDMA 时隙号
    int64_t offset_us;       // 时钟同步偏移 (us)
    int8_t rssi;             // 反向 RSSI (dBm, 负值)
    uint32_t timestamp;      // 时间戳 (s)
} sensor_data_t;

int sensor_data_to_json(const sensor_data_t *data, char *json_buf, size_t buf_size);

#endif
