/**
 * @file hf.c
 * @brief HFP Hands-Free (headset) role: connection, auto-reconnect and event logging.
 */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "esp_hf_client_api.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "audio.h"
#include "bt.h"
#include "hf.h"

static const char *TAG = "hf";

#define NVS_NAMESPACE      "intercom"           ///< NVS namespace for our settings
#define NVS_KEY_PEER       "peer"               ///< Last connected AG's address
#define RECONNECT_DELAY_US (10 * 1000 * 1000)   ///< Wait between reconnect attempts

/** Name for @p value from a designated-initialiser table, or "unknown". */
#define ENUM_NAME(names, value) \
    ((size_t)(value) < sizeof(names) / sizeof((names)[0]) && (names)[value] ? (names)[value] : "unknown")
/** @p s, or "" if it is NULL (some event strings are optional). */
#define STR_OR_EMPTY(s) ((s) ? (s) : "")

static esp_hf_client_connection_state_t s_conn_state = ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED;
static esp_hf_client_audio_state_t s_audio_state = ESP_HF_CLIENT_AUDIO_STATE_DISCONNECTED;
static esp_hf_sync_conn_hdl_t s_sync_hdl;
static esp_bd_addr_t s_peer;
static bool s_have_peer;
static bool s_auto_reconnect = true;
static esp_timer_handle_t s_reconnect_timer;

static const char *const CONN_STATE_NAMES[] = {
    [ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED] = "disconnected",
    [ESP_HF_CLIENT_CONNECTION_STATE_CONNECTING] = "connecting",
    [ESP_HF_CLIENT_CONNECTION_STATE_CONNECTED] = "RFCOMM connected",
    [ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED] = "service level connected",
    [ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTING] = "disconnecting",
};

static const char *const AUDIO_STATE_NAMES[] = {
    [ESP_HF_CLIENT_AUDIO_STATE_DISCONNECTED] = "disconnected",
    [ESP_HF_CLIENT_AUDIO_STATE_CONNECTING] = "connecting",
    [ESP_HF_CLIENT_AUDIO_STATE_CONNECTED] = "connected (CVSD)",
    [ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC] = "connected (mSBC)",
};

/** AG supported features (+BRSF), for logging. */
static const struct {
    uint32_t bit;
    const char *name;
} AG_FEATURES[] = {
    {ESP_HF_CLIENT_PEER_FEAT_3WAY, "3way"},
    {ESP_HF_CLIENT_PEER_FEAT_ECNR, "ecnr"},
    {ESP_HF_CLIENT_PEER_FEAT_VREC, "vrec"},
    {ESP_HF_CLIENT_PEER_FEAT_INBAND, "inband"},
    {ESP_HF_CLIENT_PEER_FEAT_VTAG, "vtag"},
    {ESP_HF_CLIENT_PEER_FEAT_REJECT, "reject"},
    {ESP_HF_CLIENT_PEER_FEAT_ECS, "ecs"},
    {ESP_HF_CLIENT_PEER_FEAT_ECC, "ecc"},
    {ESP_HF_CLIENT_PEER_FEAT_EXTERR, "exterr"},
    {ESP_HF_CLIENT_PEER_FEAT_CODEC, "codec"},
    {ESP_HF_CLIENT_PEER_FEAT_HF_IND, "hf_ind"},
    {ESP_HF_CLIENT_PEER_FEAT_ESCO_S4, "esco_s4"},
};

/** @brief Remember @p bda as the peer to reconnect to. */
static void peer_save(const esp_bd_addr_t bda)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    nvs_set_blob(nvs, NVS_KEY_PEER, bda, ESP_BD_ADDR_LEN);
    nvs_commit(nvs);
    nvs_close(nvs);
}

/** @brief Load the remembered peer; false if there is none. */
static bool peer_load(esp_bd_addr_t bda)
{
    nvs_handle_t nvs;
    size_t len = ESP_BD_ADDR_LEN;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    bool ok = nvs_get_blob(nvs, NVS_KEY_PEER, bda, &len) == ESP_OK && len == ESP_BD_ADDR_LEN;
    nvs_close(nvs);
    return ok;
}

/** @brief Forget the remembered peer. */
static void peer_erase(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    nvs_erase_key(nvs, NVS_KEY_PEER);
    nvs_commit(nvs);
    nvs_close(nvs);
}

/** @brief Connect to the remembered peer if auto-reconnect is on and it is still bonded. */
static void reconnect_timer_cb(void *arg)
{
    if (!s_auto_reconnect || !s_have_peer || s_conn_state != ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED) {
        return;
    }
    char bda_str[BDA_STR_LEN];
    if (!bt_is_bonded(s_peer)) {
        ESP_LOGW(TAG, "%s is no longer bonded, not reconnecting", bda_to_str(s_peer, bda_str));
        return;
    }
    ESP_LOGI(TAG, "Reconnecting to %s", bda_to_str(s_peer, bda_str));
    esp_hf_client_connect(s_peer);
}

/** @brief Try reconnecting after #RECONNECT_DELAY_US. */
static void schedule_reconnect(void)
{
    if (!s_auto_reconnect || !s_have_peer) {
        return;
    }
    esp_timer_stop(s_reconnect_timer);
    esp_timer_start_once(s_reconnect_timer, RECONNECT_DELAY_US);
}

/** @brief Log the AG's supported and call-hold features. */
static void log_ag_features(uint32_t feat, uint32_t chld_feat)
{
    char names[128] = "";
    size_t len = 0;
    for (size_t i = 0; i < sizeof(AG_FEATURES) / sizeof(AG_FEATURES[0]); i++) {
        if (feat & AG_FEATURES[i].bit) {
            len += snprintf(names + len, sizeof(names) - len, " %s", AG_FEATURES[i].name);
        }
    }
    ESP_LOGI(TAG, "AG features 0x%03" PRIx32 ":%s, call hold features 0x%02" PRIx32, feat, names, chld_feat);
}

/** @brief HF client events: track state, log everything the AG reports, start/stop audio. */
static void hf_event_cb(esp_hf_client_cb_event_t event, esp_hf_client_cb_param_t *param)
{
    char bda_str[BDA_STR_LEN];

    switch (event) {
    case ESP_HF_CLIENT_PROF_STATE_EVT:
        if (param->prof_stat.state == ESP_HF_INIT_SUCCESS) {
            ESP_LOGI(TAG, "HF client ready");
            // The HF client has just set its own CoD (Hands-free); advertise as a headset
            bt_set_cod(BT_COD_WEARABLE_HEADSET);
            reconnect_timer_cb(NULL);
        } else {
            ESP_LOGI(TAG, "HF client profile state %d", param->prof_stat.state);
        }
        break;

    case ESP_HF_CLIENT_CONNECTION_STATE_EVT: {
        esp_hf_client_connection_state_t state = param->conn_stat.state;
        s_conn_state = state;
        ESP_LOGI(TAG, "Connection %s, peer %s", ENUM_NAME(CONN_STATE_NAMES, state),
                 bda_to_str(param->conn_stat.remote_bda, bda_str));
        if (state == ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED) {
            log_ag_features(param->conn_stat.peer_feat, param->conn_stat.chld_feat);
            memcpy(s_peer, param->conn_stat.remote_bda, ESP_BD_ADDR_LEN);
            s_have_peer = true;
            peer_save(s_peer);
            s_auto_reconnect = true;
            esp_timer_stop(s_reconnect_timer);
        } else if (state == ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED) {
            schedule_reconnect();
        }
        break;
    }

    case ESP_HF_CLIENT_AUDIO_STATE_EVT: {
        esp_hf_client_audio_state_t state = param->audio_stat.state;
        s_audio_state = state;
        ESP_LOGI(TAG, "Audio %s", ENUM_NAME(AUDIO_STATE_NAMES, state));
        if (state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED || state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC) {
            s_sync_hdl = param->audio_stat.sync_conn_handle;
            ESP_LOGI(TAG, "(e)SCO handle 0x%04x, preferred frame size %u bytes",
                     s_sync_hdl, param->audio_stat.preferred_frame_size);
            audio_start(state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC ? AUDIO_FMT_MSBC : AUDIO_FMT_PCM16);
        } else if (state == ESP_HF_CLIENT_AUDIO_STATE_DISCONNECTED) {
            audio_stop();
            s_sync_hdl = 0;
        }
        break;
    }

    case ESP_HF_CLIENT_PKT_STAT_NUMS_GET_EVT:
        ESP_LOGI(TAG, "(e)SCO packets: rx total %" PRIu32 " correct %" PRIu32 " err %" PRIu32 " none %" PRIu32
                 " lost %" PRIu32 " | tx total %" PRIu32 " discarded %" PRIu32,
                 param->pkt_nums.rx_total, param->pkt_nums.rx_correct, param->pkt_nums.rx_err,
                 param->pkt_nums.rx_none, param->pkt_nums.rx_lost, param->pkt_nums.tx_total,
                 param->pkt_nums.tx_discarded);
        break;

    case ESP_HF_CLIENT_VOLUME_CONTROL_EVT:
        ESP_LOGI(TAG, "AG set %s volume to %d",
                 param->volume_control.type == ESP_HF_VOLUME_CONTROL_TARGET_SPK ? "speaker" : "mic",
                 param->volume_control.volume);
        break;

    case ESP_HF_CLIENT_AT_RESPONSE_EVT:
        ESP_LOGI(TAG, "AT response code %d, CME error %d", param->at_response.code, param->at_response.cme);
        break;

    // Phone-style indicators. Logged in full because OBI may repurpose them.
    case ESP_HF_CLIENT_CIND_CALL_EVT:
        ESP_LOGI(TAG, "Indicator call=%d", param->call.status);
        break;
    case ESP_HF_CLIENT_CIND_CALL_SETUP_EVT:
        ESP_LOGI(TAG, "Indicator callsetup=%d", param->call_setup.status);
        break;
    case ESP_HF_CLIENT_CIND_CALL_HELD_EVT:
        ESP_LOGI(TAG, "Indicator callheld=%d", param->call_held.status);
        break;
    case ESP_HF_CLIENT_CIND_SERVICE_AVAILABILITY_EVT:
        ESP_LOGI(TAG, "Indicator service=%d", param->service_availability.status);
        break;
    case ESP_HF_CLIENT_CIND_SIGNAL_STRENGTH_EVT:
        ESP_LOGI(TAG, "Indicator signal=%d", param->signal_strength.value);
        break;
    case ESP_HF_CLIENT_CIND_ROAMING_STATUS_EVT:
        ESP_LOGI(TAG, "Indicator roam=%d", param->roaming.status);
        break;
    case ESP_HF_CLIENT_CIND_BATTERY_LEVEL_EVT:
        ESP_LOGI(TAG, "Indicator battchg=%d", param->battery_level.value);
        break;

    case ESP_HF_CLIENT_COPS_CURRENT_OPERATOR_EVT:
        ESP_LOGI(TAG, "Operator \"%s\"", STR_OR_EMPTY(param->cops.name));
        break;
    case ESP_HF_CLIENT_CLIP_EVT:
        ESP_LOGI(TAG, "Caller ID \"%s\"", STR_OR_EMPTY(param->clip.number));
        break;
    case ESP_HF_CLIENT_CCWA_EVT:
        ESP_LOGI(TAG, "Call waiting \"%s\"", STR_OR_EMPTY(param->ccwa.number));
        break;
    case ESP_HF_CLIENT_CLCC_EVT:
        ESP_LOGI(TAG, "Current call %d: dir %d status %d mpty %d number \"%s\"", param->clcc.idx,
                 param->clcc.dir, param->clcc.status, param->clcc.mpty, STR_OR_EMPTY(param->clcc.number));
        break;
    case ESP_HF_CLIENT_CNUM_EVT:
        ESP_LOGI(TAG, "Subscriber number \"%s\" type %d", STR_OR_EMPTY(param->cnum.number), param->cnum.type);
        break;
    case ESP_HF_CLIENT_BSIR_EVT:
        ESP_LOGI(TAG, "In-band ring tone %s", param->bsir.state ? "provided" : "not provided");
        break;
    case ESP_HF_CLIENT_BINP_EVT:
        ESP_LOGI(TAG, "Voice tag number \"%s\"", STR_OR_EMPTY(param->binp.number));
        break;
    case ESP_HF_CLIENT_RING_IND_EVT:
        ESP_LOGI(TAG, "RING");
        break;
    case ESP_HF_CLIENT_BVRA_EVT:
        ESP_LOGI(TAG, "Voice recognition %d", param->bvra.value);
        break;
    case ESP_HF_CLIENT_BTRH_EVT:
        ESP_LOGI(TAG, "Response and hold %d", param->btrh.status);
        break;

    default:
        ESP_LOGI(TAG, "HF event %d", event);
        break;
    }
}

void hf_init(void)
{
    char bda_str[BDA_STR_LEN];
    s_have_peer = peer_load(s_peer);
    if (s_have_peer) {
        ESP_LOGI(TAG, "Last peer %s", bda_to_str(s_peer, bda_str));
    }

    const esp_timer_create_args_t args = {
        .callback = reconnect_timer_cb,
        .name = "hf_reconnect",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_reconnect_timer));

    ESP_ERROR_CHECK(esp_hf_client_register_callback(hf_event_cb));
    ESP_ERROR_CHECK(esp_hf_client_init());
}

void hf_connect(const esp_bd_addr_t bda)
{
    if (bda == NULL && !s_have_peer) {
        printf("No stored peer; give an address or pair from the Cardo first\n");
        return;
    }
    s_auto_reconnect = true;
    esp_hf_client_connect(bda != NULL ? (uint8_t *)bda : s_peer);
}

void hf_disconnect(void)
{
    s_auto_reconnect = false;
    esp_timer_stop(s_reconnect_timer);
    if (s_have_peer) {
        esp_hf_client_disconnect(s_peer);
    }
}

void hf_forget_peer(void)
{
    hf_disconnect();
    s_have_peer = false;
    peer_erase();
}

void hf_audio_open(void)
{
    if (s_conn_state != ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED) {
        printf("Not connected\n");
        return;
    }
    esp_hf_client_connect_audio(s_peer);
}

void hf_audio_close(void)
{
    if (s_conn_state != ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED) {
        printf("Not connected\n");
        return;
    }
    esp_hf_client_disconnect_audio(s_peer);
}

void hf_request_pkt_stats(void)
{
    if (s_sync_hdl == 0) {
        printf("No audio connection\n");
        return;
    }
    esp_hf_client_pkt_stat_nums_get(s_sync_hdl);
}

void hf_print_status(void)
{
    char bda_str[BDA_STR_LEN];
    printf("HF connection:  %s\n", ENUM_NAME(CONN_STATE_NAMES, s_conn_state));
    printf("HF peer:        %s\n", s_have_peer ? bda_to_str(s_peer, bda_str) : "none");
    printf("HF reconnect:   %s\n", s_auto_reconnect ? "on" : "off");
    printf("HF audio:       %s\n", ENUM_NAME(AUDIO_STATE_NAMES, s_audio_state));
}
