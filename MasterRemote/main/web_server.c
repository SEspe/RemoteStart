/*
 * MasterRemote web_server.c
 * Serves: WiFi setup portal, pin status API, nodes API, weekly timer API, OTA upload.
 * Four-tab UI: Pin Status | Nodes | Weekly Timer | OTA Update
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
#include "esp_timer.h"
#include "driver/gpio.h"

#include "version.h"
#include "web_server.h"

static const char *TAG = "web";

#define NODE_TIMEOUT_MS  30000   /* must match main.c — see FSD §3.4 */

/* ── Shared state from main.c ──────────────────────────────────────────────── */
extern volatile bool g_honda_start_cmd;
extern volatile bool g_wallas_start_cmd;
extern volatile bool g_web_honda_start;
extern volatile bool g_web_wallas_start;
extern volatile bool g_honda_force_send;
extern volatile bool g_timer_wallas_start;
extern          bool g_slave_honda_running;

/* Weekly Wallas timer accessors (function interface, see main.c) */
extern void timer_get_day(int day, bool *enabled, int *start_hh, int *start_mm, int *stop_hh, int *stop_mm);
extern void timer_set_day(int day, bool enabled, int start_hh, int start_mm, int stop_hh, int stop_mm);
extern bool timer_is_synced(void);
extern void timer_get_now_str(char *buf, size_t sz);

/* Layout must match main.c's slave_info_t exactly (shared via extern, not a header). */
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

static const char INDEX_HTML_TMPL[] =
"<!DOCTYPE html><html lang='en'><head>"
"<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>MasterRemote</title>"
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
".ok{color:#4caf50}.dim{color:#888}"
".ts{font-size:.7rem;color:#555;margin-top:6px}"
".tbl-wrap{overflow-x:auto}"
"table.nodes{border-collapse:collapse;width:100%;font-size:.78rem;white-space:nowrap}"
"table.nodes th,table.nodes td{padding:6px 10px;text-align:left;border-bottom:1px solid #0f3460}"
"table.nodes th{color:#e94560;font-size:.72rem;text-transform:uppercase}"
".ctl{display:flex;gap:8px;margin-top:8px}"
"button.go{flex:1;padding:8px;border:none;border-radius:5px;color:#fff;cursor:pointer;font-size:.85rem}"
"button.go-on{background:#4caf50}button.go-off{background:#555}"
"table.nodes input[type=time]{background:#0f3460;border:1px solid #444;color:#eee;"
"border-radius:4px;padding:3px 5px;font-size:.78rem}"
"table.nodes input[type=checkbox]{width:16px;height:16px}"
".ota-wrap{background:#fff;border-radius:8px;padding:20px;color:#333}"
".ota-wrap h3{color:#e94560;margin-bottom:12px}"
".ota-wrap p{font-size:.85rem;color:#555;margin-bottom:14px}"
"input[type=file]{display:block;margin-bottom:12px}"
".prog{margin-top:10px;font-size:.85rem;color:#e94560;min-height:1.2em}"
"button.up{padding:8px 20px;background:#e94560;border:none;border-radius:5px;"
"color:#fff;cursor:pointer;font-size:.9rem}"
"</style></head><body>"
"<div class='hdr'><h1>&#9889; MasterRemote</h1>"
"<span class='v'>v" FIRMWARE_VERSION " &nbsp;|&nbsp; %IP%</span></div>"
"<div class='tabs'>"
"<button class='tab on' onclick='show(\"status\",this)'>Pin Status</button>"
"<button class='tab' onclick='show(\"nodes\",this)'>Nodes</button>"
"<button class='tab' onclick='show(\"timer\",this)'>Weekly Timer</button>"
"<button class='tab' onclick='show(\"ota\",this)'>OTA Update</button>"
"</div>"
"<div id='status' class='pane on'>"
"<div class='card'><h3>Input Pins</h3><div class='grid' id='pins'>…</div></div>"
"<div class='card'><h3>System State</h3><div class='grid' id='state'>…</div>"
"<div class='ctl'>"
"<button class='go go-on' onclick='cmd(\"honda\",\"start\")'>Start Honda</button>"
"<button class='go go-off' onclick='cmd(\"honda\",\"stop\")'>Stop Honda</button>"
"</div>"
"<div class='ctl'>"
"<button class='go go-on' onclick='cmd(\"wallas\",\"start\")'>Start Wallas</button>"
"<button class='go go-off' onclick='cmd(\"wallas\",\"stop\")'>Stop Wallas</button>"
"</div></div>"
"<div class='card'><h3>WiFi Link</h3><div class='grid' id='wifi'>…</div></div>"
"<div class='ts' id='sts'></div></div>"
"<div id='nodes' class='pane'>"
"<div class='tbl-wrap' id='cli'>…</div><div class='ts' id='cts'></div></div>"
"<div id='timer' class='pane'>"
"<div class='card'><h3>Weekly Wallas Timer</h3>"
"<p class='warn' id='clock'>Clock: …</p>"
"<div class='tbl-wrap'><table class='nodes' id='timerTbl'>…</table></div>"
"</div></div>"
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
"document.getElementById(id).classList.add('on');btn.classList.add('on');"
"if(id==='timer')fetchTimer();}"
"function led(v){return '<div class=\"led '+(v?'on-l':'off-l')+'\"></div>';}"
"function row(n,v){return '<div class=\"row\"><span>'+n+'</span>'+led(v)+'</div>';}"
"function txtRow(n,v){return '<div class=\"row\"><span>'+n+'</span><span>'+v+'</span></div>';}"
"function cmd(unit,action){"
"fetch('/api/'+unit+'/'+action,{method:'POST'}).then(fetchStatus).catch(()=>{});}"
"var DAY_NAMES=['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];"
"function buildTimer(){"
"let h='<tr><th>Day</th><th>Enabled</th><th>Start</th><th>Stop</th></tr>';"
"for(let d=0;d<7;d++){"
"h+='<tr><td>'+DAY_NAMES[d]+'</td>'+"
"'<td><input type=\"checkbox\" id=\"ten'+d+'\" onchange=\"saveTimer('+d+')\"></td>'+"
"'<td><input type=\"time\" id=\"tst'+d+'\" onchange=\"saveTimer('+d+')\"></td>'+"
"'<td><input type=\"time\" id=\"tsp'+d+'\" onchange=\"saveTimer('+d+')\"></td></tr>';}"
"document.getElementById('timerTbl').innerHTML=h;}"
"function fetchTimer(){"
"fetch('/api/timer').then(r=>r.json()).then(d=>{"
"document.getElementById('clock').textContent='Clock: '+d.now+' (CET/CEST)'+(d.synced?'':' \\u2014 NOT SYNCED YET');"
"d.days.forEach(function(x){"
"document.getElementById('ten'+x.day).checked=x.enabled;"
"document.getElementById('tst'+x.day).value=x.start;"
"document.getElementById('tsp'+x.day).value=x.stop;});"
"}).catch(()=>{});}"
"function saveTimer(d){"
"const en=document.getElementById('ten'+d).checked?1:0;"
"const st=document.getElementById('tst'+d).value||'00:00';"
"const sp=document.getElementById('tsp'+d).value||'00:00';"
"fetch('/api/timer/set?day='+d+'&enabled='+en+'&start='+st+'&stop='+sp,{method:'POST'}).catch(()=>{});}"
"function fetchStatus(){"
"fetch('/api/status').then(r=>r.json()).then(d=>{"
"document.getElementById('pins').innerHTML="
"row('Honda Start (p15)',d.pHS)+row('Honda Manual (p14)',d.pHM)+"
"row('Wallas Start (p4)',d.pWS)+row('Wallas Manual (p5)',d.pWM)+"
"row('Honda FB LED (p13)',d.pFB);"
"document.getElementById('state').innerHTML="
"row('Honda Start CMD',d.gHS)+row('Honda Slave Running',d.gHR)+row('Wallas CMD',d.gWS);"
"document.getElementById('wifi').innerHTML="
"txtRow('Signal strength',d.rssi+' dBm')+txtRow('Channel',d.ch);"
"document.getElementById('sts').textContent='Updated: '+new Date().toLocaleTimeString();"
"}).catch(()=>{});}"
"function yn(v){return '<span class=\"'+(v?'ok':'dim')+'\">'+(v?'Yes':'No')+'</span>';}"
"function nodeRow(n,detail){"
"return '<tr><td>'+n.role+'</td>'+"
"'<td>'+(n.fw?'v'+n.fw:'\\u2014')+'</td>'+"
"'<td>'+(n.mac||'\\u2014')+'</td>'+"
"'<td>'+(n.ip||'\\u2014')+'</td>'+"
"'<td>'+yn(n.has_wifi)+'</td>'+"
"'<td>'+(n.mac?n.rssi+' dBm':'\\u2014')+'</td>'+"
"'<td>'+(n.mac?n.channel:'\\u2014')+'</td>'+"
"'<td>'+(n.connected?'<span class=\"ok\">\\u25CF connected</span>':'<span class=\"dim\">\\u25CB disconnected</span>')+'</td>'+"
"'<td>'+n.ls+'</td>'+"
"'<td>'+yn(n.ru)+'</td>'+"
"'<td>'+detail+'</td></tr>';}"
"function fetchNodes(){"
"fetch('/api/nodes').then(r=>r.json()).then(d=>{"
"let h='<table class=\"nodes\"><tr>"
"<th>Role</th><th>FW</th><th>MAC</th><th>IP</th><th>WiFi</th><th>RSSI</th><th>Ch</th>"
"<th>Status</th><th>Last seen</th><th>Running</th><th>Detail</th></tr>';"
"h+=nodeRow(d.h,'Ign:'+(d.h.ig?'Y':'N')+' Starting:'+(d.h.st?'Y':'N'));"
"h+=nodeRow(d.w,'Start CMD:'+(d.w.sc?'Y':'N'));"
"h+='</table>';"
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
"if(document.getElementById('nodes').classList.contains('on'))fetchNodes();"
"},2000);"
"fetchStatus();"
"buildTimer();"
"fetchTimer();"
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

    /* Build into a freshly-sized buffer rather than mutating a strdup() of
     * the template in place -- the IP string is usually longer than the
     * "%IP%" placeholder it replaces, so an in-place memmove overflows the
     * strdup'd buffer (heap corruption -> crash on the next allocation). */
    const char *tag = "%IP%";
    const char *p = strstr(INDEX_HTML_TMPL, tag);
    size_t before = p ? (size_t)(p - INDEX_HTML_TMPL) : strlen(INDEX_HTML_TMPL);
    const char *after = p ? p + strlen(tag) : "";
    char *html = malloc(before + strlen(ip) + strlen(after) + 1);
    if (!html) { httpd_resp_send_500(req); return ESP_OK; }
    memcpy(html, INDEX_HTML_TMPL, before);
    strcpy(html + before, ip);
    strcpy(html + before + strlen(ip), after);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, html);
    free(html);
    return ESP_OK;
}

/* GET /api/status  ── pin + global state + WiFi link JSON */
static esp_err_t h_status(httpd_req_t *req)
{
    wifi_ap_record_t ap_info = {0};
    esp_wifi_sta_get_ap_info(&ap_info);   /* leaves rssi/primary at 0 if not connected */

    char buf[320];
    snprintf(buf, sizeof(buf),
        "{\"pHS\":%s,\"pHM\":%s,\"pWS\":%s,\"pWM\":%s,\"pFB\":%s,"
        "\"gHS\":%s,\"gHR\":%s,\"gWS\":%s,\"rssi\":%d,\"ch\":%d}",
        !gpio_get_level(PIN_HONDA_START)        ? "true" : "false",
         gpio_get_level(PIN_HONDA_MANUAL_START) ? "true" : "false",
         gpio_get_level(PIN_WALLAS_START)       ? "true" : "false",
        !gpio_get_level(PIN_WALLAS_MANUAL_START)? "true" : "false",
         gpio_get_level(PIN_HONDA_STARTED_FB)   ? "true" : "false",
        g_honda_start_cmd    ? "true" : "false",
        g_slave_honda_running? "true" : "false",
        g_wallas_start_cmd   ? "true" : "false",
        (int)ap_info.rssi, (int)ap_info.primary);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* Format a node's MAC as "AA:BB:CC:DD:EE:FF", or "" if never registered. */
static void fmt_mac(char *buf, size_t sz, const slave_info_t *info)
{
    if (!info->peer_added) { buf[0] = '\0'; return; }
    snprintf(buf, sz, "%02X:%02X:%02X:%02X:%02X:%02X",
              info->mac[0], info->mac[1], info->mac[2],
              info->mac[3], info->mac[4], info->mac[5]);
}

static bool node_connected(const slave_info_t *info)
{
    return info->peer_added &&
           (esp_timer_get_time() - info->last_seen_us) < (int64_t)NODE_TIMEOUT_MS * 1000;
}

/* GET /api/nodes  ── dynamic node roster JSON (role, mac, ip, connected, last seen, status) */
static esp_err_t h_nodes(httpd_req_t *req)
{
    char lsh[32], lsw[32], mach[24], macw[24];
    fmt_last_seen(lsh, sizeof(lsh), g_slave_honda.last_seen_us);
    fmt_last_seen(lsw, sizeof(lsw), g_slave_wallas.last_seen_us);
    fmt_mac(mach, sizeof(mach), &g_slave_honda);
    fmt_mac(macw, sizeof(macw), &g_slave_wallas);

    char buf[700];
    snprintf(buf, sizeof(buf),
        "{\"h\":{\"role\":\"SlaveHonda\",\"mac\":\"%s\",\"ip\":\"%s\",\"has_wifi\":%s,"
        "\"rssi\":%d,\"channel\":%d,\"fw\":\"%s\","
        "\"connected\":%s,\"ls\":\"%s\",\"ig\":%s,\"st\":%s,\"ru\":%s},"
        "\"w\":{\"role\":\"SlaveWallas\",\"mac\":\"%s\",\"ip\":\"%s\",\"has_wifi\":%s,"
        "\"rssi\":%d,\"channel\":%d,\"fw\":\"%s\","
        "\"connected\":%s,\"ls\":\"%s\",\"ru\":%s,\"sc\":%s}}",
        mach, g_slave_honda.ip, g_slave_honda.has_wifi ? "true":"false",
        (int)g_slave_honda.rssi, (int)g_slave_honda.channel, g_slave_honda.fw_version,
        node_connected(&g_slave_honda) ? "true":"false", lsh,
        g_slave_honda.honda_ign_on   ? "true":"false",
        g_slave_honda.honda_starting ? "true":"false",
        g_slave_honda.honda_running  ? "true":"false",
        macw, g_slave_wallas.ip, g_slave_wallas.has_wifi ? "true":"false",
        (int)g_slave_wallas.rssi, (int)g_slave_wallas.channel, g_slave_wallas.fw_version,
        node_connected(&g_slave_wallas) ? "true":"false", lsw,
        g_slave_wallas.wallas_running  ? "true":"false",
        g_slave_wallas.wallas_start_cmd? "true":"false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* POST /api/honda/start | /api/honda/stop | /api/wallas/start | /api/wallas/stop
 * Manual web override — a third command source OR'd in with the Victron
 * relay and physical manual button (see g_web_honda_start/g_web_wallas_start
 * in main.c). Recomputes the combined command immediately so the UI doesn't
 * have to wait for a GPIO edge. */
static esp_err_t h_honda_start(httpd_req_t *req)
{
    g_web_honda_start = true;
    g_honda_start_cmd = true;
    g_honda_force_send = true;   /* guarantee a send even if last-known state already reads "running" */
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t h_honda_stop(httpd_req_t *req)
{
    g_web_honda_start = false;
    g_honda_start_cmd = gpio_get_level(PIN_HONDA_MANUAL_START) || !gpio_get_level(PIN_HONDA_START);
    g_honda_force_send = true;   /* guarantee a send even if last-known state already reads "stopped" */
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t h_wallas_start(httpd_req_t *req)
{
    g_web_wallas_start = true;
    g_wallas_start_cmd = true;
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t h_wallas_stop(httpd_req_t *req)
{
    g_web_wallas_start = false;
    g_wallas_start_cmd = gpio_get_level(PIN_WALLAS_START) || !gpio_get_level(PIN_WALLAS_MANUAL_START) || g_timer_wallas_start;
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

/* GET /api/timer  ── current clock + all 7 days' weekly Wallas timer config */
static esp_err_t h_timer_get(httpd_req_t *req)
{
    char now_str[24];
    timer_get_now_str(now_str, sizeof(now_str));

    char buf[900];
    int n = snprintf(buf, sizeof(buf), "{\"now\":\"%s\",\"synced\":%s,\"days\":[",
                      now_str, timer_is_synced() ? "true" : "false");
    for (int d = 0; d < 7 && n < (int)sizeof(buf) - 80; d++) {
        bool en; int sh, sm, ph, pm;
        timer_get_day(d, &en, &sh, &sm, &ph, &pm);
        n += snprintf(buf + n, sizeof(buf) - n,
                      "%s{\"day\":%d,\"enabled\":%s,\"start\":\"%02d:%02d\",\"stop\":\"%02d:%02d\"}",
                      d ? "," : "", d, en ? "true" : "false", sh, sm, ph, pm);
    }
    snprintf(buf + n, sizeof(buf) - n, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* POST /api/timer/set?day=N&enabled=0|1&start=HH:MM&stop=HH:MM  ── save one day */
static esp_err_t h_timer_set(httpd_req_t *req)
{
    char query[96] = {0};
    char day_str[4] = {0}, en_str[4] = {0}, start_str[8] = {0}, stop_str[8] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing query");
        return ESP_OK;
    }
    httpd_query_key_value(query, "day",     day_str,   sizeof(day_str));
    httpd_query_key_value(query, "enabled", en_str,    sizeof(en_str));
    httpd_query_key_value(query, "start",   start_str, sizeof(start_str));
    httpd_query_key_value(query, "stop",    stop_str,  sizeof(stop_str));

    int day = atoi(day_str);
    bool enabled = atoi(en_str) != 0;
    int start_hh = 0, start_mm = 0, stop_hh = 0, stop_mm = 0;
    sscanf(start_str, "%d:%d", &start_hh, &start_mm);
    sscanf(stop_str,  "%d:%d", &stop_hh,  &stop_mm);

    timer_set_day(day, enabled, start_hh, start_mm, stop_hh, stop_mm);
    httpd_resp_sendstr(req, "OK");
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
    int  timeout_retries = 0;

    while (remaining > 0) {
        int got = httpd_req_recv(req, buf, MIN(remaining, (int)sizeof(buf)));
        if (got <= 0) {
            /* Bounded retry: tolerate brief stalls, but never hang the httpd
             * worker forever if the client vanishes mid-upload. */
            if (got == HTTPD_SOCK_ERR_TIMEOUT && ++timeout_retries < 5) continue;
            ok = false; break;
        }
        timeout_retries = 0;
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

/* ── Start Web Server ───────────────────────────────────────────────────────── */
esp_err_t web_server_start(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 14;
    cfg.stack_size       = 8192;

    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ESP_FAIL;
    }

    static const httpd_uri_t routes[] = {
        { .uri="/",                .method=HTTP_GET,  .handler=h_root        },
        { .uri="/api/status",      .method=HTTP_GET,  .handler=h_status      },
        { .uri="/api/nodes",       .method=HTTP_GET,  .handler=h_nodes       },
        { .uri="/api/honda/start", .method=HTTP_POST, .handler=h_honda_start },
        { .uri="/api/honda/stop",  .method=HTTP_POST, .handler=h_honda_stop  },
        { .uri="/api/wallas/start",.method=HTTP_POST, .handler=h_wallas_start},
        { .uri="/api/wallas/stop", .method=HTTP_POST, .handler=h_wallas_stop },
        { .uri="/api/timer",       .method=HTTP_GET,  .handler=h_timer_get   },
        { .uri="/api/timer/set",   .method=HTTP_POST, .handler=h_timer_set   },
        { .uri="/wifi-setup",      .method=HTTP_GET,  .handler=h_wifi_setup  },
        { .uri="/wifi-save",       .method=HTTP_POST, .handler=h_wifi_save   },
        { .uri="/api/scan",        .method=HTTP_GET,  .handler=h_scan        },
        { .uri="/ota/upload",      .method=HTTP_POST, .handler=h_ota_upload  },
    };
    for (size_t i = 0; i < sizeof(routes)/sizeof(routes[0]); i++)
        httpd_register_uri_handler(server, &routes[i]);

    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}
