/*
 * MasterHonda.ino
 * ESP32 Master — Honda EU70IS & Wallas Heater Remote Start
 * Author: SEspe
 * Version: 1.0.0
 *
 * Required libraries (Arduino Library Manager):
 *   - WiFiManager            by tzapu
 *   - ESPAsyncWebServer      by lacamera
 *   - AsyncTCP               by dvarrel  (dependency)
 *   - ElegantOTA             by Ayush Sharma  v3.x
 *
 * First startup: connects phone/laptop to "MasterHonda-Config" WiFi (pw: honda1234),
 *   navigate to 192.168.4.1, enter home WiFi credentials.  Saved in flash on all
 *   subsequent boots.
 *
 * Web UI:  http://<device-ip>/        — three tabs: Status | Clients | OTA
 * OTA:     http://<device-ip>/update  — direct ElegantOTA page
 */

#define FIRMWARE_VERSION  "1.0.0"
#define FIRMWARE_NAME     "MasterHonda"

#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <WiFiManager.h>
#include <ESPAsyncWebServer.h>
#include <ElegantOTA.h>

// ── Custom MAC Addresses ──────────────────────────────────────────────────────
const uint8_t masterCustomMac[]  = {0x30, 0xAE, 0xA4, 0x89, 0x92, 0x7A};
const uint8_t slaveHondaMac[]    = {0x30, 0xAE, 0xA4, 0x1A, 0xAE, 0x33};
const uint8_t slaveWallasMac[]   = {0x30, 0xAE, 0xA4, 0x1A, 0xAE, 0x30};

// ── Pin Definitions ───────────────────────────────────────────────────────────
#define PIN_HONDA_STARTED_FB      13   // Output: Honda running LED indicator
#define PIN_STATUS_LED             2   // Onboard LED (heartbeat)
#define PIN_HONDA_START           15   // Input:  Honda start from Victron Venus relay
#define PIN_HONDA_MANUAL_START    14   // Input:  Honda manual start (pull-up)
#define PIN_WALLAS_START           4   // Input:  Wallas start from Victron Venus relay
#define PIN_WALLAS_MANUAL_START    5   // Input:  Wallas manual start (pull-up)

// ── Timing ───────────────────────────────────────────────────────────────────
#define HONDA_RESTART_BLOCK_MS   30000UL
#define WALLAS_SEND_INTERVAL_MS  15000UL

// ── Message Structures (must match slaves) ────────────────────────────────────
typedef struct {
  char  a[32];
  bool  HondaRunningFB;
  bool  HondaIgnitionOn;
  bool  HondaStart;
} MasterMsg;

typedef struct {
  char  a[32];
  bool  HondaIgnitionOn;
  bool  HondaStarting;
  bool  HondaRunning;
} SlaveHondaMsg;

typedef struct {
  char  a[32];
  bool  WallasRunning;
  bool  WallasStart;
} SlaveWallasMsg;

// ── Global State ──────────────────────────────────────────────────────────────
volatile bool gblHondaStartCmd   = false;
volatile bool gblWallasStartCmd  = false;
bool          gblSlaveHondaRunning = false;
unsigned long lastHondaSendMs    = 0;
unsigned long lastWallasSendMs   = 0;

// ── Slave Status (web display) ────────────────────────────────────────────────
struct SlaveInfo {
  unsigned long lastSeenMs;
  bool          hondaIgnOn;
  bool          hondaStarting;
  bool          hondaRunning;
  bool          wallasRunning;
  bool          wallasStartCmd;
};
SlaveInfo slaveHonda  = {};
SlaveInfo slaveWallas = {};

// ── ESP-NOW ───────────────────────────────────────────────────────────────────
esp_now_peer_info_t peerHonda;
esp_now_peer_info_t peerWallas;
MasterMsg     txHonda;
MasterMsg     txWallas;
SlaveHondaMsg rxHonda;
SlaveWallasMsg rxWallas;

// ── Web Server ────────────────────────────────────────────────────────────────
AsyncWebServer webServer(80);

// ── HTML Page ─────────────────────────────────────────────────────────────────
static const char INDEX_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MasterHonda</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:Arial,sans-serif;background:#1a1a2e;color:#eee;min-height:100vh}
  .hdr{background:#16213e;padding:12px 20px;display:flex;align-items:center;gap:14px;border-bottom:2px solid #e94560}
  .hdr h1{font-size:1.15rem;color:#e94560}
  .hdr .ver{font-size:.75rem;color:#888}
  .tabs{display:flex;background:#16213e}
  .tab{padding:10px 22px;cursor:pointer;color:#aaa;border:none;background:none;font-size:.9rem;border-bottom:3px solid transparent}
  .tab.on{color:#e94560;border-bottom-color:#e94560;font-weight:bold}
  .pane{display:none;padding:18px}
  .pane.on{display:block}
  .card{background:#16213e;border-radius:8px;padding:14px;margin-bottom:14px}
  .card h3{color:#e94560;font-size:.95rem;margin-bottom:10px}
  .grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(210px,1fr));gap:8px}
  .row{display:flex;justify-content:space-between;align-items:center;background:#0f3460;border-radius:6px;padding:7px 12px;font-size:.85rem}
  .led{width:13px;height:13px;border-radius:50%;flex-shrink:0}
  .on-led{background:#4caf50;box-shadow:0 0 6px #4caf50}
  .off-led{background:#444}
  .cli{background:#0f3460;border-radius:8px;padding:14px;margin-bottom:12px}
  .cli-name{font-weight:bold;color:#e94560;margin-bottom:8px;font-size:.9rem}
  .cli-row{display:flex;justify-content:space-between;font-size:.83rem;padding:3px 0}
  .ok{color:#4caf50}
  .dim{color:#888}
  .ts{font-size:.72rem;color:#555;margin-top:6px}
  iframe{width:100%;height:480px;border:none;border-radius:8px;background:#fff}
</style>
</head>
<body>
<div class="hdr">
  <h1>&#9889; MasterHonda</h1>
  <span class="ver">v%VER% &nbsp;|&nbsp; %IP%</span>
</div>
<div class="tabs">
  <button class="tab on" onclick="show('status',this)">Pin Status</button>
  <button class="tab" onclick="show('clients',this)">Clients</button>
  <button class="tab" onclick="show('ota',this)">OTA Update</button>
</div>

<div id="status" class="pane on">
  <div class="card"><h3>Input Pins</h3><div class="grid" id="pins">…</div></div>
  <div class="card"><h3>System State</h3><div class="grid" id="state">…</div></div>
  <div class="ts" id="sts"></div>
</div>

<div id="clients" class="pane">
  <div id="cli">…</div>
  <div class="ts" id="cts"></div>
</div>

<div id="ota" class="pane">
  <div class="card">
    <h3>Firmware OTA Update</h3>
    <p style="font-size:.83rem;color:#aaa;margin-bottom:8px">Device: %NAME% &nbsp; Version: v%VER%</p>
    <p style="font-size:.83rem;color:#aaa">Upload a compiled .bin file to update firmware wirelessly.</p>
  </div>
  <iframe src="/update"></iframe>
</div>

<script>
function show(id,btn){
  document.querySelectorAll('.pane').forEach(e=>e.classList.remove('on'));
  document.querySelectorAll('.tab').forEach(e=>e.classList.remove('on'));
  document.getElementById(id).classList.add('on');
  btn.classList.add('on');
}
function led(v){return '<div class="led '+(v?'on-led':'off-led')+'"></div>';}
function row(n,v){return '<div class="row"><span>'+n+'</span>'+led(v)+'</div>';}
function fetchStatus(){
  fetch('/api/status').then(r=>r.json()).then(d=>{
    document.getElementById('pins').innerHTML=
      row('Honda Start (pin15)',d.pHS)+row('Honda Manual (pin14)',d.pHM)+
      row('Wallas Start (pin4)',d.pWS)+row('Wallas Manual (pin5)',d.pWM)+
      row('Honda FB LED (pin13)',d.pFB);
    document.getElementById('state').innerHTML=
      row('Honda Start CMD',d.gHS)+row('Honda Slave Running',d.gHR)+row('Wallas Start CMD',d.gWS);
    document.getElementById('sts').textContent='Updated: '+new Date().toLocaleTimeString();
  }).catch(()=>{});
}
function fetchClients(){
  fetch('/api/clients').then(r=>r.json()).then(d=>{
    let h='';
    h+='<div class="cli"><div class="cli-name">SlaveHonda <span style="font-size:.72rem;color:#666">30:AE:A4:1A:AE:33</span></div>';
    h+='<div class="cli-row"><span>Last seen</span><span>'+d.h.ls+'</span></div>';
    h+='<div class="cli-row"><span>Ignition On</span><span class="'+(d.h.ig?'ok':'dim')+'">'+(d.h.ig?'YES':'No')+'</span></div>';
    h+='<div class="cli-row"><span>Starting</span><span class="'+(d.h.st?'ok':'dim')+'">'+(d.h.st?'YES':'No')+'</span></div>';
    h+='<div class="cli-row"><span>Running</span><span class="'+(d.h.ru?'ok':'dim')+'">'+(d.h.ru?'YES':'No')+'</span></div></div>';
    h+='<div class="cli"><div class="cli-name">SlaveWallas <span style="font-size:.72rem;color:#666">30:AE:A4:1A:AE:30</span></div>';
    h+='<div class="cli-row"><span>Last seen</span><span>'+d.w.ls+'</span></div>';
    h+='<div class="cli-row"><span>Wallas Running</span><span class="'+(d.w.ru?'ok':'dim')+'">'+(d.w.ru?'YES':'No')+'</span></div>';
    h+='<div class="cli-row"><span>Start CMD sent</span><span class="'+(d.w.sc?'ok':'dim')+'">'+(d.w.sc?'YES':'No')+'</span></div></div>';
    document.getElementById('cli').innerHTML=h;
    document.getElementById('cts').textContent='Updated: '+new Date().toLocaleTimeString();
  }).catch(()=>{});
}
setInterval(()=>{
  if(document.getElementById('status').classList.contains('on')) fetchStatus();
  if(document.getElementById('clients').classList.contains('on')) fetchClients();
},2000);
fetchStatus();
</script>
</body>
</html>
)rawliteral";

// ── Helpers ───────────────────────────────────────────────────────────────────
static bool macEq(const uint8_t *a, const uint8_t *b) {
  return memcmp(a, b, 6) == 0;
}

static String secsAgo(unsigned long ms) {
  if (ms == 0) return "Never";
  unsigned long s = (millis() - ms) / 1000;
  if (s < 60)  return String(s) + "s ago";
  return String(s / 60) + "m " + String(s % 60) + "s ago";
}

// ── ESP-NOW Callbacks ─────────────────────────────────────────────────────────
void onDataSent(const uint8_t *mac, esp_now_send_status_t st) {
  // silence is fine; errors show in serial if DEBUG needed
}

void onDataRecv(const uint8_t *mac, const uint8_t *data, int len) {
  if (macEq(mac, slaveHondaMac) && len >= (int)sizeof(SlaveHondaMsg)) {
    memcpy(&rxHonda, data, sizeof(SlaveHondaMsg));
    slaveHonda.lastSeenMs   = millis();
    slaveHonda.hondaIgnOn   = rxHonda.HondaIgnitionOn;
    slaveHonda.hondaStarting = rxHonda.HondaStarting;
    slaveHonda.hondaRunning  = rxHonda.HondaRunning;
    gblSlaveHondaRunning = rxHonda.HondaRunning;
    digitalWrite(PIN_HONDA_STARTED_FB, gblSlaveHondaRunning ? HIGH : LOW);
  } else if (macEq(mac, slaveWallasMac) && len >= (int)sizeof(SlaveWallasMsg)) {
    memcpy(&rxWallas, data, sizeof(SlaveWallasMsg));
    slaveWallas.lastSeenMs   = millis();
    slaveWallas.wallasRunning = rxWallas.WallasRunning;
  }
}

// ── ISR Handlers ─────────────────────────────────────────────────────────────
void IRAM_ATTR isr_HondaStart() {
  gblHondaStartCmd = (digitalRead(PIN_HONDA_MANUAL_START) || !digitalRead(PIN_HONDA_START));
}
void IRAM_ATTR isr_HondaManualStart() {
  gblHondaStartCmd = digitalRead(PIN_HONDA_MANUAL_START);
}
void IRAM_ATTR isr_WallasStart() {
  gblWallasStartCmd = digitalRead(PIN_WALLAS_START);
}
void IRAM_ATTR isr_WallasManualStart() {
  gblWallasStartCmd = !digitalRead(PIN_WALLAS_MANUAL_START);
}

// ── Send Functions ────────────────────────────────────────────────────────────
void sendToHondaSlave() {
  strlcpy(txHonda.a, "Master->SlaveHonda", sizeof(txHonda.a));
  txHonda.HondaRunningFB  = gblSlaveHondaRunning;
  txHonda.HondaIgnitionOn = digitalRead(PIN_HONDA_MANUAL_START);
  txHonda.HondaStart      = gblHondaStartCmd;
  esp_now_send(slaveHondaMac, (uint8_t *)&txHonda, sizeof(txHonda));
  delay(10);
  esp_now_send(slaveHondaMac, (uint8_t *)&txHonda, sizeof(txHonda)); // redundant send
  Serial.print("[Honda] HondaStart="); Serial.println(txHonda.HondaStart);
}

void sendToWallasSlave() {
  strlcpy(txWallas.a, "Master->SlaveWallas", sizeof(txWallas.a));
  txWallas.HondaRunningFB  = false;
  txWallas.HondaIgnitionOn = false;
  txWallas.HondaStart      = gblWallasStartCmd;
  slaveWallas.wallasStartCmd = gblWallasStartCmd;
  esp_now_send(slaveWallasMac, (uint8_t *)&txWallas, sizeof(txWallas));
  Serial.print("[Wallas] WallasStart="); Serial.println(gblWallasStartCmd);
}

// ── Web API ───────────────────────────────────────────────────────────────────
static String statusJson() {
  // short key names to save RAM
  String j = "{";
  j += "\"pHS\":" + String(!digitalRead(PIN_HONDA_START)        ? "true" : "false") + ",";
  j += "\"pHM\":" + String( digitalRead(PIN_HONDA_MANUAL_START) ? "true" : "false") + ",";
  j += "\"pWS\":" + String( digitalRead(PIN_WALLAS_START)       ? "true" : "false") + ",";
  j += "\"pWM\":" + String(!digitalRead(PIN_WALLAS_MANUAL_START)? "true" : "false") + ",";
  j += "\"pFB\":" + String( digitalRead(PIN_HONDA_STARTED_FB)   ? "true" : "false") + ",";
  j += "\"gHS\":" + String(gblHondaStartCmd    ? "true" : "false") + ",";
  j += "\"gHR\":" + String(gblSlaveHondaRunning? "true" : "false") + ",";
  j += "\"gWS\":" + String(gblWallasStartCmd   ? "true" : "false");
  j += "}";
  return j;
}

static String clientsJson() {
  String j = "{";
  j += "\"h\":{";
  j += "\"ls\":\"" + secsAgo(slaveHonda.lastSeenMs) + "\",";
  j += "\"ig\":"  + String(slaveHonda.hondaIgnOn    ? "true":"false") + ",";
  j += "\"st\":"  + String(slaveHonda.hondaStarting ? "true":"false") + ",";
  j += "\"ru\":"  + String(slaveHonda.hondaRunning  ? "true":"false");
  j += "},\"w\":{";
  j += "\"ls\":\"" + secsAgo(slaveWallas.lastSeenMs) + "\",";
  j += "\"ru\":"   + String(slaveWallas.wallasRunning  ? "true":"false") + ",";
  j += "\"sc\":"   + String(slaveWallas.wallasStartCmd ? "true":"false");
  j += "}}";
  return j;
}

void setupWebServer() {
  webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    String html = String(INDEX_HTML);
    html.replace("%VER%",  FIRMWARE_VERSION);
    html.replace("%NAME%", FIRMWARE_NAME);
    html.replace("%IP%",   WiFi.localIP().toString());
    req->send(200, "text/html", html);
  });
  webServer.on("/api/status",  HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "application/json", statusJson());
  });
  webServer.on("/api/clients", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "application/json", clientsJson());
  });
  ElegantOTA.begin(&webServer);
  webServer.begin();
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== " FIRMWARE_NAME " v" FIRMWARE_VERSION " ===");

  // Set custom MAC before WiFiManager initialises the stack
  WiFi.mode(WIFI_STA);
  esp_wifi_set_mac(WIFI_IF_STA, masterCustomMac);

  // WiFiManager: opens captive portal "MasterHonda-Config" when no credentials saved
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  if (!wm.autoConnect("MasterHonda-Config", "honda1234")) {
    Serial.println("[WiFi] Config portal timed out — restarting");
    delay(500);
    ESP.restart();
  }
  Serial.print("[WiFi] Connected, IP: ");
  Serial.println(WiFi.localIP());

  // ESP-NOW — channel is inherited from the connected WiFi AP
  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESP-NOW] Init failed — restarting");
    ESP.restart();
  }
  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);

  // Add Honda slave peer (channel 0 = use current WiFi channel)
  memset(&peerHonda, 0, sizeof(peerHonda));
  memcpy(peerHonda.peer_addr, slaveHondaMac, 6);
  peerHonda.channel = 0;
  peerHonda.encrypt = false;
  if (esp_now_add_peer(&peerHonda) != ESP_OK)
    Serial.println("[ESP-NOW] Failed to add Honda peer");
  else
    Serial.println("[ESP-NOW] SlaveHonda peer added");

  // Add Wallas slave peer
  memset(&peerWallas, 0, sizeof(peerWallas));
  memcpy(peerWallas.peer_addr, slaveWallasMac, 6);
  peerWallas.channel = 0;
  peerWallas.encrypt = false;
  if (esp_now_add_peer(&peerWallas) != ESP_OK)
    Serial.println("[ESP-NOW] Failed to add Wallas peer");
  else
    Serial.println("[ESP-NOW] SlaveWallas peer added");

  // Pin setup
  pinMode(PIN_HONDA_START,        INPUT);
  attachInterrupt(PIN_HONDA_START, isr_HondaStart, CHANGE);
  gblHondaStartCmd = !digitalRead(PIN_HONDA_START);

  pinMode(PIN_HONDA_MANUAL_START, INPUT_PULLUP);
  attachInterrupt(PIN_HONDA_MANUAL_START, isr_HondaManualStart, CHANGE);

  pinMode(PIN_WALLAS_START,       INPUT);
  attachInterrupt(PIN_WALLAS_START, isr_WallasStart, CHANGE);
  gblWallasStartCmd = digitalRead(PIN_WALLAS_START);

  pinMode(PIN_WALLAS_MANUAL_START, INPUT_PULLUP);
  attachInterrupt(PIN_WALLAS_MANUAL_START, isr_WallasManualStart, CHANGE);

  pinMode(PIN_HONDA_STARTED_FB, OUTPUT);
  digitalWrite(PIN_HONDA_STARTED_FB, LOW);
  pinMode(PIN_STATUS_LED, OUTPUT);
  digitalWrite(PIN_STATUS_LED, LOW);

  setupWebServer();
  ElegantOTA.setAutoReboot(true);

  Serial.print("[Web] http://");
  Serial.println(WiFi.localIP());
  Serial.println("[Ready]");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
  ElegantOTA.loop();

  // Honda: send when desired state differs from slave-reported state, with restart block
  if ((gblHondaStartCmd != gblSlaveHondaRunning) &&
      (millis() - lastHondaSendMs >= HONDA_RESTART_BLOCK_MS)) {
    lastHondaSendMs = millis();
    sendToHondaSlave();
  }

  // Wallas: periodic send
  if (millis() - lastWallasSendMs >= WALLAS_SEND_INTERVAL_MS) {
    lastWallasSendMs = millis();
    sendToWallasSlave();
  }

  // Heartbeat: 1 Hz blink
  digitalWrite(PIN_STATUS_LED, (millis() / 500) % 2 == 0 ? HIGH : LOW);
}
