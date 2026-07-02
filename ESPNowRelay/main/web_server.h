#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* Shared relay state — defined in main.c, rendered by web_server.c.
 * Written from the ESP-NOW recv callback (WiFi task) and read from the httpd
 * task without locking: same benign-torn-read pattern as the other units'
 * status fields (worst case a momentarily inconsistent row in the JSON). */

typedef enum {
    TC_BEACON = 0,   /* MasterRemote discovery beacon (re-broadcast)       */
    TC_CMD_HONDA,    /* Master -> SlaveHonda command (forwarded unicast)   */
    TC_CMD_WALLAS,   /* Master -> SlaveWallas command (forwarded unicast)  */
    TC_HB_HONDA,     /* SlaveHonda heartbeat (forwarded to Master)         */
    TC_HB_WALLAS,    /* SlaveWallas heartbeat (forwarded to Master)        */
    TC_OTHER,        /* unrecognized size/label/sender — counted, never relayed */
    TC_COUNT
} traffic_class_t;

typedef struct {
    uint32_t rx_count;       /* frames of this class received               */
    uint32_t fwd_count;      /* successfully handed to esp_now_send()       */
    uint32_t fwd_fail;       /* send failed, or no destination known yet    */
    uint8_t  last_src[6];    /* sender of the most recent frame             */
    uint8_t  last_dst[6];    /* where the most recent forward went          */
    bool     has_dst;
    int8_t   last_rssi;      /* RSSI of the most recent frame               */
    int64_t  last_seen_us;   /* esp_timer timestamp of the most recent frame */
} traffic_stat_t;

typedef struct {
    uint8_t  mac[6];
    bool     known;
    int64_t  last_seen_us;
    int8_t   rssi;           /* RSSI of last frame heard from this peer     */
} relay_peer_t;

extern traffic_stat_t g_traffic[TC_COUNT];
extern relay_peer_t   g_peer_master, g_peer_honda, g_peer_wallas;

esp_err_t web_server_start(void);
