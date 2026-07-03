/*
 * ESPNowRelay — ESP32-C6 ESP-NOW store-and-forward relay/repeater
 * Author: SEspe
 * Framework: ESP-IDF v6.x
 *
 * Build:  idf.py set-target esp32c6 && idf.py build
 * Flash:  idf.py -p COMx flash monitor
 *
 * Extends radio reach between MasterRemote and far slaves as a TRANSPARENT
 * PROXY — requires NO firmware changes on the existing three units:
 *
 *   - Re-broadcasts MasterRemote's discovery beacon verbatim. A slave out of
 *     Master's direct reach hears the relayed beacon (src = relay's MAC),
 *     adopts the relay as its "Master", and sends its heartbeats here.
 *   - Forwards slave heartbeats verbatim to the real Master, which registers
 *     the relay's MAC as that role's holder and sends commands here.
 *   - Forwards Master's commands verbatim to the real slave, picked by the
 *     command's label ("Master->SlaveHonda" / "Master->SlaveWallas").
 *
 * Trust and failover come for free from the existing MAC + staleness rules:
 * every unit trusts its immediate radio peer; whichever path (direct or
 * relayed) a unit hears consistently holds the role claim, and a stale path
 * is re-homed automatically within 30 s. If all direct links are healthy,
 * the relay sits in passive standby (its re-broadcast beacons are ignored
 * by slaves whose direct claim on Master is still fresh). See FSD §13.
 *
 * Pin assignments:
 *   GPIO  2  OUTPUT — Onboard LED (fast blink = forwarded traffic in the
 *                     last 10 s, slow = idle/standby)
 *
 * WiFi is preferred (web UI + OTA + channel derivation), but like SlaveHonda
 * the relay may sit in a weak spot: if the connect attempt fails with saved
 * credentials, it does NOT reopen the portal — it scans ESP-NOW channels for
 * MasterRemote's beacon and relays without an IP (no web UI in that state).
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
#define PIN_STATUS_LED  GPIO_NUM_2

/* ── ESP-NOW channel scan fallback (same pattern as SlaveHonda, FSD §3.2) ──── */
#define SCAN_CHANNEL_COUNT   13
#define SCAN_DWELL_MS        2500

/* Matches Master's NODE_TIMEOUT_MS / the slaves' MASTER_STALE_US. */
#define PEER_STALE_US  ((int64_t)30000 * 1000)

static const uint8_t BROADCAST_MAC[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

/* ── Message Structures (byte-identical to the other units' — FSD §3.3) ────── */
typedef struct {
    char  label[32];
    bool  HondaRunningFB;
    bool  HondaIgnitionOn;
    bool  HondaStart;
} master_msg_t;                 /* 35 bytes */

typedef struct {
    char  label[32];            /* "MasterRemote" */
} master_beacon_t;              /* 32 bytes, broadcast-only */

typedef struct {
    char  label[32];
    bool  HondaIgnitionOn;
    bool  HondaStarting;
    bool  HondaRunning;
    char  ip[16];
    bool  has_wifi;
    int8_t  rssi;
    uint8_t channel;
    char    fw_version[12];
} slave_honda_msg_t;            /* 66 bytes */

typedef struct {
    char  label[32];
    bool  WallasRunning;
    bool  WallasStart;
    char  ip[16];
    bool  has_wifi;
    int8_t  rssi;
    uint8_t channel;
    char    fw_version[12];
} slave_wallas_msg_t;           /* 65 bytes */

/* ── Relay State (shared with web_server.c via web_server.h) ────────────────── */
traffic_stat_t g_traffic[TC_COUNT] = {0};
relay_peer_t   g_peer_master = {0};
relay_peer_t   g_peer_honda  = {0};
relay_peer_t   g_peer_wallas = {0};

static int64_t s_last_fwd_us = 0;   /* drives the LED activity blink */

         bool     g_has_wifi = false;
volatile uint32_t g_wifi_disconnect_count = 0;
volatile int64_t  g_wifi_last_disc_ts_us  = 0;   /* 0 = never disconnected since boot */
         uint32_t g_heap_min       = UINT32_MAX;
         int64_t  g_heap_min_ts_us = 0;

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
             * governs the initial boot decision to fall back. */
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
         * (radio sleeps between beacons, only wakes at DTIM intervals) — mains/
         * battery-bank powered, no downside to disabling it. See MasterRemote's
         * main.c for the fuller comment (2026-07-02 flaky-web-server fix). */
        esp_wifi_set_ps(WIFI_PS_NONE);
        /* Max TX power (84 = 21 dBm) — not fatal if the module rejects it. */
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

/* ── ESP-NOW peer bookkeeping ───────────────────────────────────────────────── */
static void espnow_add_peer(const uint8_t *mac) {
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) != ESP_OK)
        ESP_LOGE(TAG, "Failed to add peer " MACSTR, MAC2STR(mac));
}

/* Adopt a (new) MAC for a peer slot: swap the esp_now peer-table entry and
 * refresh last_seen. Caller has already applied the staleness gate. */
static void adopt_peer(relay_peer_t *p, const uint8_t *mac, const char *role) {
    if (p->known) esp_now_del_peer(p->mac);
    memcpy(p->mac, mac, 6);
    espnow_add_peer(mac);
    p->known = true;
    p->last_seen_us = esp_timer_get_time();
    ESP_LOGI(TAG, "Adopted %s: " MACSTR, role, MAC2STR(mac));
}

/* Staleness-gated role claim, mirroring Master's register_peer() semantics:
 * refuse to hand a role to a new MAC while the current holder is still fresh
 * (closes the same casual-spoofing door — FSD §3.2/§12). */
static bool claim_role(relay_peer_t *p, const uint8_t *mac, const char *role) {
    if (p->known && memcmp(p->mac, mac, 6) == 0) {
        p->last_seen_us = esp_timer_get_time();
        return true;
    }
    if (p->known && (esp_timer_get_time() - p->last_seen_us) < PEER_STALE_US) {
        ESP_LOGW(TAG, "Ignoring %s claim from " MACSTR " -- still held by " MACSTR,
                 role, MAC2STR(mac), MAC2STR(p->mac));
        return false;
    }
    adopt_peer(p, mac, role);
    return true;
}

/* ── Traffic accounting + forwarding ────────────────────────────────────────── */
static void note_rx(traffic_class_t tc, const uint8_t *src, int8_t rssi) {
    traffic_stat_t *t = &g_traffic[tc];
    t->rx_count++;
    memcpy(t->last_src, src, 6);
    t->last_rssi    = rssi;
    t->last_seen_us = esp_timer_get_time();
}

static void forward(traffic_class_t tc, const uint8_t *dst, const uint8_t *data, int len) {
    traffic_stat_t *t = &g_traffic[tc];
    memcpy(t->last_dst, dst, 6);
    t->has_dst = true;
    if (esp_now_send(dst, data, len) == ESP_OK) {
        t->fwd_count++;
        s_last_fwd_us = esp_timer_get_time();
    } else {
        t->fwd_fail++;
    }
}

/* ── ESP-NOW receive: the relay core ────────────────────────────────────────── */
static void espnow_recv_cb(const esp_now_recv_info_t *info,
                           const uint8_t *data, int len)
{
    const uint8_t *src  = info->src_addr;
    int8_t         rssi = info->rx_ctrl ? info->rx_ctrl->rssi : 0;

    if (len == (int)sizeof(master_beacon_t)) {
        master_beacon_t b;
        memcpy(&b, data, sizeof(b));
        if (strncmp(b.label, "MasterRemote", 12) == 0) {
            bool is_current = g_peer_master.known && memcmp(src, g_peer_master.mac, 6) == 0;
            if (!is_current) {
                bool stale = !g_peer_master.known ||
                    (esp_timer_get_time() - g_peer_master.last_seen_us) > PEER_STALE_US;
                if (!stale) {
                    /* A different MAC claiming Master while ours is fresh — this
                     * also guards against relay-to-relay beacon ping-pong if a
                     * second relay is ever added. */
                    note_rx(TC_OTHER, src, rssi);
                    return;
                }
                adopt_peer(&g_peer_master, src, "Master");
            }
            g_peer_master.last_seen_us = esp_timer_get_time();
            g_peer_master.rssi = rssi;
            note_rx(TC_BEACON, src, rssi);
            forward(TC_BEACON, BROADCAST_MAC, data, len);   /* re-broadcast verbatim */
            return;
        }
    }

    if (len == (int)sizeof(master_msg_t)) {
        /* Commands are only relayed from the trusted Master MAC. */
        if (!g_peer_master.known || memcmp(src, g_peer_master.mac, 6) != 0) {
            note_rx(TC_OTHER, src, rssi);
            return;
        }
        master_msg_t m;
        memcpy(&m, data, sizeof(m));
        g_peer_master.last_seen_us = esp_timer_get_time();
        g_peer_master.rssi = rssi;
        if (strncmp(m.label, "Master->SlaveHonda", 18) == 0) {
            note_rx(TC_CMD_HONDA, src, rssi);
            if (g_peer_honda.known) forward(TC_CMD_HONDA, g_peer_honda.mac, data, len);
            else g_traffic[TC_CMD_HONDA].fwd_fail++;   /* no destination learned yet */
        } else if (strncmp(m.label, "Master->SlaveWallas", 19) == 0) {
            note_rx(TC_CMD_WALLAS, src, rssi);
            if (g_peer_wallas.known) forward(TC_CMD_WALLAS, g_peer_wallas.mac, data, len);
            else g_traffic[TC_CMD_WALLAS].fwd_fail++;
        } else {
            note_rx(TC_OTHER, src, rssi);
        }
        return;
    }

    if (len == (int)sizeof(slave_honda_msg_t)) {
        slave_honda_msg_t m;
        memcpy(&m, data, sizeof(m));
        if (strncmp(m.label, "SlaveHonda", 10) == 0) {
            if (!claim_role(&g_peer_honda, src, "SlaveHonda")) {
                note_rx(TC_OTHER, src, rssi);
                return;
            }
            g_peer_honda.rssi = rssi;
            note_rx(TC_HB_HONDA, src, rssi);
            if (g_peer_master.known) forward(TC_HB_HONDA, g_peer_master.mac, data, len);
            else g_traffic[TC_HB_HONDA].fwd_fail++;
            return;
        }
    }

    if (len == (int)sizeof(slave_wallas_msg_t)) {
        slave_wallas_msg_t m;
        memcpy(&m, data, sizeof(m));
        if (strncmp(m.label, "SlaveWallas", 11) == 0) {
            if (!claim_role(&g_peer_wallas, src, "SlaveWallas")) {
                note_rx(TC_OTHER, src, rssi);
                return;
            }
            g_peer_wallas.rssi = rssi;
            note_rx(TC_HB_WALLAS, src, rssi);
            if (g_peer_master.known) forward(TC_HB_WALLAS, g_peer_master.mac, data, len);
            else g_traffic[TC_HB_WALLAS].fwd_fail++;
            return;
        }
    }

    note_rx(TC_OTHER, src, rssi);
}

static void espnow_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t st) {}

static void espnow_init(void) {
    esp_now_init();
    esp_now_register_send_cb(espnow_send_cb);
    esp_now_register_recv_cb(espnow_recv_cb);
    espnow_add_peer(BROADCAST_MAC);   /* needed to re-broadcast the beacon */
    ESP_LOGI(TAG, "ESP-NOW ready, relaying for MasterRemote");
}

/* Blocks until a MasterRemote beacon is heard, cycling through all 2.4 GHz
 * channels. Used only when WiFi connect failed (§3.2). Retries forever. */
static void espnow_channel_scan(void) {
    ESP_LOGW(TAG, "Starting ESP-NOW channel scan for MasterRemote");
    for (;;) {
        for (uint8_t ch = 1; ch <= SCAN_CHANNEL_COUNT; ch++) {
            esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
            int64_t start = esp_timer_get_time();
            while (!g_peer_master.known &&
                   (esp_timer_get_time() - start) < (int64_t)SCAN_DWELL_MS * 1000) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            if (g_peer_master.known) {
                ESP_LOGI(TAG, "Locked to channel %d", ch);
                return;
            }
        }
        ESP_LOGW(TAG, "Full channel scan found no MasterRemote beacon — retrying");
    }
}

/* ── GPIO / Housekeeping ─────────────────────────────────────────────────────── */
static void gpio_init_pins(void) {
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_STATUS_LED),
        .mode = GPIO_MODE_OUTPUT, .intr_type = GPIO_INTR_DISABLE };
    gpio_config(&out);
    gpio_set_level(PIN_STATUS_LED, 0);
}

static void housekeeping_task(void *arg) {
    /* Baseline immediately so /api/sysinfo never reports the UINT32_MAX
     * sentinel during the first sampling interval after boot. */
    g_heap_min       = esp_get_free_heap_size();
    g_heap_min_ts_us = esp_timer_get_time();
    uint32_t n = 0;
    for (;;) {
        /* Fast blink while traffic was forwarded in the last 10 s, slow when
         * idle/standby — mirrors the slaves' commanded/idle blink pattern. */
        bool active = s_last_fwd_us &&
            (esp_timer_get_time() - s_last_fwd_us) < (int64_t)10000 * 1000;
        uint32_t half_ms = active ? 200 : 1000;
        gpio_set_level(PIN_STATUS_LED,
                       (esp_timer_get_time() / (half_ms * 1000)) % 2 == 0 ? 1 : 0);
        if (++n >= 100) {   /* heap low-water sample every ~10 s */
            n = 0;
            uint32_t cur_heap = esp_get_free_heap_size();
            if (cur_heap < g_heap_min) { g_heap_min = cur_heap; g_heap_min_ts_us = esp_timer_get_time(); }
        }
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

    xTaskCreate(housekeeping_task, "housekeep", 2048, NULL, 2, NULL);
    xTaskCreate(config_apply_task, "cfgapply",  2048, NULL, 3, NULL);

    ESP_LOGI(TAG, "Ready");
}
