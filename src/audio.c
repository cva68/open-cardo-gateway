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
    uint32_t rx_gap_max_us;  ///< Longest time between received frames
    uint32_t tx_frames;
    uint32_t tx_failed;
    uint32_t tx_skipped;
} audio_stats_t;

static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static audio_stats_t s_stats;
static esp_timer_handle_t s_stats_timer;
static bool s_stats_logging = true;

static volatile bool s_active;
static int64_t s_last_rx_us;  ///< When the previous frame arrived (0 at the start of a link)
static volatile audio_format_t s_format;
static volatile audio_tx_mode_t s_tx_mode = AUDIO_TX_SILENCE;
static volatile uint8_t s_tx_lead = 2;  ///< Frames sent ahead at the start of a link (see handle_frame())
static uint8_t s_lead_pending;          ///< Lead frames still to send on this link

static int16_t s_tone[TONE_PERIOD];  ///< One cycle of the test tone
static uint16_t s_tone_pos;

/** @brief G.711 u-law byte to 16-bit linear. */
static int16_t ulaw_decode(uint8_t u)
{
    u = ~u;
    int t = (((u & 0x0f) << 3) + 0x84) << ((u & 0x70) >> 4);
    return (int16_t)((u & 0x80) ? 0x84 - t : t - 0x84);
}

/** @brief 16-bit linear to G.711 u-law byte. */
static uint8_t ulaw_encode(int16_t s)
{
    int sign = s < 0 ? 0x80 : 0;
    int mag = sign ? -(int)s : s;
    if (mag > 32635) {
        mag = 32635;
    }
    mag += 0x84;
    int exponent = 7;
    for (int mask = 0x4000; (mag & mask) == 0 && exponent > 0; mask >>= 1) {
        exponent--;
    }
    return (uint8_t)~(sign | (exponent << 4) | ((mag >> (exponent + 3)) & 0x0f));
}

/** @brief Bytes per sample in the current format, or 0 if its frames aren't plain samples. */
static size_t sample_size(void)
{
    switch (s_format) {
    case AUDIO_FMT_PCM16: return 2;
    case AUDIO_FMT_PCM8:
    case AUDIO_FMT_ULAW:  return 1;
    default:              return 0;
    }
}

/**
 * @brief Read sample @p i of a frame in the current format, as 16-bit linear.
 *
 * Audio buffers come straight from HCI packets and may not be 2-byte aligned,
 * so samples are read and written a byte at a time.
 */
static int16_t sample_read(const uint8_t *p, size_t i)
{
    switch (s_format) {
    case AUDIO_FMT_PCM16: return (int16_t)(p[2 * i] | (p[2 * i + 1] << 8));
    case AUDIO_FMT_PCM8:  return (int16_t)((int8_t)p[i] * 256);
    case AUDIO_FMT_ULAW:  return ulaw_decode(p[i]);
    default:              return 0;
    }
}

/** @brief Write sample @p i of a frame in the current format (see sample_read()). */
static void sample_write(uint8_t *p, size_t i, int16_t s)
{
    switch (s_format) {
    case AUDIO_FMT_PCM16:
        p[2 * i] = (uint8_t)s;
        p[2 * i + 1] = (uint8_t)((uint16_t)s >> 8);
        break;
    case AUDIO_FMT_PCM8: p[i] = (uint8_t)(s >> 8); break;
    case AUDIO_FMT_ULAW: p[i] = ulaw_encode(s); break;
    default: break;
    }
}

/** @brief Fill @p data with the next @p samples samples of the test tone, or with silence. */
static void fill_samples(uint8_t *data, size_t samples, bool tone)
{
    for (size_t i = 0; i < samples; i++) {
        sample_write(data, i, tone ? s_tone[s_tone_pos] : 0);
        if (tone) {
            s_tone_pos = (s_tone_pos + 1) % TONE_PERIOD;
        }
    }
}

/** @brief Add a received frame to the stats (levels only for sample formats). */
static void measure_rx(const esp_hf_audio_buff_t *buf, bool is_bad_frame)
{
    size_t samples = sample_size() ? buf->data_len / sample_size() : 0;
    uint64_t sum_sq = 0;
    int32_t peak = 0;
    if (!is_bad_frame) {
        for (size_t i = 0; i < samples; i++) {
            int32_t s = sample_read(buf->data, i);
            sum_sq += (uint64_t)(s * s);
            if (abs(s) > peak) {
                peak = abs(s);
            }
        }
    }

    int64_t now = esp_timer_get_time();
    uint32_t gap = s_last_rx_us ? (uint32_t)(now - s_last_rx_us) : 0;
    s_last_rx_us = now;

    portENTER_CRITICAL(&s_stats_lock);
    s_stats.rx_frames++;
    if (gap > s_stats.rx_gap_max_us) {
        s_stats.rx_gap_max_us = gap;
    }
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

/** @brief Send @p tx, taking ownership of it. */
static void send_frame(const audio_io_t *io, esp_hf_sync_conn_hdl_t sync_hdl, esp_hf_audio_buff_t *tx)
{
    if (io->send(sync_hdl, tx) == ESP_OK) {
        count_tx(TX_SENT);
    } else {
        io->free(tx);
        count_tx(TX_FAILED);
    }
}

/**
 * @brief Send a frame of @p len bytes of the test tone in tone mode, or of silence (also in
 *        loopback, for a bad or lead frame). Sample formats only (u-law silence is 0xff, not 0).
 */
static void send_generated(const audio_io_t *io, esp_hf_sync_conn_hdl_t sync_hdl, uint16_t len)
{
    esp_hf_audio_buff_t *tx = io->alloc(len);
    if (tx == NULL) {
        count_tx(TX_FAILED);
        return;
    }
    tx->data_len = len;
    fill_samples(tx->data, len / sample_size(), s_tx_mode == AUDIO_TX_TONE);
    send_frame(io, sync_hdl, tx);
}

/** @brief Send the reply to a received frame. Takes ownership of @p rx: sends it back (loopback) or frees it. */
static void send_reply(const audio_io_t *io, esp_hf_sync_conn_hdl_t sync_hdl, esp_hf_audio_buff_t *rx,
                       bool is_bad_frame)
{
    uint16_t len = rx->data_len;

    if (s_tx_mode == AUDIO_TX_LOOPBACK && !is_bad_frame) {
        // Received mSBC frames can carry padding after the 57-byte frame, which the
        // send API doesn't accept (as in the IDF examples)
        if (s_format == AUDIO_FMT_MSBC && rx->data_len > ESP_HF_MSBC_ENCODED_FRAME_SIZE) {
            rx->data_len = ESP_HF_MSBC_ENCODED_FRAME_SIZE;
        }
        send_frame(io, sync_hdl, rx);
    } else if (sample_size() == 0 || s_tx_mode == AUDIO_TX_OFF) {
        // Silence and tone on mSBC would need an encoder
        io->free(rx);
        count_tx(TX_SKIPPED);
    } else {
        io->free(rx);
        send_generated(io, sync_hdl, len);
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
    // Each received frame clocks out one reply, which goes out in a later (e)SCO slot. Without
    // frames queued ahead, a reply that is late for its slot leaves the controller nothing to
    // send, so the first received frame also sends a few frames of lead
    if (sample_size() != 0 && s_tx_mode != AUDIO_TX_OFF) {
        for (; s_lead_pending > 0; s_lead_pending--) {
            send_generated(io, sync_hdl, buf->data_len);
        }
    }
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

    char levels[48] = "levels n/a";
    if (sample_size() != 0) {
        char rms[12], peak[12];
        float rms_level = st.rx_samples ? sqrtf((float)st.rx_sum_sq / st.rx_samples) : 0.0f;
        snprintf(levels, sizeof(levels), "rms %s peak %s dBFS", fmt_dbfs(rms_level, rms, sizeof(rms)),
                 fmt_dbfs((float)st.rx_peak, peak, sizeof(peak)));
    }
    ESP_LOGI(TAG, "rx %" PRIu32 " frames/s (%u-%u B) %" PRIu32 " B/s %" PRIu32 " bad, gap max %" PRIu32
             " ms, %s | tx %" PRIu32 " frames/s %" PRIu32 " failed %" PRIu32 " skipped",
             st.rx_frames, st.rx_len_min, st.rx_len_max, st.rx_bytes, st.rx_bad, st.rx_gap_max_us / 1000, levels,
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

void audio_start(audio_format_t format)
{
    s_format = format;
    s_tone_pos = 0;
    s_lead_pending = s_tx_lead;
    s_last_rx_us = 0;

    portENTER_CRITICAL(&s_stats_lock);
    memset(&s_stats, 0, sizeof(s_stats));
    portEXIT_CRITICAL(&s_stats_lock);

    hci_log_record(false);
    s_active = true;
    esp_timer_start_periodic(s_stats_timer, STATS_PERIOD_US);
    ESP_LOGI(TAG, "Started (%s), sending %s", audio_format_name(format), audio_tx_mode_name(s_tx_mode));
    if (sample_size() == 0 && s_tx_mode != AUDIO_TX_LOOPBACK && s_tx_mode != AUDIO_TX_OFF) {
        ESP_LOGW(TAG, "Only loopback can be sent in this format; sending nothing");
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

void audio_set_tx_lead(uint8_t frames)
{
    s_tx_lead = frames;
}

uint8_t audio_get_tx_lead(void)
{
    return s_tx_lead;
}

const char *audio_tx_mode_name(audio_tx_mode_t mode)
{
    switch (mode) {
    case AUDIO_TX_SILENCE:  return "silence";
    case AUDIO_TX_TONE:     return "tone";
    case AUDIO_TX_LOOPBACK: return "loopback";
    case AUDIO_TX_OFF:      return "nothing";
    }
    return "unknown";
}

const char *audio_format_name(audio_format_t format)
{
    switch (format) {
    case AUDIO_FMT_PCM16: return "CVSD, 8 kHz 16-bit PCM";
    case AUDIO_FMT_PCM8:  return "CVSD, 8 kHz 8-bit PCM";
    case AUDIO_FMT_ULAW:  return "CVSD, 8 kHz u-law";
    case AUDIO_FMT_MSBC:  return "mSBC, 16 kHz encoded";
    case AUDIO_FMT_OTHER: return "other voice setting, loopback only";
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
