/**
 * @file audio.h
 * @brief Audio frames for whichever HFP role holds the (e)SCO link: stats, test tone, loopback.
 *
 * Each received frame clocks out one frame in return. With CVSD, frames are mono samples at
 * 8 kHz: 16-bit linear PCM by default, or 8-bit u-law (or linear) when the AG asks for it in
 * the voice setting. With mSBC they stay encoded (16 kHz wideband): levels aren't measured,
 * and only loopback can be sent until an mSBC encoder is added.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/** How the frames of the current (e)SCO link are encoded. */
typedef enum {
    AUDIO_FMT_PCM16,  ///< CVSD link, 16-bit little-endian linear PCM (the default voice setting)
    AUDIO_FMT_PCM8,   ///< CVSD link, 8-bit linear PCM (voice setting 0x0040)
    AUDIO_FMT_ULAW,   ///< CVSD link, 8-bit G.711 u-law (voice setting 0x0100)
    AUDIO_FMT_MSBC,   ///< mSBC link, frames kept encoded
    AUDIO_FMT_OTHER,  ///< Any other voice setting (e.g. A-law): loopback only, no levels
} audio_format_t;

/** What we send back for each received frame. */
typedef enum {
    AUDIO_TX_SILENCE,   ///< Silence (CVSD only)
    AUDIO_TX_TONE,      ///< 1 kHz sine at -12 dBFS (CVSD only)
    AUDIO_TX_LOOPBACK,  ///< The received frame, sent straight back
    AUDIO_TX_OFF,       ///< Nothing (the controller fills our slots), to measure reception alone
} audio_tx_mode_t;

/** @brief Register the audio data callbacks; call once, after hf_init() and ag_init(). */
void audio_init(void);

/**
 * @brief Start handling frames when an (e)SCO link connects. Pauses HCI log recording.
 * @param format How the link's frames are encoded.
 */
void audio_start(audio_format_t format);

/** @brief Stop handling frames when the (e)SCO link disconnects. Resumes HCI log recording. */
void audio_stop(void);

/** @brief Choose what to send back. */
void audio_set_tx_mode(audio_tx_mode_t mode);

/** @brief What we currently send back. */
audio_tx_mode_t audio_get_tx_mode(void);

/**
 * @brief Set how many frames to send ahead when a link starts (silence, or the tone), so a
 *        reply that is late for its (e)SCO slot doesn't leave the controller without data.
 *        Applies from the next link; sample formats only. Default 2.
 */
void audio_set_tx_lead(uint8_t frames);

/** @brief Frames sent ahead when a link starts. */
uint8_t audio_get_tx_lead(void);

/** @brief Short name for @p mode, for logs and the console. */
const char *audio_tx_mode_name(audio_tx_mode_t mode);

/** @brief Short description of @p format, for logs. */
const char *audio_format_name(audio_format_t format);

/** @brief Turn the per-second frame and level log on or off. */
void audio_set_stats_logging(bool enabled);

/** @brief Whether the per-second log is on. */
bool audio_get_stats_logging(void);
