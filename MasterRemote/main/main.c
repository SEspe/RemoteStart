/*
 * MasterRemote — ESP32 Master Remote Start Controller
 * Author: Stein Espe
 * Framework: ESP-IDF v6.x
 *
 * Build:  idf.py build
 * Flash:  idf.py -p COMx flash monitor
 * OTA:    http://<device-ip>/ota  (after first WiFi setup)
 *
 * First startup: connect to "MasterRemote-Config" WiFi (pw: honda1234),
 *   open 192.168.4.1, enter your router credentials.
 *
 * Peer discovery: no custom/hardcoded MAC addresses. Each unit uses its own
 * factory MAC. MasterRemote broadcasts a beacon; slaves that hear it learn
 * Master's MAC and register themselves via their normal heartbeat — see
 * FSD_RemoteStart.md §3.2.
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

/* ── Peer Discovery ────────────────────────────────────────────────────────── */
static const uint8_t BROADCAST_MAC[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
#define BEACON_INTERVAL_MS   2000
#define NODE_TIMEOUT_MS      30000

/* ── Pin Definitions ───────────────────────────────────────────────────────── */
#define PIN_HONDA_STARTED_FB     GPIO_NUM_13   /* Output: Honda running LED   */
#define PIN_STATUS_LED           GPIO_NUM_2    /* Output: onboard heartbeat   */
#define PIN_HONDA_START          GPIO_NUM_15   /* Input:  Victron relay       */
#define PIN_HONDA_MANUAL_START   GPIO_NUM_14   /* Input:  manual push (PU)    */
#define PIN_WALLAS_START         GPIO_NUM_4    /* Input:  Victron relay       */
#define PIN_WALLAS_MANUAL_START  GPIO_NUM_5    /* Input:  manual push (PU)    */

/* ── Timing (ms) ───────────────────────────────────────────────────────────── */
#define HONDA_RESTART_BLOCK_MS   30000
#define WALLAS_SEND_INTERVAL_MS  15000

/* ── Message Structures (shared with slaves — see FSD §3.3) ───────────────── */
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
    int8_t  rssi;
    uint8_t channel;
    char    fw_version[12];
} slave_honda_msg_t;

typedef struct {
    char  label[32];
    bool  WallasRunning;
    bool  WallasStart;
    char  ip[16];
    bool  has_wifi;
    int8_t  rssi;
    uint8_t channel;
    char    fw_version[12];
} slave_wallas_msg_t;

/* ── Global State (written from ISR + read from task) ──────────────────────── */
volatile bool g_honda_start_cmd  = false;
volatile bool g_wallas_start_cmd = false;
         bool g_slave_honda_running = false;

/* Web UI manual-start override — a third source OR'd in alongside the Victron
 * relay and physical manual button. Set/cleared by web_server.c's
 * /api/honda/start|stop and /api/wallas/start|stop handlers. */
volatile bool g_web_honda_start  = false;
volatile bool g_web_wallas_start = false;

/* ── Slave Roster (populated dynamically by ESP-NOW recv callback) ─────────── */
typedef struct {
    uint8_t  mac[6];
    bool     peer_added;
    char     ip[16];
    bool     has_wifi;
    int64_t  last_seen_us;
    bool     honda_ign_on;
    bool     honda_starting;
    bool     honda_running;
    bool     wallas_running;
    bool     wallas_start_cmd;
    int8_t   rssi;
    uint8_t  channel;
    char     fw_version[12];
} slave_info_t;

slave_info_t g_slave_honda  = {0};
slave_info_t g_slave_wallas = {0};

/* ── WiFi / provisioning ────────────────────────────────────────────────────── */
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
static EventGroupHandle_t  s_wifi_eg;
static int                 s_retry_count = 0;
#define MAX_RETRY 5

/* ── NVS helpers ────────────────────────────────────────────────────────────── */
static esp_err_t nvs_load_wifi(char *ssid, char *pass, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t sl = len, pl = len;
    err  = nvs_get_str(h, "ssid", ssid, &sl);
    err |= nvs_get_str(h, "pass", pass, &pl);
    nvs_close(h);
    return err;
}

static void nvs_save_wifi(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ssid", ssid);
        nvs_set_str(h, "pass", pass);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "WiFi credentials saved");
    }
}

/* ── WiFi Event Handler ─────────────────────────────────────────────────────── */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            if (s_retry_count < MAX_RETRY) {
                esp_wifi_connect();
                s_retry_count++;
                ESP_LOGI(TAG, "WiFi retry %d/%d", s_retry_count, MAX_RETRY);
            } else {
                xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
}

/* ── SoftAP config portal (served by web_server.c) ─────────────────────────── */
/* web_server.c provides /wifi-setup page; credentials posted to /wifi-save    */
/* After save, this flag triggers NVS write + restart.                          */
volatile bool g_wifi_save_requested = false;
char          g_new_ssid[64] = {0};
char          g_new_pass[64] = {0};
static bool   g_portal_mode  = false;

static void start_config_portal(void)
{
    ESP_LOGI(TAG, "Starting config portal AP: %s", WIFI_AP_SSID);

    esp_netif_create_default_wifi_ap();
    wifi_config_t ap_cfg = {
        .ap = {
            .ssid           = WIFI_AP_SSID,
            .password       = WIFI_AP_PASSWORD,
            .ssid_len       = strlen(WIFI_AP_SSID),
            .channel        = 1,
            .authmode       = WIFI_AUTH_WPA2_PSK,
            .max_connection = 4,
        }
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    web_server_start();   /* serves /wifi-setup and /wifi-save */

    /* Wait until user submits credentials */
    while (!g_wifi_save_requested) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    nvs_save_wifi(g_new_ssid, g_new_pass);
    ESP_LOGI(TAG, "Credentials saved, restarting…");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

/* ── WiFi Init ──────────────────────────────────────────────────────────────── */
static void wifi_init_and_connect(void)
{
    s_wifi_eg = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));   /* own factory MAC, never overridden */

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,  &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);

    char ssid[64] = {0}, pass[64] = {0};
    if (nvs_load_wifi(ssid, pass, sizeof(ssid)) == ESP_OK && ssid[0] != '\0') {
        wifi_config_t sta_cfg = {0};
        strlcpy((char *)sta_cfg.sta.ssid,     ssid, sizeof(sta_cfg.sta.ssid));
        strlcpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
        ESP_ERROR_CHECK(esp_wifi_start());

        EventBits_t bits = xEventGroupWaitBits(s_wifi_eg,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(15000));

        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "WiFi connected");
            web_server_start();
            return;
        }
        ESP_LOGW(TAG, "WiFi connect failed, opening portal");
        esp_wifi_stop();
    }

    /* No credentials or connect failed → open config portal (WiFi is mandatory for Master) */
    g_portal_mode = true;
    start_config_portal();
    /* Never returns — portal restarts device after save */
}

/* ── ESP-NOW ────────────────────────────────────────────────────────────────── */
static void espnow_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t st)
{
    /* Silently discard; log if needed for debug */
}

static void espnow_add_peer(const uint8_t *mac)
{
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;   /* 0 = use current WiFi channel */
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) != ESP_OK)
        ESP_LOGE(TAG, "Failed to add peer " MACSTR, MAC2STR(mac));
}

/* Register (or re-register, if the MAC changed — e.g. hardware swap) a slave's
 * real MAC as an ESP-NOW peer. See FSD §3.2. */
static void register_peer(slave_info_t *info, const uint8_t *mac)
{
    if (info->peer_added && memcmp(info->mac, mac, 6) == 0) return;
    if (info->peer_added) esp_now_del_peer(info->mac);
    espnow_add_peer(mac);
    memcpy(info->mac, mac, 6);
    info->peer_added = true;
    ESP_LOGI(TAG, "Registered peer " MACSTR, MAC2STR(mac));
}

static bool node_connected(const slave_info_t *info)
{
    return info->peer_added &&
           (esp_timer_get_time() - info->last_seen_us) < (int64_t)NODE_TIMEOUT_MS * 1000;
}

static void espnow_recv_cb(const esp_now_recv_info_t *info,
                           const uint8_t *data, int len)
{
    const uint8_t *src = info->src_addr;

    if (len == (int)sizeof(slave_honda_msg_t)) {
        slave_honda_msg_t msg;
        memcpy(&msg, data, sizeof(msg));
        if (strncmp(msg.label, "SlaveHonda", 10) == 0) {
            register_peer(&g_slave_honda, src);
            g_slave_honda.last_seen_us   = esp_timer_get_time();
            g_slave_honda.honda_ign_on   = msg.HondaIgnitionOn;
            g_slave_honda.honda_starting = msg.HondaStarting;
            g_slave_honda.honda_running  = msg.HondaRunning;
            g_slave_honda.has_wifi       = msg.has_wifi;
            g_slave_honda.rssi           = msg.rssi;
            g_slave_honda.channel        = msg.channel;
            strlcpy(g_slave_honda.ip, msg.ip, sizeof(g_slave_honda.ip));
            strlcpy(g_slave_honda.fw_version, msg.fw_version, sizeof(g_slave_honda.fw_version));
            g_slave_honda_running = msg.HondaRunning;
            gpio_set_level(PIN_HONDA_STARTED_FB, msg.HondaRunning ? 1 : 0);
            return;
        }
    }
    if (len == (int)sizeof(slave_wallas_msg_t)) {
        slave_wallas_msg_t msg;
        memcpy(&msg, data, sizeof(msg));
        if (strncmp(msg.label, "SlaveWallas", 11) == 0) {
            register_peer(&g_slave_wallas, src);
            g_slave_wallas.last_seen_us  = esp_timer_get_time();
            g_slave_wallas.wallas_running = msg.WallasRunning;
            g_slave_wallas.has_wifi       = msg.has_wifi;
            g_slave_wallas.rssi           = msg.rssi;
            g_slave_wallas.channel        = msg.channel;
            strlcpy(g_slave_wallas.ip, msg.ip, sizeof(g_slave_wallas.ip));
            strlcpy(g_slave_wallas.fw_version, msg.fw_version, sizeof(g_slave_wallas.fw_version));
        }
    }
}

static void espnow_init(void)
{
    ESP_ERROR_CHECK(esp_now_init());
    esp_now_register_send_cb(espnow_send_cb);
    esp_now_register_recv_cb(espnow_recv_cb);
    espnow_add_peer(BROADCAST_MAC);   /* needed to send the discovery beacon */
    ESP_LOGI(TAG, "ESP-NOW ready");
}

static void send_beacon(void)
{
    master_beacon_t beacon = {0};
    strlcpy(beacon.label, "MasterRemote", sizeof(beacon.label));
    esp_now_send(BROADCAST_MAC, (uint8_t *)&beacon, sizeof(beacon));
}

static void send_to_honda(void)
{
    if (!node_connected(&g_slave_honda)) return;   /* nothing to send to yet */
    master_msg_t msg = {0};
    strlcpy(msg.label, "Master->SlaveHonda", sizeof(msg.label));
    msg.HondaRunningFB  = g_slave_honda_running;
    msg.HondaIgnitionOn = gpio_get_level(PIN_HONDA_MANUAL_START);
    msg.HondaStart      = g_honda_start_cmd;
    esp_now_send(g_slave_honda.mac, (uint8_t *)&msg, sizeof(msg));
    vTaskDelay(pdMS_TO_TICKS(20));
    esp_now_send(g_slave_honda.mac, (uint8_t *)&msg, sizeof(msg)); /* redundant */
    ESP_LOGI(TAG, "Honda CMD: start=%d", msg.HondaStart);
}

static void send_to_wallas(void)
{
    if (!node_connected(&g_slave_wallas)) return;   /* nothing to send to yet */
    master_msg_t msg = {0};
    strlcpy(msg.label, "Master->SlaveWallas", sizeof(msg.label));
    msg.HondaStart = g_wallas_start_cmd;
    g_slave_wallas.wallas_start_cmd = g_wallas_start_cmd;
    esp_now_send(g_slave_wallas.mac, (uint8_t *)&msg, sizeof(msg));
    ESP_LOGI(TAG, "Wallas CMD: start=%d", msg.HondaStart);
}

/* ── GPIO ISR ───────────────────────────────────────────────────────────────── */
static void IRAM_ATTR isr_honda_start(void *arg)
{
    g_honda_start_cmd = (gpio_get_level(PIN_HONDA_MANUAL_START) ||
                         !gpio_get_level(PIN_HONDA_START) || g_web_honda_start);
}
static void IRAM_ATTR isr_honda_manual(void *arg)
{
    g_honda_start_cmd = (gpio_get_level(PIN_HONDA_MANUAL_START) || g_web_honda_start);
}
static void IRAM_ATTR isr_wallas_start(void *arg)
{
    g_wallas_start_cmd = (gpio_get_level(PIN_WALLAS_START) || g_web_wallas_start);
}
static void IRAM_ATTR isr_wallas_manual(void *arg)
{
    g_wallas_start_cmd = (!gpio_get_level(PIN_WALLAS_MANUAL_START) || g_web_wallas_start);
}

static void gpio_init(void)
{
    /* Outputs */
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_HONDA_STARTED_FB) | (1ULL << PIN_STATUS_LED),
        .mode         = GPIO_MODE_OUTPUT,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&out);
    gpio_set_level(PIN_HONDA_STARTED_FB, 0);
    gpio_set_level(PIN_STATUS_LED, 0);

    /* Inputs without pull-up (driven by Victron relay) */
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << PIN_HONDA_START) | (1ULL << PIN_WALLAS_START),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&in);

    /* Inputs with internal pull-up (manual push buttons) */
    gpio_config_t in_pu = {
        .pin_bit_mask = (1ULL << PIN_HONDA_MANUAL_START) | (1ULL << PIN_WALLAS_MANUAL_START),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&in_pu);

    /* Read initial states */
    g_honda_start_cmd  = !gpio_get_level(PIN_HONDA_START);
    g_wallas_start_cmd =  gpio_get_level(PIN_WALLAS_START);

    /* Install ISR service and attach handlers */
    gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    gpio_isr_handler_add(PIN_HONDA_START,        isr_honda_start,  NULL);
    gpio_isr_handler_add(PIN_HONDA_MANUAL_START, isr_honda_manual, NULL);
    gpio_isr_handler_add(PIN_WALLAS_START,       isr_wallas_start, NULL);
    gpio_isr_handler_add(PIN_WALLAS_MANUAL_START,isr_wallas_manual,NULL);
}

/* ── Beacon Task ────────────────────────────────────────────────────────────── */
static void beacon_task(void *arg)
{
    for (;;) {
        send_beacon();
        vTaskDelay(pdMS_TO_TICKS(BEACON_INTERVAL_MS));
    }
}

/* ── Main Control Task ──────────────────────────────────────────────────────── */
static void master_task(void *arg)
{
    int64_t last_honda_send_us  = 0;
    int64_t last_wallas_send_us = 0;

    for (;;) {
        int64_t now = esp_timer_get_time();

        /* Honda: send when desired ≠ actual and restart block has elapsed */
        if ((g_honda_start_cmd != g_slave_honda_running) &&
            ((now - last_honda_send_us) >= (int64_t)HONDA_RESTART_BLOCK_MS * 1000)) {
            last_honda_send_us = now;
            send_to_honda();
        }

        /* Wallas: periodic refresh */
        if ((now - last_wallas_send_us) >= (int64_t)WALLAS_SEND_INTERVAL_MS * 1000) {
            last_wallas_send_us = now;
            send_to_wallas();
        }

        /* Heartbeat LED: 1 Hz */
        gpio_set_level(PIN_STATUS_LED,
                       (esp_timer_get_time() / 500000) % 2 == 0 ? 1 : 0);

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ── app_main ───────────────────────────────────────────────────────────────── */
void app_main(void)
{
    ESP_LOGI(TAG, "=== %s v%s ===", FIRMWARE_NAME, FIRMWARE_VERSION);

    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* GPIO */
    gpio_init();

    /* WiFi → if no credentials: opens portal (blocks until saved + restart) */
    wifi_init_and_connect();

    /* ESP-NOW (must be after WiFi connected for correct channel) */
    espnow_init();

    /* Beacon + main control logic tasks */
    xTaskCreate(beacon_task, "beacon", 2048, NULL, 4, NULL);
    xTaskCreate(master_task, "master", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Ready");
}
