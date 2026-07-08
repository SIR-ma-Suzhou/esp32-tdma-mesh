
#include "hw_crypto.h"

#include <string.h>
#include <stdbool.h>
#include <limits.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_mac.h"

#include "mbedtls/gcm.h"
#include "mbedtls/md.h"

static const char *TAG = "hw_crypto";

#define HW_CRYPTO_KEY_WINDOW_US    (30ULL * 60ULL * 1000000ULL)

#define HW_CRYPTO_HMAC_SHA256_LEN  32

static uint8_t s_master_key[HW_CRYPTO_AES_GCM_KEY_LEN];
static bool s_key_ready = false;

static uint8_t s_default_node_mac[6];
static bool s_default_node_mac_ready = false;

static void hw_crypto_secure_zero(void *buf, size_t len)
{
    volatile uint8_t *p = (volatile uint8_t *)buf;

    while (len--) {
        *p++ = 0;
    }
}

static void hw_crypto_store_u64_be(uint8_t out[8], uint64_t value)
{
    out[0] = (uint8_t)(value >> 56);
    out[1] = (uint8_t)(value >> 48);
    out[2] = (uint8_t)(value >> 40);
    out[3] = (uint8_t)(value >> 32);
    out[4] = (uint8_t)(value >> 24);
    out[5] = (uint8_t)(value >> 16);
    out[6] = (uint8_t)(value >> 8);
    out[7] = (uint8_t)(value);
}

static esp_err_t hw_crypto_load_default_node_mac(void)
{
    esp_err_t err = esp_efuse_mac_get_default(s_default_node_mac);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read default MAC: %s", esp_err_to_name(err));
        return err;
    }

    s_default_node_mac_ready = true;
    return ESP_OK;
}

esp_err_t hw_crypto_set_default_node_mac(const uint8_t node_mac[6])
{
    if (node_mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(s_default_node_mac, node_mac, sizeof(s_default_node_mac));
    s_default_node_mac_ready = true;

    return ESP_OK;
}

static esp_err_t hw_crypto_get_default_node_mac(uint8_t node_mac[6])
{
    if (node_mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_default_node_mac_ready) {
        esp_err_t err = hw_crypto_load_default_node_mac();
        if (err != ESP_OK) {
            return err;
        }
    }

    memcpy(node_mac, s_default_node_mac, 6);
    return ESP_OK;
}

esp_err_t hw_crypto_derive_session_key(uint8_t session_key[HW_CRYPTO_AES_GCM_KEY_LEN],
                                       const uint8_t master_key[HW_CRYPTO_AES_GCM_KEY_LEN],
                                       const uint8_t node_mac[6],
                                       uint64_t network_time_us)
{
    if (session_key == NULL || master_key == NULL || node_mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md_info == NULL) {
        return ESP_FAIL;
    }

    uint64_t time_window = network_time_us / HW_CRYPTO_KEY_WINDOW_US;

    uint8_t info[7 + 6 + 8];
    uint8_t digest[HW_CRYPTO_HMAC_SHA256_LEN];

    memcpy(info, "HWGCMv1", 7);
    memcpy(info + 7, node_mac, 6);
    hw_crypto_store_u64_be(info + 7 + 6, time_window);

    int ret = mbedtls_md_hmac(md_info,
                              master_key,
                              HW_CRYPTO_AES_GCM_KEY_LEN,
                              info,
                              sizeof(info),
                              digest);

    if (ret != 0) {
        hw_crypto_secure_zero(info, sizeof(info));
        hw_crypto_secure_zero(digest, sizeof(digest));
        ESP_LOGE(TAG, "HMAC-SHA256 derive failed: -0x%04X", -ret);
        return ESP_FAIL;
    }

    memcpy(session_key, digest, HW_CRYPTO_AES_GCM_KEY_LEN);

    hw_crypto_secure_zero(info, sizeof(info));
    hw_crypto_secure_zero(digest, sizeof(digest));

    return ESP_OK;
}

esp_err_t hw_crypto_init(const uint8_t key[HW_CRYPTO_AES_GCM_KEY_LEN])
{
    if (key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(s_master_key, key, HW_CRYPTO_AES_GCM_KEY_LEN);
    s_key_ready = true;

    (void)hw_crypto_load_default_node_mac();

    ESP_LOGI(TAG, "AES-GCM initialized with dynamic session key mode");

    return ESP_OK;
}

size_t hw_crypto_get_packet_len(size_t payload_len)
{
    return sizeof(hw_crypto_packet_header_t)
           + payload_len
           + HW_CRYPTO_GCM_TAG_LEN;
}

static void hw_crypto_make_iv(uint8_t iv[HW_CRYPTO_GCM_IV_LEN],
                              uint64_t network_time_us)
{
    uint32_t random_part = esp_random();

    hw_crypto_store_u64_be(iv, network_time_us);

    iv[8]  = (uint8_t)(random_part >> 24);
    iv[9]  = (uint8_t)(random_part >> 16);
    iv[10] = (uint8_t)(random_part >> 8);
    iv[11] = (uint8_t)(random_part);
}

static bool hw_crypto_time_is_valid(uint64_t packet_time_us,
                                    uint64_t current_time_us,
                                    uint64_t allowed_diff_us)
{
    uint64_t diff;

    if (packet_time_us > current_time_us) {
        diff = packet_time_us - current_time_us;
    } else {
        diff = current_time_us - packet_time_us;
    }

    return diff <= allowed_diff_us;
}

esp_err_t hw_crypto_encrypt_inplace_with_node_mac(uint8_t *buffer,
                                                  size_t buffer_size,
                                                  size_t plaintext_len,
                                                  uint8_t msg_type,
                                                  uint64_t network_time_us,
                                                  const uint8_t node_mac[6],
                                                  size_t *out_packet_len)
{
    if (!s_key_ready) {
        ESP_LOGE(TAG, "Master key is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (buffer == NULL || node_mac == NULL || out_packet_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (plaintext_len > UINT16_MAX) {
        ESP_LOGE(TAG, "Plaintext too large");
        return ESP_ERR_INVALID_SIZE;
    }

    size_t packet_len = hw_crypto_get_packet_len(plaintext_len);

    if (buffer_size < packet_len) {
        ESP_LOGE(TAG, "Buffer too small, need %u, got %u",
                 (unsigned)packet_len, (unsigned)buffer_size);
        return ESP_ERR_NO_MEM;
    }

    hw_crypto_packet_header_t *header = (hw_crypto_packet_header_t *)buffer;
    uint8_t *payload = buffer + sizeof(hw_crypto_packet_header_t);
    uint8_t *tag = payload + plaintext_len;

    memset(header, 0, sizeof(hw_crypto_packet_header_t));

    header->magic = HW_CRYPTO_PACKET_MAGIC;
    header->version = HW_CRYPTO_PACKET_VERSION;
    header->msg_type = msg_type;
    header->flags = 0;
    header->timestamp_us = network_time_us;
    header->payload_len = (uint16_t)plaintext_len;

    hw_crypto_make_iv(header->iv, network_time_us);

    uint8_t session_key[HW_CRYPTO_AES_GCM_KEY_LEN];

    esp_err_t err = hw_crypto_derive_session_key(session_key,
                                                 s_master_key,
                                                 node_mac,
                                                 network_time_us);
    if (err != ESP_OK) {
        return err;
    }

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);

    int ret = mbedtls_gcm_setkey(&gcm,
                                 MBEDTLS_CIPHER_ID_AES,
                                 session_key,
                                 HW_CRYPTO_AES_GCM_KEY_LEN * 8);

    if (ret != 0) {
        mbedtls_gcm_free(&gcm);
        hw_crypto_secure_zero(session_key, sizeof(session_key));
        ESP_LOGE(TAG, "mbedtls_gcm_setkey failed: -0x%04X", -ret);
        return ESP_FAIL;
    }

    ret = mbedtls_gcm_crypt_and_tag(&gcm,
                                    MBEDTLS_GCM_ENCRYPT,
                                    plaintext_len,
                                    header->iv,
                                    HW_CRYPTO_GCM_IV_LEN,
                                    (const unsigned char *)header,
                                    sizeof(hw_crypto_packet_header_t),
                                    payload,
                                    payload,
                                    HW_CRYPTO_GCM_TAG_LEN,
                                    tag);

    mbedtls_gcm_free(&gcm);
    hw_crypto_secure_zero(session_key, sizeof(session_key));

    if (ret != 0) {
        ESP_LOGE(TAG, "AES-GCM encrypt failed: -0x%04X", -ret);
        return ESP_FAIL;
    }

    *out_packet_len = packet_len;
    return ESP_OK;
}

esp_err_t hw_crypto_encrypt_inplace(uint8_t *buffer,
                                    size_t buffer_size,
                                    size_t plaintext_len,
                                    uint8_t msg_type,
                                    uint64_t network_time_us,
                                    size_t *out_packet_len)
{
    uint8_t node_mac[6];

    esp_err_t err = hw_crypto_get_default_node_mac(node_mac);
    if (err != ESP_OK) {
        return err;
    }

    return hw_crypto_encrypt_inplace_with_node_mac(buffer,
                                                   buffer_size,
                                                   plaintext_len,
                                                   msg_type,
                                                   network_time_us,
                                                   node_mac,
                                                   out_packet_len);
}

esp_err_t hw_crypto_decrypt_inplace_with_node_mac(uint8_t *buffer,
                                                  size_t packet_len,
                                                  const uint8_t node_mac[6],
                                                  uint64_t current_time_us,
                                                  uint64_t allowed_diff_us,
                                                  uint8_t **out_plaintext,
                                                  size_t *out_plaintext_len)
{
    if (!s_key_ready) {
        ESP_LOGE(TAG, "Master key is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (buffer == NULL || node_mac == NULL || out_plaintext == NULL || out_plaintext_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (packet_len < sizeof(hw_crypto_packet_header_t) + HW_CRYPTO_GCM_TAG_LEN) {
        ESP_LOGE(TAG, "Packet too short");
        return ESP_ERR_INVALID_SIZE;
    }

    hw_crypto_packet_header_t *header = (hw_crypto_packet_header_t *)buffer;

    if (header->magic != HW_CRYPTO_PACKET_MAGIC) {
        ESP_LOGE(TAG, "Invalid packet magic");
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (header->version != HW_CRYPTO_PACKET_VERSION) {
        ESP_LOGE(TAG, "Invalid packet version");
        return ESP_ERR_INVALID_VERSION;
    }

    size_t payload_len = header->payload_len;
    size_t expected_len = hw_crypto_get_packet_len(payload_len);

    if (packet_len != expected_len) {
        ESP_LOGE(TAG, "Invalid packet length, expected %u, got %u",
                 (unsigned)expected_len, (unsigned)packet_len);
        return ESP_ERR_INVALID_SIZE;
    }

    if (!hw_crypto_time_is_valid(header->timestamp_us,
                                 current_time_us,
                                 allowed_diff_us)) {
        ESP_LOGW(TAG, "Packet timestamp invalid");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t *payload = buffer + sizeof(hw_crypto_packet_header_t);
    uint8_t *tag = payload + payload_len;

    uint8_t session_key[HW_CRYPTO_AES_GCM_KEY_LEN];

    esp_err_t err = hw_crypto_derive_session_key(session_key,
                                                 s_master_key,
                                                 node_mac,
                                                 header->timestamp_us);
    if (err != ESP_OK) {
        return err;
    }

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);

    int ret = mbedtls_gcm_setkey(&gcm,
                                 MBEDTLS_CIPHER_ID_AES,
                                 session_key,
                                 HW_CRYPTO_AES_GCM_KEY_LEN * 8);

    if (ret != 0) {
        mbedtls_gcm_free(&gcm);
        hw_crypto_secure_zero(session_key, sizeof(session_key));
        ESP_LOGE(TAG, "mbedtls_gcm_setkey failed: -0x%04X", -ret);
        return ESP_FAIL;
    }

    ret = mbedtls_gcm_auth_decrypt(&gcm,
                                   payload_len,
                                   header->iv,
                                   HW_CRYPTO_GCM_IV_LEN,
                                   (const unsigned char *)header,
                                   sizeof(hw_crypto_packet_header_t),
                                   tag,
                                   HW_CRYPTO_GCM_TAG_LEN,
                                   payload,
                                   payload);

    mbedtls_gcm_free(&gcm);
    hw_crypto_secure_zero(session_key, sizeof(session_key));

    if (ret != 0) {
        ESP_LOGE(TAG, "AES-GCM decrypt/auth failed: -0x%04X", -ret);
        return ESP_ERR_INVALID_CRC;
    }

    *out_plaintext = payload;
    *out_plaintext_len = payload_len;

    return ESP_OK;
}

esp_err_t hw_crypto_decrypt_inplace(uint8_t *buffer,
                                    size_t packet_len,
                                    uint64_t current_time_us,
                                    uint64_t allowed_diff_us,
                                    uint8_t **out_plaintext,
                                    size_t *out_plaintext_len)
{
    uint8_t node_mac[6];

    esp_err_t err = hw_crypto_get_default_node_mac(node_mac);
    if (err != ESP_OK) {
        return err;
    }

    return hw_crypto_decrypt_inplace_with_node_mac(buffer,
                                                   packet_len,
                                                   node_mac,
                                                   current_time_us,
                                                   allowed_diff_us,
                                                   out_plaintext,
                                                   out_plaintext_len);
}
