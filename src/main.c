/**
 * @file main.c
 * @brief Entry point: brings up Bluetooth, both HFP roles, the audio path and the console.
 */
#include "nvs_flash.h"
#include "ag.h"
#include "audio.h"
#include "bt.h"
#include "console.h"
#include "hf.h"

/** Name shown to other devices. */
#define DEVICE_NAME "OpenIntercom"

/** @brief Initialise NVS (bonds, PHY calibration, last peer), erasing it if its layout changed. */
static void nvs_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

/** @brief ESP-IDF entry point. */
void app_main(void)
{
    nvs_init();
    bt_init(DEVICE_NAME);
    hf_init();
    ag_init();
    audio_init();
    console_start();
}
