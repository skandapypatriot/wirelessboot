#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_flash_partitions.h"
#include "esp_image_format.h"
#include "esp_app_format.h"
#include "esp_secure_boot.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "mbedtls/sha256.h"
#include "mbedtls/pk.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/asn1.h"

#define BOOT_BUTTON_GPIO 9
#define OTA_SERVICE_UUID 0x180A
#define OTA_CHAR_UUID    0x2A57

#define CHUNK_SIZE 240
#define MAX_FIRMWARE_SIZE (3800 * 1024)
#define SIG_SIZE 64  // ECDSA P-256 r||s (32+32)

#define EVT_CONNECTED      (1 << 0)
#define EVT_DISCONNECTED   (1 << 1)
#define EVT_WRITE_COMPLETE (1 << 2)

static const char *TAG = "ble_ota";

// Embedded public key (replace with your own)
// Generated from: openssl ecparam -genkey -name prime256v1 -noout -out private.pem
// Public key extracted: openssl ec -in private.pem -pubout -out public.pem
static const uint8_t ota_pubkey_pem[] = {
    0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86,
    0x48, 0xce, 0x3d, 0x02, 0x01, 0x06, 0x08, 0x2a,
    0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03,
    0x42, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t ota_chr_val_handle = 0;
static EventGroupHandle_t ble_events;
static const esp_partition_t *ota_partition = NULL;
static esp_ota_handle_t ota_handle = 0;
static bool ota_in_progress = false;
static uint32_t total_received = 0;
static uint32_t expected_size = 0;
static uint32_t expected_sig_size = 0;
static uint8_t sha256_ctx[32];
static mbedtls_sha256_context sha256;
static uint8_t sig_buffer[SIG_SIZE];
static uint32_t sig_received = 0;

static int ota_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg);

static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(OTA_SERVICE_UUID),
        .characteristics = (struct ble_gatt_chr_def[]) { {
            .uuid = BLE_UUID16_DECLARE(OTA_CHAR_UUID),
            .access_cb = ota_access_cb,
            .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            .val_handle = &ota_chr_val_handle,
        }, {
            0,
        } },
    }, {
        0,
    }
};

static int ota_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    }

    struct os_mbuf *om = ctxt->om;
    uint16_t len = OS_MBUF_PKTLEN(om);
    
    if (len == 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    if (!ota_in_progress) {
        if (len < 5) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        
        uint8_t hdr[5];
        os_mbuf_copydata(om, 0, 5, hdr);
        expected_size = (hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | (hdr[3] << 24));
        expected_sig_size = hdr[4];
        
        if (expected_size > MAX_FIRMWARE_SIZE || expected_size == 0) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        if (expected_sig_size != SIG_SIZE && expected_sig_size != 0) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }

        esp_err_t err = esp_ota_begin(ota_partition, OTA_SIZE_UNKNOWN, &ota_handle);
        if (err != ESP_OK) {
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }

        mbedtls_sha256_init(&sha256);
        mbedtls_sha256_starts(&sha256, 0);
        
        ota_in_progress = true;
        total_received = 0;
        sig_received = 0;
        
        if (len > 5) {
            mbedtls_sha256_update(&sha256, om->om_data + 5, len - 5);
            err = esp_ota_write(ota_handle, om->om_data + 5, len - 5);
            if (err != ESP_OK) {
                esp_ota_end(ota_handle);
                ota_in_progress = false;
                return BLE_ATT_ERR_INSUFFICIENT_RES;
            }
            total_received += len - 5;
        }
    } else {
        uint32_t remaining_fw = (expected_size > total_received) ? (expected_size - total_received) : 0;
        
        if (remaining_fw > 0) {
            uint32_t fw_chunk = (len < remaining_fw) ? len : remaining_fw;
            mbedtls_sha256_update(&sha256, om->om_data, fw_chunk);
            esp_err_t err = esp_ota_write(ota_handle, om->om_data, fw_chunk);
            if (err != ESP_OK) {
                esp_ota_end(ota_handle);
                ota_in_progress = false;
                return BLE_ATT_ERR_INSUFFICIENT_RES;
            }
            total_received += fw_chunk;
            
            if (len > fw_chunk) {
                uint32_t sig_chunk = len - fw_chunk;
                uint32_t copy_len = (sig_chunk < SIG_SIZE - sig_received) ? sig_chunk : (SIG_SIZE - sig_received);
                memcpy(sig_buffer + sig_received, om->om_data + fw_chunk, copy_len);
                sig_received += copy_len;
            }
        } else {
            uint32_t copy_len = (len < SIG_SIZE - sig_received) ? len : (SIG_SIZE - sig_received);
            memcpy(sig_buffer + sig_received, om->om_data, copy_len);
            sig_received += copy_len;
        }
    }

    return 0;
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            conn_handle = event->connect.conn_handle;
            xEventGroupSetBits(ble_events, EVT_CONNECTED);
        } else {
            conn_handle = BLE_HS_CONN_HANDLE_NONE;
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        conn_handle = BLE_HS_CONN_HANDLE_NONE;
        xEventGroupSetBits(ble_events, EVT_DISCONNECTED);
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        ble_gap_adv_start();
        break;
    default:
        break;
    }
    return 0;
}

static void ble_advertise(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name = (uint8_t *)"OTA-Recovery";
    fields.name_len = strlen("OTA-Recovery");
    fields.name_is_complete = 1;
    fields.uuids16 = (ble_uuid16_t[]) { BLE_UUID16_INIT(OTA_SERVICE_UUID) };
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    ble_gap_adv_set_fields(&fields);

    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min = 0x00A0;
    adv_params.itvl_max = 0x00F0;
    adv_params.channel_map = 0;
    adv_params.filter_policy = BLE_HS_ADV_FP_NO_WL;

    ble_gap_adv_start(0, NULL, BLE_HS_FOREVER, &adv_params, gap_event_cb, NULL);
}

static void on_sync(void)
{
    ble_advertise();
}

static void on_reset(int reason)
{
    ESP_LOGI(TAG, "BLE reset: %d", reason);
}

static void nimble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static bool check_boot_button(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    
    vTaskDelay(pdMS_TO_TICKS(10));
    return gpio_get_level(BOOT_BUTTON_GPIO) == 0;
}

static bool verify_signature(const uint8_t *hash, const uint8_t *sig, size_t sig_len)
{
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    
    int ret = mbedtls_pk_parse_public_key(&pk, ota_pubkey_pem, sizeof(ota_pubkey_pem));
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to parse public key: -0x%04x", -ret);
        mbedtls_pk_free(&pk);
        return false;
    }
    
    if (!mbedtls_pk_can_do(&pk, MBEDTLS_PK_ECDSA)) {
        ESP_LOGE(TAG, "Key is not ECDSA");
        mbedtls_pk_free(&pk);
        return false;
    }
    
    mbedtls_ecdsa_context *ecdsa = mbedtls_pk_ec(pk);
    
    ret = mbedtls_ecdsa_read_signature(ecdsa, hash, 32, sig, sig_len);
    if (ret != 0) {
        ESP_LOGE(TAG, "Signature verification failed: -0x%04x", -ret);
        mbedtls_pk_free(&pk);
        return false;
    }
    
    mbedtls_pk_free(&pk);
    return true;
}

static void verify_and_commit(void)
{
    uint8_t computed_hash[32];
    mbedtls_sha256_finish(&sha256, computed_hash);
    mbedtls_sha256_free(&sha256);
    
    memcpy(sha256_ctx, computed_hash, 32);
    
    if (expected_sig_size == SIG_SIZE) {
        if (sig_received != SIG_SIZE) {
            ESP_LOGE(TAG, "Incomplete signature: got %" PRIu32 "/%d bytes", sig_received, SIG_SIZE);
            esp_ota_end(ota_handle);
            ota_in_progress = false;
            return;
        }
        
        if (!verify_signature(computed_hash, sig_buffer, SIG_SIZE)) {
            ESP_LOGE(TAG, "Signature verification failed");
            esp_ota_end(ota_handle);
            ota_in_progress = false;
            return;
        }
        ESP_LOGI(TAG, "Signature verified");
    } else {
        ESP_LOGW(TAG, "No signature provided (dev mode)");
    }
    
    esp_err_t err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA end failed: %s", esp_err_to_name(err));
        return;
    }
    
    err = esp_ota_set_boot_partition(ota_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Set boot partition failed: %s", esp_err_to_name(err));
        return;
    }
    
    esp_restart();
}

static void ota_complete_task(void *arg)
{
    if (total_received == expected_size) {
        verify_and_commit();
    } else {
        ESP_LOGE(TAG, "Size mismatch: got %" PRIu32 ", expected %" PRIu32, total_received, expected_size);
        esp_ota_end(ota_handle);
        ota_in_progress = false;
    }
    vTaskDelete(NULL);
}

static void ble_host_task(void *param)
{
    ble_events = xEventGroupCreate();
    nimble_port_freertos_init(on_sync);
    nimble_port_run();
    vTaskDelete(NULL);
}

static void ble_gatts_init(void)
{
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(gatt_svr_svcs);
    ble_gatts_add_svcs(gatt_svr_svcs);
}

void app_main(void)
{
    if (!check_boot_button()) {
        const esp_partition_t *running = esp_ota_get_running_partition();
        const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
        if (next != NULL && next != running) {
            esp_ota_set_boot_partition(next);
        }
        esp_restart();
    }

    nvs_flash_init();
    
    ota_partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (ota_partition == NULL) {
        while (1) { vTaskDelay(portMAX_DELAY); }
    }

    ble_gatts_init();
    
    xTaskCreate(nimble_host_task, "nimble_host", 4096, NULL, 5, NULL);
    xTaskCreate(ble_host_task, "ble_host", 4096, NULL, 5, NULL);
    
    while (1) {
        EventBits_t bits = xEventGroupWaitBits(ble_events, EVT_DISCONNECTED, pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
        if (bits & EVT_DISCONNECTED) {
            if (ota_in_progress && total_received == expected_size) {
                xTaskCreate(ota_complete_task, "ota_complete", 4096, NULL, 5, NULL);
            } else if (ota_in_progress) {
                esp_ota_end(ota_handle);
                ota_in_progress = false;
            }
        }
    }
}