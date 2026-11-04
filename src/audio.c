/**
 * @file audio.c
 * @brief Audio frames for whichever HFP role holds the (e)SCO link: stats, test tone, loopback.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "esp_hf_ag_api.h"
#include "esp_hf_client_api.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "audio.h"

#if CONFIG_BT_HCI_LOG_DEBUG_EN
/** @brief ESP-IDF internal (bt_hci_log.c): pause or resume HCI log recording. */
extern void bt_hci_log_record_hci_enable(bool enable);
#endif

static const char *TAG = "audio";

/** A role's audio buffer and send functions; each HFP role has its own, and one carries audio at a time. */
typedef struct {
    esp_hf_audio_buff_t *(*alloc)(uint16_t size);
    void (*free)(esp_hf_audio_buff_t *buf);
    esp_err_t (*send)(esp_hf_sync_conn_hdl_t sync_hdl, esp_hf_audio_buff_t *buf);
} audio_io_t;

/** Hands-Free role audio functions. */
static const audio_io_t HF_IO = {
    esp_hf_client_audio_buff_alloc, esp_hf_client_audio_buff_free, esp_hf_client_audio_data_send,
};
/** Audio Gateway role audio functions. */
static const audio_io_t AG_IO = {
    esp_hf_ag_audio_buff_alloc, esp_hf_ag_audio_buff_free, esp_hf_ag_audio_data_send,
};

#define CVSD_SAMPLE_RATE 8000                          ///< CVSD links carry 8 kHz PCM
#define TONE_HZ          1000                          ///< Test tone frequency
#define TONE_AMPLITUDE   8192                          ///< Test tone level, -12 dBFS
#define TONE_PERIOD      (CVSD_SAMPLE_RATE / TONE_HZ)  ///< Samples per tone cycle
#define STATS_PERIOD_US  (1000 * 1000)                 ///< How often the stats are logged

/** Frame counts and levels, accumulated between stats logs. */
typedef struct {
    uint32_t rx_frames;
    uint32_t rx_bad;
    uint32_t rx_bytes;
    uint16_t rx_len_min;
    uint16_t rx_len_max;
    uint32_t rx_samples;
    uint64_t rx_sum_sq;
    int32_t rx_peak;
    uint32_t tx_frames;
    uint32_t tx_failed;
    uint32_t tx_skipped;
} audio_stats_t;

static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static audio_stats_t s_stats;
static esp_timer_handle_t s_stats_timer;
static bool s_stats_logging = true;

static volatile bool s_active;
static volatile bool s_msbc;
static volatile audio_tx_mode_t s_tx_mode = AUDIO_TX_SILENCE;

static int16_t s_tone[TONE_PERIOD];  ///< One cycle of the test tone
static uint16_t s_tone_pos;

/**
 * @brief Read sample @p i from 16-bit little-endian PCM.
 *
 * Audio buffers come straight from HCI packets and may not be 2-byte aligned,
 * so samples are read and written a byte at a time.
 */
static inline int16_t pcm_read(const uint8_t *p, size_t i)
{
    return (int16_t)(p[2 * i] | (p[2 * i + 1] << 8));
}

/** @brief Write sample @p i to 16-bit little-endian PCM (see pcm_read()). */
static inline void pcm_write(uint8_t *p, size_t i, int16_t s)
{
    p[2 * i] = (uint8_t)s;
    p[2 * i + 1] = (uint8_t)((uint16_t)s >> 8);
}

/** @brief Fill @p data with the next @p samples samples of the test tone. */
static void fill_tone(uint8_t *data, size_t samples)
{
    for (size_t i = 0; i < samples; i++) {
        pcm_write(data, i, s_tone[s_tone_pos]);
        s_tone_pos = (s_tone_pos + 1) % TONE_PERIOD;
    }
}

/** @brief Add a received frame to the stats (levels only for CVSD). */
static void measure_rx(const esp_hf_audio_buff_t *buf, bool is_bad_frame)
{
    size_t samples = buf->data_len / 2;
    uint64_t sum_sq = 0;
    int32_t peak = 0;
    if (!is_bad_frame && !s_msbc) {
        for (size_t i = 0; i < samples; i++) {
            int32_t s = pcm_read(buf->data, i);
            sum_sq += (uint64_t)(s * s);
            if (abs(s) > peak) {
                peak = abs(s);
            }
        }
    }

    portENTER_CRITICAL(&s_stats_lock);
    s_stats.rx_frames++;
    s_stats.rx_bytes += buf->data_len;
    if (s_stats.rx_len_min == 0 || buf->data_len < s_stats.rx_len_min) {
        s_stats.rx_len_min = buf->data_len;
    }
    if (buf->data_len > s_stats.rx_len_max) {
        s_stats.rx_len_max = buf->data_len;
    }
    if (is_bad_frame) {
        s_stats.rx_bad++;
    } else {
        s_stats.rx_samples += samples;
        s_stats.rx_sum_sq += sum_sq;
        if (peak > s_stats.rx_peak) {
            s_stats.rx_peak = peak;
        }
    }
    portEXIT_CRITICAL(&s_stats_lock);
}

/** Outcome of replying to a frame. */
typedef enum { TX_SENT, TX_FAILED, TX_SKIPPED } tx_result_t;

/** @brief Add a reply's outcome to the stats. */
static void count_tx(tx_result_t result)
{
    portENTER_CRITICAL(&s_stats_lock);
    if (result == TX_SENT) {
        s_stats.tx_frames++;
    } else if (result == TX_FAILED) {
        s_stats.tx_failed++;
    } else {
        s_stats.tx_skipped++;
    }
    portEXIT_CRITICAL(&s_stats_lock);
}

/** @brief Send the reply to a received frame. Takes ownership of @p rx: sends it back (loopback) or frees it. */
static void send_reply(const audio_io_t *io, esp_hf_sync_conn_hdl_t sync_hdl, esp_hf_audio_buff_t *rx,
                       bool is_bad_frame)
{
    esp_hf_audio_buff_t *tx;
    uint16_t len = rx->data_len;

    if (s_tx_mode == AUDIO_TX_LOOPBACK && !is_bad_frame) {
        tx = rx;
        // Received mSBC frames can carry padding after the 57-byte frame, which the
        // send API doesn't accept (as in the IDF examples)
        if (s_msbc && tx->data_len > ESP_HF_MSBC_ENCODED_FRAME_SIZE) {
            tx->data_len = ESP_HF_MSBC_ENCODED_FRAME_SIZE;
        }
    } else if (s_msbc) {
        // Silence and tone would need an mSBC encoder
        io->free(rx);
        count_tx(TX_SKIPPED);
        return;
    } else {
        io->free(rx);
        tx = io->alloc(len);
        if (tx == NULL) {
            count_tx(TX_FAILED);
            return;
        }
        tx->data_len = len;
        if (s_tx_mode == AUDIO_TX_TONE) {
            fill_tone(tx->data, len / 2);
        } else {
            memset(tx->data, 0, len);
        }
    }

    if (io->send(sync_hdl, tx) == ESP_OK) {
        count_tx(TX_SENT);
    } else {
        io->free(tx);
        count_tx(TX_FAILED);
    }
}

/** @brief Handle a received (e)SCO frame. Runs in the Bluetooth stack's task, so keep it short. */
static void handle_frame(const audio_io_t *io, esp_hf_sync_conn_hdl_t sync_hdl, esp_hf_audio_buff_t *buf,
                         bool is_bad_frame)
{
    if (!s_active || buf->data_len == 0) {
        io->free(buf);
        return;
    }
    measure_rx(buf, is_bad_frame);
    send_reply(io, sync_hdl, buf, is_bad_frame);
}

/** @brief Audio data callback for the Hands-Free role. */
static void hf_audio_data_cb(esp_hf_sync_conn_hdl_t sync_hdl, esp_hf_audio_buff_t *buf, bool is_bad_frame)
{
    handle_frame(&HF_IO, sync_hdl, buf, is_bad_frame);
}

/** @brief Audio data callback for the Audio Gateway role. */
static void ag_audio_data_cb(esp_hf_sync_conn_hdl_t sync_hdl, esp_hf_audio_buff_t *buf, bool is_bad_frame)
{
    handle_frame(&AG_IO, sync_hdl, buf, is_bad_frame);
}

/**
 * @brief Pause or resume HCI log recording. It pauses while audio is connected, so audio
 *        packets don't push the signalling we want to inspect out of the buffer.
 */
static void hci_log_record(bool enable)
{
#if CONFIG_BT_HCI_LOG_DEBUG_EN
    bt_hci_log_record_hci_enable(enable);
#endif
}

/** @brief Format a linear 16-bit level as dBFS with one decimal place, without float printf. */
static char *fmt_dbfs(float level, char *out, size_t len)
{
    if (level < 1.0f) {
        snprintf(out, len, "-inf");
    } else {
        int tenths = (int)lroundf(200.0f * log10f(level / 32768.0f));
        snprintf(out, len, "%s%d.%d", tenths < 0 ? "-" : "", abs(tenths) / 10, abs(tenths) % 10);
    }
    return out;
}

/** @brief Log and reset the stats once a second. */
static void stats_timer_cb(void *arg)
{
    audio_stats_t st;
    portENTER_CRITICAL(&s_stats_lock);
    st = s_stats;
    memset(&s_stats, 0, sizeof(s_stats));
    portEXIT_CRITICAL(&s_stats_lock);

    if (!s_stats_logging) {
        return;
    }

    char levels[48] = "levels n/a (mSBC)";
    if (!s_msbc) {
        char rms[12], peak[12];
        float rms_level = st.rx_samples ? sqrtf((float)st.rx_sum_sq / st.rx_samples) : 0.0f;
        snprintf(levels, sizeof(levels), "rms %s peak %s dBFS", fmt_dbfs(rms_level, rms, sizeof(rms)),
                 fmt_dbfs((float)st.rx_peak, peak, sizeof(peak)));
    }
    ESP_LOGI(TAG, "rx %" PRIu32 " frames/s (%u-%u B) %" PRIu32 " B/s %" PRIu32 " bad, %s | tx %" PRIu32
             " frames/s %" PRIu32 " failed %" PRIu32 " skipped",
             st.rx_frames, st.rx_len_min, st.rx_len_max, st.rx_bytes, st.rx_bad, levels,
             st.tx_frames, st.tx_failed, st.tx_skipped);
}

void audio_init(void)
{
    const esp_timer_create_args_t args = {
        .callback = stats_timer_cb,
        .name = "audio_stats",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_stats_timer));

    for (int i = 0; i < TONE_PERIOD; i++) {
        s_tone[i] = (int16_t)lroundf(TONE_AMPLITUDE * sinf(2.0f * (float)M_PI * i / TONE_PERIOD));
    }

    ESP_ERROR_CHECK(esp_hf_client_register_audio_data_callback(hf_audio_data_cb));
    ESP_ERROR_CHECK(esp_hf_ag_register_audio_data_callback(ag_audio_data_cb));
}

void audio_start(bool msbc)
{
    s_msbc = msbc;
    s_tone_pos = 0;

    portENTER_CRITICAL(&s_stats_lock);
    memset(&s_stats, 0, sizeof(s_stats));
    portEXIT_CRITICAL(&s_stats_lock);

    hci_log_record(false);
    s_active = true;
    esp_timer_start_periodic(s_stats_timer, STATS_PERIOD_US);
    ESP_LOGI(TAG, "Started (%s), sending %s", msbc ? "mSBC, 16 kHz encoded" : "CVSD, 8 kHz PCM",
             audio_tx_mode_name(s_tx_mode));
    if (msbc && s_tx_mode != AUDIO_TX_LOOPBACK) {
        ESP_LOGW(TAG, "Only loopback can be sent on mSBC until we have an encoder; sending nothing");
    }
}

void audio_stop(void)
{
    s_active = false;
    esp_timer_stop(s_stats_timer);
    hci_log_record(true);
}

void audio_set_tx_mode(audio_tx_mode_t mode)
{
    s_tx_mode = mode;
}

audio_tx_mode_t audio_get_tx_mode(void)
{
    return s_tx_mode;
}

const char *audio_tx_mode_name(audio_tx_mode_t mode)
{
    switch (mode) {
    case AUDIO_TX_SILENCE:  return "silence";
    case AUDIO_TX_TONE:     return "tone";
    case AUDIO_TX_LOOPBACK: return "loopback";
    }
    return "unknown";
}

void audio_set_stats_logging(bool enabled)
{
    s_stats_logging = enabled;
}

bool audio_get_stats_logging(void)
{
    return s_stats_logging;
}
