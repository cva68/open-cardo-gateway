/**
 * @file ag.h
 * @brief HFP Audio Gateway (phone) role: we connect out to a headset's Hands-Free service,
 *        as a phone or another brand's "universal intercom" would.
 *
 * The Freecom 2x pairs with us this way as Rider A, sends AT+OBISF during setup, and asks
 * for audio with AT+BVRA=1 when its Intercom button is tapped.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_bt_defs.h"

#define AG_VOICE_SETTING_CVSD   0x0060  ///< Linear 16-bit 2's complement input, CVSD air coding
#define AG_VOICE_SETTING_ULAW   0x0100  ///< 8-bit u-law input, CVSD air coding
#define AG_VOICE_SETTING_TRANS  0x0063  ///< Linear 16-bit input, transparent air coding (mSBC)

/** @brief Register and start the AG. */
void ag_init(void);

/**
 * @brief Connect to a headset's Hands-Free service (pairing first if needed).
 * @param bda Headset to connect to.
 */
void ag_connect(const esp_bd_addr_t bda);

/** @brief Disconnect from the headset. */
void ag_disconnect(void);

/** @brief Open the audio link on its own, without any call state. */
void ag_audio_open(void);

/** @brief Close the audio link. */
void ag_audio_close(void);

/**
 * @brief Start or end a pretend call (call indicator plus audio), for headsets that only
 *        route audio during a call.
 */
void ag_call(bool active);

/**
 * @brief Override the (e)SCO parameters requested when we open audio (64 kbit/s each way).
 *
 * Uses a Bluedroid internal; the override applies whichever codec is negotiated, and
 * is lost on reboot.
 * @param packet_types   HCI mask: bits 0-5 allow HV1..EV5, bits 6-9 disallow 2-EV3..3-EV5.
 * @param max_latency    Max latency in ms, or 0xFFFF for don't care.
 * @param retrans_effort 0 none, 1 optimise for power, 2 for quality, 0xFF don't care.
 * @param voice_setting  HCI voice setting: AG_VOICE_SETTING_CVSD (16-bit linear PCM over HCI,
 *                       CVSD on air), AG_VOICE_SETTING_TRANS (mSBC) or any other value.
 */
void ag_set_esco_params(uint16_t packet_types, uint16_t max_latency, uint8_t retrans_effort, uint16_t voice_setting);

/** @brief Go back to Bluedroid's default (e)SCO parameters. */
void ag_reset_esco_params(void);

/**
 * @brief Choose the codec (+BCS) and link for the next audio connection: CVSD on the
 *        Freecom 2x's EV5 intercom link with u-law samples (the default, chosen again on each
 *        connection), or mSBC on Bluedroid's default link.
 *
 * Sets the codec with a Bluedroid internal (needs a service level connection) and the
 * (e)SCO request as ag_set_esco_params() / ag_reset_esco_params() would.
 * @param msbc mSBC rather than CVSD.
 */
void ag_set_codec(bool msbc);

/**
 * @brief Choose how to answer AT+OBISF (OBI supported features).
 * @param value Reply "+OBISF: <value>" then OK, or ERROR if negative.
 */
void ag_set_obisf_reply(int value);

/** @brief Ask the controller for (e)SCO packet counts; logged when they arrive. */
void ag_request_pkt_stats(void);

/** @brief Print the AG connection, audio, call and OBISF state to the console. */
void ag_print_status(void);
