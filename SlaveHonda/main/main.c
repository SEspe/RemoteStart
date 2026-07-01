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
 *   GPIO  2  OUTPUT — Onboard LED (heartbeat)
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
#define PIN_RUNNING_FB      GPIO_NUM_15   /* LOW = engine running      */
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
volatile bool g_start_cmd    = false;
         bool g_honda_running = false;
         bool g_honda_starting= false;
         bool g_honda_ign_on  = false;
         bool g_has_wifi      = false;

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
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_STA);   /* own factory MAC, never overridden */
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,   &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP,&wifi_event_handler, NULL);

    char ssid[64]={0}, pass[64]={0};
    if (nvs_load_wifi(ssid, pass, sizeof(ssid)) == ESP_OK && ssid[0]) {
        wifi_config_t sta = {0};
        strlcpy((char*)sta.sta.ssid,     ssid, sizeof(sta.sta.ssid));
        strlcpy((char*)sta.sta.password, pass, sizeof(sta.sta.password));
        esp_wifi_set_config(WIFI_IF_STA, &sta);
        esp_wifi_start();
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
static void honda_start(void) {
    if (g_honda_starting || g_honda_running) return;
    g_honda_starting = true;
    g_honda_ign_on   = true;
    ESP_LOGI(TAG, "Ignition ON");
    gpio_set_level(PIN_IGNITION_RELAY, 0);   /* active LOW */
    gpio_set_level(PIN_LED_EXT,        1);
    vTaskDelay(pdMS_TO_TICKS(HONDA_IGN_WARMUP_MS));
    ESP_LOGI(TAG, "Cranking...");
    gpio_set_level(PIN_STARTER_RELAY, 0);
    vTaskDelay(pdMS_TO_TICKS(HONDA_CRANK_MS));
    gpio_set_level(PIN_STARTER_RELAY, 1);
    vTaskDelay(pdMS_TO_TICKS(1000));
    g_honda_running  = !gpio_get_level(PIN_RUNNING_FB);
    g_honda_starting = false;
    ESP_LOGI(TAG, "Running: %d", g_honda_running);
}

static void honda_stop(void) {
    ESP_LOGI(TAG, "Stop");
    gpio_set_level(PIN_IGNITION_RELAY, 1);
    gpio_set_level(PIN_STARTER_RELAY,  1);
    gpio_set_level(PIN_LED_EXT,        0);
    g_honda_running  = false;
    g_honda_starting = false;
    g_honda_ign_on   = false;
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
    g_honda_running = !gpio_get_level(PIN_RUNNING_FB);
    slave_msg_t msg = {0};
    strlcpy(msg.label, "SlaveHonda->Master", sizeof(msg.label));
    msg.HondaIgnitionOn = g_honda_ign_on;
    msg.HondaStarting   = g_honda_starting;
    msg.HondaRunning    = g_honda_running;
    msg.has_wifi         = g_has_wifi;
    msg.rssi             = s_link_rssi;
    msg.channel          = s_link_channel;
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
    ESP_LOGI(TAG, "Recv HondaStart=%d", msg.HondaStart);
    g_start_cmd = msg.HondaStart;

    if (g_start_cmd) {
        if (!g_honda_running && !g_honda_starting)
            xTaskCreate((void(*)(void*))honda_start, "hstart", 4096, NULL, 5, NULL);
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
            while (!s_master_known &&
                   (esp_timer_get_time() - start) < (int64_t)SCAN_DWELL_MS * 1000) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            if (s_master_known) {
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
        .mode = GPIO_MODE_OUTPUT, .intr_type = GPIO_INTR_DISABLE };
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
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(STATUS_SEND_MS));
        send_status();
    }
}

/* ── Heartbeat Task ──────────────────────────────────────────────────────────── */
static void heartbeat_task(void *arg) {
    for (;;) {
        gpio_set_level(PIN_LED_ONBOARD,
                       (esp_timer_get_time() / 500000) % 2 == 0 ? 1 : 0);
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

    xTaskCreate(status_task,    "status",    3072, NULL, 4, NULL);
    xTaskCreate(heartbeat_task, "heartbeat", 1024, NULL, 2, NULL);

    ESP_LOGI(TAG, "Ready");
}
