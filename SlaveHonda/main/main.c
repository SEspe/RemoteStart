/*
 * SlaveHonda — ESP32 Honda EU70IS Generator Slave
 * Author: SEspe
 * Framework: ESP-IDF v6.x
 *
 * Build:  idf.py build
 * Flash:  idf.py -p COMx flash monitor
 *
 * Pin assignments (relays active-LOW):
 *   GPIO 13  OUTPUT — Starter relay     (LOW = crank ON)
 *   GPIO 14  OUTPUT — Ignition relay    (LOW = ignition ON)
 *   GPIO 15  INPUT  — Running feedback  (LOW = engine running)
 *   GPIO  4  OUTPUT — External status LED
 *   GPIO  2  OUTPUT — Onboard LED (fast blink when start commanded by Master; slow = idle)
 *
 * Peer discovery: no custom/hardcoded MAC addresses. This unit uses its own
 * factory MAC. WiFi is preferred (same as the other units) but the generator
 * location may be out of router range: if the WiFi connect attempt fails,
 * this unit does NOT reopen the config portal — instead it scans ESP-NOW
 * channels for MasterRemote's discovery beacon and operates without an IP.
 * See FSD_RemoteStart.md §3.2 / §9.2.
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_now.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"

#include "version.h"
#include "web_server.h"

static const char *TAG = FIRMWARE_NAME;

/* ── Pins ────────────────────────────────────────────────────────────────────── */
#define PIN_STARTER_RELAY   GPIO_NUM_13   /* active LOW = crank ON     */
#define PIN_IGNITION_RELAY  GPIO_NUM_14   /* active LOW = ignition ON  */
#define PIN_RUNNING_FB      GPIO_NUM_15   /* LOW = engine running -- NOT WIRED on this
                                            * hardware revision as of 2026-07-02; the
                                            * pin has no pull resistor and floats, so
                                            * its reading is untrustworthy. g_honda_running
                                            * is hardcoded false below until it's connected
                                            * to a real feedback sensor -- see FSD §2.3/§12. */
#define PIN_LED_EXT         GPIO_NUM_4
#define PIN_LED_ONBOARD     GPIO_NUM_2

/* ── Timing ──────────────────────────────────────────────────────────────────── */
#define HONDA_CRANK_MS         3000
#define HONDA_IGN_WARMUP_MS   10000
#define STATUS_SEND_MS        10000

/* ── ESP-NOW channel scan fallback (see FSD §3.2/§3.4) ─────────────────────── */
#define SCAN_CHANNEL_COUNT   13
#define SCAN_DWELL_MS        2500

/* ── Message Structures (see FSD §3.3) ─────────────────────────────────────── */
typedef struct {
    char  label[32];
    bool  HondaRunningFB;
    bool  HondaIgnitionOn;
    bool  HondaStart;
} master_msg_t;

typedef struct {
    char  label[32];   /* "MasterRemote" */
} master_beacon_t;      /* broadcast-only, discovery */

typedef struct {
    char  label[32];
    bool  HondaIgnitionOn;
    bool  HondaStarting;
    bool  HondaRunning;
    char  ip[16];
    bool  has_wifi;
    int8_t  rssi;      /* RSSI of last frame heard from MasterRemote, dBm */
    uint8_t channel;   /* channel that frame was received on */
    char    fw_version[12];
} slave_msg_t;

/* ── Global State ────────────────────────────────────────────────────────────── */
volatile bool     g_start_cmd    = false;
         bool     g_honda_running = false;
         bool     g_honda_starting= false;
         bool     g_honda_ign_on  = false;
         bool     g_has_wifi      = false;
volatile uint32_t g_wifi_disconnect_count = 0;
volatile int64_t  g_wifi_last_disc_ts_us  = 0;   /* 0 = never disconnected since boot */
         uint32_t g_heap_min       = UINT32_MAX;   /* self-sampled low-water mark, paired with a timestamp (esp_get_minimum_free_heap_size() has no "when") */
         int64_t  g_heap_min_ts_us = 0;

/* ── Master discovery state ─────────────────────────────────────────────────── */
uint8_t          g_master_mac[6] = {0};   /* exposed to web_server.c */
volatile bool    g_master_known  = false;
volatile int8_t  g_espnow_rssi    = 0;   /* last frame heard from MasterRemote -- exposed to web_server.c */
volatile uint8_t g_espnow_channel = 0;
static int64_t s_master_last_seen_us = 0;
#define MASTER_STALE_US  ((int64_t)30000 * 1000)   /* matches Master's own NODE_TIMEOUT_MS */

/* ── WiFi ────────────────────────────────────────────────────────────────────── */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static EventGroupHandle_t s_wifi_eg;
static int s_retry = 0;
static bool s_wifi_ever_connected = false;
#define MAX_RETRY 5

static esp_err_t nvs_load_wifi(char *ssid, char *pass, size_t len) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return ESP_FAIL;
    size_t sl=len, pl=len;
    esp_err_t e  = nvs_get_str(h, "ssid", ssid, &sl);
              e |= nvs_get_str(h, "pass", pass, &pl);
    nvs_close(h); return e;
}

static void nvs_save_wifi(const char *ssid, const char *pass) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ssid", ssid); nvs_set_str(h, "pass", pass);
        nvs_commit(h); nvs_close(h);
    }
}

volatile bool g_wifi_save_requested = false;
char          g_new_ssid[64] = {0};
char          g_new_pass[64] = {0};
static bool   g_portal_mode  = false;

/* ── IP configuration (DHCP by default, optional static IP) ────────────────── */
bool          g_ipcfg_static = false;
char          g_ipcfg_ip[16]   = {0};
char          g_ipcfg_mask[16] = {0};
char          g_ipcfg_gw[16]   = {0};
char          g_ipcfg_dns[16]  = {0};
volatile bool g_ipcfg_save_requested = false;

static void nvs_load_ipcfg(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t mode = 0;
    nvs_get_u8(h, "ipmode", &mode);
    g_ipcfg_static = (mode == 1);
    size_t l;
    l = sizeof(g_ipcfg_ip);   if (nvs_get_str(h, "ip",   g_ipcfg_ip,   &l) != ESP_OK) g_ipcfg_ip[0]   = '\0';
    l = sizeof(g_ipcfg_mask); if (nvs_get_str(h, "mask", g_ipcfg_mask, &l) != ESP_OK) g_ipcfg_mask[0] = '\0';
    l = sizeof(g_ipcfg_gw);   if (nvs_get_str(h, "gw",   g_ipcfg_gw,   &l) != ESP_OK) g_ipcfg_gw[0]   = '\0';
    l = sizeof(g_ipcfg_dns);  if (nvs_get_str(h, "dns",  g_ipcfg_dns,  &l) != ESP_OK) g_ipcfg_dns[0]  = '\0';
    nvs_close(h);
}

static void nvs_save_ipcfg(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "ipmode", g_ipcfg_static ? 1 : 0);
        nvs_set_str(h, "ip",   g_ipcfg_ip);
        nvs_set_str(h, "mask", g_ipcfg_mask);
        nvs_set_str(h, "gw",   g_ipcfg_gw);
        nvs_set_str(h, "dns",  g_ipcfg_dns);
        nvs_commit(h); nvs_close(h);
    }
}

/* Applies a saved static IP to the STA netif before esp_wifi_start(); a no-op
 * (leaves the netif's default DHCP client running) when in DHCP mode or when
 * the saved ip/mask are missing/invalid. */
static void apply_static_ip(esp_netif_t *sta_netif) {
    if (!g_ipcfg_static || !g_ipcfg_ip[0] || !g_ipcfg_mask[0]) return;

    esp_netif_ip_info_t ip_info = {0};
    if (esp_netif_str_to_ip4(g_ipcfg_ip,   &ip_info.ip)      != ESP_OK ||
        esp_netif_str_to_ip4(g_ipcfg_mask, &ip_info.netmask) != ESP_OK) {
        ESP_LOGW(TAG, "Saved static IP config invalid — falling back to DHCP");
        return;
    }
    if (g_ipcfg_gw[0]) esp_netif_str_to_ip4(g_ipcfg_gw, &ip_info.gw);

    esp_netif_dhcpc_stop(sta_netif);
    esp_netif_set_ip_info(sta_netif, &ip_info);
    if (g_ipcfg_dns[0]) {
        esp_netif_dns_info_t dns_info = {0};
        dns_info.ip.type = ESP_IPADDR_TYPE_V4;
        if (esp_netif_str_to_ip4(g_ipcfg_dns, &dns_info.ip.u_addr.ip4) == ESP_OK)
            esp_netif_set_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns_info);
    }
    ESP_LOGI(TAG, "Static IP applied: %s / %s gw %s", g_ipcfg_ip, g_ipcfg_mask,
             g_ipcfg_gw[0] ? g_ipcfg_gw : "-");
}

/* Watches for a WiFi-credentials or IP-config save requested from the always-
 * available web UI WiFi tab (as opposed to the first-boot config portal,
 * which has its own blocking wait loop in start_config_portal() below) and
 * persists + reboots to apply. */
static void config_apply_task(void *arg) {
    for (;;) {
        if (g_wifi_save_requested) {
            nvs_save_wifi(g_new_ssid, g_new_pass);
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }
        if (g_ipcfg_save_requested) {
            nvs_save_ipcfg();
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void wifi_event_handler(void *a, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START && !g_portal_mode) esp_wifi_connect();
        else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            g_wifi_disconnect_count++;
            g_wifi_last_disc_ts_us = esp_timer_get_time();
            /* Once we've connected at least once, retry forever — MAX_RETRY only
             * governs the initial boot decision to fall back to the config portal. */
            if (s_wifi_ever_connected || s_retry++ < MAX_RETRY) esp_wifi_connect();
            else xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry = 0;
        s_wifi_ever_connected = true;
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "IP acquired");
    }
}

static void start_config_portal(void) {
    ESP_LOGI(TAG, "Opening portal: %s", WIFI_AP_SSID);
    esp_netif_create_default_wifi_ap();
    wifi_config_t ap_cfg = { .ap = {
        .ssid_len=strlen(WIFI_AP_SSID), .channel=1,
        .authmode=WIFI_AUTH_WPA2_PSK, .max_connection=4 }};
    memcpy(ap_cfg.ap.ssid,     WIFI_AP_SSID,     strlen(WIFI_AP_SSID));
    memcpy(ap_cfg.ap.password, WIFI_AP_PASSWORD,  strlen(WIFI_AP_PASSWORD));
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    esp_wifi_start();
    web_server_start();
    while (!g_wifi_save_requested) vTaskDelay(pdMS_TO_TICKS(200));
    nvs_save_wifi(g_new_ssid, g_new_pass);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

/* Returns true if WiFi connected normally. Returns false (WiFi driver left
 * started, but not associated) when connect failed with existing credentials
 * — caller falls back to the ESP-NOW channel scan instead of the portal. */
static bool wifi_init_and_connect(void) {
    s_wifi_eg = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_STA);   /* own factory MAC, never overridden */
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,   &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP,&wifi_event_handler, NULL);

    nvs_load_ipcfg();

    char ssid[64]={0}, pass[64]={0};
    if (nvs_load_wifi(ssid, pass, sizeof(ssid)) == ESP_OK && ssid[0]) {
        wifi_config_t sta = {0};
        strlcpy((char*)sta.sta.ssid,     ssid, sizeof(sta.sta.ssid));
        strlcpy((char*)sta.sta.password, pass, sizeof(sta.sta.password));
        esp_wifi_set_config(WIFI_IF_STA, &sta);
        apply_static_ip(sta_netif);
        esp_wifi_start();
        /* Default modem-sleep power save adds real latency to HTTP over WiFi
         * (radio sleeps between beacons, only wakes at DTIM intervals) -- mains/
         * battery-bank powered, no downside to disabling it. See MasterRemote's
         * main.c for the fuller comment (2026-07-02 flaky-web-server fix). */
        esp_wifi_set_ps(WIFI_PS_NONE);
        /* Max TX power (84 = 21 dBm) -- not fatal if the module rejects it. */
        esp_err_t txp_err = esp_wifi_set_max_tx_power(84);
        if (txp_err != ESP_OK) ESP_LOGW(TAG, "set_max_tx_power failed: %s", esp_err_to_name(txp_err));
        EventBits_t b = xEventGroupWaitBits(s_wifi_eg,
            WIFI_CONNECTED_BIT|WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
        if (b & WIFI_CONNECTED_BIT) { web_server_start(); return true; }
        ESP_LOGW(TAG, "Connect failed — falling back to ESP-NOW channel scan (no portal)");
        return false;   /* leave WiFi driver started so we can set the channel manually */
    }

    /* Never configured at all → still need the portal for initial setup */
    g_portal_mode = true;
    start_config_portal();
    return true;   /* unreachable — start_config_portal() restarts the device */
}

/* ── IP helper ──────────────────────────────────────────────────────────────── */
static void get_ip_str(char *buf, size_t sz) {
    esp_netif_ip_info_t info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (g_has_wifi && netif && esp_netif_get_ip_info(netif, &info) == ESP_OK)
        snprintf(buf, sz, IPSTR, IP2STR(&info.ip));
    else
        buf[0] = '\0';
}

/* ── Honda State Machine ─────────────────────────────────────────────────────── */
static void honda_stop(void) {
    ESP_LOGI(TAG, "Stop");
    gpio_set_level(PIN_IGNITION_RELAY, 1);
    gpio_set_level(PIN_STARTER_RELAY,  1);
    gpio_set_level(PIN_LED_EXT,        0);
    g_honda_running = false;
    g_honda_ign_on  = false;
    /* g_honda_starting is deliberately NOT cleared here -- it is owned by the
     * start-sequence task, which clears it itself on every exit path. Clearing
     * it from a stop command mid-sequence would let a new start command spawn
     * a second sequence task while the first is still driving the relays. */
}

/* Blocking start sequence. The spawner must set g_honda_starting before
 * creating the task (claiming the sequence); every exit path here clears it.
 * The delays poll g_start_cmd so a stop command aborts the sequence promptly
 * instead of cranking a generator that was just told to stop. */
static void honda_start(void) {
    g_honda_ign_on = true;
    ESP_LOGI(TAG, "Ignition ON");
    gpio_set_level(PIN_IGNITION_RELAY, 0);   /* active LOW */
    gpio_set_level(PIN_LED_EXT,        1);
    for (int t = 0; t < HONDA_IGN_WARMUP_MS && g_start_cmd; t += 100)
        vTaskDelay(pdMS_TO_TICKS(100));
    if (g_start_cmd) {
        ESP_LOGI(TAG, "Cranking...");
        gpio_set_level(PIN_STARTER_RELAY, 0);
        for (int t = 0; t < HONDA_CRANK_MS && g_start_cmd; t += 100)
            vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(PIN_STARTER_RELAY, 1);
    }
    if (!g_start_cmd) {
        ESP_LOGW(TAG, "Start sequence aborted by stop command");
        honda_stop();
        g_honda_starting = false;
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    /* PIN_RUNNING_FB not wired (see its #define above), so this can't be measured --
     * assume the sequence succeeded. This is the safer of two unreliable defaults:
     * a false positive here just prevents an automatic retry of a failed start
     * (recoverable with a manual Stop+Start), whereas reporting false would make
     * MasterRemote think the start never took and keep resending it forever,
     * re-cranking the starter on an already-running engine every
     * HONDA_RESTART_BLOCK_MS (confirmed live 2026-07-02). */
    g_honda_running  = true;
    g_honda_starting = false;
    ESP_LOGI(TAG, "Running: %d", g_honda_running);
}

/* A FreeRTOS task function must never return -- returning aborts the whole
 * system. This wrapper runs the sequence and then deletes itself. */
static void honda_start_task(void *arg) {
    honda_start();
    vTaskDelete(NULL);
}

/* ── ESP-NOW ─────────────────────────────────────────────────────────────────── */
static void espnow_add_peer(const uint8_t *mac) {
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) != ESP_OK)
        ESP_LOGE(TAG, "Failed to add peer " MACSTR, MAC2STR(mac));
}

static void send_status(void) {
    if (!g_master_known) return;   /* Master not discovered yet — nothing to send to */
    /* Do NOT touch g_honda_running here -- it's owned by honda_start()/honda_stop()
     * now that PIN_RUNNING_FB is unwired (see its #define above). This function used
     * to force it false on every 10 s heartbeat, which stomped a just-completed
     * successful start back to "not running" -- MasterRemote only stops resending a
     * start command once it sees a running match, so that false report caused it to
     * resend every HONDA_RESTART_BLOCK_MS forever, re-cranking the starter on an
     * already-running engine each time. Confirmed live 2026-07-02. */
    slave_msg_t msg = {0};
    strlcpy(msg.label, "SlaveHonda->Master", sizeof(msg.label));
    msg.HondaIgnitionOn = g_honda_ign_on;
    msg.HondaStarting   = g_honda_starting;
    msg.HondaRunning    = g_honda_running;
    msg.has_wifi         = g_has_wifi;
    msg.rssi             = g_espnow_rssi;
    msg.channel          = g_espnow_channel;
    strlcpy(msg.fw_version, FIRMWARE_VERSION, sizeof(msg.fw_version));
    get_ip_str(msg.ip, sizeof(msg.ip));
    esp_now_send(g_master_mac, (uint8_t *)&msg, sizeof(msg));
}

static void espnow_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t st) {}

static void espnow_recv_cb(const esp_now_recv_info_t *info,
                           const uint8_t *data, int len)
{
    if (info->rx_ctrl) {
        g_espnow_rssi    = info->rx_ctrl->rssi;
        g_espnow_channel = info->rx_ctrl->channel;
    }

    if (len == (int)sizeof(master_beacon_t)) {
        master_beacon_t beacon;
        memcpy(&beacon, data, sizeof(beacon));
        if (strncmp(beacon.label, "MasterRemote", 12) != 0) return;

        bool is_current_master = g_master_known && memcmp(info->src_addr, g_master_mac, 6) == 0;
        if (is_current_master) {
            s_master_last_seen_us = esp_timer_get_time();
            return;
        }
        bool master_stale = !g_master_known ||
            (esp_timer_get_time() - s_master_last_seen_us) > MASTER_STALE_US;
        if (!master_stale) {
            /* A different MAC is claiming to be MasterRemote while our current
             * one is still active -- ignore it rather than blindly re-homing
             * trust (see FSD §3.2 / §12). */
            ESP_LOGW(TAG, "Ignoring beacon from " MACSTR " -- current Master still active",
                     MAC2STR(info->src_addr));
            return;
        }
        memcpy(g_master_mac, info->src_addr, 6);
        espnow_add_peer(g_master_mac);
        g_master_known = true;
        s_master_last_seen_us = esp_timer_get_time();
        ESP_LOGI(TAG, "Master found: " MACSTR, MAC2STR(g_master_mac));
        send_status();   /* immediate registration heartbeat */
        return;
    }
    if (len != (int)sizeof(master_msg_t)) return;
    if (!g_master_known || memcmp(info->src_addr, g_master_mac, 6) != 0) {
        ESP_LOGW(TAG, "Ignoring command from unrecognized sender " MACSTR, MAC2STR(info->src_addr));
        return;   /* not from the registered Master -- ignore */
    }
    s_master_last_seen_us = esp_timer_get_time();
    master_msg_t msg;
    memcpy(&msg, data, sizeof(msg));
    ESP_LOGI(TAG, "Recv HondaStart=%d", msg.HondaStart);
    g_start_cmd = msg.HondaStart;

    if (g_start_cmd) {
        if (!g_honda_running && !g_honda_starting) {
            /* Claim the sequence here, before the task exists -- recv callbacks
             * run one at a time, so this closes the window where Master's
             * redundant double-send (20 ms apart) could spawn two sequences. */
            g_honda_starting = true;
            if (xTaskCreate(honda_start_task, "hstart", 4096, NULL, 5, NULL) != pdPASS) {
                g_honda_starting = false;   /* couldn't spawn -- release the claim */
                ESP_LOGE(TAG, "Failed to create start-sequence task");
            }
        }
    } else {
        honda_stop();
    }
    send_status();
}

static void espnow_init(void) {
    esp_now_init();
    esp_now_register_send_cb(espnow_send_cb);
    esp_now_register_recv_cb(espnow_recv_cb);
    ESP_LOGI(TAG, "ESP-NOW ready, listening for MasterRemote beacon");
}

/* Blocks until a MasterRemote beacon is heard, cycling through all 2.4 GHz
 * channels. Used only when WiFi connect failed (§3.2). Retries forever. */
static void espnow_channel_scan(void) {
    ESP_LOGW(TAG, "Starting ESP-NOW channel scan for MasterRemote");
    for (;;) {
        for (uint8_t ch = 1; ch <= SCAN_CHANNEL_COUNT; ch++) {
            esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
            int64_t start = esp_timer_get_time();
            while (!g_master_known &&
                   (esp_timer_get_time() - start) < (int64_t)SCAN_DWELL_MS * 1000) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            if (g_master_known) {
                ESP_LOGI(TAG, "Locked to channel %d", ch);
                return;
            }
        }
        ESP_LOGW(TAG, "Full channel scan found no MasterRemote beacon — retrying");
    }
}

/* ── GPIO Init ───────────────────────────────────────────────────────────────── */
static void gpio_init_pins(void) {
    gpio_config_t out = {
        .pin_bit_mask = (1ULL<<PIN_STARTER_RELAY)|(1ULL<<PIN_IGNITION_RELAY)|
                        (1ULL<<PIN_LED_EXT)|(1ULL<<PIN_LED_ONBOARD),
        /* INPUT_OUTPUT (not plain OUTPUT): enables the read-back buffer so
         * gpio_get_level() returns the actual driven level. A plain OUTPUT
         * pin always reads 0, which made the web UI's ignition/starter dots
         * show permanently true (active-LOW inversion of a stuck 0) -- FSD v1.36. */
        .mode = GPIO_MODE_INPUT_OUTPUT, .intr_type = GPIO_INTR_DISABLE };
    gpio_config(&out);
    gpio_set_level(PIN_STARTER_RELAY,  1);   /* relay OFF */
    gpio_set_level(PIN_IGNITION_RELAY, 1);   /* relay OFF */
    gpio_set_level(PIN_LED_EXT,        0);
    gpio_set_level(PIN_LED_ONBOARD,    0);

    gpio_config_t in = {
        .pin_bit_mask = (1ULL<<PIN_RUNNING_FB),
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_DISABLE };
    gpio_config(&in);
}

/* ── Status Task ─────────────────────────────────────────────────────────────── */
static void status_task(void *arg) {
    /* Baseline immediately so /api/sysinfo never reports the UINT32_MAX
     * sentinel during the first sampling interval after boot. */
    g_heap_min       = esp_get_free_heap_size();
    g_heap_min_ts_us = esp_timer_get_time();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(STATUS_SEND_MS));
        send_status();
        uint32_t cur_heap = esp_get_free_heap_size();
        if (cur_heap < g_heap_min) { g_heap_min = cur_heap; g_heap_min_ts_us = esp_timer_get_time(); }
    }
}

/* ── Heartbeat Task ──────────────────────────────────────────────────────────── */
static void heartbeat_task(void *arg) {
    for (;;) {
        /* Fast blink when start commanded by Master, slow when idle — same
         * pattern as SlaveWallas's onboard LED. */
        uint32_t half_ms = g_start_cmd ? 200 : 1000;
        gpio_set_level(PIN_LED_ONBOARD,
                       (esp_timer_get_time() / (half_ms * 1000)) % 2 == 0 ? 1 : 0);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ── app_main ────────────────────────────────────────────────────────────────── */
void app_main(void)
{
    ESP_LOGI(TAG, "=== %s v%s ===", FIRMWARE_NAME, FIRMWARE_VERSION);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase(); nvs_flash_init();
    }

    gpio_init_pins();
    g_has_wifi = wifi_init_and_connect();
    espnow_init();

    if (!g_has_wifi) {
        espnow_channel_scan();   /* blocks until MasterRemote is found */
    }

    xTaskCreate(status_task,       "status",   3072, NULL, 4, NULL);
    xTaskCreate(heartbeat_task,    "heartbeat",1024, NULL, 2, NULL);
    xTaskCreate(config_apply_task, "cfgapply", 2048, NULL, 3, NULL);

    ESP_LOGI(TAG, "Ready");
}
