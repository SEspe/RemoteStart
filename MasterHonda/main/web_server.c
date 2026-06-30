/*
 * MasterHonda web_server.c
 * Serves: WiFi setup portal, pin status API, clients API, OTA upload.
 * Three-tab UI: Pin Status | Clients | OTA Update
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <sys/param.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "driver/gpio.h"

#include "version.h"
#include "web_server.h"

static const char *TAG = "web";

/* ── Shared state from main.c ──────────────────────────────────────────────── */
extern volatile bool g_honda_start_cmd;
extern volatile bool g_wallas_start_cmd;
extern          bool g_slave_honda_running;

typedef struct { int64_t last_seen_us; bool honda_ign_on; bool honda_starting;
                 bool honda_running; bool wallas_running; bool wallas_start_cmd; } slave_info_t;
extern slave_info_t g_slave_honda;
extern slave_info_t g_slave_wallas;

extern volatile bool g_wifi_save_requested;
extern char          g_new_ssid[64];
extern char          g_new_pass[64];

/* Pin numbers (mirror main.c) */
#define PIN_HONDA_STARTED_FB     13
#define PIN_HONDA_START          15
#define PIN_HONDA_MANUAL_START   14
#define PIN_WALLAS_START          4
#define PIN_WALLAS_MANUAL_START   5

/* ── HTML Pages ─────────────────────────────────────────────────────────────── */
static const char WIFI_SETUP_HTML[] =
"<!DOCTYPE html><html><head><meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>WiFi Setup</title>"
"<style>body{font-family:Arial,sans-serif;background:#1a1a2e;color:#eee;display:flex;"
"justify-content:center;align-items:center;min-height:100vh;margin:0}"
".box{background:#16213e;border-radius:10px;padding:30px;width:300px}"
"h2{color:#e94560;margin-bottom:20px}label{display:block;margin-bottom:4px;font-size:.85rem}"
"input{width:100%;padding:8px;margin-bottom:14px;background:#0f3460;border:1px solid #444;"
"border-radius:5px;color:#eee;box-sizing:border-box}"
"button{width:100%;padding:10px;background:#e94560;border:none;border-radius:5px;"
"color:#fff;cursor:pointer;font-size:1rem}"
"button:hover{background:#c73652}</style></head>"
"<body><div class='box'><h2>&#9889; " FIRMWARE_NAME "<br>WiFi Setup</h2>"
"<form action='/wifi-save' method='POST'>"
"<label>Network (SSID)</label><input name='ssid' type='text' required>"
"<label>Password</label><input name='pass' type='password'>"
"<button type='submit'>Connect &amp; Save</button>"
"</form></div></body></html>";

static const char INDEX_HTML_TMPL[] =
"<!DOCTYPE html><html lang='en'><head>"
"<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>MasterHonda</title>"
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
".cli{background:#0f3460;border-radius:7px;padding:12px;margin-bottom:10px}"
".cn{font-weight:bold;color:#e94560;margin-bottom:6px;font-size:.88rem}"
".cr{display:flex;justify-content:space-between;font-size:.82rem;padding:2px 0}"
".ok{color:#4caf50}.dim{color:#888}"
".ts{font-size:.7rem;color:#555;margin-top:6px}"
".ota-wrap{background:#fff;border-radius:8px;padding:20px;color:#333}"
".ota-wrap h3{color:#e94560;margin-bottom:12px}"
".ota-wrap p{font-size:.85rem;color:#555;margin-bottom:14px}"
"input[type=file]{display:block;margin-bottom:12px}"
".prog{margin-top:10px;font-size:.85rem;color:#e94560;min-height:1.2em}"
"button.up{padding:8px 20px;background:#e94560;border:none;border-radius:5px;"
"color:#fff;cursor:pointer;font-size:.9rem}"
"</style></head><body>"
"<div class='hdr'><h1>&#9889; MasterHonda</h1>"
"<span class='v'>v" FIRMWARE_VERSION " &nbsp;|&nbsp; %IP%</span></div>"
"<div class='tabs'>"
"<button class='tab on' onclick='show(\"status\",this)'>Pin Status</button>"
"<button class='tab' onclick='show(\"clients\",this)'>Clients</button>"
"<button class='tab' onclick='show(\"ota\",this)'>OTA Update</button>"
"</div>"
"<div id='status' class='pane on'>"
"<div class='card'><h3>Input Pins</h3><div class='grid' id='pins'>…</div></div>"
"<div class='card'><h3>System State</h3><div class='grid' id='state'>…</div></div>"
"<div class='ts' id='sts'></div></div>"
"<div id='clients' class='pane'>"
"<div id='cli'>…</div><div class='ts' id='cts'></div></div>"
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
"row('Honda Start (p15)',d.pHS)+row('Honda Manual (p14)',d.pHM)+"
"row('Wallas Start (p4)',d.pWS)+row('Wallas Manual (p5)',d.pWM)+"
"row('Honda FB LED (p13)',d.pFB);"
"document.getElementById('state').innerHTML="
"row('Honda Start CMD',d.gHS)+row('Honda Slave Running',d.gHR)+row('Wallas CMD',d.gWS);"
"document.getElementById('sts').textContent='Updated: '+new Date().toLocaleTimeString();"
"}).catch(()=>{});}"
"function fetchClients(){"
"fetch('/api/clients').then(r=>r.json()).then(d=>{"
"let h='';"
"h+='<div class=\"cli\"><div class=\"cn\">SlaveHonda "
"<span style=\"font-weight:normal;color:#666;font-size:.72rem\">30:AE:A4:1A:AE:33</span></div>';"
"h+='<div class=\"cr\"><span>Last seen</span><span>'+d.h.ls+'</span></div>';"
"h+='<div class=\"cr\"><span>Ignition On</span><span class=\"'+(d.h.ig?'ok':'dim')+'\">'+(d.h.ig?'YES':'No')+'</span></div>';"
"h+='<div class=\"cr\"><span>Starting</span><span class=\"'+(d.h.st?'ok':'dim')+'\">'+(d.h.st?'YES':'No')+'</span></div>';"
"h+='<div class=\"cr\"><span>Running</span><span class=\"'+(d.h.ru?'ok':'dim')+'\">'+(d.h.ru?'YES':'No')+'</span></div></div>';"
"h+='<div class=\"cli\"><div class=\"cn\">SlaveWallas "
"<span style=\"font-weight:normal;color:#666;font-size:.72rem\">30:AE:A4:1A:AE:30</span></div>';"
"h+='<div class=\"cr\"><span>Last seen</span><span>'+d.w.ls+'</span></div>';"
"h+='<div class=\"cr\"><span>Running</span><span class=\"'+(d.w.ru?'ok':'dim')+'\">'+(d.w.ru?'YES':'No')+'</span></div>';"
"h+='<div class=\"cr\"><span>Start CMD</span><span class=\"'+(d.w.sc?'ok':'dim')+'\">'+(d.w.sc?'YES':'No')+'</span></div></div>';"
"document.getElementById('cli').innerHTML=h;"
"document.getElementById('cts').textContent='Updated: '+new Date().toLocaleTimeString();"
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
"if(document.getElementById('status').classList.contains('on'))fetchStatus();"
"if(document.getElementById('clients').classList.contains('on'))fetchClients();"
"},2000);"
"fetchStatus();"
"</script></body></html>";

/* ── Helpers ────────────────────────────────────────────────────────────────── */
static void fmt_last_seen(char *buf, size_t sz, int64_t ts_us)
{
    if (ts_us == 0) { snprintf(buf, sz, "Never"); return; }
    int64_t s = (esp_timer_get_time() - ts_us) / 1000000;
    if (s < 60) snprintf(buf, sz, "%llds ago", (long long)s);
    else        snprintf(buf, sz, "%lldm %llds ago", (long long)s/60, (long long)s%60);
}

/* ── HTTP Handlers ──────────────────────────────────────────────────────────── */

/* GET /  ── main dashboard */
static esp_err_t h_root(httpd_req_t *req)
{
    char ip[16] = "?";
    esp_netif_ip_info_t info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) { esp_netif_get_ip_info(netif, &info); snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip)); }

    /* Simple template: replace %IP% */
    char *html = strdup(INDEX_HTML_TMPL);
    if (!html) { httpd_resp_send_500(req); return ESP_OK; }
    char *p = strstr(html, "%IP%");
    if (p) { memmove(p + strlen(ip), p + 4, strlen(p + 4) + 1); memcpy(p, ip, strlen(ip)); }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, html);
    free(html);
    return ESP_OK;
}

/* GET /api/status  ── pin + global state JSON */
static esp_err_t h_status(httpd_req_t *req)
{
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"pHS\":%s,\"pHM\":%s,\"pWS\":%s,\"pWM\":%s,\"pFB\":%s,"
        "\"gHS\":%s,\"gHR\":%s,\"gWS\":%s}",
        !gpio_get_level(PIN_HONDA_START)        ? "true" : "false",
         gpio_get_level(PIN_HONDA_MANUAL_START) ? "true" : "false",
         gpio_get_level(PIN_WALLAS_START)       ? "true" : "false",
        !gpio_get_level(PIN_WALLAS_MANUAL_START)? "true" : "false",
         gpio_get_level(PIN_HONDA_STARTED_FB)   ? "true" : "false",
        g_honda_start_cmd    ? "true" : "false",
        g_slave_honda_running? "true" : "false",
        g_wallas_start_cmd   ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* GET /api/clients  ── slave status JSON */
static esp_err_t h_clients(httpd_req_t *req)
{
    char lsh[32], lsw[32];
    fmt_last_seen(lsh, sizeof(lsh), g_slave_honda.last_seen_us);
    fmt_last_seen(lsw, sizeof(lsw), g_slave_wallas.last_seen_us);

    char buf[320];
    snprintf(buf, sizeof(buf),
        "{\"h\":{\"ls\":\"%s\",\"ig\":%s,\"st\":%s,\"ru\":%s},"
        "\"w\":{\"ls\":\"%s\",\"ru\":%s,\"sc\":%s}}",
        lsh,
        g_slave_honda.honda_ign_on   ? "true":"false",
        g_slave_honda.honda_starting ? "true":"false",
        g_slave_honda.honda_running  ? "true":"false",
        lsw,
        g_slave_wallas.wallas_running  ? "true":"false",
        g_slave_wallas.wallas_start_cmd? "true":"false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* GET /wifi-setup  ── config portal page */
static esp_err_t h_wifi_setup(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, WIFI_SETUP_HTML);
    return ESP_OK;
}

/* POST /wifi-save  ── save credentials from portal form */
static esp_err_t h_wifi_save(httpd_req_t *req)
{
    char body[256] = {0};
    int  len = MIN(req->content_len, (int)sizeof(body) - 1);
    httpd_req_recv(req, body, len);

    /* Parse ssid=...&pass=... from URL-encoded body */
    char *sp = strstr(body, "ssid=");
    char *pp = strstr(body, "pass=");
    if (sp) {
        sp += 5;
        char *end = strchr(sp, '&');
        if (end) *end = '\0';
        strlcpy(g_new_ssid, sp, sizeof(g_new_ssid));
        if (end) *end = '&';
    }
    if (pp) {
        pp += 5;
        char *end = strchr(pp, '&');
        if (end) *end = '\0';
        strlcpy(g_new_pass, pp, sizeof(g_new_pass));
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<!DOCTYPE html><html><body style='font-family:Arial;background:#1a1a2e;color:#eee;"
        "display:flex;justify-content:center;align-items:center;min-height:100vh'>"
        "<div style='text-align:center'><h2 style='color:#e94560'>Saved!</h2>"
        "<p>Connecting to WiFi and rebooting…</p></div></body></html>");

    g_wifi_save_requested = true;
    return ESP_OK;
}

/* POST /ota/upload  ── raw binary OTA upload */
static esp_err_t h_ota_upload(httpd_req_t *req)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_OK;
    }

    esp_ota_handle_t ota = 0;
    if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_OK;
    }

    char buf[1024];
    int  remaining = req->content_len;
    bool ok = true;

    while (remaining > 0) {
        int got = httpd_req_recv(req, buf, MIN(remaining, (int)sizeof(buf)));
        if (got <= 0) {
            if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ok = false; break;
        }
        if (esp_ota_write(ota, buf, got) != ESP_OK) { ok = false; break; }
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

/* ── Start Web Server ───────────────────────────────────────────────────────── */
esp_err_t web_server_start(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 8;
    cfg.stack_size       = 8192;

    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ESP_FAIL;
    }

    static const httpd_uri_t routes[] = {
        { .uri="/",           .method=HTTP_GET,  .handler=h_root       },
        { .uri="/api/status", .method=HTTP_GET,  .handler=h_status     },
        { .uri="/api/clients",.method=HTTP_GET,  .handler=h_clients    },
        { .uri="/wifi-setup", .method=HTTP_GET,  .handler=h_wifi_setup },
        { .uri="/wifi-save",  .method=HTTP_POST, .handler=h_wifi_save  },
        { .uri="/ota/upload", .method=HTTP_POST, .handler=h_ota_upload },
    };
    for (size_t i = 0; i < sizeof(routes)/sizeof(routes[0]); i++)
        httpd_register_uri_handler(server, &routes[i]);

    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}
