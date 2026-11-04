/**
 * @file ag.c
 * @brief HFP Audio Gateway (phone) role: connect out to a headset, answer its AT commands,
 *        log the ones Bluedroid doesn't know, and open audio when asked.
 */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "esp_hf_ag_api.h"
#include "esp_log.h"
#include "audio.h"
#include "bt.h"
#include "ag.h"

static const char *TAG = "ag";

/** Name for @p value from a designated-initialiser table, or "unknown". */
#define ENUM_NAME(names, value) \
    ((size_t)(value) < sizeof(names) / sizeof((names)[0]) && (names)[value] ? (names)[value] : "unknown")
/** @p s, or "" if it is NULL (some event strings are optional). */
#define STR_OR_EMPTY(s) ((s) ? (s) : "")

#define SIGNAL_LEVEL  5  ///< Signal strength we report to the headset (0-5)
#define BATTERY_LEVEL 5  ///< Battery level we report to the headset (0-5)

/** (e)SCO parameters; layout copied from Bluedroid's tBTM_ESCO_PARAMS (btm_api.h). */
typedef struct {
    uint32_t tx_bw;
    uint32_t rx_bw;
    uint16_t max_latency;
    uint16_t voice_contfmt;
    uint16_t packet_types;
    uint8_t retrans_effort;
} bta_esco_params_t;
/**
 * @brief Bluedroid internal (bta_ag_sco.c), not exposed by ESP-IDF: override the (e)SCO
 *        parameters the AG requests, or go back to the defaults if @p set_reset is false.
 */
extern void bta_ag_set_esco_param(bool set_reset, bta_esco_params_t *param);

#define ESCO_BANDWIDTH_64K   8000    ///< 64 kbit/s, in bytes/s
#define VOICE_SETTING_CVSD   0x0060  ///< Linear 16-bit 2's complement input, CVSD air coding
#define VOICE_SETTING_TRANS  0x0063  ///< As above, transparent air coding (mSBC)

/**
 * Our answer to AT+OBISF (OBI supported features, sent by the Cardo right after the
 * standard SLC setup): "+OBISF: <value>" then OK, or ERROR if negative.
 */
static int s_obisf_reply = 3;

static esp_hf_connection_state_t s_conn_state = ESP_HF_CONNECTION_STATE_DISCONNECTED;
static esp_hf_audio_state_t s_audio_state = ESP_HF_AUDIO_STATE_DISCONNECTED;
static esp_hf_sync_conn_hdl_t s_sync_hdl;
static esp_bd_addr_t s_peer;
static bool s_have_peer;
static bool s_call_active;
static char s_no_number[] = "";  ///< Phone number for our pretend calls

static const char *const CONN_STATE_NAMES[] = {
    [ESP_HF_CONNECTION_STATE_DISCONNECTED] = "disconnected",
    [ESP_HF_CONNECTION_STATE_CONNECTING] = "connecting",
    [ESP_HF_CONNECTION_STATE_CONNECTED] = "RFCOMM connected",
    [ESP_HF_CONNECTION_STATE_SLC_CONNECTED] = "service level connected",
    [ESP_HF_CONNECTION_STATE_DISCONNECTING] = "disconnecting",
};

static const char *const AUDIO_STATE_NAMES[] = {
    [ESP_HF_AUDIO_STATE_DISCONNECTED] = "disconnected",
    [ESP_HF_AUDIO_STATE_CONNECTING] = "connecting",
    [ESP_HF_AUDIO_STATE_CONNECTED] = "connected (CVSD)",
    [ESP_HF_AUDIO_STATE_CONNECTED_MSBC] = "connected (mSBC)",
};

/** Hands-Free supported features (AT+BRSF from the headset), per the HFP spec, for logging. */
static const struct {
    uint32_t bit;
    const char *name;
} HF_FEATURES[] = {
    {0x001, "ecnr"},
    {0x002, "3way"},
    {0x004, "cli"},
    {0x008, "vrec"},
    {0x010, "rvol"},
    {0x020, "ecs"},
    {0x040, "ecc"},
    {0x080, "codec"},
    {0x100, "hf_ind"},
    {0x200, "esco_s4"},
};

/** @brief Log the headset's supported features. */
static void log_hf_features(uint32_t feat)
{
    char names[128] = "";
    size_t len = 0;
    for (size_t i = 0; i < sizeof(HF_FEATURES) / sizeof(HF_FEATURES[0]); i++) {
        if (feat & HF_FEATURES[i].bit) {
            len += snprintf(names + len, sizeof(names) - len, " %s", HF_FEATURES[i].name);
        }
    }
    ESP_LOGI(TAG, "HF features 0x%03" PRIx32 ":%s", feat, names);
}

/** @brief Send all our indicator values (+CIEV) to the headset. */
static void report_indicators(esp_bd_addr_t bda)
{
    esp_hf_ag_ciev_report(bda, ESP_HF_IND_TYPE_CALL, s_call_active);
    esp_hf_ag_ciev_report(bda, ESP_HF_IND_TYPE_CALLSETUP, ESP_HF_CALL_SETUP_STATUS_IDLE);
    esp_hf_ag_ciev_report(bda, ESP_HF_IND_TYPE_SERVICE, ESP_HF_NETWORK_STATE_AVAILABLE);
    esp_hf_ag_ciev_report(bda, ESP_HF_IND_TYPE_SIGNAL, SIGNAL_LEVEL);
    esp_hf_ag_ciev_report(bda, ESP_HF_IND_TYPE_BATTCHG, BATTERY_LEVEL);
}

/**
 * @brief AG events: track state, answer the headset's AT commands (including AT+OBISF),
 *        treat its answer/hang-up/dial as call control, and start/stop audio.
 */
static void ag_event_cb(esp_hf_cb_event_t event, esp_hf_cb_param_t *param)
{
    char bda_str[BDA_STR_LEN];

    switch (event) {
    case ESP_HF_PROF_STATE_EVT:
        ESP_LOGI(TAG, "AG profile state %d%s", param->prof_stat.state,
                 param->prof_stat.state == ESP_HF_INIT_SUCCESS ? " (ready)" : "");
        break;

    case ESP_HF_CONNECTION_STATE_EVT: {
        esp_hf_connection_state_t state = param->conn_stat.state;
        s_conn_state = state;
        ESP_LOGI(TAG, "Connection %s, peer %s", ENUM_NAME(CONN_STATE_NAMES, state),
                 bda_to_str(param->conn_stat.remote_bda, bda_str));
        if (state == ESP_HF_CONNECTION_STATE_SLC_CONNECTED) {
            log_hf_features(param->conn_stat.peer_feat);
            memcpy(s_peer, param->conn_stat.remote_bda, ESP_BD_ADDR_LEN);
            s_have_peer = true;
        } else if (state == ESP_HF_CONNECTION_STATE_DISCONNECTED) {
            s_call_active = false;
        }
        break;
    }

    case ESP_HF_AUDIO_STATE_EVT: {
        esp_hf_audio_state_t state = param->audio_stat.state;
        s_audio_state = state;
        ESP_LOGI(TAG, "Audio %s", ENUM_NAME(AUDIO_STATE_NAMES, state));
        if (state == ESP_HF_AUDIO_STATE_CONNECTED || state == ESP_HF_AUDIO_STATE_CONNECTED_MSBC) {
            s_sync_hdl = param->audio_stat.sync_conn_handle;
            ESP_LOGI(TAG, "(e)SCO handle 0x%04x, preferred frame size %u bytes",
                     s_sync_hdl, param->audio_stat.preferred_frame_size);
            audio_start(state == ESP_HF_AUDIO_STATE_CONNECTED_MSBC);
        } else if (state == ESP_HF_AUDIO_STATE_DISCONNECTED) {
            audio_stop();
            s_sync_hdl = 0;
        }
        break;
    }

    case ESP_HF_UNAT_RESPONSE_EVT: {
        // Vendor extensions, OBI's among them, arrive here
        const char *at = STR_OR_EMPTY(param->unat_rep.unat);
        if (strncmp(at, "+OBISF", 6) == 0 && s_obisf_reply >= 0) {
            char reply[24];
            snprintf(reply, sizeof(reply), "+OBISF: %d", s_obisf_reply);
            ESP_LOGI(TAG, "Unknown AT command from HF: \"%s\", replying \"%s\" and OK", at, reply);
            // Sends just the result line; OK follows separately
            esp_hf_ag_unknown_at_send(param->unat_rep.remote_addr, reply);
            esp_hf_ag_cmee_send(param->unat_rep.remote_addr, ESP_HF_AT_RESPONSE_CODE_OK, 0);
        } else {
            // esp_hf_ag_unknown_at_send() rejects NULL despite documenting it as ERROR,
            // so send ERROR this way
            ESP_LOGI(TAG, "Unknown AT command from HF: \"%s\", replying ERROR", at);
            esp_hf_ag_cmee_send(param->unat_rep.remote_addr, ESP_HF_AT_RESPONSE_CODE_ERR, 0);
        }
        break;
    }

    case ESP_HF_CIND_RESPONSE_EVT:
        esp_hf_ag_cind_response(param->cind_rep.remote_addr, s_call_active, ESP_HF_CALL_SETUP_STATUS_IDLE,
                                ESP_HF_NETWORK_STATE_AVAILABLE, SIGNAL_LEVEL, ESP_HF_ROAMING_STATUS_INACTIVE,
                                BATTERY_LEVEL, ESP_HF_CALL_HELD_STATUS_NONE);
        break;

    case ESP_HF_IND_UPDATE_EVT:
        report_indicators(param->ind_upd.remote_addr);
        break;

    case ESP_HF_COPS_RESPONSE_EVT: {
        static char operator_name[] = "OpenIntercom";
        esp_hf_ag_cops_response(param->cops_rep.remote_addr, operator_name);
        break;
    }

    case ESP_HF_CLCC_RESPONSE_EVT:
        if (s_call_active) {
            esp_hf_ag_clcc_response(param->clcc_rep.remote_addr, 1, ESP_HF_CURRENT_CALL_DIRECTION_OUTGOING,
                                    ESP_HF_CURRENT_CALL_STATUS_ACTIVE, ESP_HF_CURRENT_CALL_MODE_VOICE,
                                    ESP_HF_CURRENT_CALL_MPTY_TYPE_SINGLE, s_no_number, ESP_HF_CALL_ADDR_TYPE_UNKNOWN);
        }
        // Index 0 ends the list with OK
        esp_hf_ag_clcc_response(param->clcc_rep.remote_addr, 0, ESP_HF_CURRENT_CALL_DIRECTION_OUTGOING,
                                ESP_HF_CURRENT_CALL_STATUS_ACTIVE, ESP_HF_CURRENT_CALL_MODE_VOICE,
                                ESP_HF_CURRENT_CALL_MPTY_TYPE_SINGLE, s_no_number, ESP_HF_CALL_ADDR_TYPE_UNKNOWN);
        break;

    case ESP_HF_CNUM_RESPONSE_EVT:
        esp_hf_ag_cmee_send(param->cnum_rep.remote_addr, ESP_HF_AT_RESPONSE_CODE_OK, 0);
        break;

    // The headset's buttons: answer, hang up, dial. Treat them as starting or ending a call.
    case ESP_HF_ATA_RESPONSE_EVT:
        ESP_LOGI(TAG, "HF answered (ATA)");
        s_call_active = true;
        esp_hf_ag_answer_call(param->ata_rep.remote_addr, 1, 0, ESP_HF_CALL_STATUS_CALL_IN_PROGRESS,
                              ESP_HF_CALL_SETUP_STATUS_IDLE, s_no_number, ESP_HF_CALL_ADDR_TYPE_UNKNOWN);
        break;

    case ESP_HF_CHUP_RESPONSE_EVT:
        ESP_LOGI(TAG, "HF hung up (AT+CHUP)");
        s_call_active = false;
        esp_hf_ag_end_call(param->chup_rep.remote_addr, 0, 0, ESP_HF_CALL_STATUS_NO_CALLS,
                           ESP_HF_CALL_SETUP_STATUS_IDLE, s_no_number, ESP_HF_CALL_ADDR_TYPE_UNKNOWN);
        break;

    case ESP_HF_DIAL_EVT:
        ESP_LOGI(TAG, "HF dialled (type %d, \"%s\"), starting a call", param->out_call.type,
                 STR_OR_EMPTY(param->out_call.num_or_loc));
        s_call_active = true;
        esp_hf_ag_cmee_send(param->out_call.remote_addr, ESP_HF_AT_RESPONSE_CODE_OK, 0);
        esp_hf_ag_out_call(param->out_call.remote_addr, 1, 0, ESP_HF_CALL_STATUS_CALL_IN_PROGRESS,
                           ESP_HF_CALL_SETUP_STATUS_IDLE, s_no_number, ESP_HF_CALL_ADDR_TYPE_UNKNOWN);
        break;

    case ESP_HF_VOLUME_CONTROL_EVT:
        ESP_LOGI(TAG, "HF reports %s volume %d",
                 param->volume_control.type == ESP_HF_VOLUME_TYPE_SPK ? "speaker" : "mic",
                 param->volume_control.volume);
        break;

    case ESP_HF_BVRA_RESPONSE_EVT:
        ESP_LOGI(TAG, "HF voice recognition %d", param->vra_rep.value);
        break;

    case ESP_HF_NREC_RESPONSE_EVT:
        ESP_LOGI(TAG, "HF noise reduction/echo cancellation %s", param->nrec.state ? "on" : "off");
        break;

    case ESP_HF_VTS_RESPONSE_EVT:
        ESP_LOGI(TAG, "HF DTMF \"%s\"", STR_OR_EMPTY(param->vts_rep.code));
        break;

    case ESP_HF_WBS_RESPONSE_EVT:
        ESP_LOGI(TAG, "HF wideband speech support %d", param->wbs_rep.codec);
        break;

    case ESP_HF_BCS_RESPONSE_EVT:
        ESP_LOGI(TAG, "Codec negotiated: %d", param->bcs_rep.mode);
        break;

    case ESP_HF_PKT_STAT_NUMS_GET_EVT:
        ESP_LOGI(TAG, "(e)SCO packets: rx total %" PRIu32 " correct %" PRIu32 " err %" PRIu32 " none %" PRIu32
                 " lost %" PRIu32 " | tx total %" PRIu32 " discarded %" PRIu32,
                 param->pkt_nums.rx_total, param->pkt_nums.rx_correct, param->pkt_nums.rx_err,
                 param->pkt_nums.rx_none, param->pkt_nums.rx_lost, param->pkt_nums.tx_total,
                 param->pkt_nums.tx_discarded);
        break;

    default:
        ESP_LOGI(TAG, "AG event %d", event);
        break;
    }
}

void ag_init(void)
{
    ESP_ERROR_CHECK(esp_hf_ag_register_callback(ag_event_cb));
    ESP_ERROR_CHECK(esp_hf_ag_init());
}

void ag_connect(const esp_bd_addr_t bda)
{
    memcpy(s_peer, bda, ESP_BD_ADDR_LEN);
    s_have_peer = true;
    esp_hf_ag_slc_connect(s_peer);
}

void ag_disconnect(void)
{
    if (!s_have_peer) {
        printf("No AG peer\n");
        return;
    }
    esp_hf_ag_slc_disconnect(s_peer);
}

void ag_audio_open(void)
{
    if (s_conn_state != ESP_HF_CONNECTION_STATE_SLC_CONNECTED) {
        printf("AG not connected\n");
        return;
    }
    esp_hf_ag_audio_connect(s_peer);
}

void ag_audio_close(void)
{
    if (s_conn_state != ESP_HF_CONNECTION_STATE_SLC_CONNECTED) {
        printf("AG not connected\n");
        return;
    }
    esp_hf_ag_audio_disconnect(s_peer);
}

void ag_call(bool active)
{
    if (s_conn_state != ESP_HF_CONNECTION_STATE_SLC_CONNECTED) {
        printf("AG not connected\n");
        return;
    }
    s_call_active = active;
    if (active) {
        esp_hf_ag_out_call(s_peer, 1, 0, ESP_HF_CALL_STATUS_CALL_IN_PROGRESS, ESP_HF_CALL_SETUP_STATUS_IDLE,
                           s_no_number, ESP_HF_CALL_ADDR_TYPE_UNKNOWN);
    } else {
        esp_hf_ag_end_call(s_peer, 0, 0, ESP_HF_CALL_STATUS_NO_CALLS, ESP_HF_CALL_SETUP_STATUS_IDLE,
                           s_no_number, ESP_HF_CALL_ADDR_TYPE_UNKNOWN);
    }
}

void ag_set_esco_params(uint16_t packet_types, uint16_t max_latency, uint8_t retrans_effort, bool msbc)
{
    bta_esco_params_t params = {
        .tx_bw = ESCO_BANDWIDTH_64K,
        .rx_bw = ESCO_BANDWIDTH_64K,
        .max_latency = max_latency,
        .voice_contfmt = msbc ? VOICE_SETTING_TRANS : VOICE_SETTING_CVSD,
        .packet_types = packet_types,
        .retrans_effort = retrans_effort,
    };
    bta_ag_set_esco_param(true, &params);
    printf("(e)SCO: %s, packet types 0x%04x, max latency %u ms, retransmission %u\n",
           msbc ? "mSBC" : "CVSD", packet_types, max_latency, retrans_effort);
}

void ag_reset_esco_params(void)
{
    bta_ag_set_esco_param(false, NULL);
    printf("(e)SCO: Bluedroid defaults\n");
}

void ag_set_obisf_reply(int value)
{
    s_obisf_reply = value;
}

void ag_request_pkt_stats(void)
{
    if (s_sync_hdl == 0) {
        printf("No AG audio connection\n");
        return;
    }
    esp_hf_ag_pkt_stat_nums_get(s_sync_hdl);
}

void ag_print_status(void)
{
    char bda_str[BDA_STR_LEN];
    printf("AG connection:  %s\n", ENUM_NAME(CONN_STATE_NAMES, s_conn_state));
    printf("AG peer:        %s\n", s_have_peer ? bda_to_str(s_peer, bda_str) : "none");
    printf("AG audio:       %s\n", ENUM_NAME(AUDIO_STATE_NAMES, s_audio_state));
    printf("AG call:        %s\n", s_call_active ? "active" : "none");
    if (s_obisf_reply >= 0) {
        printf("AG OBISF reply: +OBISF: %d\n", s_obisf_reply);
    } else {
        printf("AG OBISF reply: ERROR\n");
    }
}
