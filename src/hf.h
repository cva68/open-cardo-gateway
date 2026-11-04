/**
 * @file hf.h
 * @brief HFP Hands-Free (headset) role: the Cardo connects to us as a phone would.
 *
 * The Freecom 2x never connects out to us in this role, so the Audio Gateway role
 * (ag.h) is the working path; this one is kept for experiments with other AGs.
 */
#pragma once

#include "esp_bt_defs.h"

/** @brief Register and start the HF client, and reconnect to the last peer if there is one. */
void hf_init(void);

/**
 * @brief Connect to an AG and turn auto-reconnect on.
 * @param bda AG to connect to, or NULL for the last peer.
 */
void hf_connect(const esp_bd_addr_t bda);

/** @brief Disconnect and turn auto-reconnect off until hf_connect() or a new connection. */
void hf_disconnect(void);

/** @brief Disconnect and forget the stored peer (bonds are removed separately). */
void hf_forget_peer(void);

/** @brief Open the audio link to the connected AG. */
void hf_audio_open(void);

/** @brief Close the audio link to the connected AG. */
void hf_audio_close(void);

/** @brief Ask the controller for (e)SCO packet counts; logged when they arrive. */
void hf_request_pkt_stats(void);

/** @brief Print the HF connection and audio state to the console. */
void hf_print_status(void);
