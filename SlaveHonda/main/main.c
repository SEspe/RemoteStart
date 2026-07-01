/*
 * SlaveHonda — ESP32 Honda EU70IS Generator Slave
 * Author: Stein Espe
 * Framework: ESP-IDF v5.x
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

/* ── Custom MACs ─────────────────────────────────────────────────────────────── */
static const uint8_t SLAVE_MAC[]  = {0x30,0xAE,0xA4,0x1A,0xAE,0x33};
static const uint8_t MASTER_MAC[] = {0x30,0xAE,0xA4,0x89,0x92,0x7A};

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

/* ── Message Structures ──────────────────────────────────────────────────────── */
typedef struct {
    char  label[32];
    bool  HondaRunningFB;
    bool  HondaIgnitionOn;
    bool  HondaStart;
} master_msg_t;

typedef struct {
    char  label[32];
    bool  HondaIgnitionOn;
    bool  HondaStarting;
    bool  HondaRunning;
} slave_msg_t;

/* ── Global State ────────────────────────────────────────────────────────────── */
volatile bool g_start_cmd    = false;
         bool g_honda_running = false;
         bool g_honda_starting= false;
         bool g_honda_ign_on  = false;

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

static void wifi_init_and_connect(void) {
    s_wifi_eg = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_mac(WIFI_IF_STA, SLAVE_MAC);
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
        if (b & WIFI_CONNECTED_BIT) { web_server_start(); return; }
        ESP_LOGW(TAG, "Connect failed, opening portal");
        esp_wifi_stop();
    }
    g_portal_mode = true;
    start_config_portal();
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

/* ── ESP-NOW Peer and Send ───────────────────────────────────────────────────── */
static esp_now_peer_info_t s_peer_master;

static void send_status(void) {
    g_honda_running = !gpio_get_level(PIN_RUNNING_FB);
    slave_msg_t msg = {0};
    strlcpy(msg.label, "SlaveHonda->Master", sizeof(msg.label));
    msg.HondaIgnitionOn = g_honda_ign_on;
    msg.HondaStarting   = g_honda_starting;
    msg.HondaRunning    = g_honda_running;
    esp_now_send(MASTER_MAC, (uint8_t *)&msg, sizeof(msg));
}

static void espnow_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t st) {}

static void espnow_recv_cb(const esp_now_recv_info_t *info,
                           const uint8_t *data, int len)
{
    if (len < (int)sizeof(master_msg_t)) return;
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
    memset(&s_peer_master, 0, sizeof(s_peer_master));
    memcpy(s_peer_master.peer_addr, MASTER_MAC, 6);
    s_peer_master.channel = 0;
    s_peer_master.encrypt = false;
    esp_now_add_peer(&s_peer_master);
    ESP_LOGI(TAG, "ESP-NOW ready");
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
    wifi_init_and_connect();
    espnow_init();

    xTaskCreate(status_task,    "status",    3072, NULL, 4, NULL);
    xTaskCreate(heartbeat_task, "heartbeat", 1024, NULL, 2, NULL);

    ESP_LOGI(TAG, "Ready");
}
