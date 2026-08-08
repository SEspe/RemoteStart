/*
 * MasterRemote web_server.c
 * Serves: WiFi setup portal, pin status API, nodes API, weekly timer API, OTA upload.
 * Six-tab UI: Pin Status | Debug | Nodes | Weekly Timer | WiFi | OTA Update
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/param.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_tls.h"
#include "cJSON.h"

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

extern volatile bool     g_wifi_save_requested;
extern char              g_new_ssid[64];
extern char              g_new_pass[64];
extern volatile uint32_t g_wifi_disconnect_count;
extern volatile int64_t  g_wifi_last_disc_ts_us;
extern          uint32_t g_heap_min;
extern          int64_t  g_heap_min_ts_us;

extern bool              g_ipcfg_static;
extern char              g_ipcfg_ip[16];
extern char              g_ipcfg_mask[16];
extern char              g_ipcfg_gw[16];
extern char              g_ipcfg_dns[16];
extern volatile bool     g_ipcfg_save_requested;

/* Pin numbers (mirror main.c) */
#define PIN_HONDA_STARTED_FB     13
#define PIN_HONDA_START          15
#define PIN_HONDA_MANUAL_START   14
#define PIN_WALLAS_START          4
#define PIN_WALLAS_MANUAL_START   5

/* ── GitHub-releases OTA state ──────────────────────────────────────────────
 * Install runs in a background task (gh_ota_task) so the httpd worker that
 * accepted /api/ota/github/install isn't blocked for the whole download --
 * the OTA tab's progress bar needs to poll /api/ota/github/progress
 * concurrently while it runs. Single-writer(task)/single-reader(httpd)
 * polling, no locking, matching this codebase's existing status-field style. */
#define GH_RELEASES_URL     "https://api.github.com/repos/SEspe/RemoteStart/releases?per_page=8"
#define GH_ASSET_URL_PREFIX "https://github.com/SEspe/RemoteStart/releases/download/"
#define GH_REPO_URL         "https://github.com/SEspe/RemoteStart"
enum { GH_OTA_IDLE = 0, GH_OTA_DOWNLOADING, GH_OTA_SUCCESS, GH_OTA_ERROR };
static volatile int     g_gh_ota_state   = GH_OTA_IDLE;
static volatile int     g_gh_ota_percent = 0;
static volatile int64_t g_gh_ota_total   = 0;
static char             g_gh_ota_url[256] = "";
static char             g_gh_ota_err[128] = "";
static char             g_gh_ota_trace[600] = "";   /* HTTP event trace, for diagnosing install
                                                        failures without needing USB/serial access */

/* Traces low-level esp_http_client events during gh_ota_task's download --
 * exposed via /api/ota/github/progress. This is how the GitHub redirect
 * buffer-overflow bug (buffer_size_tx too small for the ~2KB signed
 * redirect target) was diagnosed and fixed without USB access. */
static esp_err_t gh_http_event_handler(esp_http_client_event_t *evt)
{
    size_t len = strlen(g_gh_ota_trace);
    if (len >= sizeof(g_gh_ota_trace) - 60) return ESP_OK;
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR: {
            esp_tls_error_handle_t eh = (esp_tls_error_handle_t)evt->data;
            if (eh) {
                snprintf(g_gh_ota_trace + len, sizeof(g_gh_ota_trace) - len,
                         "|ERR(le=%d,tlsc=%d,flags=%d)",
                         (int)eh->last_error, eh->esp_tls_error_code, eh->esp_tls_flags);
            } else {
                snprintf(g_gh_ota_trace + len, sizeof(g_gh_ota_trace) - len, "|ERR(noeh)");
            }
            break;
        }
        case HTTP_EVENT_ON_CONNECTED:
            snprintf(g_gh_ota_trace + len, sizeof(g_gh_ota_trace) - len, "|CONN"); break;
        case HTTP_EVENT_HEADERS_SENT:
            snprintf(g_gh_ota_trace + len, sizeof(g_gh_ota_trace) - len, "|SENT"); break;
        case HTTP_EVENT_ON_HEADERS_COMPLETE:
            snprintf(g_gh_ota_trace + len, sizeof(g_gh_ota_trace) - len,
                     "|HDRDONE(st=%d)", esp_http_client_get_status_code(evt->client));
            break;
        case HTTP_EVENT_ON_HEADER:
            if (evt->header_key && strcasecmp(evt->header_key, "Location") == 0)
                snprintf(g_gh_ota_trace + len, sizeof(g_gh_ota_trace) - len, "|LOC=%.50s", evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            snprintf(g_gh_ota_trace + len, sizeof(g_gh_ota_trace) - len, "|DATA=%d", evt->data_len); break;
        case HTTP_EVENT_ON_FINISH:
            snprintf(g_gh_ota_trace + len, sizeof(g_gh_ota_trace) - len, "|FIN"); break;
        case HTTP_EVENT_DISCONNECTED:
            snprintf(g_gh_ota_trace + len, sizeof(g_gh_ota_trace) - len, "|DISC"); break;
        default: break;
    }
    return ESP_OK;
}

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
".dbg{display:grid;grid-template-columns:repeat(auto-fill,minmax(120px,1fr));gap:6px}"
".dbg .row{flex-direction:column;align-items:stretch;gap:4px}"
".dbg .row>span:first-child{display:inline-flex;align-items:center;justify-content:center;gap:4px;font-weight:bold}"
".btns{display:flex;gap:4px}"
".on-l{background:#4caf50;box-shadow:0 0 5px #4caf50}.off-l{background:#444}"
".ok{color:#4caf50}.dim{color:#888}"
".ts{font-size:.7rem;color:#555;margin-top:6px}"
".tbl-wrap{overflow-x:auto}"
"table.nodes{border-collapse:collapse;width:100%;font-size:.78rem;white-space:nowrap}"
"table.nodes th,table.nodes td{padding:6px 10px;text-align:left;border-bottom:1px solid #0f3460}"
"table.nodes th{color:#e94560;font-size:.72rem;text-transform:uppercase}"
"table.nodes a{color:#4ea8ff;text-decoration:none}"
"table.nodes a:hover{text-decoration:underline}"
".ctl{display:flex;gap:8px;margin-top:8px}"
"button.go{flex:1;padding:8px;border:none;border-radius:5px;color:#fff;cursor:pointer;font-size:.85rem}"
"button.go-on{background:#4caf50}button.go-off{background:#555}"
"button.go:disabled{background:#333;color:#777;cursor:default}"
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
"a.up.gh-link{display:inline-block;padding:8px 20px;border-radius:5px;font-size:.9rem;"
"text-decoration:none;color:#fff;background:#0f3460;border:1px solid #e94560}"
".wf-lbl{display:block;margin:10px 0 4px;font-size:.85rem}"
".wf-in{width:100%;max-width:280px;padding:8px;background:#0f3460;border:1px solid #444;"
"border-radius:5px;color:#eee;box-sizing:border-box}"
".wf-btn{padding:10px 20px;border:none;border-radius:5px;color:#fff;"
"cursor:pointer;font-size:.9rem;margin-top:10px}"
".wf-btn-s{background:#0f3460;border:1px solid #e94560}"
".wf-btn-c{background:#e94560}"
".wf-sts{font-size:.75rem;color:#888;min-height:1rem;margin:6px 0}"
".wf-nets{max-height:160px;overflow-y:auto;margin-bottom:10px;max-width:320px}"
".wf-net{background:#0f3460;border-radius:5px;padding:8px 12px;margin-bottom:4px;"
"cursor:pointer;display:flex;justify-content:space-between;align-items:center;"
"font-size:.85rem}"
".wf-net:hover{background:#1a4a80}"
".wf-meta{font-size:.72rem;color:#aaa}"
".wf-lk{color:#e94560}"
".wf-radio{font-size:.88rem;margin-right:18px}"
".wf-warn{font-size:.78rem;color:#e9b400;margin:10px 0}"
".gh-card select{width:100%;max-width:360px;padding:8px;background:#0f3460;border:1px solid #444;"
"border-radius:5px;color:#eee;box-sizing:border-box;margin:8px 0}"
".gh-bar-wrap{background:#eee;border-radius:5px;height:14px;margin-top:10px;overflow:hidden;display:none}"
".gh-bar{background:#e94560;height:100%;width:0%;transition:width .3s}"
".ver-wrap{overflow-x:auto}"
"table.ver{border-collapse:collapse;width:100%}"
"table.ver td{padding:7px 5px;border-bottom:1px solid #e6e6e6;vertical-align:middle}"
"table.ver tr:last-child td{border-bottom:none}"
".ver-u{font-size:.95rem;color:#333;white-space:nowrap;padding-right:12px}"
".ver-l{display:block;font-size:.62rem;color:#888;margin-bottom:2px}"
".ver-v{display:block;background:#e4e4e4;border-radius:6px;padding:5px 8px;text-align:center;"
"font-size:.95rem;color:#222;min-width:80px}"
".ver-v.old{background:#ffd27f;color:#4a3500;font-weight:bold}"
".ver-v.na{background:#f0f0f0;color:#aaa}"
".ver-cfg{display:block;font-size:.78rem;color:#555;margin-top:12px}"
".ver-cfg input{width:170px;padding:5px 7px;border:1px solid #ccc;border-radius:5px;"
"color:#333;background:#fff;margin-left:6px}"
"</style></head><body>"
"<div class='hdr'><h1>&#9889; MasterRemote</h1>"
"<span class='v'>v" FIRMWARE_VERSION " &nbsp;|&nbsp; %IP%</span></div>"
"<div class='tabs'>"
"<button class='tab on' onclick='show(\"status\",this)'>Pin Status</button>"
"<button class='tab' onclick='show(\"debug\",this)'>Debug</button>"
"<button class='tab' onclick='show(\"nodes\",this)'>Nodes</button>"
"<button class='tab' onclick='show(\"timer\",this)'>Weekly Timer</button>"
"<button class='tab' onclick='show(\"wifinet\",this)'>WiFi</button>"
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
"<div class='ts' id='sts'></div></div>"
"<div id='debug' class='pane'>"
"<div class='card'><h3>WiFi Link</h3><div class='grid' id='wifi'>…</div></div>"
"<div class='card'><h3>System</h3><div class='grid' id='sys'>…</div></div>"
"<div class='card'><h3>Manual GPIO Test</h3>"
"<p class='wf-warn'>Hardware bring-up only. Every GPIO's live level is shown as a "
"dot (green = HIGH). Pins with ON/OFF buttons can be forced to OUTPUT and "
"driven, overriding normal operation until next reboot. Reserved pins "
"(strapping: 0,2,5,12,15; flash: 6-11; UART0 console: 1,3; input-only: 34-39) "
"show status only, no buttons.</p>"
"<p class='wf-warn'>On this unit specifically, GPIO4 (Wallas / Victron relay) and "
"GPIO14 (Honda manual switch) are <b>inputs driven by external contacts</b> &mdash; "
"forcing them to OUTPUT puts the pin in contention with whatever is wired to it. "
"Read their dots freely; only drive them with the external wiring disconnected.</p>"
"<div class='dbg' id='dbg'>…</div></div>"
"</div>"
"<div id='nodes' class='pane'>"
"<div class='tbl-wrap' id='cli'>…</div><div class='ts' id='cts'></div></div>"
"<div id='timer' class='pane'>"
"<div class='card'><h3>Weekly Wallas Timer</h3>"
"<p class='warn' id='clock'>Clock: …</p>"
"<div class='tbl-wrap'><table class='nodes' id='timerTbl'>…</table></div>"
"<div class='ctl'><button class='go go-on' id='timerSetBtn' disabled onclick='saveAllTimer()'>Set</button></div>"
"</div></div>"
"<div id='wifinet' class='pane'>"
"<div class='card'><h3>WiFi Network</h3>"
"<div class='grid' id='wfLive'>…</div>"
"<div class='wf-sts' id='wfSts'></div>"
"<button class='wf-btn wf-btn-s' onclick='wfScan()'>&#128246; Scan Networks</button>"
"<div class='wf-nets' id='wfNets'></div>"
"<form action='/wifi-save' method='POST'>"
"<label class='wf-lbl'>SSID</label>"
"<input class='wf-in' id='wfSid' name='ssid' type='text' placeholder='tap network or type' required>"
"<label class='wf-lbl'>Password</label>"
"<input class='wf-in' name='pass' type='password' placeholder='leave blank if open'>"
"<br><button class='wf-btn wf-btn-c' type='submit'>Connect &amp; Save</button>"
"</form></div>"
"<div class='card'><h3>IP Configuration</h3>"
"<div class='grid' id='ipLive'>…</div>"
"<form action='/api/ipconfig/save' method='POST' onsubmit='return ipValidate()'>"
"<label class='wf-radio'><input type='radio' name='mode' value='dhcp' id='ipDhcp' checked "
"onchange='ipModeChange()'> Automatic (DHCP)</label>"
"<label class='wf-radio'><input type='radio' name='mode' value='static' id='ipStatic' "
"onchange='ipModeChange()'> Static IP</label>"
"<div id='ipFields' style='display:none'>"
"<label class='wf-lbl'>IP Address</label><input class='wf-in' name='ip' id='ipAddr' type='text' placeholder='192.168.1.50'>"
"<label class='wf-lbl'>Subnet Mask</label><input class='wf-in' name='mask' id='ipMask' type='text' placeholder='255.255.255.0'>"
"<label class='wf-lbl'>Gateway</label><input class='wf-in' name='gw' id='ipGw' type='text' placeholder='192.168.1.1'>"
"<label class='wf-lbl'>DNS Server</label><input class='wf-in' name='dns' id='ipDns' type='text' placeholder='optional'>"
"</div>"
"<p class='wf-warn'>Saving reboots the device to apply. If a static IP is unreachable afterward, "
"recover over USB (erase NVS to restore DHCP).</p>"
"<button class='wf-btn wf-btn-c' type='submit'>Save &amp; Apply</button>"
"</form></div>"
"<div class='card'><h3>Device</h3>"
"<p class='wf-warn'>Rebooting drops the WiFi/ESP-NOW link briefly and clears in-memory state "
"(node roster, manual overrides). Physical relay/GPIO state is unaffected.</p>"
"<button class='wf-btn wf-btn-s' onclick='rebootDevice()'>&#128260; Reboot Now</button>"
"<div class='wf-sts' id='rebootSts'></div>"
"</div></div>"
"<div id='ota' class='pane'>"
"<div class='ota-wrap'>"
"<h3>Firmware Versions</h3>"
"<p><b>GitHub</b> = newest release asset for that unit &nbsp;&#8226;&nbsp; "
"<b>Stored</b> = image sitting in the unit's inactive OTA slot &nbsp;&#8226;&nbsp; "
"<b>Installed</b> = what it is running now. An amber Installed box means GitHub has a newer build.</p>"
"<div class='ver-wrap' id='verTbl'></div>"
"<label class='ver-cfg'>ESPNowRelay address (optional, this browser only)"
"<input id='verRelay' type='text' placeholder='e.g. 192.168.1.103' oninput='verSaveRelay()'></label>"
"<div class='prog' id='verSts'></div>"
"<button class='up' onclick='verRefresh()'>Refresh Versions</button>"
"</div>"
"<div class='ota-wrap gh-card'>"
"<h3>Install from GitHub Releases</h3>"
"<p>Fetch and flash a release straight from <code>github.com/SEspe/RemoteStart</code> &mdash; no manual download needed.</p>"
"<button class='up' onclick='ghRefresh()'>Refresh Releases</button> "
"<a class='up gh-link' href='" GH_REPO_URL "' target='_blank' rel='noopener'>Go GitHub</a>"
"<div class='prog' id='ghSts'></div>"
"<select id='ghSel' style='display:none'></select><br>"
"<button class='up' id='ghInstallBtn' style='display:none' onclick='ghInstall()'>Install Selected</button>"
"<div class='gh-bar-wrap' id='ghBarWrap'><div class='gh-bar' id='ghBar'></div></div>"
"<div class='prog' id='ghProg'></div>"
"</div>"
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
"if(id==='timer')fetchTimer();"
"if(id==='debug'){fetchStatus();fetchSys();}"
"if(id==='wifinet')fetchIpCfg();"
"if(id==='ota'&&!g_verLoaded){g_verLoaded=true;verRefresh();}}"
"function led(v){return '<div class=\"led '+(v?'on-l':'off-l')+'\"></div>';}"
"function row(n,v){return '<div class=\"row\"><span>'+n+'</span>'+led(v)+'</div>';}"
"function txtRow(n,v){return '<div class=\"row\"><span>'+n+'</span><span>'+v+'</span></div>';}"
"var DBG_PINS=[4,13,14,16,17,18,19,21,22,23,25,26,27,32,33];"
"function buildDbg(){"
"var h='';"
"for(var p=0;p<=39;p++){"
"var manip=DBG_PINS.indexOf(p)!==-1;"
"var ctl=manip?"
"'<span class=\"btns\"><button class=\"go go-on\" onclick=\"gset('+p+',1)\">ON</button>"
"<button class=\"go go-off\" onclick=\"gset('+p+',0)\">OFF</button></span>':"
"'<span class=\"dim\" style=\"font-size:.7rem\">reserved</span>';"
"h+='<div class=\"row\"><span>GPIO'+p+' : <span class=\"led off-l\" id=\"dl'+p+'\"></span></span>'+ctl+'</div>';}"
"document.getElementById('dbg').innerHTML=h;}"
"function fetchDbg(){"
"fetch('/api/gpio/status').then(r=>r.json()).then(d=>{"
"d.lvl.forEach(function(v,p){"
"var el=document.getElementById('dl'+p);"
"if(el)el.className='led '+(v?'on-l':'off-l');});"
"}).catch(()=>{});}"
"function gset(pin,level){fetch('/api/gpio/set?pin='+pin+'&level='+level).catch(()=>{});}"
"function fmtKB(n){return Math.round(n/1024)+' KB';}"
"function fmtUptime(s){var d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);"
"return (d?d+'d ':'')+(h||d?h+'h ':'')+m+'m';}"
"function fetchSys(){"
"fetch('/api/sysinfo').then(r=>r.json()).then(d=>{"
"document.getElementById('sys').innerHTML="
"txtRow('Free Heap',fmtKB(d.heap))+"
"txtRow('Min Free Heap (low-water)',fmtKB(d.heapMin)+' \\u2014 '+fmtUptime(d.heapMinAgo)+' ago')+"
"txtRow('Uptime',fmtUptime(d.uptime))+txtRow('WiFi Reconnects',d.wifiDisc)+"
"txtRow('Last WiFi Reconnect',d.wifiDiscAgo<0?'never':fmtUptime(d.wifiDiscAgo)+' ago');"
"}).catch(()=>{});}"
"function cmd(unit,action){"
"fetch('/api/'+unit+'/'+action,{method:'POST'}).then(fetchStatus).catch(()=>{});}"
"var DAY_NAMES=['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];"
"function buildTimer(){"
"let h='<tr><th>Day</th><th>Enabled</th><th>Start</th><th>Stop</th></tr>';"
"for(let d=0;d<7;d++){"
"h+='<tr><td>'+DAY_NAMES[d]+'</td>'+"
"'<td><input type=\"checkbox\" id=\"ten'+d+'\" onchange=\"markTimerDirty()\"></td>'+"
"'<td><input type=\"time\" id=\"tst'+d+'\" onchange=\"markTimerDirty()\"></td>'+"
"'<td><input type=\"time\" id=\"tsp'+d+'\" onchange=\"markTimerDirty()\"></td></tr>';}"
"document.getElementById('timerTbl').innerHTML=h;}"
"function markTimerDirty(){document.getElementById('timerSetBtn').disabled=false;}"
"function fetchTimer(){"
"fetch('/api/timer').then(r=>r.json()).then(d=>{"
"document.getElementById('clock').textContent='Clock: '+d.now+' (CET/CEST)'+(d.synced?'':' \\u2014 NOT SYNCED YET');"
"d.days.forEach(function(x){"
"document.getElementById('ten'+x.day).checked=x.enabled;"
"document.getElementById('tst'+x.day).value=x.start;"
"document.getElementById('tsp'+x.day).value=x.stop;});"
"document.getElementById('timerSetBtn').disabled=true;"
"}).catch(()=>{});}"
"function saveAllTimer(){"
"const btn=document.getElementById('timerSetBtn');btn.disabled=true;"
"let ps=[];"
"for(let d=0;d<7;d++){"
"const en=document.getElementById('ten'+d).checked?1:0;"
"const st=document.getElementById('tst'+d).value||'00:00';"
"const sp=document.getElementById('tsp'+d).value||'00:00';"
"ps.push(fetch('/api/timer/set?day='+d+'&enabled='+en+'&start='+st+'&stop='+sp,{method:'POST'}));}"
"Promise.all(ps).catch(()=>{btn.disabled=false;});}"
"function fetchStatus(){"
"fetch('/api/status').then(r=>r.json()).then(d=>{"
"document.getElementById('pins').innerHTML="
"row('Honda Start (p15)',d.pHS)+row('Honda Manual (p14)',d.pHM)+"
"row('Wallas Start (p4)',d.pWS)+row('Wallas Manual (p5)',d.pWM)+"
"row('Honda FB LED (p13)',d.pFB);"
"document.getElementById('state').innerHTML="
"row('Honda Start CMD',d.gHS)+row('Honda Slave Running',d.gHR)+row('Wallas CMD',d.gWS);"
"document.getElementById('wifi').innerHTML="
"txtRow('Own MAC',d.mac||'\\u2014')+"
"txtRow('Signal strength',d.rssi+' dBm')+txtRow('Channel',d.ch);"
"document.getElementById('sts').textContent='Updated: '+new Date().toLocaleTimeString();"
"}).catch(()=>{});}"
"function yn(v){return '<span class=\"'+(v?'ok':'dim')+'\">'+(v?'Yes':'No')+'</span>';}"
"function nodeRow(n,detail){"
"return '<tr><td>'+n.role+'</td>'+"
"'<td>'+(n.fw?'v'+n.fw:'\\u2014')+'</td>'+"
"'<td>'+(n.mac||'\\u2014')+'</td>'+"
"'<td>'+(n.ip?'<a href=\"http://'+n.ip+'/\" target=\"_blank\" rel=\"noopener\">'+n.ip+'</a>':'\\u2014')+'</td>'+"
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
/* ── OTA tab: firmware version table ───────────────────────────────────────
 * MasterRemote's table covers all four units. Its own row comes from
 * /api/version locally; SlaveHonda's and SlaveWallas's from the same endpoint
 * on their addresses (taken from /api/nodes, so it follows a DHCP move);
 * ESPNowRelay is not in the ESP-NOW roster at all -- the relay is deliberately
 * transparent and never announces itself to Master -- so its address is typed
 * once and kept in the browser's localStorage rather than adding an ESP-NOW
 * message type to a repeater whose whole point is that it changes nothing. */
"var VER_ROWS=[],g_verLoaded=false;"
"function verEsc(s){return String(s).replace(/[&<>\"']/g,'');}"
"function verNum(v){return String(v||'').replace(/^v/,'').split('.').map(function(x){"
"return parseInt(x,10)||0;});}"
"function verOlder(a,b){var A=verNum(a),B=verNum(b),n=Math.max(A.length,B.length);"
"for(var i=0;i<n;i++){var x=A[i]||0,y=B[i]||0;if(x!==y)return x<y;}return false;}"
"function verCell(l,v,c){return '<td><span class=\"ver-l\">'+l+'</span>"
"<span class=\"ver-v '+(v?c:'na')+'\">'+(v?'v'+verEsc(v):'\\u2014')+'</span></td>';}"
"function verRow(u){"
"var upd=u.gh&&u.installed&&verOlder(u.installed,u.gh);"
"return '<tr><td class=\"ver-u\">'+verEsc(u.name)+'</td>'+"
"verCell('GitHub',u.gh,'')+verCell('Stored',u.stored,'')+"
"verCell('Installed',u.installed,upd?'old':'')+'</tr>';}"
"function verRender(){document.getElementById('verTbl').innerHTML="
"'<table class=\"ver\">'+VER_ROWS.map(verRow).join('')+'</table>';}"
/* Never rejects: an unreachable or unconfigured unit resolves to null and just
 * leaves its cells blank, instead of failing the whole table. */
"function verFetch(u,ms){return new Promise(function(res){"
"if(u===null){res(null);return;}"
"var c=new AbortController(),t=setTimeout(function(){c.abort();},ms);"
"fetch(u,{signal:c.signal}).then(function(r){return r.ok?r.json():null;})"
".then(function(j){clearTimeout(t);res(j);})"
".catch(function(){clearTimeout(t);res(null);});});}"
"function verSaveRelay(){try{localStorage.setItem('verRelayAddr',"
"document.getElementById('verRelay').value.trim());}catch(e){}}"
"function verInitRelay(){try{document.getElementById('verRelay').value="
"localStorage.getItem('verRelayAddr')||'';}catch(e){}}"
"function verRefresh(){"
"var sts=document.getElementById('verSts');sts.textContent='Reading versions\\u2026';"
"var rows=[{name:'" FIRMWARE_NAME "',base:''},{name:'SlaveHonda',base:null},"
"{name:'SlaveWallas',base:null},{name:'ESPNowRelay',base:null}];"
"verFetch('/api/nodes',5000).then(function(n){"
"if(n&&n.h&&n.h.ip)rows[1].base='http://'+n.h.ip;"
"if(n&&n.w&&n.w.ip)rows[2].base='http://'+n.w.ip;"
"var ra='';try{ra=(localStorage.getItem('verRelayAddr')||'').trim();}catch(e){}"
"if(ra)rows[3].base='http://'+ra;"
"return Promise.all([verFetch('/api/ota/github/latest',15000)].concat("
"rows.map(function(r){return verFetch(r.base===null?null:r.base+'/api/version',6000);})));"
"}).then(function(res){"
"var gh=res[0]||{};"
"VER_ROWS=rows.map(function(r,i){var v=res[i+1]||{};"
"return {name:r.name,gh:gh[r.name]||'',installed:v.installed||'',stored:v.stored||''};});"
"verRender();"
"sts.textContent='Updated '+new Date().toLocaleTimeString()+"
"(res[0]?'':' \\u2014 GitHub unreachable');});}"
"var g_ghReleases=[],g_ghPollTimer=null;"
"function ghRefresh(){"
"var sts=document.getElementById('ghSts');"
"sts.textContent='Loading releases\\u2026';"
"document.getElementById('ghSel').style.display='none';"
"document.getElementById('ghInstallBtn').style.display='none';"
"fetch('/api/ota/github/releases').then(function(r){return r.json();})"
".then(function(a){"
"g_ghReleases=a;"
"if(!a.length){sts.textContent='No matching releases found for this unit.';return;}"
"sts.textContent=a.length+' release'+(a.length!==1?'s':'')+' found';"
"var sel=document.getElementById('ghSel');"
"sel.innerHTML='';"
"a.forEach(function(r,i){"
"var o=document.createElement('option');"
"o.value=i;"
"o.textContent=r.tag+' \\u2014 '+r.published.slice(0,10)+' ('+r.asset+')';"
"sel.appendChild(o);});"
"sel.style.display='block';"
"document.getElementById('ghInstallBtn').style.display='inline-block';"
"}).catch(function(){sts.textContent='Failed to fetch releases from GitHub';});}"
"function ghInstall(){"
"var sel=document.getElementById('ghSel');"
"var r=g_ghReleases[sel.value];"
"if(!r)return;"
"if(!confirm('Install '+r.tag+' ('+r.asset+') now? The device will download and flash it, then reboot.'))return;"
"document.getElementById('ghInstallBtn').disabled=true;"
"sel.disabled=true;"
"document.getElementById('ghProg').textContent='Starting install\\u2026';"
"fetch('/api/ota/github/install',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({url:r.url,size:r.size})})"
".then(function(resp){"
"if(!resp.ok)throw new Error('HTTP '+resp.status);"
"document.getElementById('ghBarWrap').style.display='block';"
"g_ghPollTimer=setInterval(ghPoll,1000);"
"}).catch(function(e){"
"document.getElementById('ghProg').textContent='Install failed to start: '+e.message;"
"document.getElementById('ghInstallBtn').disabled=false;"
"sel.disabled=false;});}"
"function ghPoll(){"
"fetch('/api/ota/github/progress').then(function(r){return r.json();}).then(function(d){"
"document.getElementById('ghBar').style.width=d.percent+'%';"
"if(d.state==='downloading'){"
"document.getElementById('ghProg').textContent='Downloading & flashing: '+d.percent+'%';"
"}else if(d.state==='success'){"
"document.getElementById('ghProg').textContent='Success! Rebooting\\u2026';"
"clearInterval(g_ghPollTimer);"
"}else if(d.state==='error'){"
"document.getElementById('ghProg').textContent='Error: '+d.error;"
"clearInterval(g_ghPollTimer);"
"document.getElementById('ghInstallBtn').disabled=false;"
"document.getElementById('ghSel').disabled=false;"
"}}).catch(function(){});}"
"function wfEsc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;');}"
"function wfScan(){"
"var st=document.getElementById('wfSts');"
"st.textContent='Scanning\\u2026';"
"document.getElementById('wfNets').innerHTML='';"
"fetch('/api/scan').then(function(r){return r.json();})"
".then(function(a){"
"st.textContent=a.length+' network'+(a.length!==1?'s':'')+' found';"
"var c=document.getElementById('wfNets');"
"a.forEach(function(n){"
"var d=document.createElement('div');d.className='wf-net';"
"d.innerHTML='<span>'+wfEsc(n.ssid)+'</span>"
"<span class=wf-meta>'+n.rssi+'dBm "
"'+(n.auth?'<span class=wf-lk>&#128274;</span>':'open')+'</span>';"
"d.onclick=function(){document.getElementById('wfSid').value=n.ssid;};"
"c.appendChild(d);});}).catch(function(){"
"st.textContent='Scan failed \\u2014 try again';});}"
"var g_liveIp='',g_liveMask='',g_liveGw='',g_liveDns='';"
"function ipModeChange(){"
"var isStatic=document.getElementById('ipStatic').checked;"
"document.getElementById('ipFields').style.display=isStatic?'block':'none';"
"if(isStatic){"
"var ipEl=document.getElementById('ipAddr'),mEl=document.getElementById('ipMask'),gEl=document.getElementById('ipGw'),dEl=document.getElementById('ipDns');"
"if(!ipEl.value&&g_liveIp)ipEl.value=g_liveIp;"
"if(!mEl.value&&g_liveMask)mEl.value=g_liveMask;"
"if(!gEl.value&&g_liveGw)gEl.value=g_liveGw;"
"if(!dEl.value&&g_liveDns)dEl.value=g_liveDns;}}"
"function ipValidate(){"
"if(!document.getElementById('ipStatic').checked)return true;"
"var re=/^\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}$/;"
"if(!re.test(document.getElementById('ipAddr').value)||!re.test(document.getElementById('ipMask').value)){"
"alert('Enter a valid IP address and subnet mask');return false;}"
"return true;}"
"function fetchIpCfg(){"
"fetch('/api/ipconfig').then(r=>r.json()).then(d=>{"
"g_liveIp=d.liveIp||'';g_liveMask=d.liveMask||'';g_liveGw=d.liveGw||'';g_liveDns=d.liveDns||'';"
"document.getElementById('wfLive').innerHTML="
"txtRow('Configured SSID',d.ssid?wfEsc(d.ssid):'not configured')+"
"txtRow('Connected To',d.apSsid?wfEsc(d.apSsid):'not associated');"
"var sidEl=document.getElementById('wfSid');"
"if(!sidEl.value&&d.ssid)sidEl.value=d.ssid;"
"document.getElementById('ipLive').innerHTML="
"txtRow('Address Source',d.static?'Static (configured)':'Automatic (DHCP)')+"
"txtRow('Current IP',d.liveIp||'\\u2014')+txtRow('Subnet Mask',d.liveMask||'\\u2014')+"
"txtRow('Gateway',d.liveGw||'\\u2014')+txtRow('DNS Server',d.liveDns||'\\u2014');"
"document.getElementById('ipDhcp').checked=!d.static;"
"document.getElementById('ipStatic').checked=d.static;"
"document.getElementById('ipAddr').value=d.ip;"
"document.getElementById('ipMask').value=d.mask;"
"document.getElementById('ipGw').value=d.gw;"
"document.getElementById('ipDns').value=d.dns;"
"ipModeChange();"
"}).catch(()=>{});}"
"function rebootDevice(){"
"if(!confirm('Reboot the device now?'))return;"
"document.getElementById('rebootSts').textContent='Rebooting\\u2026';"
"fetch('/api/reboot',{method:'POST'}).catch(()=>{});}"
"setInterval(()=>{"
"var stOn=document.getElementById('status').classList.contains('on');"
"var dbOn=document.getElementById('debug').classList.contains('on');"
"if(stOn||dbOn)fetchStatus();"
"if(dbOn){fetchSys();fetchDbg();}"
"if(document.getElementById('nodes').classList.contains('on'))fetchNodes();"
"},2000);"
"fetchStatus();"
"fetchSys();"
"buildDbg();"
"fetchDbg();"
"buildTimer();"
"fetchTimer();"
"verInitRelay();"
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
    if (netif && esp_netif_get_ip_info(netif, &info) == ESP_OK)
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));

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

    uint8_t own_mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, own_mac);
    char mac_str[20];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             own_mac[0], own_mac[1], own_mac[2], own_mac[3], own_mac[4], own_mac[5]);

    char buf[360];
    snprintf(buf, sizeof(buf),
        "{\"pHS\":%s,\"pHM\":%s,\"pWS\":%s,\"pWM\":%s,\"pFB\":%s,"
        "\"gHS\":%s,\"gHR\":%s,\"gWS\":%s,\"rssi\":%d,\"ch\":%d,\"mac\":\"%s\"}",
        !gpio_get_level(PIN_HONDA_START)        ? "true" : "false",
         gpio_get_level(PIN_HONDA_MANUAL_START) ? "true" : "false",
        !gpio_get_level(PIN_WALLAS_START)       ? "true" : "false",   /* inverted on this hardware */
         gpio_get_level(PIN_WALLAS_MANUAL_START)? "true" : "false",   /* non-inverted like PIN_HONDA_MANUAL_START, reconnected 2026-07-04 */
         gpio_get_level(PIN_HONDA_STARTED_FB)   ? "true" : "false",
        g_honda_start_cmd    ? "true" : "false",
        g_slave_honda_running? "true" : "false",
        g_wallas_start_cmd   ? "true" : "false",
        (int)ap_info.rssi, (int)ap_info.primary, mac_str);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* GET /api/sysinfo  ── self-monitoring: heap + low-water mark + uptime + WiFi
 * reconnect count, for spotting a slow resource leak before it kills the
 * web server (a steadily falling heap low-water mark is the tell). */
static esp_err_t h_sysinfo(httpd_req_t *req)
{
    /* wifiDiscAgo: seconds since the last WIFI_EVENT_STA_DISCONNECTED, or -1
     * if none since boot -- lets you compare units querying at roughly the
     * same moment to tell a synchronized (shared router/AP-side) disconnect
     * apart from independent per-unit RF flakiness. */
    long long disc_ago = g_wifi_last_disc_ts_us
        ? (long long)((esp_timer_get_time() - g_wifi_last_disc_ts_us) / 1000000)
        : -1;
    char buf[192];
    snprintf(buf, sizeof(buf), "{\"heap\":%lu,\"heapMin\":%lu,\"heapMinAgo\":%lld,\"uptime\":%lld,\"wifiDisc\":%lu,\"wifiDiscAgo\":%lld}",
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)g_heap_min,
        (long long)((esp_timer_get_time() - g_heap_min_ts_us) / 1000000),
        (long long)(esp_timer_get_time() / 1000000),
        (unsigned long)g_wifi_disconnect_count,
        disc_ago);
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
    /* PIN_WALLAS_START reads inverted, PIN_WALLAS_MANUAL_START non-inverted
     * on this hardware -- see main.c gpio_init()/isr_wallas_recompute. */
    g_wallas_start_cmd = !gpio_get_level(PIN_WALLAS_START) ||
                          gpio_get_level(PIN_WALLAS_MANUAL_START) ||
                          g_timer_wallas_start;
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
    url_decode(g_new_ssid);
    url_decode(g_new_pass);

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

/* GET /api/gpio/set?pin=N&level=0|1  ── DEBUG ONLY: force any non-reserved
 * GPIO to OUTPUT and drive it. For hardware bring-up when pin wiring is
 * uncertain -- reconfigures the pin every call, overriding whatever normal
 * operation was doing with it until the next reboot.
 *
 * Note this unit's exposure differs from the slaves': its Victron-relay and
 * manual-switch pins are INPUTS fed by external contacts (§2), so driving one
 * of those puts the ESP32 pad in contention with whatever is wired to it. The
 * Debug tab says so; the reserved-pin rules below already cover GPIO5/GPIO15
 * (strapping), leaving GPIO4 and GPIO14 as the pins to be careful with. */
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

/* One literal key the release scanner watches for in the streaming response. */
typedef struct { const char *pat; size_t len; size_t k; } gh_key_t;

/* Advances a literal matcher by one byte; true when the whole key completes. */
static bool gh_key_feed(gh_key_t *k, char c)
{
    if (c == k->pat[k->k]) {
        if (++k->k == k->len) { k->k = 0; return true; }
    } else {
        k->k = (c == k->pat[0]) ? 1 : 0;
    }
    return false;
}

/* GET /api/ota/github/releases  ── recent GitHub releases with a binary matching
 * this unit (FIRMWARE_NAME), newest first. Releases without a matching asset
 * (e.g. a single-unit tag for a different unit) are omitted entirely.
 *
 * The body is scanned byte-by-byte as it streams in, rather than buffered
 * whole and handed to cJSON. Every published release carries four per-unit
 * assets of ~20 JSON fields each, so the listing had grown past 68 KB; a full
 * parse needs the raw buffer plus a cJSON tree several times its size, which
 * against the ~200 KB heap these units run with had already stopped fitting on
 * MasterRemote (§12, v1.48) while still squeaking through on the others. Since
 * the payload grows with every release, trimming per_page would only have
 * postponed it -- the scanner instead holds nothing but the fields it is
 * currently capturing, so peak cost no longer depends on the release count.
 *
 * Key order within the JSON is what makes this work: tag_name and published_at
 * belong to the release object and precede its assets array, and within an
 * asset, size precedes browser_download_url. So each URL completes a candidate
 * using the most recent tag/date/size seen, and the asset filename is the
 * URL's own last path segment -- no separate "name" lookup needed. */
static esp_err_t h_ota_gh_releases(httpd_req_t *req)
{
    esp_http_client_config_t cfg = {
        .url = GH_RELEASES_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "HTTP client init failed");
        return ESP_OK;
    }
    esp_http_client_set_header(client, "User-Agent", "RemoteStart-ESP32");
    esp_http_client_set_header(client, "Accept", "application/vnd.github+json");

    if (esp_http_client_open(client, 0) != ESP_OK) {
        esp_http_client_cleanup(client);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "GitHub connection failed");
        return ESP_OK;
    }
    /* Return value deliberately unused: it is -1 for a chunked response, which
     * the scanner handles fine -- only the status code is a real gate here. */
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        char msg[64];
        snprintf(msg, sizeof(msg), "GitHub API returned HTTP %d", status);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, msg);
        return ESP_OK;
    }

    const size_t out_cap = 3072;
    char *out = malloc(out_cap);
    if (!out) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_OK;
    }

    gh_key_t k_tag  = { "\"tag_name\":\"",             12, 0 };
    gh_key_t k_pub  = { "\"published_at\":\"",         16, 0 };
    gh_key_t k_size = { "\"size\":",                    7, 0 };
    gh_key_t k_url  = { "\"browser_download_url\":\"", 24, 0 };

    enum { CAP_NONE = 0, CAP_TAG, CAP_PUB, CAP_SIZE, CAP_URL };
    int    cap = CAP_NONE;
    size_t caplen = 0;
    char   tag[40] = "", pub[40] = "", url[220] = "", emitted[40] = "";
    long   asize = 0;

    const char *prefix = FIRMWARE_NAME "_v";
    const size_t plen  = strlen(prefix);
    const size_t dlen  = strlen(GH_ASSET_URL_PREFIX);

    size_t n = 0;
    out[n++] = '[';
    bool first = true, full = false;
    char buf[512];
    int  r;

    while (!full && (r = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < r && !full; i++) {
            char c = buf[i];

            if (cap != CAP_NONE) {
                bool done = (cap == CAP_SIZE) ? (c < '0' || c > '9') : (c == '"');
                if (!done) {
                    switch (cap) {   /* anything overlong is silently truncated */
                    case CAP_TAG:  if (caplen < sizeof(tag) - 1) tag[caplen++] = c; break;
                    case CAP_PUB:  if (caplen < sizeof(pub) - 1) pub[caplen++] = c; break;
                    case CAP_URL:  if (caplen < sizeof(url) - 1) url[caplen++] = c; break;
                    case CAP_SIZE: if (caplen < 12) { asize = asize * 10 + (c - '0'); caplen++; } break;
                    }
                    continue;
                }
                if (cap == CAP_TAG) tag[caplen] = '\0';
                if (cap == CAP_PUB) pub[caplen] = '\0';
                if (cap == CAP_URL) {
                    url[caplen] = '\0';
                    /* One entry per release: the first matching asset wins, any
                     * later asset of the same release is skipped by tag. */
                    const char *slash = strrchr(url, '/');
                    const char *base  = slash ? slash + 1 : url;
                    size_t blen = strlen(base);
                    if (strncmp(url, GH_ASSET_URL_PREFIX, dlen) == 0 &&
                        blen > plen && strncmp(base, prefix, plen) == 0 &&
                        blen > 4 && strcmp(base + blen - 4, ".bin") == 0 &&
                        strcmp(tag, emitted) != 0) {
                        char entry[420];
                        int m = snprintf(entry, sizeof(entry),
                            "%s{\"tag\":\"%s\",\"published\":\"%s\",\"asset\":\"%s\","
                            "\"url\":\"%s\",\"size\":%ld}",
                            first ? "" : ",", tag, pub, base, url, asize);
                        if (m > 0 && n + (size_t)m < out_cap - 2) {
                            memcpy(out + n, entry, (size_t)m);
                            n += (size_t)m;
                            first = false;
                            strlcpy(emitted, tag, sizeof(emitted));
                        } else {
                            full = true;   /* listing full; newest are already in */
                        }
                    }
                }
                cap = CAP_NONE;
                /* fall through so the terminating byte still feeds the matchers */
            }

            bool m_tag  = gh_key_feed(&k_tag,  c);
            bool m_pub  = gh_key_feed(&k_pub,  c);
            bool m_size = gh_key_feed(&k_size, c);
            bool m_url  = gh_key_feed(&k_url,  c);
            if      (m_tag)  { cap = CAP_TAG;  caplen = 0; }
            else if (m_pub)  { cap = CAP_PUB;  caplen = 0; }
            else if (m_size) { cap = CAP_SIZE; caplen = 0; asize = 0; }
            else if (m_url)  { cap = CAP_URL;  caplen = 0; }
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    out[n++] = ']';
    out[n] = '\0';
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    free(out);
    return ESP_OK;
}

/* Unit names as they appear in release asset filenames ("<unit>_v<ver>.bin",
 * see .github/workflows/release.yml). Order fixes the JSON key order of
 * /api/ota/github/latest; every unit builds the full four-unit answer so the
 * handler is identical across the four firmwares, even though only
 * MasterRemote's page reads more than its own key. */
static const char *const GH_UNITS[] = { "MasterRemote", "SlaveHonda", "SlaveWallas", "ESPNowRelay" };
#define GH_UNIT_COUNT (sizeof(GH_UNITS)/sizeof(GH_UNITS[0]))

/* GET /api/ota/github/latest ── newest published version per unit, e.g.
 * {"MasterRemote":"1.6.13","SlaveHonda":"1.4.11",...}; a unit with no matching
 * asset in the fetched page of releases gets "".
 *
 * The listing is newest-first, so the first asset seen for a unit is its
 * latest, and the scan can stop as soon as all four are known (normally within
 * the first release, since a unified v* tag carries all four bins). Streams
 * byte-by-byte for the same heap reason as h_ota_gh_releases above -- only
 * browser_download_url is captured here, since the version is part of the
 * asset filename. */
static esp_err_t h_ota_gh_latest(httpd_req_t *req)
{
    char latest[GH_UNIT_COUNT][24];
    for (size_t u = 0; u < GH_UNIT_COUNT; u++) latest[u][0] = '\0';
    size_t found = 0;

    esp_http_client_config_t cfg = {
        .url = GH_RELEASES_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "HTTP client init failed");
        return ESP_OK;
    }
    esp_http_client_set_header(client, "User-Agent", "RemoteStart-ESP32");
    esp_http_client_set_header(client, "Accept", "application/vnd.github+json");

    if (esp_http_client_open(client, 0) != ESP_OK) {
        esp_http_client_cleanup(client);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "GitHub connection failed");
        return ESP_OK;
    }
    esp_http_client_fetch_headers(client);   /* -1 on a chunked body; only the status matters */
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        char msg[64];
        snprintf(msg, sizeof(msg), "GitHub API returned HTTP %d", status);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, msg);
        return ESP_OK;
    }

    gh_key_t k_url = { "\"browser_download_url\":\"", 24, 0 };
    bool   cap = false;
    size_t caplen = 0;
    char   url[220];
    char   buf[512];
    int    r;
    const size_t dlen = strlen(GH_ASSET_URL_PREFIX);

    while (found < GH_UNIT_COUNT && (r = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < r && found < GH_UNIT_COUNT; i++) {
            char c = buf[i];

            if (cap) {
                if (c != '"') {
                    if (caplen < sizeof(url) - 1) url[caplen++] = c;
                    continue;
                }
                url[caplen] = '\0';
                cap = false;

                const char *slash = strrchr(url, '/');
                const char *base  = slash ? slash + 1 : url;
                size_t blen = strlen(base);
                if (strncmp(url, GH_ASSET_URL_PREFIX, dlen) == 0 &&
                    blen > 4 && strcmp(base + blen - 4, ".bin") == 0) {
                    for (size_t u = 0; u < GH_UNIT_COUNT; u++) {
                        size_t ulen = strlen(GH_UNITS[u]);
                        if (blen <= ulen + 2 || strncmp(base, GH_UNITS[u], ulen) != 0 ||
                            base[ulen] != '_' || base[ulen + 1] != 'v') continue;
                        if (latest[u][0]) break;   /* older release of a unit already recorded */
                        size_t vlen = blen - 4 - (ulen + 2);
                        if (vlen == 0 || vlen >= sizeof(latest[u])) break;
                        /* Restrict to version characters: this ends up in a JSON
                         * body, and the filename comes off the network. */
                        bool ok = true;
                        for (size_t k = 0; k < vlen && ok; k++) {
                            char v = base[ulen + 2 + k];
                            ok = (v >= '0' && v <= '9') || (v >= 'a' && v <= 'z') ||
                                 (v >= 'A' && v <= 'Z') || v == '.' || v == '-' || v == '_';
                        }
                        if (!ok) break;
                        memcpy(latest[u], base + ulen + 2, vlen);
                        latest[u][vlen] = '\0';
                        found++;
                        break;
                    }
                }
                /* fall through so the closing quote still feeds the matcher */
            }

            if (gh_key_feed(&k_url, c)) { cap = true; caplen = 0; }
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    char out[240];
    snprintf(out, sizeof(out), "{\"%s\":\"%s\",\"%s\":\"%s\",\"%s\":\"%s\",\"%s\":\"%s\"}",
             GH_UNITS[0], latest[0], GH_UNITS[1], latest[1],
             GH_UNITS[2], latest[2], GH_UNITS[3], latest[3]);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}

/* Copies a fixed-size esp_app_desc_t text field into a NUL-terminated,
 * JSON-safe buffer. The field is not guaranteed to be terminated, and the
 * passive slot can hold an image built by anything, so bytes outside plain
 * printable ASCII (and the two characters that would break the JSON body) are
 * dropped rather than trusted. */
static void ver_sanitize(char *dst, size_t cap, const char *src, size_t src_len)
{
    size_t n = 0;
    for (size_t i = 0; i < src_len && src[i] && n < cap - 1; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') dst[n++] = (char)c;
    }
    dst[n] = '\0';
}

/* GET /api/version ── the three versions the OTA tab's table shows for this
 * unit: what it runs now, and what is left in the inactive OTA slot (the
 * previous image, still bootable). "stored" is empty when that slot has never
 * been written or holds no valid app image.
 *
 * Served with an open CORS header on purpose: MasterRemote's OTA tab fetches
 * this same endpoint from the other units' addresses to fill its four-unit
 * table, which is a cross-origin request from the browser's point of view. The
 * response is read-only version information on a LAN-only device. */
static esp_err_t h_version(httpd_req_t *req)
{
    char stored[40] = "", stored_name[40] = "";
    esp_app_desc_t desc;
    const esp_partition_t *nxt = esp_ota_get_next_update_partition(NULL);
    if (nxt && esp_ota_get_partition_description(nxt, &desc) == ESP_OK) {
        ver_sanitize(stored, sizeof(stored), desc.version, sizeof(desc.version));
        ver_sanitize(stored_name, sizeof(stored_name), desc.project_name, sizeof(desc.project_name));
    }
    const esp_partition_t *run = esp_ota_get_running_partition();

    char buf[260];
    snprintf(buf, sizeof(buf),
             "{\"name\":\"%s\",\"installed\":\"%s\",\"stored\":\"%s\","
             "\"storedName\":\"%s\",\"running\":\"%s\"}",
             FIRMWARE_NAME, FIRMWARE_VERSION, stored, stored_name,
             run ? run->label : "");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* Background task performing the actual download+flash for a GitHub-release
 * install, kicked off by h_ota_gh_install below. */
static void gh_ota_task(void *arg)
{
    (void)arg;
    g_gh_ota_trace[0] = '\0';
    esp_http_client_config_t http_cfg = {
        .url = g_gh_ota_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        /* GitHub's release-asset redirect target is a signed URL (JWT query
         * params) around 2 KB long. esp_http_client builds the outgoing
         * "GET <path>?<query> HTTP/1.1" request line into buffer_size_tx
         * (defaults to 512 bytes, independent of buffer_size which is RX
         * only) -- too small for that redirected request line, so it fails
         * silently with "Out of buffer" (visible only via serial log) and
         * esp_https_ota_begin() surfaces it as a generic ESP_FAIL with no
         * TLS error detail. Both buffers need to be large enough. */
        .buffer_size    = 4096,
        .buffer_size_tx = 4096,
        .event_handler  = gh_http_event_handler,
    };
    esp_https_ota_config_t ota_cfg = { .http_config = &http_cfg };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        g_gh_ota_state = GH_OTA_ERROR;
        snprintf(g_gh_ota_err, sizeof(g_gh_ota_err), "begin:%s", esp_err_to_name(err));
        ESP_LOGE(TAG, "GitHub OTA begin failed: %s (url=%s)", esp_err_to_name(err), g_gh_ota_url);
        vTaskDelete(NULL);
        return;
    }

    do {
        err = esp_https_ota_perform(handle);
        int64_t read = esp_https_ota_get_image_len_read(handle);
        if (g_gh_ota_total > 0) {
            int pct = (int)(read * 100 / g_gh_ota_total);
            g_gh_ota_percent = pct > 99 ? 99 : pct;
        }
    } while (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

    esp_err_t finish_err = ESP_FAIL;
    bool complete = esp_https_ota_is_complete_data_received(handle);
    if (err == ESP_OK && complete) {
        finish_err = esp_https_ota_finish(handle);
    } else {
        ESP_LOGE(TAG, "GitHub OTA perform failed: perform=%s complete=%d",
                 esp_err_to_name(err), (int)complete);
        esp_https_ota_abort(handle);
    }

    if (finish_err == ESP_OK) {
        g_gh_ota_state   = GH_OTA_SUCCESS;
        g_gh_ota_percent = 100;
        ESP_LOGI(TAG, "GitHub OTA success — rebooting");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else {
        g_gh_ota_state = GH_OTA_ERROR;
        snprintf(g_gh_ota_err, sizeof(g_gh_ota_err), "%s:%s",
                 err == ESP_OK ? "finish" : "perform",
                 esp_err_to_name(err == ESP_OK ? finish_err : err));
        ESP_LOGE(TAG, "GitHub OTA failed: %s", g_gh_ota_err);
    }
    vTaskDelete(NULL);
}

/* POST /api/ota/github/install  ── body: {"url":"...","size":N}
 * url must point at this repo's release-download host/path (defends against
 * a crafted request pointing the device at an arbitrary host). Starts
 * gh_ota_task and returns immediately -- see the state comment above. */
static esp_err_t h_ota_gh_install(httpd_req_t *req)
{
    if (g_gh_ota_state == GH_OTA_DOWNLOADING) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Install already in progress");
        return ESP_OK;
    }

    char body[400] = {0};
    int len = MIN(req->content_len, (int)sizeof(body) - 1);
    if (len <= 0 || httpd_req_recv(req, body, len) <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing body");
        return ESP_OK;
    }

    cJSON *root = cJSON_Parse(body);
    cJSON *url_j  = root ? cJSON_GetObjectItemCaseSensitive(root, "url")  : NULL;
    cJSON *size_j = root ? cJSON_GetObjectItemCaseSensitive(root, "size") : NULL;
    if (!cJSON_IsString(url_j) || !cJSON_IsNumber(size_j) ||
        strncmp(url_j->valuestring, GH_ASSET_URL_PREFIX, strlen(GH_ASSET_URL_PREFIX)) != 0) {
        if (root) cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid or disallowed URL");
        return ESP_OK;
    }

    strlcpy(g_gh_ota_url, url_j->valuestring, sizeof(g_gh_ota_url));
    g_gh_ota_total   = (int64_t)size_j->valuedouble;
    g_gh_ota_percent = 0;
    g_gh_ota_err[0]  = '\0';
    cJSON_Delete(root);

    g_gh_ota_state = GH_OTA_DOWNLOADING;
    if (xTaskCreate(gh_ota_task, "gh_ota", 8192, NULL, 4, NULL) != pdPASS) {
        g_gh_ota_state = GH_OTA_ERROR;
        strlcpy(g_gh_ota_err, "Failed to start install task", sizeof(g_gh_ota_err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to start install task");
        return ESP_OK;
    }
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

/* GET /api/ota/github/progress  ── polled by the OTA tab's progress bar */
static esp_err_t h_ota_gh_progress(httpd_req_t *req)
{
    const char *state_str = g_gh_ota_state == GH_OTA_DOWNLOADING ? "downloading" :
                             g_gh_ota_state == GH_OTA_SUCCESS     ? "success"     :
                             g_gh_ota_state == GH_OTA_ERROR       ? "error"       : "idle";
    char buf[900];
    snprintf(buf, sizeof(buf), "{\"state\":\"%s\",\"percent\":%d,\"error\":\"%s\",\"trace\":\"%s\"}",
             state_str, g_gh_ota_percent, g_gh_ota_err, g_gh_ota_trace);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* Copies an SSID (not NUL-terminated in the 32-byte wifi structs) into a JSON
 * string body: escapes " and \, drops control characters, always terminates.
 * dst wants 2*32+1 bytes to be guaranteed lossless. */
static void ssid_json_esc(char *dst, size_t dsz, const uint8_t *ssid)
{
    size_t j = 0;
    for (int k = 0; k < 32 && ssid[k]; k++) {
        unsigned char c = ssid[k];
        if (c < 0x20) continue;
        size_t need = (c == '"' || c == '\\') ? 2 : 1;
        if (j + need >= dsz) break;
        if (need == 2) dst[j++] = '\\';
        dst[j++] = c;
    }
    dst[j] = '\0';
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
        char esc[70];
        ssid_json_esc(esc, sizeof(esc), recs[i].ssid);
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

/* GET /api/ipconfig  ── everything the WiFi tab shows: the configured WiFi
 * network, the saved DHCP/static config, and the addressing actually in use.
 * The live_* fields are read from the netif itself, so they are equally valid
 * whether the address came from a static config or from the router's DHCP.
 * ssid is what's provisioned (present even when the unit can't associate);
 * apSsid is the AP it is genuinely joined to right now (empty when not
 * associated -- e.g. SlaveHonda running in its ESP-NOW-only fallback). */
static esp_err_t h_ipcfg_get(httpd_req_t *req)
{
    char live_ip[16] = "", live_mask[16] = "", live_gw[16] = "", live_dns[16] = "";
    esp_netif_ip_info_t info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &info) == ESP_OK) {
        snprintf(live_ip,   sizeof(live_ip),   IPSTR, IP2STR(&info.ip));
        snprintf(live_mask, sizeof(live_mask), IPSTR, IP2STR(&info.netmask));
        snprintf(live_gw,   sizeof(live_gw),   IPSTR, IP2STR(&info.gw));
    }
    esp_netif_dns_info_t dns;
    if (netif && esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK &&
        dns.ip.u_addr.ip4.addr != 0)
        snprintf(live_dns, sizeof(live_dns), IPSTR, IP2STR(&dns.ip.u_addr.ip4));

    char cfg_ssid[66] = "", ap_ssid[66] = "";
    wifi_config_t wcfg;
    if (esp_wifi_get_config(WIFI_IF_STA, &wcfg) == ESP_OK)
        ssid_json_esc(cfg_ssid, sizeof(cfg_ssid), wcfg.sta.ssid);
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        ssid_json_esc(ap_ssid, sizeof(ap_ssid), ap.ssid);

    char buf[600];
    snprintf(buf, sizeof(buf),
        "{\"static\":%s,\"ip\":\"%s\",\"mask\":\"%s\",\"gw\":\"%s\",\"dns\":\"%s\","
        "\"ssid\":\"%s\",\"apSsid\":\"%s\","
        "\"liveIp\":\"%s\",\"liveMask\":\"%s\",\"liveGw\":\"%s\",\"liveDns\":\"%s\"}",
        g_ipcfg_static ? "true" : "false",
        g_ipcfg_ip, g_ipcfg_mask, g_ipcfg_gw, g_ipcfg_dns,
        cfg_ssid, ap_ssid,
        live_ip, live_mask, live_gw, live_dns);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* POST /api/ipconfig/save  ── mode=dhcp|static&ip=&mask=&gw=&dns=
 * Validates IPv4 syntax before accepting a static config -- a bad address
 * would otherwise still associate to the AP (WiFi auth doesn't care) but
 * leave the unit unreachable over IP, indistinguishable from a hang. */
static esp_err_t h_ipcfg_save(httpd_req_t *req)
{
    char body[256] = {0};
    int  len = MIN(req->content_len, (int)sizeof(body) - 1);
    httpd_req_recv(req, body, len);

    char mode[8] = {0}, ip[16] = {0}, mask[16] = {0}, gw[16] = {0}, dns[16] = {0};
    char *mp = strstr(body, "mode="); char *ip_p = strstr(body, "ip=");
    char *mk_p = strstr(body, "mask="); char *gw_p = strstr(body, "gw=");
    char *dn_p = strstr(body, "dns=");
#define GRAB(p, dst, skip) if(p){p+=skip; char *e=strchr(p,'&'); if(e)*e='\0'; strlcpy(dst,p,sizeof(dst)); if(e)*e='&';}
    GRAB(mp, mode, 5) GRAB(ip_p, ip, 3) GRAB(mk_p, mask, 5) GRAB(gw_p, gw, 3) GRAB(dn_p, dns, 4)
#undef GRAB
    url_decode(ip); url_decode(mask); url_decode(gw); url_decode(dns);

    bool want_static = (strcmp(mode, "static") == 0);
    if (want_static) {
        esp_ip4_addr_t test;
        if (!ip[0] || !mask[0] ||
            esp_netif_str_to_ip4(ip, &test) != ESP_OK || esp_netif_str_to_ip4(mask, &test) != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid IP address or subnet mask");
            return ESP_OK;
        }
        if (gw[0]  && esp_netif_str_to_ip4(gw,  &test) != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid gateway"); return ESP_OK;
        }
        if (dns[0] && esp_netif_str_to_ip4(dns, &test) != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid DNS server"); return ESP_OK;
        }
    }

    g_ipcfg_static = want_static;
    strlcpy(g_ipcfg_ip,   ip,   sizeof(g_ipcfg_ip));
    strlcpy(g_ipcfg_mask, mask, sizeof(g_ipcfg_mask));
    strlcpy(g_ipcfg_gw,   gw,   sizeof(g_ipcfg_gw));
    strlcpy(g_ipcfg_dns,  dns,  sizeof(g_ipcfg_dns));

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<!DOCTYPE html><html><body style='font-family:Arial;background:#1a1a2e;color:#eee;"
        "display:flex;justify-content:center;align-items:center;min-height:100vh'>"
        "<div style='text-align:center'><h2 style='color:#e94560'>Saved!</h2>"
        "<p>Applying network settings and rebooting…</p></div></body></html>");
    g_ipcfg_save_requested = true;
    return ESP_OK;
}

/* POST /api/reboot  ── manual reboot from the WiFi tab's Device card */
static esp_err_t h_reboot(httpd_req_t *req)
{
    httpd_resp_sendstr(req, "OK");
    ESP_LOGW(TAG, "Manual reboot requested via web UI");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

/* ── Start Web Server ───────────────────────────────────────────────────────── */
esp_err_t web_server_start(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 28;   /* headroom above the current route count -- an exact-fit cap
                                    silently drops routes registered after the limit is hit, and
                                    the registration loop below now logs if that happens (SlaveHonda
                                    hit exactly this in v1.9.0: /ota/upload silently never registered) */
    cfg.stack_size       = 8192;
    cfg.lru_purge_enable = true;   /* evict the least-recently-used session when all
                                      socket slots (default 7) are taken -- without this,
                                      abandoned keep-alive/timed-out client connections
                                      permanently exhaust the pool and the server accepts
                                      TCP but never services HTTP again until a reboot
                                      (root-caused live on 2026-07-02: ping OK, TCP
                                      handshake OK, HTTP dead -- FSD v1.35) */
    cfg.max_open_sockets = 7;      /* explicit because it is half of a budget: httpd also holds
                                      a listener and a control socket, so 7 sessions occupy 9 of
                                      CONFIG_LWIP_MAX_SOCKETS. That was 10, leaving a single
                                      descriptor for everything else -- and a browser parked on
                                      a polling tab fills all 7 session slots, so the OUTBOUND
                                      TLS socket for a GitHub fetch or install had none left and
                                      failed with ESP_ERR_ESP_TLS_CANNOT_CREATE_SOCKET (v1.48).
                                      The pool is now 16; keep headroom if either number moves. */

    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ESP_FAIL;
    }

    static const httpd_uri_t routes[] = {
        { .uri="/",                .method=HTTP_GET,  .handler=h_root        },
        { .uri="/api/status",      .method=HTTP_GET,  .handler=h_status      },
        { .uri="/api/sysinfo",     .method=HTTP_GET,  .handler=h_sysinfo     },
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
        { .uri="/api/gpio/set",    .method=HTTP_GET,  .handler=h_gpio_set    },
        { .uri="/api/gpio/status", .method=HTTP_GET,  .handler=h_gpio_status },
        { .uri="/api/ipconfig",      .method=HTTP_GET,  .handler=h_ipcfg_get  },
        { .uri="/api/ipconfig/save", .method=HTTP_POST, .handler=h_ipcfg_save },
        { .uri="/api/reboot",        .method=HTTP_POST, .handler=h_reboot     },
        { .uri="/ota/upload",      .method=HTTP_POST, .handler=h_ota_upload  },
        { .uri="/api/version",       .method=HTTP_GET,  .handler=h_version    },
        { .uri="/api/ota/github/releases", .method=HTTP_GET,  .handler=h_ota_gh_releases },
        { .uri="/api/ota/github/latest",   .method=HTTP_GET,  .handler=h_ota_gh_latest   },
        { .uri="/api/ota/github/install",  .method=HTTP_POST, .handler=h_ota_gh_install  },
        { .uri="/api/ota/github/progress", .method=HTTP_GET,  .handler=h_ota_gh_progress },
    };
    for (size_t i = 0; i < sizeof(routes)/sizeof(routes[0]); i++)
        if (httpd_register_uri_handler(server, &routes[i]) != ESP_OK)
            ESP_LOGE(TAG, "Failed to register URI: %s", routes[i].uri);

    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}
