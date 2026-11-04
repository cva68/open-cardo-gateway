/**
 * @file audio.h
 * @brief Audio frames for whichever HFP role holds the (e)SCO link: stats, test tone, loopback.
 *
 * Each received frame clocks out one frame in return. With CVSD, frames are 16-bit mono
 * PCM at 8 kHz. With mSBC they stay encoded (16 kHz wideband): levels aren't measured,
 * and only loopback can be sent until an mSBC encoder is added.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/** What we send back for each received frame. */
typedef enum {
    AUDIO_TX_SILENCE,   ///< Zeros (CVSD only)
    AUDIO_TX_TONE,      ///< 1 kHz sine at -12 dBFS (CVSD only)
    AUDIO_TX_LOOPBACK,  ///< The received frame, sent straight back
} audio_tx_mode_t;

/** @brief Register the audio data callbacks; call once, after hf_init() and ag_init(). */
void audio_init(void);

/**
 * @brief Start handling frames when an (e)SCO link connects. Pauses HCI log recording.
 * @param msbc Whether the link carries mSBC (wideband) rather than CVSD.
 */
void audio_start(bool msbc);

/** @brief Stop handling frames when the (e)SCO link disconnects. Resumes HCI log recording. */
void audio_stop(void);

/** @brief Choose what to send back. */
void audio_set_tx_mode(audio_tx_mode_t mode);

/** @brief What we currently send back. */
audio_tx_mode_t audio_get_tx_mode(void);

/** @brief Short name for @p mode, for logs and the console. */
const char *audio_tx_mode_name(audio_tx_mode_t mode);

/** @brief Turn the per-second frame and level log on or off. */
void audio_set_stats_logging(bool enabled);

/** @brief Whether the per-second log is on. */
bool audio_get_stats_logging(void);
