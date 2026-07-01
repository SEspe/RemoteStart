/*
 * SlaveWallas — ESP32-C6 Wallas Heater Slave
 * Author: Stein Espe
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
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

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

/* ── Global State ────────────────────────────────────────────────────────────── */
volatile bool g_start_cmd     = false;
         bool g_wallas_running = false;

/* ── Master discovery state ─────────────────────────────────────────────────── */
static uint8_t s_master_mac[6]  = {0};
static bool    s_master_known   = false;
static int8_t  s_link_rssi      = 0;   /* last frame heard from MasterRemote */
static uint8_t s_link_channel   = 0;

/* ── WiFi ────────────────────────────────────────────────────────────────────── */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static EventGroupHandle_t s_wifi_eg;
static int s_retry = 0;
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

static void wifi_event_handler(void *a, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START && !g_portal_mode) esp_wifi_connect();
        else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            if (s_retry++ < MAX_RETRY) esp_wifi_connect();
            else xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry = 0;
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

static void wifi_init_and_connect(void) {
    s_wifi_eg = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_STA);   /* own factory MAC, never overridden */
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,    &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);

    char ssid[64]={0}, pass[64]={0};
    if (nvs_load_wifi(ssid, pass, sizeof(ssid)) == ESP_OK && ssid[0]) {
        wifi_config_t sta = {0};
        strlcpy((char*)sta.sta.ssid,     ssid, sizeof(sta.sta.ssid));
        strlcpy((char*)sta.sta.password, pass, sizeof(sta.sta.password));
        esp_wifi_set_config(WIFI_IF_STA, &sta);
        esp_wifi_start();
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
    if (!s_master_known) return;   /* Master not discovered yet — nothing to send to */
    g_wallas_running = gpio_get_level(PIN_WALLAS_FB);
    slave_wallas_msg_t msg = {0};
    strlcpy(msg.label, "SlaveWallas->Master", sizeof(msg.label));
    msg.WallasRunning = g_wallas_running;
    msg.WallasStart   = g_start_cmd;
    msg.has_wifi      = true;   /* WiFi is mandatory for this unit */
    msg.rssi          = s_link_rssi;
    msg.channel       = s_link_channel;
    strlcpy(msg.fw_version, FIRMWARE_VERSION, sizeof(msg.fw_version));
    get_ip_str(msg.ip, sizeof(msg.ip));
    esp_now_send(s_master_mac, (uint8_t *)&msg, sizeof(msg));
}

static void espnow_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t st) {}

static void espnow_recv_cb(const esp_now_recv_info_t *info,
                           const uint8_t *data, int len)
{
    if (info->rx_ctrl) {
        s_link_rssi    = info->rx_ctrl->rssi;
        s_link_channel = info->rx_ctrl->channel;
    }

    if (len == (int)sizeof(master_beacon_t)) {
        master_beacon_t beacon;
        memcpy(&beacon, data, sizeof(beacon));
        if (strncmp(beacon.label, "MasterRemote", 12) == 0 &&
            (!s_master_known || memcmp(info->src_addr, s_master_mac, 6) != 0)) {
            memcpy(s_master_mac, info->src_addr, 6);
            espnow_add_peer(s_master_mac);
            s_master_known = true;
            ESP_LOGI(TAG, "Master found: " MACSTR, MAC2STR(s_master_mac));
            send_status();   /* immediate registration heartbeat */
        }
        return;
    }
    if (len != (int)sizeof(master_msg_t)) return;
    master_msg_t msg;
    memcpy(&msg, data, sizeof(msg));
    ESP_LOGI(TAG, "Recv WallasStart=%d", msg.HondaStart);
    g_start_cmd = msg.HondaStart;
    if (g_start_cmd) wallas_start(); else wallas_stop();
    send_status();
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
        .mode = GPIO_MODE_OUTPUT, .intr_type = GPIO_INTR_DISABLE };
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
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(STATUS_SEND_MS));
        send_status();
    }
}

static void heartbeat_task(void *arg) {
    for (;;) {
        /* Fast blink when start commanded by Master, slow when idle. Keyed off
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

    xTaskCreate(status_task,    "status",    3072, NULL, 4, NULL);
    xTaskCreate(heartbeat_task, "heartbeat", 1024, NULL, 2, NULL);

    ESP_LOGI(TAG, "Ready");
}
