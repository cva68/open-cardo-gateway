/**
 * @file bt.h
 * @brief Bluetooth stack bring-up, pairing, bonds, discovery and advertising data.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_bt_defs.h"

/** Buffer size for a Bluetooth address formatted by bda_to_str(), including the terminator. */
#define BDA_STR_LEN 18

/** Class of Device: Audio/Video, Wearable Headset; Audio and Rendering services. */
#define BT_COD_WEARABLE_HEADSET 0x240404

/**
 * @brief Start the controller and Bluedroid (Classic only) and become discoverable.
 * @param device_name Name shown to other devices.
 */
void bt_init(const char *device_name);

/**
 * @brief Set our Class of Device.
 *
 * The HF client sets 0x240408 (Hands-free) when it starts, so call this after that.
 * @param cod 24-bit value as seen over the air, e.g. 0x240404.
 */
void bt_set_cod(uint32_t cod);

/** @brief Our current Class of Device, as a 24-bit value. */
uint32_t bt_get_cod(void);

/**
 * @brief Replace our EIR with only manufacturer-specific data, as the Freecom advertises
 *        in intercom pairing mode.
 * @param data Company ID (little-endian) followed by its data.
 * @param len  Length of @p data; 0 restores the default name and service UUIDs.
 */
void bt_set_eir_manufacturer(const uint8_t *data, size_t len);

/** @brief Inquire for ~10 s, logging each device's name, CoD, RSSI and raw EIR. */
void bt_scan(void);

/**
 * @brief Query a device's SDP service UUIDs (connecting if needed); logged when they arrive.
 * @param bda Device to query.
 */
void bt_query_services(const esp_bd_addr_t bda);

/** @brief Whether we hold a link key for @p bda. */
bool bt_is_bonded(const esp_bd_addr_t bda);

/** @brief Print the bonded devices to the console. */
void bt_print_bonds(void);

/** @brief Remove every bond. */
void bt_remove_all_bonds(void);

/**
 * @brief Format a Bluetooth address as aa:bb:cc:dd:ee:ff.
 * @param bda Address to format.
 * @param str Output buffer of at least #BDA_STR_LEN bytes.
 * @return @p str
 */
char *bda_to_str(const esp_bd_addr_t bda, char *str);

/**
 * @brief Parse a Bluetooth address in aa:bb:cc:dd:ee:ff form.
 * @return true if @p str was a valid address.
 */
bool bda_from_str(const char *str, esp_bd_addr_t bda);
