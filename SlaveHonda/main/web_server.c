/*
 * SlaveHonda web_server.c
 * Serves: WiFi setup portal, pin status API, debug GPIO test, OTA upload.
 * Three-tab UI: Status | Debug GPIO | OTA Update
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
#include "esp_timer.h"
#include "driver/gpio.h"

#include "version.h"
#include "web_server.h"

static const char *TAG = "web";

/* ── Shared state from main.c ──────────────────────────────────────────────── */
extern volatile bool g_start_cmd;
extern          bool g_honda_running;
extern          bool g_honda_starting;
extern          bool g_honda_ign_on;
extern volatile bool g_wifi_save_requested;
extern char          g_new_ssid[64];
extern char          g_new_pass[64];

#define PIN_STARTER_RELAY   13
#define PIN_IGNITION_RELAY  14
#define PIN_RUNNING_FB      15

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
"<title>SlaveHonda</title>"
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
".led{width:12px;height:12px;border-radius:50%}"
".on-l{background:#4caf50;box-shadow:0 0 5px #4caf50}.off-l{background:#444}"
".ts{font-size:.7rem;color:#555;margin-top:6px}"
".dim{color:#666}"
".warn{font-size:.78rem;color:#e9b400;margin-bottom:10px}"
".dbg{display:grid;grid-template-columns:repeat(auto-fill,minmax(120px,1fr));gap:6px}"
".dbg .row{flex-direction:column;align-items:stretch;gap:4px}"
".dbg .row>span:first-child{text-align:center;font-weight:bold}"
".btns{display:flex;gap:4px}"
"button.go{flex:1;padding:6px;border:none;border-radius:4px;color:#fff;cursor:pointer;font-size:.78rem}"
"button.go-on{background:#4caf50}button.go-off{background:#555}"
".ota-wrap{background:#fff;border-radius:8px;padding:20px;color:#333}"
".ota-wrap h3{color:#e94560;margin-bottom:12px}"
".ota-wrap p{font-size:.85rem;color:#555;margin-bottom:14px}"
"input[type=file]{display:block;margin-bottom:12px}"
".prog{margin-top:10px;font-size:.85rem;color:#e94560;min-height:1.2em}"
"button.up{padding:8px 20px;background:#e94560;border:none;border-radius:5px;"
"color:#fff;cursor:pointer;font-size:.9rem}"
"</style></head><body>"
"<div class='hdr'><h1>&#9889; SlaveHonda</h1>"
"<span class='v'>v" FIRMWARE_VERSION " &nbsp;|&nbsp; %IP%</span></div>"
"<div class='tabs'>"
"<button class='tab on' onclick='show(\"status\",this)'>Status</button>"
"<button class='tab' onclick='show(\"debug\",this)'>Debug GPIO</button>"
"<button class='tab' onclick='show(\"ota\",this)'>OTA Update</button>"
"</div>"
"<div id='status' class='pane on'>"
"<div class='card'><h3>Honda Generator</h3><div class='grid' id='pins'>…</div></div>"
"<div class='ts' id='sts'></div></div>"
"<div id='debug' class='pane'>"
"<div class='card'><h3>Manual GPIO Test</h3>"
"<p class='warn'>Hardware bring-up only. Every GPIO's live level is shown as a "
"dot (green = HIGH). Pins with ON/OFF buttons can be forced to OUTPUT and "
"driven, overriding normal operation until next reboot. Reserved pins "
"(strapping: 0,2,5,12,15; flash: 6-11; UART0 console: 1,3; input-only: 34-39) "
"show status only, no buttons.</p>"
"<div class='dbg' id='dbg'>…</div></div></div>"
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
"function led(v){return '<div class=\"led '+(v?'on-l':'off-l')+'\"></div>';}"
"function row(n,v){return '<div class=\"row\"><span>'+n+'</span>'+led(v)+'</div>';}"
"function fetchStatus(){"
"fetch('/api/status').then(r=>r.json()).then(d=>{"
"document.getElementById('pins').innerHTML="
"row('Ignition ON (p14)',d.ign)+row('Starter active (p13)',d.sta)+"
"row('Running FB (p15)',d.run)+row('Start CMD from Master',d.cmd)+"
"row('Starting sequence',d.starting);"
"document.getElementById('sts').textContent='Updated: '+new Date().toLocaleTimeString();"
"}).catch(()=>{});}"
"var DBG_PINS=[4,13,14,16,17,18,19,21,22,23,25,26,27,32,33];"
"function buildDbg(){"
"var h='';"
"for(var p=0;p<=39;p++){"
"var manip=DBG_PINS.indexOf(p)!==-1;"
"var ctl=manip?"
"'<span class=\"btns\"><button class=\"go go-on\" onclick=\"gset('+p+',1)\">ON</button>"
"<button class=\"go go-off\" onclick=\"gset('+p+',0)\">OFF</button></span>':"
"'<span class=\"dim\" style=\"font-size:.7rem\">reserved</span>';"
"h+='<div class=\"row\"><span><span class=\"led off-l\" id=\"dl'+p+'\"></span> GPIO'+p+'</span>'+ctl+'</div>';}"
"document.getElementById('dbg').innerHTML=h;}"
"function fetchDbg(){"
"fetch('/api/gpio/status').then(r=>r.json()).then(d=>{"
"d.lvl.forEach(function(v,p){"
"var el=document.getElementById('dl'+p);"
"if(el)el.className='led '+(v?'on-l':'off-l');});"
"}).catch(()=>{});}"
"function gset(pin,level){fetch('/api/gpio/set?pin='+pin+'&level='+level).catch(()=>{});}"
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
"if(document.getElementById('status').classList.contains('on'))fetchStatus();"
"if(document.getElementById('debug').classList.contains('on'))fetchDbg();"
"},2000);"
"fetchStatus();"
"buildDbg();"
"fetchDbg();"
"</script></body></html>";

/* ── Handlers ────────────────────────────────────────────────────────────────── */
static esp_err_t h_root(httpd_req_t *req)
{
    char ip[16] = "?";
    esp_netif_ip_info_t info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) { esp_netif_get_ip_info(netif, &info); snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip)); }

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

static esp_err_t h_status(httpd_req_t *req)
{
    char buf[192];
    snprintf(buf, sizeof(buf),
        "{\"ign\":%s,\"sta\":%s,\"run\":%s,\"cmd\":%s,\"starting\":%s}",
        !gpio_get_level(PIN_IGNITION_RELAY) ? "true":"false",
        !gpio_get_level(PIN_STARTER_RELAY)  ? "true":"false",
        !gpio_get_level(PIN_RUNNING_FB)     ? "true":"false",
        g_start_cmd      ? "true":"false",
        g_honda_starting ? "true":"false");
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

static esp_err_t h_wifi_save(httpd_req_t *req)
{
    char body[256] = {0};
    int  len = MIN(req->content_len, (int)sizeof(body) - 1);
    httpd_req_recv(req, body, len);
    char *sp = strstr(body, "ssid="); char *pp = strstr(body, "pass=");
    if (sp) { sp+=5; char *e=strchr(sp,'&'); if(e)*e='\0'; strlcpy(g_new_ssid, sp, 64); if(e)*e='&'; }
    if (pp) { pp+=5; char *e=strchr(pp,'&'); if(e)*e='\0'; strlcpy(g_new_pass, pp, 64); }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<!DOCTYPE html><html><body style='font-family:Arial;background:#1a1a2e;color:#eee;"
        "display:flex;justify-content:center;align-items:center;min-height:100vh'>"
        "<div style='text-align:center'><h2 style='color:#e94560'>Saved!</h2>"
        "<p>Connecting and rebooting…</p></div></body></html>");
    g_wifi_save_requested = true;
    return ESP_OK;
}

static esp_err_t h_ota_upload(httpd_req_t *req)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition"); return ESP_OK; }
    esp_ota_handle_t ota = 0;
    if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed"); return ESP_OK;
    }
    char buf[1024]; int remaining = req->content_len; bool ok = true; int timeout_retries = 0;
    while (remaining > 0) {
        int got = httpd_req_recv(req, buf, MIN(remaining, (int)sizeof(buf)));
        if (got <= 0) { if (got == HTTPD_SOCK_ERR_TIMEOUT && ++timeout_retries<5) continue; ok=false; break; }
        timeout_retries = 0;
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

/* GET /api/gpio/set?pin=N&level=0|1  ── DEBUG ONLY: force any non-reserved
 * GPIO to OUTPUT and drive it. For hardware bring-up when pin wiring is
 * uncertain -- reconfigures the pin every call, overriding whatever normal
 * operation was doing with it until the next reboot. */
static esp_err_t h_gpio_set(httpd_req_t *req)
{
    char query[64] = {0}, pin_str[8] = {0}, lvl_str[8] = {0};
    int pin = -1, level = -1;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "pin", pin_str, sizeof(pin_str)) == ESP_OK)
            pin = atoi(pin_str);
        if (httpd_query_key_value(query, "level", lvl_str, sizeof(lvl_str)) == ESP_OK)
            level = atoi(lvl_str);
    }
    if (pin < 0 || pin > 39 || (level != 0 && level != 1)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad pin/level");
        return ESP_OK;
    }
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, level);
    ESP_LOGW(TAG, "DEBUG: forced GPIO%d = %d", pin, level);
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

/* GET /api/gpio/status  ── DEBUG ONLY: read-only level of every GPIO 0-39,
 * for the Debug tab's status dots. gpio_get_level() is passive (just reads
 * the GPIO_IN register) so this is safe to call even on reserved pins
 * (strapping, flash, UART0) -- unlike h_gpio_set, it never reconfigures a pin. */
static esp_err_t h_gpio_status(httpd_req_t *req)
{
    char buf[200];
    int n = snprintf(buf, sizeof(buf), "{\"lvl\":[");
    for (int i = 0; i <= 39 && n < (int)sizeof(buf) - 4; i++)
        n += snprintf(buf + n, sizeof(buf) - n, "%s%d", i ? "," : "", gpio_get_level(i));
    snprintf(buf + n, sizeof(buf) - n, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
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
    cfg.max_uri_handlers = 8;
    cfg.stack_size       = 6144;
    if (httpd_start(&server, &cfg) != ESP_OK) { ESP_LOGE(TAG, "Start failed"); return ESP_FAIL; }
    static const httpd_uri_t routes[] = {
        { .uri="/",                .method=HTTP_GET,  .handler=h_root        },
        { .uri="/api/status",      .method=HTTP_GET,  .handler=h_status      },
        { .uri="/api/gpio/set",    .method=HTTP_GET,  .handler=h_gpio_set    },
        { .uri="/api/gpio/status", .method=HTTP_GET,  .handler=h_gpio_status },
        { .uri="/wifi-setup",      .method=HTTP_GET,  .handler=h_wifi_setup  },
        { .uri="/wifi-save",       .method=HTTP_POST, .handler=h_wifi_save   },
        { .uri="/api/scan",        .method=HTTP_GET,  .handler=h_scan        },
        { .uri="/ota/upload",      .method=HTTP_POST, .handler=h_ota_upload  },
    };
    for (size_t i=0; i<sizeof(routes)/sizeof(routes[0]); i++)
        httpd_register_uri_handler(server, &routes[i]);
    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}
