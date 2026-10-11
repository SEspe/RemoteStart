/*
 * SlaveWallas — ESP32-C6 Wallas Heater Slave
 * Author: SEspe
 * Framework: ESP-IDF v6.x
 *
 * Build:  idf.py build
 * Flash:  idf.py -p COMx flash monitor
 *
 * Pin assignments:
 *   GPIO 19  OUTPUT — Wallas heater relay  (HIGH = relay ON = heater running)
 *   GPIO  2  OUTPUT — Onboard LED (fast blink when start commanded by Master)
 *   GPIO 18  INPUT  — Wallas running feedback (HIGH = running)
 *   GPIO 23  OUTPUT — Heater indicator LED (mirrors relay, HIGH = heater ON)
 *
 * GPIO12/13 are reserved for the native USB-Serial/JTAG console (D-/D+) and
 * must not be used as regular GPIO — see FSD_RemoteStart.md §2.4.
 *
 * Peer discovery: no custom/hardcoded MAC addresses. This unit uses its own
 * factory MAC, listens for MasterRemote's discovery beacon, and registers
 * itself via its normal heartbeat — see FSD_RemoteStart.md §3.2. WiFi is
 * mandatory for this unit (Wallas is expected to always be within range).
 *
 * Standalone (v1.56): with no MasterRemote heard for 30 s the relay follows
 * this unit's own weekly timer and Start/Stop buttons instead; while Master
 * is present it follows Master, the buttons act through Master, and the two
 * weekly timers are kept in sync (last edit wins) — see FSD §3.5.
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_now.h"
#include "esp_netif.h"
#include "mdns.h"
#include "esp_netif_sntp.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"

#include "version.h"
#include "web_server.h"

static const char *TAG = FIRMWARE_NAME;

/* ── Pins ────────────────────────────────────────────────────────────────────── */
#define PIN_WALLAS_RELAY   GPIO_NUM_19   /* HIGH = heater ON           */
#define PIN_STATUS_LED     GPIO_NUM_2    /* onboard LED                */
#define PIN_WALLAS_FB      GPIO_NUM_18   /* HIGH = heater running      */
#define PIN_HEATER_LED     GPIO_NUM_23   /* indicator LED, mirrors relay */

/* ── Timing ──────────────────────────────────────────────────────────────────── */
#define STATUS_SEND_MS   10000

/* ── Message Structures (see FSD §3.3) ─────────────────────────────────────── */
typedef struct {
    char  label[32];
    bool  HondaRunningFB;
    bool  HondaIgnitionOn;
    bool  HondaStart;        /* carries WallasStart for this slave */
} master_msg_t;

typedef struct {
    char  label[32];   /* "MasterRemote" */
} master_beacon_t;      /* broadcast-only, discovery */

typedef struct {
    char  label[32];
    bool  WallasRunning;
    bool  WallasStart;
    char  ip[16];
    bool  has_wifi;
    int8_t  rssi;      /* RSSI of last frame heard from MasterRemote, dBm */
    uint8_t channel;   /* channel that frame was received on */
    char    fw_version[12];
} slave_wallas_msg_t;

/* Weekly-timer sync and the Start/Stop buttons (v1.56) -- byte-identical to
 * MasterRemote's and ESPNowRelay's copies. Types are told apart by length, so
 * these sizes (71, 33) must stay distinct from the others (32, 35, 65, 66). */
typedef struct __attribute__((packed)) {
    char     label[32];     /* "Master->WallasTimer" or "SlaveWallas->Timer" */
    uint32_t rev;           /* last-edit-wins: the higher rev is kept by both */
    uint8_t  day[7][5];     /* enabled, start hh, start mm, stop hh, stop mm; [0] = Sunday */
} wallas_timer_msg_t;
_Static_assert(sizeof(wallas_timer_msg_t) == 71, "wallas_timer_msg_t wire size");

typedef struct __attribute__((packed)) {
    char    label[32];      /* "SlaveWallas->WebCmd" */
    uint8_t start;          /* 1 = Start Wallas pressed, 0 = Stop Wallas */
} wallas_webcmd_msg_t;
_Static_assert(sizeof(wallas_webcmd_msg_t) == 33, "wallas_webcmd_msg_t wire size");

/* ── Global State ────────────────────────────────────────────────────────────── */
volatile bool     g_start_cmd     = false;
         bool     g_wallas_running = false;
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
    memcpy(ap_cfg.ap.ssid,     WIFI_AP_SSID,    strlen(WIFI_AP_SSID));
    memcpy(ap_cfg.ap.password, WIFI_AP_PASSWORD, strlen(WIFI_AP_PASSWORD));
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    esp_wifi_start();
    web_server_start();
    while (!g_wifi_save_requested) vTaskDelay(pdMS_TO_TICKS(200));
    nvs_save_wifi(g_new_ssid, g_new_pass);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

/* http://slavewallas.local/ (v1.61): the unit answers on a fixed name, so
 * nobody has to find its DHCP address after setup -- the portal's Saved!
 * page redirects there by itself. Best-effort: a failure never blocks
 * bring-up. mDNS is same-subnet only; other subnets still need the IP. */
static void start_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) { ESP_LOGW(TAG, "mdns_init: %s", esp_err_to_name(err)); return; }
    mdns_hostname_set(MDNS_HOSTNAME);
    mdns_instance_name_set(FIRMWARE_NAME);
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS: http://%s.local/", MDNS_HOSTNAME);
}

static void wifi_init_and_connect(void) {
    s_wifi_eg = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    /* Router client lists show this name -- must be set before DHCP starts. */
    esp_netif_set_hostname(sta_netif, MDNS_HOSTNAME);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_STA);   /* own factory MAC, never overridden */
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,    &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);

    nvs_load_ipcfg();

    char ssid[64]={0}, pass[64]={0};
    if (nvs_load_wifi(ssid, pass, sizeof(ssid)) == ESP_OK && ssid[0]) {
        wifi_config_t sta = {0};
        strlcpy((char*)sta.sta.ssid,     ssid, sizeof(sta.sta.ssid));
        strlcpy((char*)sta.sta.password, pass, sizeof(sta.sta.password));
        esp_wifi_set_config(WIFI_IF_STA, &sta);
        apply_static_ip(sta_netif);
        start_mdns();
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
        if (b & WIFI_CONNECTED_BIT) { web_server_start(); return; }
        ESP_LOGW(TAG, "Connect failed, opening portal");
        esp_wifi_stop();
    }
    /* WiFi is mandatory for SlaveWallas — always fall back to the portal, never ESP-NOW-only */
    g_portal_mode = true;
    start_config_portal();
}

/* ── IP helper ──────────────────────────────────────────────────────────────── */
static void get_ip_str(char *buf, size_t sz) {
    esp_netif_ip_info_t info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &info) == ESP_OK)
        snprintf(buf, sz, IPSTR, IP2STR(&info.ip));
    else
        buf[0] = '\0';
}

/* ── Wallas Control ──────────────────────────────────────────────────────────── */
static void wallas_start(void) {
    ESP_LOGI(TAG, "Wallas START");
    gpio_set_level(PIN_WALLAS_RELAY, 1);
    gpio_set_level(PIN_HEATER_LED, 1);
}
static void wallas_stop(void) {
    ESP_LOGI(TAG, "Wallas STOP");
    gpio_set_level(PIN_WALLAS_RELAY, 0);
    gpio_set_level(PIN_HEATER_LED, 0);
}

/* ── Who drives the relay (v1.56) ─────────────────────────────────────────────
 * Master present (beacon within 30 s): the relay follows Master's command,
 * which already ORs the Victron relay, manual switch, web buttons (Master's
 * and ours, see wallas_web_set) and the shared weekly timer.
 * Standalone: the relay follows this unit's own Start/Stop and weekly timer.
 * Losing Master therefore drops its sources; the heater stays on only if our
 * own timer or a local Start asks for it. */
static volatile bool s_master_cmd       = false;  /* last command from Master */
static volatile bool s_master_cmd_valid = false;  /* one has arrived since Master was (re)found */
static volatile bool s_local_web_start  = false;  /* Start pressed here while standalone */
static volatile bool s_timer_active     = false;  /* own timer window, evaluated while standalone */

bool master_present(void)
{
    return g_master_known &&
           (esp_timer_get_time() - s_master_last_seen_us) <= MASTER_STALE_US;
}

static void apply_relay(void)
{
    bool cmd;
    if (master_present()) {
        if (!s_master_cmd_valid) return;   /* hold until Master's first command */
        cmd = s_master_cmd;
    } else {
        cmd = s_local_web_start || s_timer_active;
    }
    if (cmd != g_start_cmd || gpio_get_level(PIN_WALLAS_RELAY) != (int)cmd) {
        g_start_cmd = cmd;
        if (cmd) wallas_start(); else wallas_stop();
    }
}

bool wallas_local_web_start(void) { return s_local_web_start; }
bool wallas_timer_active(void)    { return s_timer_active; }

/* ── Weekly timer (same table and rules as MasterRemote's, §6.1a) ─────────── */
#define NTP_MIN_VALID_EPOCH  1704067200   /* 2024-01-01 UTC */
#define WALLAS_TZ  "CET-1CEST,M3.5.0,M10.5.0/3"   /* Europe/Oslo, auto DST */
#define TIMER_SYNC_INTERVAL_US  ((int64_t)60 * 1000000)

typedef struct {
    bool    enabled;
    uint8_t start_hh, start_mm;
    uint8_t stop_hh,  stop_mm;
} day_timer_t;

static day_timer_t s_wallas_timer[7] = {0};   /* [0] = Sunday, like struct tm.tm_wday */
static uint32_t    s_timer_rev       = 0;
static bool        s_ntp_synced      = false;
static volatile int64_t   s_timer_sync_due_us = 0;
static wallas_timer_msg_t s_timer_in;
static volatile bool      s_timer_in_pending  = false;

static void timer_nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = sizeof(s_wallas_timer);
    nvs_get_blob(h, "walltimer", s_wallas_timer, &sz);
    nvs_get_u32(h, "walltrev", &s_timer_rev);
    nvs_close(h);
}

static void timer_nvs_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "walltimer", s_wallas_timer, sizeof(s_wallas_timer));
    nvs_set_u32(h, "walltrev", s_timer_rev);
    nvs_commit(h);
    nvs_close(h);
}

/* Edit time once NTP has synced, so the newer of the two units' edits wins;
 * never lower than rev + 1, so it still rises before sync. */
static uint32_t timer_next_rev(void)
{
    uint32_t now = (uint32_t)time(NULL);
    return (now >= NTP_MIN_VALID_EPOCH && now > s_timer_rev) ? now : s_timer_rev + 1;
}

void timer_get_day(int day, bool *enabled, int *start_hh, int *start_mm, int *stop_hh, int *stop_mm)
{
    if (day < 0 || day > 6) { *enabled = false; *start_hh = *start_mm = *stop_hh = *stop_mm = 0; return; }
    const day_timer_t *d = &s_wallas_timer[day];
    *enabled  = d->enabled;
    *start_hh = d->start_hh; *start_mm = d->start_mm;
    *stop_hh  = d->stop_hh;  *stop_mm  = d->stop_mm;
}

void timer_set_day(int day, bool enabled, int start_hh, int start_mm, int stop_hh, int stop_mm)
{
    if (day < 0 || day > 6) return;
    if (start_hh < 0 || start_hh > 23 || start_mm < 0 || start_mm > 59 ||
        stop_hh  < 0 || stop_hh  > 23 || stop_mm  < 0 || stop_mm  > 59) return;
    day_timer_t *d = &s_wallas_timer[day];
    d->enabled  = enabled;
    d->start_hh = (uint8_t)start_hh; d->start_mm = (uint8_t)start_mm;
    d->stop_hh  = (uint8_t)stop_hh;  d->stop_mm  = (uint8_t)stop_mm;
    s_timer_rev = timer_next_rev();
    timer_nvs_save();
    s_timer_sync_due_us = esp_timer_get_time() + 1000000;   /* one send after the 7-day burst */
}

bool     timer_is_synced(void) { return s_ntp_synced; }
uint32_t timer_get_rev(void)   { return s_timer_rev; }

void timer_get_now_str(char *buf, size_t sz)
{
    if (!s_ntp_synced) { snprintf(buf, sz, "not synced"); return; }
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(buf, sz, "%Y-%m-%d %H:%M:%S", &tm_now);
}

static void ntp_init(void)
{
    setenv("TZ", WALLAS_TZ, 1);
    tzset();
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&cfg);
}

static bool timer_window_active(void)
{
    if (!s_ntp_synced) return false;
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    const day_timer_t *d = &s_wallas_timer[tm_now.tm_wday];
    if (!d->enabled) return false;
    int cur   = tm_now.tm_hour * 60 + tm_now.tm_min;
    int start = d->start_hh * 60 + d->start_mm;
    int stop  = d->stop_hh  * 60 + d->stop_mm;
    return (start <= stop) ? (cur >= start && cur < stop)
                           : (cur >= start || cur < stop);   /* wraps past midnight */
}

static void timer_fill_msg(wallas_timer_msg_t *m)
{
    memset(m, 0, sizeof(*m));
    strlcpy(m->label, "SlaveWallas->Timer", sizeof(m->label));
    m->rev = s_timer_rev;
    for (int d = 0; d < 7; d++) {
        m->day[d][0] = s_wallas_timer[d].enabled ? 1 : 0;
        m->day[d][1] = s_wallas_timer[d].start_hh; m->day[d][2] = s_wallas_timer[d].start_mm;
        m->day[d][3] = s_wallas_timer[d].stop_hh;  m->day[d][4] = s_wallas_timer[d].stop_mm;
    }
}

static bool timer_apply_msg(const wallas_timer_msg_t *m)
{
    if (m->rev <= s_timer_rev) return false;
    for (int d = 0; d < 7; d++) {
        const uint8_t *x = m->day[d];
        if (x[1] > 23 || x[2] > 59 || x[3] > 23 || x[4] > 59) continue;
        s_wallas_timer[d].enabled  = x[0] != 0;
        s_wallas_timer[d].start_hh = x[1]; s_wallas_timer[d].start_mm = x[2];
        s_wallas_timer[d].stop_hh  = x[3]; s_wallas_timer[d].stop_mm  = x[4];
    }
    s_timer_rev = m->rev;
    timer_nvs_save();
    return true;
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
    g_wallas_running = gpio_get_level(PIN_WALLAS_FB);
    slave_wallas_msg_t msg = {0};
    strlcpy(msg.label, "SlaveWallas->Master", sizeof(msg.label));
    msg.WallasRunning = g_wallas_running;
    msg.WallasStart   = g_start_cmd;
    msg.has_wifi      = true;   /* WiFi is mandatory for this unit */
    msg.rssi          = g_espnow_rssi;
    msg.channel       = g_espnow_channel;
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
    if (len == (int)sizeof(wallas_timer_msg_t)) {
        if (!g_master_known || memcmp(info->src_addr, g_master_mac, 6) != 0) return;
        if (strncmp((const char *)data, "Master->WallasTimer", 19) != 0) return;
        if (s_timer_in_pending) return;   /* the next periodic send covers it */
        memcpy(&s_timer_in, data, sizeof(s_timer_in));
        s_timer_in_pending = true;
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
    ESP_LOGI(TAG, "Recv WallasStart=%d", msg.HondaStart);
    s_master_cmd       = msg.HondaStart;
    s_master_cmd_valid = true;
    apply_relay();
    send_status();
}

static void send_timer_to_master(void)
{
    wallas_timer_msg_t m;
    timer_fill_msg(&m);
    esp_now_send(g_master_mac, (uint8_t *)&m, sizeof(m));
}

static void send_webcmd_to_master(bool start)
{
    wallas_webcmd_msg_t m = {0};
    strlcpy(m.label, "SlaveWallas->WebCmd", sizeof(m.label));
    m.start = start ? 1 : 0;
    esp_now_send(g_master_mac, (uint8_t *)&m, sizeof(m));
}

/* Start/Stop Wallas on this unit's Status tab. With Master present it is the
 * same as pressing it on Master (so both pages agree); standalone it sets our
 * own start flag. */
void wallas_web_set(bool start)
{
    if (master_present()) {
        send_webcmd_to_master(start);
    } else {
        s_local_web_start = start;
        apply_relay();
    }
}

static void espnow_init(void) {
    esp_now_init();
    esp_now_register_send_cb(espnow_send_cb);
    esp_now_register_recv_cb(espnow_recv_cb);
    ESP_LOGI(TAG, "ESP-NOW ready, listening for MasterRemote beacon");
}

/* ── GPIO Init ───────────────────────────────────────────────────────────────── */
static void gpio_init_pins(void) {
    gpio_config_t out = {
        .pin_bit_mask = (1ULL<<PIN_WALLAS_RELAY)|(1ULL<<PIN_STATUS_LED)|(1ULL<<PIN_HEATER_LED),
        /* INPUT_OUTPUT (not plain OUTPUT): enables the read-back buffer so
         * gpio_get_level() returns the actual driven level -- a plain OUTPUT
         * pin always reads 0, which made /api/status's relay/led fields
         * permanently false regardless of true state (FSD v1.36). */
        .mode = GPIO_MODE_INPUT_OUTPUT, .intr_type = GPIO_INTR_DISABLE };
    gpio_config(&out);
    gpio_set_level(PIN_WALLAS_RELAY, 0);
    gpio_set_level(PIN_STATUS_LED,   0);
    gpio_set_level(PIN_HEATER_LED,   0);

    gpio_config_t in = {
        .pin_bit_mask = (1ULL<<PIN_WALLAS_FB),
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_DISABLE };
    gpio_config(&in);
}

/* ── Tasks ───────────────────────────────────────────────────────────────────── */
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

/* Mode transitions, own timer, and timer sync -- once a second. */
static void control_task(void *arg) {
    bool was_present = false;
    for (;;) {
        time_t now_s = time(NULL);
        if (!s_ntp_synced && now_s >= NTP_MIN_VALID_EPOCH) {
            s_ntp_synced = true;
            ESP_LOGI(TAG, "NTP synced");
        }

        bool present = master_present();
        if (present && !was_present) {
            ESP_LOGI(TAG, "MasterRemote present -- following it");
            /* A Start pressed here while standalone becomes Master's web start,
             * so the heater doesn't drop when Master takes over. Heartbeat
             * first, so a Master that lost its roster accepts the press. */
            send_status();
            if (s_local_web_start) send_webcmd_to_master(true);
            s_local_web_start   = false;
            s_timer_sync_due_us = 0;
        } else if (!present && was_present) {
            ESP_LOGW(TAG, "MasterRemote gone -- standalone");
            s_master_cmd_valid = false;
        }
        was_present = present;

        s_timer_active = !present && timer_window_active();
        apply_relay();

        if (s_timer_in_pending) {
            if (timer_apply_msg(&s_timer_in))
                ESP_LOGI(TAG, "Weekly timer taken from MasterRemote (rev %lu)", (unsigned long)s_timer_rev);
            else if (s_timer_in.rev < s_timer_rev)
                s_timer_sync_due_us = 0;   /* ours is newer -- send it back now */
            s_timer_in_pending = false;
        }
        int64_t now = esp_timer_get_time();
        if (present && now >= s_timer_sync_due_us) {
            s_timer_sync_due_us = now + TIMER_SYNC_INTERVAL_US;
            send_timer_to_master();
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void heartbeat_task(void *arg) {
    for (;;) {
        /* Fast blink when start commanded (by Master, or locally when standalone), slow when idle. Keyed off
         * g_start_cmd (not g_wallas_running) since the running-feedback sensor
         * (PIN_WALLAS_FB) may not be wired up. */
        uint32_t half_ms = g_start_cmd ? 200 : 1000;
        gpio_set_level(PIN_STATUS_LED,
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
    wifi_init_and_connect();
    espnow_init();

    /* Standalone support: own clock and weekly timer (WiFi is up by now) */
    ntp_init();
    timer_nvs_load();

    xTaskCreate(status_task,       "status",   3072, NULL, 4, NULL);
    xTaskCreate(heartbeat_task,    "heartbeat",1024, NULL, 2, NULL);
    xTaskCreate(config_apply_task, "cfgapply", 2048, NULL, 3, NULL);
    xTaskCreate(control_task,      "control",  3072, NULL, 4, NULL);

    ESP_LOGI(TAG, "Ready");
}
