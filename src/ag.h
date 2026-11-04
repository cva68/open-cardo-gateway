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
 * @param msbc           Transparent air coding (mSBC) rather than CVSD.
 */
void ag_set_esco_params(uint16_t packet_types, uint16_t max_latency, uint8_t retrans_effort, bool msbc);

/** @brief Go back to Bluedroid's default (e)SCO parameters. */
void ag_reset_esco_params(void);

/**
 * @brief Choose how to answer AT+OBISF (OBI supported features).
 * @param value Reply "+OBISF: <value>" then OK, or ERROR if negative.
 */
void ag_set_obisf_reply(int value);

/** @brief Ask the controller for (e)SCO packet counts; logged when they arrive. */
void ag_request_pkt_stats(void);

/** @brief Print the AG connection, audio, call and OBISF state to the console. */
void ag_print_status(void);
