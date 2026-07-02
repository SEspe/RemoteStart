/*
 * ESPNowRelay web_server.c
 * Serves: WiFi setup portal, relay traffic/peer status API, OTA upload.
 * Two-tab UI: Status | OTA Update
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/param.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "version.h"
#include "web_server.h"

static const char *TAG = "web";

extern volatile bool     g_wifi_save_requested;
extern char              g_new_ssid[64];
extern char              g_new_pass[64];
extern volatile uint32_t g_wifi_disconnect_count;
extern          uint32_t g_heap_min;
extern          int64_t  g_heap_min_ts_us;
extern          bool     g_has_wifi;

/* ── HTML ────────────────────────────────────────────────────────────────────── */
static const char WIFI_SETUP_HTML[] =
"<!DOCTYPE html><html><head><meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>WiFi Setup</title>"
"<style>"
"body{font-family:Arial,sans-serif;background:#1a1a2e;color:#eee;display:flex;"
"justify-content:center;align-items:center;min-height:100vh;margin:0}"
".box{background:#16213e;border-radius:10px;padding:24px;width:320px}"
"h2{color:#e94560;margin-bottom:16px}"
"label{display:block;margin-bottom:4px;font-size:.85rem}"
"input{width:100%;padding:8px;margin-bottom:12px;background:#0f3460;border:1px solid #444;"
"border-radius:5px;color:#eee;box-sizing:border-box}"
".btn{width:100%;padding:10px;border:none;border-radius:5px;color:#fff;"
"cursor:pointer;font-size:.9rem;margin-bottom:8px}"
".btn-s{background:#0f3460;border:1px solid #e94560}"
".btn-c{background:#e94560}"
".sts{font-size:.75rem;color:#888;min-height:1rem;margin-bottom:6px}"
".nets{max-height:180px;overflow-y:auto;margin-bottom:12px}"
".net{background:#0f3460;border-radius:5px;padding:8px 12px;margin-bottom:4px;"
"cursor:pointer;display:flex;justify-content:space-between;align-items:center;"
"font-size:.85rem}"
".net:hover{background:#1a4a80}"
".meta{font-size:.72rem;color:#aaa}"
".lk{color:#e94560}"
"</style></head>"
"<body><div class='box'>"
"<h2>&#9889; " FIRMWARE_NAME " WiFi</h2>"
"<button class='btn btn-s' onclick='doScan()'>&#128246; Scan Networks</button>"
"<div class='sts' id='sts'></div>"
"<div class='nets' id='nets'></div>"
"<form action='/wifi-save' method='POST'>"
"<label>SSID</label>"
"<input id='sid' name='ssid' type='text' placeholder='tap network or type' required>"
"<label>Password</label>"
"<input name='pass' type='password' placeholder='leave blank if open'>"
"<button class='btn btn-c' type='submit'>Connect &amp; Save</button>"
"</form>"
"<script>"
"function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;');}"
"function doScan(){"
"var st=document.getElementById('sts');"
"st.textContent='Scanning\\u2026';"
"document.getElementById('nets').innerHTML='';"
"fetch('/api/scan').then(function(r){return r.json();})"
".then(function(a){"
"st.textContent=a.length+' network'+(a.length!==1?'s':'')+' found';"
"var c=document.getElementById('nets');"
"a.forEach(function(n){"
"var d=document.createElement('div');d.className='net';"
"d.innerHTML='<span>'+esc(n.ssid)+'</span>"
"<span class=meta>'+n.rssi+'dBm "
"'+(n.auth?'<span class=lk>&#128274;</span>':'open')+'</span>';"
"d.onclick=function(){document.getElementById('sid').value=n.ssid;};"
"c.appendChild(d);});}).catch(function(){"
"st.textContent='Scan failed \\u2014 try again';});}"
"</script>"
"</div></body></html>";

static const char INDEX_HTML[] =
"<!DOCTYPE html><html lang='en'><head>"
"<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESPNowRelay</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:Arial,sans-serif;background:#1a1a2e;color:#eee}"
".hdr{background:#16213e;padding:12px 20px;display:flex;align-items:center;gap:14px;"
"border-bottom:2px solid #e94560}"
".hdr h1{font-size:1.1rem;color:#e94560}.hdr .v{font-size:.75rem;color:#888}"
".tabs{display:flex;background:#16213e}"
".tab{padding:10px 20px;cursor:pointer;color:#aaa;border:none;background:none;"
"font-size:.9rem;border-bottom:3px solid transparent}"
".tab.on{color:#e94560;border-bottom-color:#e94560;font-weight:bold}"
".pane{display:none;padding:18px}.pane.on{display:block}"
".card{background:#16213e;border-radius:8px;padding:14px;margin-bottom:14px}"
".card h3{color:#e94560;font-size:.9rem;margin-bottom:10px}"
".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:8px}"
".row{display:flex;justify-content:space-between;align-items:center;"
"background:#0f3460;border-radius:5px;padding:7px 12px;font-size:.83rem}"
".tbl{overflow-x:auto}"
"table.t{width:100%;border-collapse:collapse;font-size:.78rem}"
"table.t th,table.t td{padding:6px 8px;text-align:left;border-bottom:1px solid #0f3460;white-space:nowrap}"
"table.t th{color:#e94560}"
".ts{font-size:.7rem;color:#555;margin-top:6px}"
".dim{color:#666}"
".ota-wrap{background:#fff;border-radius:8px;padding:20px;color:#333}"
".ota-wrap h3{color:#e94560;margin-bottom:12px}"
".ota-wrap p{font-size:.85rem;color:#555;margin-bottom:14px}"
"input[type=file]{display:block;margin-bottom:12px}"
".prog{margin-top:10px;font-size:.85rem;color:#e94560;min-height:1.2em}"
"button.up{padding:8px 20px;background:#e94560;border:none;border-radius:5px;"
"color:#fff;cursor:pointer;font-size:.9rem}"
"</style></head><body>"
"<div class='hdr'><h1>&#9889; ESPNowRelay</h1>"
"<span class='v'>v" FIRMWARE_VERSION " &nbsp;|&nbsp; %IP%</span></div>"
"<div class='tabs'>"
"<button class='tab on' onclick='show(\"status\",this)'>Status</button>"
"<button class='tab' onclick='show(\"ota\",this)'>OTA Update</button>"
"</div>"
"<div id='status' class='pane on'>"
"<div class='card'><h3>ESP-NOW Traffic</h3><div class='tbl' id='traffic'>&#8230;</div></div>"
"<div class='card'><h3>Peers</h3><div class='tbl' id='peers'>&#8230;</div></div>"
"<div class='card'><h3>WiFi Link</h3><div class='grid' id='wifi'>&#8230;</div></div>"
"<div class='card'><h3>System</h3><div class='grid' id='sys'>&#8230;</div></div>"
"<div class='ts' id='sts'></div></div>"
"<div id='ota' class='pane'>"
"<div class='ota-wrap'>"
"<h3>OTA Firmware Update</h3>"
"<p>Device: " FIRMWARE_NAME " &nbsp;&#8212;&nbsp; v" FIRMWARE_VERSION "</p>"
"<p>Select a compiled <code>.bin</code> file and click Upload.</p>"
"<input type='file' id='fw' accept='.bin'>"
"<button class='up' onclick='upload()'>Upload &amp; Flash</button>"
"<div class='prog' id='prog'></div>"
"</div></div>"
"<script>"
"function show(id,btn){"
"document.querySelectorAll('.pane').forEach(e=>e.classList.remove('on'));"
"document.querySelectorAll('.tab').forEach(e=>e.classList.remove('on'));"
"document.getElementById(id).classList.add('on');btn.classList.add('on');}"
"function txtRow(n,v){return '<div class=\"row\"><span>'+n+'</span><span>'+v+'</span></div>';}"
"function fmtKB(n){return Math.round(n/1024)+' KB';}"
"function fmtUptime(s){var d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);"
"return (d?d+'d ':'')+(h||d?h+'h ':'')+m+'m';}"
"function fetchStatus(){"
"fetch('/api/status').then(r=>r.json()).then(d=>{"
"let h='<table class=\"t\"><tr><th>Traffic</th><th>Rx</th><th>Fwd</th><th>Fail</th>"
"<th>Last from</th><th>Last to</th><th>RSSI</th><th>Last seen</th></tr>';"
"d.traffic.forEach(t=>{h+='<tr><td>'+t.n+'</td><td>'+t.rx+'</td><td>'+t.fwd+'</td>"
"<td>'+t.fail+'</td><td>'+(t.src||'\\u2014')+'</td><td>'+(t.dst||'\\u2014')+'</td>"
"<td>'+(t.src?t.rssi+' dBm':'\\u2014')+'</td><td>'+t.ls+'</td></tr>';});"
"h+='</table>';"
"document.getElementById('traffic').innerHTML=h;"
"let p='<table class=\"t\"><tr><th>Peer</th><th>MAC</th><th>RSSI</th><th>Last seen</th></tr>';"
"d.peers.forEach(x=>{p+='<tr><td>'+x.n+'</td><td>'+(x.mac||'<span class=\"dim\">not discovered</span>')+'</td>"
"<td>'+(x.mac?x.rssi+' dBm':'\\u2014')+'</td><td>'+x.ls+'</td></tr>';});"
"p+='</table>';"
"document.getElementById('peers').innerHTML=p;"
"document.getElementById('wifi').innerHTML="
"txtRow('AP Signal (RSSI)',d.rssi?d.rssi+' dBm':'not associated')+txtRow('Channel',d.ch||'\\u2014');"
"document.getElementById('sts').textContent='Updated: '+new Date().toLocaleTimeString();"
"}).catch(()=>{});}"
"function fetchSys(){"
"fetch('/api/sysinfo').then(r=>r.json()).then(d=>{"
"document.getElementById('sys').innerHTML="
"txtRow('Free Heap',fmtKB(d.heap))+"
"txtRow('Min Free Heap (low-water)',fmtKB(d.heapMin)+' \\u2014 '+fmtUptime(d.heapMinAgo)+' ago')+"
"txtRow('Uptime',fmtUptime(d.uptime))+txtRow('WiFi Reconnects',d.wifiDisc);"
"}).catch(()=>{});}"
"function upload(){"
"const f=document.getElementById('fw').files[0];"
"if(!f){document.getElementById('prog').textContent='Select a .bin file first';return;}"
"const p=document.getElementById('prog');"
"const xhr=new XMLHttpRequest();"
"xhr.open('POST','/ota/upload',true);"
"xhr.setRequestHeader('Content-Type','application/octet-stream');"
"xhr.upload.onprogress=e=>{"
"if(e.lengthComputable)p.textContent='Uploading: '+Math.round(e.loaded/e.total*100)+'%';};"
"xhr.onload=()=>{p.textContent=xhr.status===200?'Success! Rebooting…':'Error: '+xhr.status;};"
"xhr.onerror=()=>{p.textContent='Upload error';};"
"xhr.send(f);}"
"setInterval(()=>{"
"if(document.getElementById('status').classList.contains('on')){fetchStatus();fetchSys();}"
"},2000);"
"fetchStatus();"
"fetchSys();"
"</script></body></html>";

/* ── Helpers ────────────────────────────────────────────────────────────────── */
static void fmt_ago(char *buf, size_t sz, int64_t ts_us)
{
    if (ts_us == 0) { snprintf(buf, sz, "Never"); return; }
    int64_t s = (esp_timer_get_time() - ts_us) / 1000000;
    if (s < 60) snprintf(buf, sz, "%llds ago", (long long)s);
    else        snprintf(buf, sz, "%lldm %llds ago", (long long)s/60, (long long)s%60);
}

static void fmt_mac(char *buf, size_t sz, const uint8_t *mac, bool valid)
{
    if (!valid) { buf[0] = '\0'; return; }
    snprintf(buf, sz, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ── Handlers ────────────────────────────────────────────────────────────────── */
static esp_err_t h_root(httpd_req_t *req)
{
    char ip[16] = "?";
    esp_netif_ip_info_t info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &info) == ESP_OK)
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));

    /* Build into a freshly-sized buffer rather than mutating a strdup() of
     * the template in place -- the IP string is usually longer than the
     * "%IP%" placeholder it replaces, so an in-place memmove overflows the
     * strdup'd buffer (heap corruption -> crash on the next allocation). */
    const char *tag = "%IP%";
    const char *p = strstr(INDEX_HTML, tag);
    size_t before = p ? (size_t)(p - INDEX_HTML) : strlen(INDEX_HTML);
    const char *after = p ? p + strlen(tag) : "";
    char *html = malloc(before + strlen(ip) + strlen(after) + 1);
    if (!html) { httpd_resp_send_500(req); return ESP_OK; }
    memcpy(html, INDEX_HTML, before);
    strcpy(html + before, ip);
    strcpy(html + before + strlen(ip), after);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, html);
    free(html);
    return ESP_OK;
}

/* GET /api/status  ── relay traffic counters + peer roster + AP link */
static esp_err_t h_status(httpd_req_t *req)
{
    static const char *TC_NAMES[TC_COUNT] = {
        "Master beacon", "Cmd to SlaveHonda", "Cmd to SlaveWallas",
        "HB from SlaveHonda", "HB from SlaveWallas", "Other/unknown"
    };

    wifi_ap_record_t ap_info = {0};
    esp_wifi_sta_get_ap_info(&ap_info);   /* leaves rssi/primary at 0 if not associated */

    /* Static: single httpd worker, and ~1.3 KB is unfriendly to the stack. */
    static char buf[1600];
    int n = snprintf(buf, sizeof(buf), "{\"traffic\":[");

    for (int i = 0; i < TC_COUNT && n < (int)sizeof(buf) - 200; i++) {
        const traffic_stat_t *t = &g_traffic[i];
        char src[20], dst[20], ls[24];
        fmt_mac(src, sizeof(src), t->last_src, t->rx_count > 0);
        fmt_mac(dst, sizeof(dst), t->last_dst, t->has_dst);
        fmt_ago(ls, sizeof(ls), t->last_seen_us);
        n += snprintf(buf + n, sizeof(buf) - n,
            "%s{\"n\":\"%s\",\"rx\":%lu,\"fwd\":%lu,\"fail\":%lu,"
            "\"src\":\"%s\",\"dst\":\"%s\",\"rssi\":%d,\"ls\":\"%s\"}",
            i ? "," : "", TC_NAMES[i],
            (unsigned long)t->rx_count, (unsigned long)t->fwd_count,
            (unsigned long)t->fwd_fail,
            src, dst, (int)t->last_rssi, ls);
    }

    n += snprintf(buf + n, sizeof(buf) - n, "],\"peers\":[");

    static const char *PEER_NAMES[3] = { "MasterRemote", "SlaveHonda", "SlaveWallas" };
    const relay_peer_t *peers[3] = { &g_peer_master, &g_peer_honda, &g_peer_wallas };
    for (int i = 0; i < 3 && n < (int)sizeof(buf) - 120; i++) {
        char mac[20], ls[24];
        fmt_mac(mac, sizeof(mac), peers[i]->mac, peers[i]->known);
        fmt_ago(ls, sizeof(ls), peers[i]->known ? peers[i]->last_seen_us : 0);
        n += snprintf(buf + n, sizeof(buf) - n,
            "%s{\"n\":\"%s\",\"mac\":\"%s\",\"rssi\":%d,\"ls\":\"%s\"}",
            i ? "," : "", PEER_NAMES[i], mac, (int)peers[i]->rssi, ls);
    }

    snprintf(buf + n, sizeof(buf) - n, "],\"rssi\":%d,\"ch\":%d}",
             (int)ap_info.rssi, (int)ap_info.primary);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* GET /api/sysinfo  ── self-monitoring: heap + low-water mark + uptime + WiFi
 * reconnect count, for spotting a slow resource leak before it kills the
 * web server (a steadily falling heap low-water mark is the tell). */
static esp_err_t h_sysinfo(httpd_req_t *req)
{
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"heap\":%lu,\"heapMin\":%lu,\"heapMinAgo\":%lld,\"uptime\":%lld,\"wifiDisc\":%lu}",
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)g_heap_min,
        (long long)((esp_timer_get_time() - g_heap_min_ts_us) / 1000000),
        (long long)(esp_timer_get_time() / 1000000),
        (unsigned long)g_wifi_disconnect_count);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t h_wifi_setup(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, WIFI_SETUP_HTML);
    return ESP_OK;
}

/* In-place decode of application/x-www-form-urlencoded text: '+' becomes a
 * space and %XX becomes the encoded byte. Without this, any SSID or password
 * containing a space or symbol was saved in its encoded form, and the device
 * rebooted into a connect-fail/portal loop. Decoded text never grows. */
static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *s)
{
    char *o = s;
    while (*s) {
        int hi, lo;
        if (*s == '+') { *o++ = ' '; s++; }
        else if (*s == '%' && (hi = hexval(s[1])) >= 0 && (lo = hexval(s[2])) >= 0) {
            *o++ = (char)(hi * 16 + lo);
            s += 3;
        } else *o++ = *s++;
    }
    *o = '\0';
}

static esp_err_t h_wifi_save(httpd_req_t *req)
{
    char body[256]={0};
    int len=MIN(req->content_len,(int)sizeof(body)-1);
    httpd_req_recv(req, body, len);
    char *sp=strstr(body,"ssid="); char *pp=strstr(body,"pass=");
    if(sp){sp+=5;char *e=strchr(sp,'&');if(e)*e='\0';strlcpy(g_new_ssid,sp,64);if(e)*e='&';}
    if(pp){pp+=5;char *e=strchr(pp,'&');if(e)*e='\0';strlcpy(g_new_pass,pp,64);}
    url_decode(g_new_ssid);
    url_decode(g_new_pass);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<!DOCTYPE html><html><body style='font-family:Arial;background:#1a1a2e;color:#eee;"
        "display:flex;justify-content:center;align-items:center;min-height:100vh'>"
        "<div style='text-align:center'><h2 style='color:#e94560'>Saved!</h2>"
        "<p>Connecting and rebooting…</p></div></body></html>");
    g_wifi_save_requested = true;
    return ESP_OK;
}

/* POST /ota/upload  ── raw binary OTA upload */
static esp_err_t h_ota_upload(httpd_req_t *req)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition"); return ESP_OK; }
    esp_ota_handle_t ota = 0;
    if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed"); return ESP_OK;
    }
    char buf[1024]; int remaining=req->content_len; bool ok=true; int timeout_retries=0;
    while (remaining > 0) {
        int got=httpd_req_recv(req, buf, MIN(remaining,(int)sizeof(buf)));
        if (got<=0) { if(got==HTTPD_SOCK_ERR_TIMEOUT && ++timeout_retries<5) continue; ok=false; break; }
        timeout_retries=0;
        if (esp_ota_write(ota, buf, got) != ESP_OK) { ok=false; break; }
        remaining -= got;
    }
    if (!ok || esp_ota_end(ota) != ESP_OK) {
        esp_ota_abort(ota);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA failed");
        return ESP_OK;
    }
    esp_ota_set_boot_partition(part);
    httpd_resp_sendstr(req, "OK");
    ESP_LOGI(TAG, "OTA success — rebooting");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* GET /api/scan  ── WiFi scan, returns JSON array */
static esp_err_t h_scan(httpd_req_t *req)
{
    wifi_scan_config_t scfg = {
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 120,
        .scan_time.active.max = 300,
    };
    esp_wifi_scan_start(&scfg, true);

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count > 20) count = 20;

    wifi_ap_record_t *recs = count ? malloc(count * sizeof(wifi_ap_record_t)) : NULL;
    if (recs) esp_wifi_scan_get_ap_records(&count, recs);
    else count = 0;

    char *buf = malloc((int)count * 140 + 8);
    if (!buf) {
        free(recs);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }

    strcpy(buf, "[");
    bool first = true;
    for (int i = 0; i < (int)count; i++) {
        if (recs[i].ssid[0] == '\0') continue;
        char esc[70] = {0};
        int j = 0;
        for (int k = 0; recs[i].ssid[k] && k < 32 && j < 66; k++) {
            unsigned char c = recs[i].ssid[k];
            if (c == '"' || c == '\\') esc[j++] = '\\';
            if (c >= 0x20) esc[j++] = c;
        }
        char entry[140];
        snprintf(entry, sizeof(entry), "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                 first ? "" : ",", esc, recs[i].rssi, (int)recs[i].authmode);
        strcat(buf, entry);
        first = false;
    }
    strcat(buf, "]");
    free(recs);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    free(buf);
    return ESP_OK;
}

/* ── Start ───────────────────────────────────────────────────────────────────── */
esp_err_t web_server_start(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16;   /* headroom above the current route count -- an exact-fit
                                    cap silently drops routes registered past the limit
                                    (hit SlaveHonda in v1.9.0: /ota/upload never registered) */
    cfg.stack_size       = 6144;
    cfg.lru_purge_enable = true;   /* evict LRU session when the socket pool (default 7)
                                      is full -- abandoned client connections otherwise
                                      exhaust it and wedge HTTP until reboot (FSD v1.35) */
    if (httpd_start(&server, &cfg) != ESP_OK) { ESP_LOGE(TAG, "Start failed"); return ESP_FAIL; }
    static const httpd_uri_t routes[] = {
        { .uri="/",            .method=HTTP_GET,  .handler=h_root       },
        { .uri="/api/status",  .method=HTTP_GET,  .handler=h_status     },
        { .uri="/api/sysinfo", .method=HTTP_GET,  .handler=h_sysinfo    },
        { .uri="/wifi-setup",  .method=HTTP_GET,  .handler=h_wifi_setup },
        { .uri="/wifi-save",   .method=HTTP_POST, .handler=h_wifi_save  },
        { .uri="/api/scan",    .method=HTTP_GET,  .handler=h_scan       },
        { .uri="/ota/upload",  .method=HTTP_POST, .handler=h_ota_upload },
    };
    for (size_t i=0; i<sizeof(routes)/sizeof(routes[0]); i++)
        if (httpd_register_uri_handler(server, &routes[i]) != ESP_OK)
            ESP_LOGE(TAG, "Failed to register URI: %s", routes[i].uri);
    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}
