/*
 * ESPNowRelay web_server.c
 * Serves: WiFi setup portal, relay traffic/peer status API, OTA upload.
 * Four-tab UI: Status | Debug | WiFi | OTA Update
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
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_tls.h"
#include "cJSON.h"

#include "version.h"
#include "web_server.h"

static const char *TAG = "web";

extern volatile bool     g_wifi_save_requested;
extern char              g_new_ssid[64];
extern char              g_new_pass[64];
extern volatile uint32_t g_wifi_disconnect_count;
extern volatile int64_t  g_wifi_last_disc_ts_us;
extern          uint32_t g_heap_min;
extern          int64_t  g_heap_min_ts_us;
extern          bool     g_has_wifi;

extern bool              g_ipcfg_static;
extern char              g_ipcfg_ip[16];
extern char              g_ipcfg_mask[16];
extern char              g_ipcfg_gw[16];
extern char              g_ipcfg_dns[16];
extern volatile bool     g_ipcfg_save_requested;

/* ── GitHub-releases OTA state ──────────────────────────────────────────────
 * Install runs in a background task (gh_ota_task) so the httpd worker that
 * accepted /api/ota/github/install isn't blocked for the whole download --
 * the OTA tab's progress bar needs to poll /api/ota/github/progress
 * concurrently while it runs. Single-writer(task)/single-reader(httpd)
 * polling, no locking, matching this codebase's existing status-field style. */
#define GH_RELEASES_URL     "https://api.github.com/repos/SEspe/RemoteStart/releases?per_page=8"
#define GH_ASSET_URL_PREFIX "https://github.com/SEspe/RemoteStart/releases/download/"
enum { GH_OTA_IDLE = 0, GH_OTA_DOWNLOADING, GH_OTA_SUCCESS, GH_OTA_ERROR };
static volatile int     g_gh_ota_state   = GH_OTA_IDLE;
static volatile int     g_gh_ota_percent = 0;
static volatile int64_t g_gh_ota_total   = 0;
static char             g_gh_ota_url[256] = "";
static char             g_gh_ota_err[128] = "";
static char             g_gh_ota_trace[600] = "";   /* DEBUG: HTTP event trace for diagnosing install failures */

/* DEBUG: traces low-level esp_http_client events during gh_ota_task's
 * download, since ESP_LOGx output isn't visible without a USB/serial
 * connection to this unit -- exposed via /api/ota/github/progress. */
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
"</style></head><body>"
"<div class='hdr'><h1>&#9889; ESPNowRelay</h1>"
"<span class='v'>v" FIRMWARE_VERSION " &nbsp;|&nbsp; %IP%</span></div>"
"<div class='tabs'>"
"<button class='tab on' onclick='show(\"status\",this)'>Status</button>"
"<button class='tab' onclick='show(\"debug\",this)'>Debug</button>"
"<button class='tab' onclick='show(\"wifinet\",this)'>WiFi</button>"
"<button class='tab' onclick='show(\"ota\",this)'>OTA Update</button>"
"</div>"
"<div id='status' class='pane on'>"
"<div class='card'><h3>ESP-NOW Traffic</h3><div class='tbl' id='traffic'>&#8230;</div></div>"
"<div class='card'><h3>Peers</h3><div class='tbl' id='peers'>&#8230;</div></div>"
"<div class='ts' id='sts'></div></div>"
"<div id='debug' class='pane'>"
"<div class='card'><h3>Radio Link</h3><div class='grid' id='wifi'>&#8230;</div></div>"
"<div class='card'><h3>System</h3><div class='grid' id='sys'>&#8230;</div></div>"
"</div>"
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
"(traffic counters, peer roster).</p>"
"<button class='wf-btn wf-btn-s' onclick='rebootDevice()'>&#128260; Reboot Now</button>"
"<div class='wf-sts' id='rebootSts'></div>"
"</div></div>"
"<div id='ota' class='pane'>"
"<div class='ota-wrap gh-card'>"
"<h3>Install from GitHub Releases</h3>"
"<p>Fetch and flash a release straight from <code>github.com/SEspe/RemoteStart</code> &mdash; no manual download needed.</p>"
"<button class='up' onclick='ghRefresh()'>Refresh Releases</button>"
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
"if(id==='debug'){fetchStatus();fetchSys();}"
"if(id==='wifinet')fetchIpCfg();}"
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
"function peerSum(x){return x&&x.mac?x.rssi+' dBm \\u2014 '+x.mac:'not discovered';}"
"document.getElementById('wifi').innerHTML="
"txtRow('Own MAC',d.mac||'\\u2014')+"
"txtRow('AP Signal (RSSI)',d.rssi?d.rssi+' dBm':'not associated')+txtRow('WiFi Channel',d.ch||'\\u2014')+"
"txtRow('ESP-NOW: MasterRemote',peerSum(d.peers[0]))+"
"txtRow('ESP-NOW: SlaveHonda',peerSum(d.peers[1]))+"
"txtRow('ESP-NOW: SlaveWallas',peerSum(d.peers[2]));"
"document.getElementById('sts').textContent='Updated: '+new Date().toLocaleTimeString();"
"}).catch(()=>{});}"
"function fetchSys(){"
"fetch('/api/sysinfo').then(r=>r.json()).then(d=>{"
"document.getElementById('sys').innerHTML="
"txtRow('Free Heap',fmtKB(d.heap))+"
"txtRow('Min Free Heap (low-water)',fmtKB(d.heapMin)+' \\u2014 '+fmtUptime(d.heapMinAgo)+' ago')+"
"txtRow('Uptime',fmtUptime(d.uptime))+txtRow('WiFi Reconnects',d.wifiDisc)+"
"txtRow('Last WiFi Reconnect',d.wifiDiscAgo<0?'never':fmtUptime(d.wifiDiscAgo)+' ago');"
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
"if(dbOn)fetchSys();"
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

    uint8_t own_mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, own_mac);
    char mac_str[20];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             own_mac[0], own_mac[1], own_mac[2], own_mac[3], own_mac[4], own_mac[5]);

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

    snprintf(buf + n, sizeof(buf) - n, "],\"rssi\":%d,\"ch\":%d,\"mac\":\"%s\"}",
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

/* GET /api/ota/github/releases  ── recent GitHub releases with a binary matching
 * this unit (FIRMWARE_NAME), newest first. Releases without a matching asset
 * (e.g. a single-unit tag for a different unit) are omitted entirely. */
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
    int content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200 || content_len <= 0) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "GitHub API error");
        return ESP_OK;
    }

    char *raw = malloc((size_t)content_len + 1);
    if (!raw) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_OK;
    }
    int got = 0;
    while (got < content_len) {
        int r = esp_http_client_read(client, raw + got, content_len - got);
        if (r <= 0) break;
        got += r;
    }
    raw[got] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    cJSON *arr = cJSON_Parse(raw);
    free(raw);
    if (!arr || !cJSON_IsArray(arr)) {
        if (arr) cJSON_Delete(arr);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Bad GitHub response");
        return ESP_OK;
    }

    const size_t out_cap = 3072;
    char *out = malloc(out_cap);
    if (!out) {
        cJSON_Delete(arr);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_OK;
    }
    size_t n = 0;
    out[n++] = '[';
    bool first = true;
    const char *prefix = FIRMWARE_NAME "_v";
    size_t plen = strlen(prefix);

    cJSON *rel;
    cJSON_ArrayForEach(rel, arr) {
        cJSON *tag    = cJSON_GetObjectItemCaseSensitive(rel, "tag_name");
        cJSON *pub    = cJSON_GetObjectItemCaseSensitive(rel, "published_at");
        cJSON *assets = cJSON_GetObjectItemCaseSensitive(rel, "assets");
        if (!cJSON_IsString(tag) || !cJSON_IsArray(assets)) continue;

        cJSON *asset;
        cJSON_ArrayForEach(asset, assets) {
            cJSON *name = cJSON_GetObjectItemCaseSensitive(asset, "name");
            cJSON *url  = cJSON_GetObjectItemCaseSensitive(asset, "browser_download_url");
            cJSON *size = cJSON_GetObjectItemCaseSensitive(asset, "size");
            if (!cJSON_IsString(name) || !cJSON_IsString(url) || !cJSON_IsNumber(size)) continue;
            size_t nlen = strlen(name->valuestring);
            if (nlen <= plen || strncmp(name->valuestring, prefix, plen) != 0) continue;
            if (nlen < 4 || strcmp(name->valuestring + nlen - 4, ".bin") != 0) continue;

            char entry[420];
            int m = snprintf(entry, sizeof(entry),
                "%s{\"tag\":\"%s\",\"published\":\"%s\",\"asset\":\"%s\",\"url\":\"%s\",\"size\":%d}",
                first ? "" : ",",
                tag->valuestring, cJSON_IsString(pub) ? pub->valuestring : "",
                name->valuestring, url->valuestring, (int)size->valuedouble);
            if (m > 0 && n + (size_t)m < out_cap - 2) {
                memcpy(out + n, entry, (size_t)m);
                n += (size_t)m;
                first = false;
            }
            break;   /* one matching asset per release is enough */
        }
    }
    cJSON_Delete(arr);
    out[n++] = ']';
    out[n] = '\0';

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    free(out);
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
 * associated -- e.g. this unit running in its ESP-NOW-only fallback). */
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
        { .uri="/api/ipconfig",      .method=HTTP_GET,  .handler=h_ipcfg_get  },
        { .uri="/api/ipconfig/save", .method=HTTP_POST, .handler=h_ipcfg_save },
        { .uri="/api/reboot",        .method=HTTP_POST, .handler=h_reboot     },
        { .uri="/ota/upload",  .method=HTTP_POST, .handler=h_ota_upload },
        { .uri="/api/ota/github/releases", .method=HTTP_GET,  .handler=h_ota_gh_releases },
        { .uri="/api/ota/github/install",  .method=HTTP_POST, .handler=h_ota_gh_install  },
        { .uri="/api/ota/github/progress", .method=HTTP_GET,  .handler=h_ota_gh_progress },
    };
    for (size_t i=0; i<sizeof(routes)/sizeof(routes[0]); i++)
        if (httpd_register_uri_handler(server, &routes[i]) != ESP_OK)
            ESP_LOGE(TAG, "Failed to register URI: %s", routes[i].uri);
    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}
