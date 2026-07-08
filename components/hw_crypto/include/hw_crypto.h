#ifndef HW_CRYPTO_H
#define HW_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HW_CRYPTO_AES_GCM_KEY_LEN   16

#define HW_CRYPTO_GCM_IV_LEN        12

#define HW_CRYPTO_GCM_TAG_LEN       16

#define HW_CRYPTO_PACKET_MAGIC      0x4847434DUL

#define HW_CRYPTO_PACKET_VERSION    1

#define HW_CRYPTO_DEFAULT_TIME_WINDOW_US (5 * 1000 * 1000ULL)

typedef struct __attribute__((packed)) {
    uint32_t magic;                         // 包魔数，固定为 HW_CRYPTO_PACKET_MAGIC
    uint8_t version;                        // 包版本，固定为 HW_CRYPTO_PACKET_VERSION
    uint8_t msg_type;                       // 消息类型，例如传感器数据、报警数据、调试数据
    uint8_t flags;                          // 标志位，当前保留
    uint8_t reserved;                       // 保留字段，当前置 0
    uint64_t timestamp_us;                  // 全网同步时间戳，单位：微秒
    uint16_t payload_len;                   // 明文长度 / 密文长度
    uint8_t iv[HW_CRYPTO_GCM_IV_LEN];       // AES-GCM IV / Nonce
} hw_crypto_packet_header_t;

esp_err_t hw_crypto_init(const uint8_t key[HW_CRYPTO_AES_GCM_KEY_LEN]);

esp_err_t hw_crypto_set_default_node_mac(const uint8_t node_mac[6]);

size_t hw_crypto_get_packet_len(size_t payload_len);

esp_err_t hw_crypto_derive_session_key(uint8_t session_key[HW_CRYPTO_AES_GCM_KEY_LEN],
                                       const uint8_t master_key[HW_CRYPTO_AES_GCM_KEY_LEN],
                                       const uint8_t node_mac[6],
                                       uint64_t network_time_us);

esp_err_t hw_crypto_encrypt_inplace(uint8_t *buffer,
                                    size_t buffer_size,
                                    size_t plaintext_len,
                                    uint8_t msg_type,
                                    uint64_t network_time_us,
                                    size_t *out_packet_len);

esp_err_t hw_crypto_decrypt_inplace(uint8_t *buffer,
                                    size_t packet_len,
                                    uint64_t current_time_us,
                                    uint64_t allowed_diff_us,
                                    uint8_t **out_plaintext,
                                    size_t *out_plaintext_len);

esp_err_t hw_crypto_encrypt_inplace_with_node_mac(uint8_t *buffer,
                                                  size_t buffer_size,
                                                  size_t plaintext_len,
                                                  uint8_t msg_type,
                                                  uint64_t network_time_us,
                                                  const uint8_t node_mac[6],
                                                  size_t *out_packet_len);

esp_err_t hw_crypto_decrypt_inplace_with_node_mac(uint8_t *buffer,
                                                  size_t packet_len,
                                                  const uint8_t node_mac[6],
                                                  uint64_t current_time_us,
                                                  uint64_t allowed_diff_us,
                                                  uint8_t **out_plaintext,
                                                  size_t *out_plaintext_len);

#ifdef __cplusplus
}
#endif

#endif /* HW_CRYPTO_H */
