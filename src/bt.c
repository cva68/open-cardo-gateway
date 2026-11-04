/**
 * @file bt.c
 * @brief Bluetooth stack bring-up, pairing, bonds, discovery and advertising data.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "bt.h"

static const char *TAG = "bt";

/** Legacy (pre-SSP) pairing PIN, the usual default for headsets. */
static const char LEGACY_PIN[] = "0000";

char *bda_to_str(const esp_bd_addr_t bda, char *str)
{
    snprintf(str, BDA_STR_LEN, "%02x:%02x:%02x:%02x:%02x:%02x",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    return str;
}

bool bda_from_str(const char *str, esp_bd_addr_t bda)
{
    return sscanf(str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                  &bda[0], &bda[1], &bda[2], &bda[3], &bda[4], &bda[5]) == ESP_BD_ADDR_LEN;
}

/** @brief Length of the significant part of EIR data: AD structures up to the first zero length. */
static size_t eir_len(const uint8_t *eir, size_t max)
{
    size_t i = 0;
    while (i < max && eir[i] != 0 && i + 1 + eir[i] <= max) {
        i += 1 + eir[i];
    }
    return i;
}

/** @brief Log an inquiry result: address, name, CoD, RSSI and raw EIR. */
static void log_disc_result(esp_bt_gap_cb_param_t *param)
{
    char bda_str[BDA_STR_LEN];
    char name[ESP_BT_GAP_MAX_BDNAME_LEN + 1] = "";
    uint32_t cod = 0;
    int rssi = 0;
    uint8_t *eir = NULL;

    for (int i = 0; i < param->disc_res.num_prop; i++) {
        esp_bt_gap_dev_prop_t *p = &param->disc_res.prop[i];
        switch (p->type) {
        case ESP_BT_GAP_DEV_PROP_BDNAME:
            snprintf(name, sizeof(name), "%.*s", p->len, (char *)p->val);
            break;
        case ESP_BT_GAP_DEV_PROP_COD:
            cod = *(uint32_t *)p->val;
            break;
        case ESP_BT_GAP_DEV_PROP_RSSI:
            rssi = *(int8_t *)p->val;
            break;
        case ESP_BT_GAP_DEV_PROP_EIR:
            eir = p->val;
            break;
        }
    }

    if (name[0] == '\0' && eir != NULL) {
        uint8_t len = 0;
        uint8_t *n = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &len);
        if (n == NULL) {
            n = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &len);
        }
        if (n != NULL) {
            snprintf(name, sizeof(name), "%.*s", len, (char *)n);
        }
    }

    ESP_LOGI(TAG, "Found %s \"%s\" CoD 0x%06" PRIx32 " RSSI %d",
             bda_to_str(param->disc_res.bda, bda_str), name, cod, rssi);

    if (eir != NULL) {
        // Raw EIR in hex, for service UUIDs and any manufacturer-specific data
        size_t len = eir_len(eir, ESP_BT_GAP_EIR_DATA_LEN);
        char hex[2 * ESP_BT_GAP_EIR_DATA_LEN + 1];
        for (size_t i = 0; i < len; i++) {
            sprintf(&hex[2 * i], "%02x", eir[i]);
        }
        hex[2 * len] = '\0';
        ESP_LOGI(TAG, "  EIR %s", hex);
    }
}

/** @brief Log the service UUIDs from an SDP query. */
static void log_services(esp_bt_gap_cb_param_t *param)
{
    char bda_str[BDA_STR_LEN];
    bda_to_str(param->rmt_srvcs.bda, bda_str);
    if (param->rmt_srvcs.stat != ESP_BT_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "Service query of %s failed, status 0x%02x", bda_str, param->rmt_srvcs.stat);
        return;
    }
    ESP_LOGI(TAG, "%s offers %d service(s):", bda_str, param->rmt_srvcs.num_uuids);
    for (int i = 0; i < param->rmt_srvcs.num_uuids; i++) {
        esp_bt_uuid_t *u = &param->rmt_srvcs.uuid_list[i];
        if (u->len == ESP_UUID_LEN_16) {
            ESP_LOGI(TAG, "  0x%04x", u->uuid.uuid16);
        } else if (u->len == ESP_UUID_LEN_32) {
            ESP_LOGI(TAG, "  0x%08" PRIx32, u->uuid.uuid32);
        } else {
            // Bluedroid stores these in the usual big-endian order
            char hex[2 * ESP_UUID_LEN_128 + 1];
            for (int b = 0; b < ESP_UUID_LEN_128; b++) {
                sprintf(&hex[2 * b], "%02x", u->uuid.uuid128[b]);
            }
            ESP_LOGI(TAG, "  %s", hex);
        }
    }
}

/** @brief Name of a link power mode, for logs. */
static const char *pm_mode_name(esp_bt_pm_mode_t mode)
{
    switch (mode) {
    case ESP_BT_PM_MD_ACTIVE: return "active";
    case ESP_BT_PM_MD_HOLD:   return "hold";
    case ESP_BT_PM_MD_SNIFF:  return "sniff";
    case ESP_BT_PM_MD_PARK:   return "park";
    default:                  return "unknown";
    }
}

/** @brief GAP events: log links, pairing and discovery, and answer pairing requests. */
static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    char bda_str[BDA_STR_LEN];

    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT:
        log_disc_result(param);
        break;

    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        ESP_LOGI(TAG, "Scan %s", param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED ? "started" : "finished");
        break;

    case ESP_BT_GAP_RMT_SRVCS_EVT:
        log_services(param);
        break;

    case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT:
        if (param->acl_conn_cmpl_stat.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "ACL connected to %s (handle 0x%04x)",
                     bda_to_str(param->acl_conn_cmpl_stat.bda, bda_str), param->acl_conn_cmpl_stat.handle);
        } else {
            ESP_LOGW(TAG, "ACL connection to %s failed, status 0x%02x",
                     bda_to_str(param->acl_conn_cmpl_stat.bda, bda_str), param->acl_conn_cmpl_stat.stat);
        }
        break;

    case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT:
        // HCI reason codes: 0x08 link timeout (out of range), 0x13 remote closed, 0x16 we closed
        ESP_LOGI(TAG, "ACL disconnected from %s, reason 0x%02x",
                 bda_to_str(param->acl_disconn_cmpl_stat.bda, bda_str), param->acl_disconn_cmpl_stat.reason);
        break;

    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Paired with \"%s\" (%s), link key type %d", param->auth_cmpl.device_name,
                     bda_to_str(param->auth_cmpl.bda, bda_str), param->auth_cmpl.lk_type);
        } else {
            ESP_LOGW(TAG, "Pairing with %s failed, status 0x%02x",
                     bda_to_str(param->auth_cmpl.bda, bda_str), param->auth_cmpl.stat);
        }
        break;

    case ESP_BT_GAP_PIN_REQ_EVT: {
        ESP_LOGI(TAG, "Legacy PIN requested by %s%s, replying %s", bda_to_str(param->pin_req.bda, bda_str),
                 param->pin_req.min_16_digit ? " (16 digits required)" : "", LEGACY_PIN);
        esp_bt_pin_code_t pin = {0};
        memcpy(pin, LEGACY_PIN, strlen(LEGACY_PIN));
        esp_bt_gap_pin_reply(param->pin_req.bda, true, strlen(LEGACY_PIN), pin);
        break;
    }

    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(TAG, "SSP confirmation from %s (value %06" PRIu32 "), accepting",
                 bda_to_str(param->cfm_req.bda, bda_str), param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;

    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(TAG, "SSP passkey for %s: %06" PRIu32,
                 bda_to_str(param->key_notif.bda, bda_str), param->key_notif.passkey);
        break;

    case ESP_BT_GAP_KEY_REQ_EVT:
        ESP_LOGW(TAG, "SSP passkey entry requested by %s, which we can't do",
                 bda_to_str(param->key_req.bda, bda_str));
        break;

    case ESP_BT_GAP_ENC_CHG_EVT:
        ESP_LOGI(TAG, "Encryption with %s now mode %d",
                 bda_to_str(param->enc_chg.bda, bda_str), param->enc_chg.enc_mode);
        break;

    case ESP_BT_GAP_MODE_CHG_EVT:
        ESP_LOGI(TAG, "Link to %s now %s (interval %u slots)", bda_to_str(param->mode_chg.bda, bda_str),
                 pm_mode_name(param->mode_chg.mode), param->mode_chg.interval);
        break;

    case ESP_BT_GAP_REMOVE_BOND_DEV_COMPLETE_EVT:
        ESP_LOGI(TAG, "Removed bond with %s, status 0x%02x",
                 bda_to_str(param->remove_bond_dev_cmpl.bda, bda_str), param->remove_bond_dev_cmpl.status);
        break;

    default:
        ESP_LOGD(TAG, "GAP event %d", event);
        break;
    }
}

void bt_init(const char *device_name)
{
    // Classic BT only, so hand the BLE controller memory back to the heap
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));

    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&bluedroid_cfg));
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_bt_gap_register_callback(gap_cb));
    ESP_ERROR_CHECK(esp_bt_gap_set_device_name(device_name));

    // No display or keyboard, so SSP uses "Just Works"; legacy pairing gets PIN_REQ_EVT
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    ESP_ERROR_CHECK(esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap)));
    esp_bt_pin_code_t no_pin = {0};
    ESP_ERROR_CHECK(esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_VARIABLE, 0, no_pin));

    ESP_ERROR_CHECK(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE));

    char bda_str[BDA_STR_LEN];
    ESP_LOGI(TAG, "Bluetooth up as \"%s\", address %s", device_name,
             bda_to_str(esp_bt_dev_get_address(), bda_str));
}

void bt_set_cod(uint32_t value)
{
    esp_bt_cod_t cod = {
        .minor = (value >> 2) & 0x3f,
        .major = (value >> 8) & 0x1f,
        .service = (value >> 13) & 0x7ff,
    };
    esp_bt_gap_set_cod(cod, ESP_BT_INIT_COD);
}

uint32_t bt_get_cod(void)
{
    esp_bt_cod_t cod = {0};
    esp_bt_gap_get_cod(&cod);
    return (cod.service << 13) | (cod.major << 8) | (cod.minor << 2);
}

void bt_set_eir_manufacturer(const uint8_t *data, size_t len)
{
    static uint8_t s_mfr[ESP_BT_EIR_MAX_LEN - 2];  // less the AD length and type bytes
    if (len > sizeof(s_mfr)) {
        len = sizeof(s_mfr);
    }
    if (len > 0) {
        memcpy(s_mfr, data, len);
    }
    esp_bt_eir_data_t eir = {
        .fec_required = true,
        .include_name = len == 0,
        .include_uuid = len == 0,
        .manufacturer_len = len,
        .p_manufacturer_data = len > 0 ? s_mfr : NULL,
    };
    esp_bt_gap_config_eir_data(&eir);
}

void bt_scan(void)
{
    // 8 x 1.28 s inquiry, unlimited responses
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 8, 0);
}

void bt_query_services(const esp_bd_addr_t bda)
{
    esp_bt_gap_get_remote_services((uint8_t *)bda);
}

/**
 * @brief List the bonded devices.
 * @param[out] count Number of devices.
 * @return malloc'd list for the caller to free, or NULL if there are none.
 */
static esp_bd_addr_t *get_bonds(int *count)
{
    *count = esp_bt_gap_get_bond_device_num();
    if (*count <= 0) {
        return NULL;
    }
    esp_bd_addr_t *list = calloc(*count, sizeof(esp_bd_addr_t));
    if (list == NULL || esp_bt_gap_get_bond_device_list(count, list) != ESP_OK) {
        free(list);
        *count = 0;
        return NULL;
    }
    return list;
}

bool bt_is_bonded(const esp_bd_addr_t bda)
{
    int count;
    esp_bd_addr_t *list = get_bonds(&count);
    bool found = false;
    for (int i = 0; i < count && !found; i++) {
        found = memcmp(list[i], bda, ESP_BD_ADDR_LEN) == 0;
    }
    free(list);
    return found;
}

void bt_print_bonds(void)
{
    int count;
    esp_bd_addr_t *list = get_bonds(&count);
    char bda_str[BDA_STR_LEN];
    printf("%d bonded device(s)\n", count);
    for (int i = 0; i < count; i++) {
        printf("  %s\n", bda_to_str(list[i], bda_str));
    }
    free(list);
}

void bt_remove_all_bonds(void)
{
    int count;
    esp_bd_addr_t *list = get_bonds(&count);
    for (int i = 0; i < count; i++) {
        esp_bt_gap_remove_bond_device(list[i]);
    }
    free(list);
}
