# 基于 ESP32-S3 的低功耗 TDMA 自组网系统

基于 ESP32-S3 + ESP-NOW 的低功耗、高可靠、全加密 TDMA 无线自组网系统。在无任何基础设施（Wi-Fi 路由器、4G/5G 基站）的环境下，自主完成多跳组网、亚毫秒级时间同步、无碰撞数据传输与云端数据汇聚。

## 硬件要求

| 组件 | 型号 | 数量 |
|------|------|------|
| 主控 | ESP32-S3-DevKitC-1（乐鑫官方） | 4 块（1 网关 + 3 终端） |
| 温湿度传感器 | DHT22 | 依终端数 |
| 人体红外传感器 | HC-SR501 | 依终端数 |
| USB 电流表 | FNIRSI FNB48 | 1 个（演示用） |
| 电池 | 18650 电池盒 + 3.3V LDO | 依终端数 |

## 软件要求

- **ESP-IDF v5.1.7**（安装路径 `D:\esp-idf_5.1.7` 或自定义）
- 目标芯片 `esp32s3`
- Python 3.x（用于 Web 仪表板本地测试或工具脚本）
- EMQX MQTT Broker（云端或本地 Docker）

## 快速构建与烧录

```bash
# 1. 激活 ESP-IDF 环境
. /path/to/esp-idf/export.sh

# 2. 设置目标芯片
idf.py set-target esp32s3

# 3. 配置角色
#    打开 main/main.c，修改宏：
#      NODE_IS_ROOT  1 → 网关   0 → 终端
#      ENABLE_WIFI   1 → 网关    0 → 终端
#      ROOT_NUM      6 → ESP-NOW 信道（1-13）

# 4. 构建
idf.py build

# 5. 烧录并监视
idf.py -p /dev/ttyUSB0 flash monitor
```

**终端 MAC 地址**（修改 `main/main.c` 中 `TERMINAL_MAC_LIST`）：
```
Black:  E8:F6:0A:A6:F8:8C → 网关
White:  E8:F6:0A:A6:F1:D0 → 终端 1
Board2: E8:F6:0A:A6:F8:A0 → 终端 2
Board3: E8:F6:0A:A7:29:8C → 终端 3
```

## 测试模式

修改 `main/main.c` 中的测试宏：

```c
#define TEST_MODE       0    // 0=正常模式, 1=丢包率测试
#define TEST_PACKET_COUNT 1000
#define TEST_CSMA_MODE  0    // 0=TDMA, 1=CSMA 对照
```

- `TEST_MODE=1, TEST_CSMA_MODE=0`：终端发送 1000 个 `TEST:N` 包，网关统计 TDMA 丢包率
- `TEST_MODE=1, TEST_CSMA_MODE=1`：同上但使用 CSMA 随机退避作为对照组
- `TEST_MODE=0`：正常传感器数据上报运行

## MQTT 云端配置

| 项 | 值 |
|----|-----|
| Broker | `mqtts://d4c11111.ala.cn-hangzhou.emqxsl.cn:8883` |
| TLS | ESP 证书包（`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y`） |
| 传感器主题 | `esp32/sensor` |
| 拓扑主题 | `esp32/topology` |
| TDMA 状态主题 | `esp32/tdma_status` |
| 系统状态主题 | `esp32/status` |

Web 仪表板文件 `web/dashboard_multi_node_select_power_fixed.html` 通过 WebSocket（端口 8083）连接同一 Broker。

## 目标性能指标

| 指标 | 目标值 |
|------|--------|
| 终端休眠功耗（叶子节点） | < 100 μA |
| 中继节点休眠功耗 | ~ 800 μA |
| 多跳时间同步精度 | < 1 ms |
| 单跳通信距离 | > 100 m |
| TDMA 丢包率 | < 0.5% |
| 异常事件上报延迟 | < 2 s |
| 加密方式 | AES-128-GCM 硬件加速 |
| TDMA 超帧周期 | 500 ms |

## 文件结构

```
project_vr1/
├── CMakeLists.txt              # 顶层构建脚本
├── sdkconfig                   # ESP-IDF 配置
├── partitions-8MIB.csv         # 分区表
├── dependencies.lock           # 组件版本锁定
├── main/
│   ├── CMakeLists.txt
│   └── main.c                  # 主程序（网关/终端共用，宏切换角色）
├── components/
│   ├── espnow_core/            # ESP-NOW 收发管理 + 发送队列 + 回调链
│   ├── time_sync/              # 信标时间同步 + 多跳中继
│   ├── tdma_scheduler/         # 500ms 超帧调度 + 时隙分配
│   ├── simple_route/           # 树形路由（Hello/RSSI/心跳/TTL）
│   ├── hw_crypto/              # AES-128-GCM 硬件加密
│   ├── mqtt_handler/           # MQTT/TLS 客户端（网关侧）
│   ├── json_converter/         # 传感器数据 JSON 序列化
│   ├── sensor_dht/             # DHT22 温湿度驱动
│   ├── sensor_pir/             # HC-SR501 人体红外驱动
│   ├── sensor_batt/            # 电池电压 ADC 驱动
│   ├── topology_aggregation/   # 拓扑数据聚合
│   └── esp-now/                # esp-now 托管组件（ESP-IDF 组件管理器自动下载）
└── web/
    └── dashboard_multi_node_select_power_fixed.html  # Web 仪表板
```

## 设计文档

详细设计说明见《设计报告_基于ESP32-S3的低功耗TDMA自组网系统.docx》。
