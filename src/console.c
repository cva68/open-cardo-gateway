/**
 * @file console.c
 * @brief UART command console for driving tests (type `help` for the commands).
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_console.h"
#include "esp_system.h"
#include "sdkconfig.h"
#include "ag.h"
#include "audio.h"
#include "bt.h"
#include "console.h"
#include "hf.h"

#if CONFIG_BT_HCI_LOG_DEBUG_EN
/** @brief ESP-IDF internal (bt_hci_log.c): print and clear the HCI log. */
extern void bt_hci_log_hci_data_show(void);
#endif

/** @brief Print a usage line; returns the command's failure code. */
static int usage(const char *text)
{
    printf("usage: %s\n", text);
    return 1;
}

/** @brief `status`: both roles, the audio settings and free memory. */
static int cmd_status(int argc, char **argv)
{
    ag_print_status();
    hf_print_status();
    printf("TX:             %s\n", audio_tx_mode_name(audio_get_tx_mode()));
    printf("Stats log:      %s\n", audio_get_stats_logging() ? "on" : "off");
    printf("Free heap:      %" PRIu32 " bytes (lowest %" PRIu32 ")\n",
           esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
    return 0;
}

/** @brief `tx`: choose what to send back for each received frame. */
static int cmd_tx(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "silence") == 0) {
        audio_set_tx_mode(AUDIO_TX_SILENCE);
    } else if (argc == 2 && strcmp(argv[1], "tone") == 0) {
        audio_set_tx_mode(AUDIO_TX_TONE);
    } else if (argc == 2 && strcmp(argv[1], "loop") == 0) {
        audio_set_tx_mode(AUDIO_TX_LOOPBACK);
    } else {
        return usage("tx silence|tone|loop");
    }
    printf("Sending %s\n", audio_tx_mode_name(audio_get_tx_mode()));
    if (audio_get_tx_mode() != AUDIO_TX_LOOPBACK) {
        printf("Note: on mSBC (wideband) links only loopback can be sent for now; this mode sends nothing\n");
    }
    return 0;
}

/** @brief `stats`: turn the per-second audio log on or off. */
static int cmd_stats(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "on") == 0) {
        audio_set_stats_logging(true);
    } else if (argc == 2 && strcmp(argv[1], "off") == 0) {
        audio_set_stats_logging(false);
    } else {
        return usage("stats on|off");
    }
    return 0;
}

/** @brief `ag ...`: Audio Gateway (phone) role. */
static int cmd_ag(int argc, char **argv)
{
    const char *usage_text = "ag connect <aa:bb:cc:dd:ee:ff> | disconnect | audio on|off | call on|off | pkt | "
                             "obisf <n>|error | esco <pkt types hex> <max latency> <retrans> [msbc]|default";
    esp_bd_addr_t bda;
    if (argc == 3 && strcmp(argv[1], "connect") == 0 && bda_from_str(argv[2], bda)) {
        ag_connect(bda);
    } else if (argc == 2 && strcmp(argv[1], "disconnect") == 0) {
        ag_disconnect();
    } else if (argc == 3 && strcmp(argv[1], "audio") == 0 && strcmp(argv[2], "on") == 0) {
        ag_audio_open();
    } else if (argc == 3 && strcmp(argv[1], "audio") == 0 && strcmp(argv[2], "off") == 0) {
        ag_audio_close();
    } else if (argc == 3 && strcmp(argv[1], "call") == 0 && strcmp(argv[2], "on") == 0) {
        ag_call(true);
    } else if (argc == 3 && strcmp(argv[1], "call") == 0 && strcmp(argv[2], "off") == 0) {
        ag_call(false);
    } else if (argc == 2 && strcmp(argv[1], "pkt") == 0) {
        ag_request_pkt_stats();
    } else if (argc == 3 && strcmp(argv[1], "esco") == 0 && strcmp(argv[2], "default") == 0) {
        ag_reset_esco_params();
    } else if ((argc == 5 || (argc == 6 && strcmp(argv[5], "msbc") == 0)) && strcmp(argv[1], "esco") == 0) {
        char *end1, *end2, *end3;
        unsigned long types = strtoul(argv[2], &end1, 16);
        unsigned long latency = strtoul(argv[3], &end2, 0);
        unsigned long retrans = strtoul(argv[4], &end3, 0);
        if (*end1 != '\0' || *end2 != '\0' || *end3 != '\0' || types > 0xffff || latency > 0xffff || retrans > 0xff) {
            return usage(usage_text);
        }
        ag_set_esco_params(types, latency, retrans, argc == 6);
    } else if (argc == 3 && strcmp(argv[1], "obisf") == 0 && strcmp(argv[2], "error") == 0) {
        ag_set_obisf_reply(-1);
    } else if (argc == 3 && strcmp(argv[1], "obisf") == 0) {
        char *end;
        long value = strtol(argv[2], &end, 0);
        if (*end != '\0' || value < 0) {
            return usage(usage_text);
        }
        ag_set_obisf_reply((int)value);
    } else {
        return usage(usage_text);
    }
    return 0;
}

/** @brief `hf ...`: Hands-Free (headset) role. */
static int cmd_hf(int argc, char **argv)
{
    const char *usage_text = "hf connect [aa:bb:cc:dd:ee:ff] | disconnect | audio on|off | pkt";
    esp_bd_addr_t bda;
    if (argc == 2 && strcmp(argv[1], "connect") == 0) {
        hf_connect(NULL);
    } else if (argc == 3 && strcmp(argv[1], "connect") == 0 && bda_from_str(argv[2], bda)) {
        hf_connect(bda);
    } else if (argc == 2 && strcmp(argv[1], "disconnect") == 0) {
        hf_disconnect();
    } else if (argc == 3 && strcmp(argv[1], "audio") == 0 && strcmp(argv[2], "on") == 0) {
        hf_audio_open();
    } else if (argc == 3 && strcmp(argv[1], "audio") == 0 && strcmp(argv[2], "off") == 0) {
        hf_audio_close();
    } else if (argc == 2 && strcmp(argv[1], "pkt") == 0) {
        hf_request_pkt_stats();
    } else {
        return usage(usage_text);
    }
    return 0;
}

/** @brief `bonds`: list bonded devices. */
static int cmd_bonds(int argc, char **argv)
{
    bt_print_bonds();
    return 0;
}

/** @brief `unpair`: forget the HF peer and remove every bond. */
static int cmd_unpair(int argc, char **argv)
{
    hf_forget_peer();
    bt_remove_all_bonds();
    return 0;
}

/** @brief `scan`: inquiry for nearby devices. */
static int cmd_scan(int argc, char **argv)
{
    bt_scan();
    return 0;
}

/** @brief `services`: SDP query of a device. */
static int cmd_services(int argc, char **argv)
{
    esp_bd_addr_t bda;
    if (argc != 2 || !bda_from_str(argv[1], bda)) {
        return usage("services aa:bb:cc:dd:ee:ff");
    }
    bt_query_services(bda);
    return 0;
}

/** @brief `cod`: show or set our Class of Device. */
static int cmd_cod(int argc, char **argv)
{
    if (argc == 2) {
        char *end;
        uint32_t cod = strtoul(argv[1], &end, 16);
        if (*end != '\0' || cod > 0xffffff) {
            return usage("cod [24-bit hex, e.g. 240404]");
        }
        bt_set_cod(cod);
    } else if (argc != 1) {
        return usage("cod [24-bit hex, e.g. 240404]");
    }
    printf("Class of Device 0x%06" PRIx32 "\n", bt_get_cod());
    return 0;
}

/** @brief `eir`: advertise only manufacturer data, or restore the default EIR. */
static int cmd_eir(int argc, char **argv)
{
    const char *usage_text = "eir default|<manufacturer data hex, company ID first, e.g. 00024100>";
    if (argc != 2) {
        return usage(usage_text);
    }
    if (strcmp(argv[1], "default") == 0) {
        bt_set_eir_manufacturer(NULL, 0);
        return 0;
    }
    uint8_t data[64];
    size_t len = strlen(argv[1]) / 2;
    if (strlen(argv[1]) % 2 != 0 || len == 0 || len > sizeof(data)) {
        return usage(usage_text);
    }
    for (size_t i = 0; i < len; i++) {
        char byte[3] = {argv[1][2 * i], argv[1][2 * i + 1], '\0'};
        char *end;
        data[i] = strtoul(byte, &end, 16);
        if (*end != '\0') {
            return usage(usage_text);
        }
    }
    bt_set_eir_manufacturer(data, len);
    return 0;
}

#if CONFIG_BT_HCI_LOG_DEBUG_EN
/** @brief `hcilog`: dump and clear the HCI log, framed for tools/hcilog_to_btsnoop.py. */
static int cmd_hcilog(int argc, char **argv)
{
    printf("--- HCI log start ---\n");
    bt_hci_log_hci_data_show();
    printf("--- HCI log end ---\n");
    return 0;
}
#endif

/** The console commands, in the order `help` lists them. */
static const esp_console_cmd_t COMMANDS[] = {
    {.command = "status", .help = "Show both roles, the audio settings and free memory", .func = cmd_status},
    {.command = "tx", .help = "Choose what to send: silence, 1 kHz tone, or loopback of received audio",
     .hint = "silence|tone|loop", .func = cmd_tx},
    {.command = "stats", .help = "Turn the per-second audio log on or off", .hint = "on|off", .func = cmd_stats},
    {.command = "ag", .help = "Audio Gateway (phone) role: connect out to a headset, open audio, fake a call, tune (e)SCO",
     .hint = "connect <addr>|disconnect|audio on|off|call on|off|pkt|obisf <n>|error|esco <types> <lat> <re> [msbc]|default",
     .func = cmd_ag},
    {.command = "hf", .help = "Hands-Free (headset) role: connect to a phone-like AG (default: the last peer), open audio",
     .hint = "connect [addr]|disconnect|audio on|off|pkt", .func = cmd_hf},
    {.command = "bonds", .help = "List bonded devices", .func = cmd_bonds},
    {.command = "unpair", .help = "Forget the HF peer and remove all bonds", .func = cmd_unpair},
    {.command = "scan", .help = "Search for nearby devices for ~10 s and log name, CoD, RSSI and EIR", .func = cmd_scan},
    {.command = "services", .help = "List a device's SDP service UUIDs", .hint = "aa:bb:cc:dd:ee:ff",
     .func = cmd_services},
    {.command = "cod", .help = "Show or set our Class of Device", .hint = "[24-bit hex]", .func = cmd_cod},
    {.command = "eir", .help = "Advertise only manufacturer data in EIR, or restore the default name and UUIDs",
     .hint = "default|<hex>", .func = cmd_eir},
#if CONFIG_BT_HCI_LOG_DEBUG_EN
    {.command = "hcilog", .help = "Dump and clear the HCI log. Recording pauses while audio is connected.",
     .func = cmd_hcilog},
#endif
};

void console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "intercom>";
    esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl));

    ESP_ERROR_CHECK(esp_console_register_help_command());
    for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&COMMANDS[i]));
    }
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
