/*
 * SlaveHonda.ino
 * ESP32 Slave — Honda EU70IS Generator Remote Start
 * Author: Stein Espe
 * Version: 1.0.0
 *
 * Required libraries (Arduino Library Manager):
 *   - WiFiManager            by tzapu
 *   - ESPAsyncWebServer      by lacamera
 *   - AsyncTCP               by dvarrel  (dependency)
 *   - ElegantOTA             by Ayush Sharma  v3.x
 *
 * First startup: open "SlaveHonda-Config" WiFi (pw: honda1234),
 *   navigate to 192.168.4.1, enter home WiFi credentials.
 *
 * Web UI:  http://<device-ip>/        — two tabs: Status | OTA
 * OTA:     http://<device-ip>/update
 *
 * Pin assignments (relay outputs active-LOW):
 *   pin 13 (OUTPUT) — Starter relay     (LOW = crank ON)
 *   pin 14 (OUTPUT) — Ignition relay    (LOW = ignition ON)
 *   pin 15 (INPUT)  — Running feedback  (LOW when generator running)
 *   pin  4 (OUTPUT) — External LED indicator
 *   pin  2 (OUTPUT) — Onboard LED (heartbeat)
 */

#define FIRMWARE_VERSION  "1.0.0"
#define FIRMWARE_NAME     "SlaveHonda"

#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <WiFiManager.h>
#include <ESPAsyncWebServer.h>
#include <ElegantOTA.h>

// ── Custom MAC Addresses ──────────────────────────────────────────────────────
const uint8_t slaveCustomMac[]  = {0x30, 0xAE, 0xA4, 0x1A, 0xAE, 0x33};
const uint8_t masterCustomMac[] = {0x30, 0xAE, 0xA4, 0x89, 0x92, 0x7A};

// ── Pin Definitions ───────────────────────────────────────────────────────────
#define PIN_STARTER_RELAY   13   // Output: starter motor relay (active LOW)
#define PIN_IGNITION_RELAY  14   // Output: ignition relay (active LOW)
#define PIN_RUNNING_FB      15   // Input:  engine running feedback (active LOW)
#define PIN_LED_EXT          4   // Output: external status LED
#define PIN_LED_ONBOARD      2   // Output: onboard LED (heartbeat)

// ── Timing ───────────────────────────────────────────────────────────────────
#define HONDA_CRANK_MS       3000UL   // Starter crank duration
#define HONDA_IGN_WARMUP_MS 10000UL   // Wait after ignition on before cranking
#define STATUS_SEND_MS      10000UL   // Periodic status report to master

// ── Message Structures (must match master) ────────────────────────────────────
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
} SlaveMsg;

// ── Global State ──────────────────────────────────────────────────────────────
volatile bool gblStartCommand = false;
bool          gblHondaRunning = false;
bool          gblHondaStarting = false;
bool          gblHondaIgnOn   = false;
unsigned long lastStatusSend  = 0;

MasterMsg rxMsg;
SlaveMsg  txMsg;

// ── ESP-NOW Peer ──────────────────────────────────────────────────────────────
esp_now_peer_info_t peerMaster;

// ── Web Server ────────────────────────────────────────────────────────────────
AsyncWebServer webServer(80);

// ── HTML Page ─────────────────────────────────────────────────────────────────
static const char INDEX_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SlaveHonda</title>
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
  .ts{font-size:.72rem;color:#555;margin-top:6px}
  iframe{width:100%;height:480px;border:none;border-radius:8px;background:#fff}
</style>
</head>
<body>
<div class="hdr">
  <h1>&#9889; SlaveHonda</h1>
  <span class="ver">v%VER% &nbsp;|&nbsp; %IP%</span>
</div>
<div class="tabs">
  <button class="tab on" onclick="show('status',this)">Status</button>
  <button class="tab" onclick="show('ota',this)">OTA Update</button>
</div>

<div id="status" class="pane on">
  <div class="card"><h3>Honda Generator Status</h3><div class="grid" id="pins">…</div></div>
  <div class="ts" id="sts"></div>
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
  document.getElementById(id).classList.add('on');btn.classList.add('on');
}
function led(v){return '<div class="led '+(v?'on-led':'off-led')+'"></div>';}
function row(n,v){return '<div class="row"><span>'+n+'</span>'+led(v)+'</div>';}
function fetchStatus(){
  fetch('/api/status').then(r=>r.json()).then(d=>{
    document.getElementById('pins').innerHTML=
      row('Ignition ON (pin14)',d.ign)+row('Starter active (pin13)',d.sta)+
      row('Running FB (pin15)',d.run)+row('Start CMD from Master',d.cmd)+
      row('Starting sequence',d.starting);
    document.getElementById('sts').textContent='Updated: '+new Date().toLocaleTimeString();
  }).catch(()=>{});
}
setInterval(()=>{if(document.getElementById('status').classList.contains('on'))fetchStatus();},2000);
fetchStatus();
</script>
</body>
</html>
)rawliteral";

// ── Honda Start Sequence ──────────────────────────────────────────────────────
void hondaStart() {
  if (gblHondaStarting || gblHondaRunning) return;
  gblHondaStarting = true;
  gblHondaIgnOn    = true;

  Serial.println("[Honda] Ignition ON");
  digitalWrite(PIN_IGNITION_RELAY, LOW);   // active low
  digitalWrite(PIN_LED_EXT,        HIGH);

  Serial.println("[Honda] Waiting for ignition warm-up...");
  delay(HONDA_IGN_WARMUP_MS);

  Serial.println("[Honda] Cranking...");
  digitalWrite(PIN_STARTER_RELAY, LOW);    // active low
  delay(HONDA_CRANK_MS);
  digitalWrite(PIN_STARTER_RELAY, HIGH);   // release starter

  Serial.println("[Honda] Crank done, checking running...");
  delay(1000);

  // Check hardware feedback (active low = running)
  gblHondaRunning  = !digitalRead(PIN_RUNNING_FB);
  gblHondaStarting = false;
  Serial.print("[Honda] Running: "); Serial.println(gblHondaRunning);
}

void hondaStop() {
  Serial.println("[Honda] Stopping");
  digitalWrite(PIN_IGNITION_RELAY, HIGH);  // relay off
  digitalWrite(PIN_STARTER_RELAY,  HIGH);  // relay off
  digitalWrite(PIN_LED_EXT,        LOW);
  gblHondaRunning  = false;
  gblHondaStarting = false;
  gblHondaIgnOn    = false;
}

void hondaMain() {
  if (gblStartCommand) {
    if (!gblHondaRunning && !gblHondaStarting) hondaStart();
  } else {
    if (gblHondaRunning || gblHondaIgnOn) hondaStop();
  }
}

// ── Send Status to Master ─────────────────────────────────────────────────────
void sendStatus() {
  // Update running feedback from hardware
  gblHondaRunning = !digitalRead(PIN_RUNNING_FB);

  strlcpy(txMsg.a, "SlaveHonda->Master", sizeof(txMsg.a));
  txMsg.HondaIgnitionOn = gblHondaIgnOn;
  txMsg.HondaStarting   = gblHondaStarting;
  txMsg.HondaRunning    = gblHondaRunning;
  esp_now_send(masterCustomMac, (uint8_t *)&txMsg, sizeof(txMsg));
}

// ── ESP-NOW Callbacks ─────────────────────────────────────────────────────────
void onDataSent(const uint8_t *mac, esp_now_send_status_t st) {}

void onDataRecv(const uint8_t *mac, const uint8_t *data, int len) {
  if (len < (int)sizeof(MasterMsg)) return;
  memcpy(&rxMsg, data, sizeof(MasterMsg));
  Serial.print("[Recv] HondaStart="); Serial.println(rxMsg.HondaStart);
  gblStartCommand = rxMsg.HondaStart;
  hondaMain();
  sendStatus();
}

// ── Web API ───────────────────────────────────────────────────────────────────
static String statusJson() {
  String j = "{";
  j += "\"ign\":"     + String(!digitalRead(PIN_IGNITION_RELAY) ? "true":"false") + ",";
  j += "\"sta\":"     + String(!digitalRead(PIN_STARTER_RELAY)  ? "true":"false") + ",";
  j += "\"run\":"     + String(!digitalRead(PIN_RUNNING_FB)     ? "true":"false") + ",";
  j += "\"cmd\":"     + String(gblStartCommand  ? "true":"false") + ",";
  j += "\"starting\":" + String(gblHondaStarting ? "true":"false");
  j += "}";
  return j;
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== " FIRMWARE_NAME " v" FIRMWARE_VERSION " ===");

  // Relay outputs — HIGH = relay OFF (active low)
  pinMode(PIN_IGNITION_RELAY, OUTPUT); digitalWrite(PIN_IGNITION_RELAY, HIGH);
  pinMode(PIN_STARTER_RELAY,  OUTPUT); digitalWrite(PIN_STARTER_RELAY,  HIGH);
  pinMode(PIN_LED_EXT,        OUTPUT); digitalWrite(PIN_LED_EXT,        LOW);
  pinMode(PIN_LED_ONBOARD,    OUTPUT); digitalWrite(PIN_LED_ONBOARD,    LOW);
  pinMode(PIN_RUNNING_FB,     INPUT);

  // Set custom MAC before WiFiManager
  WiFi.mode(WIFI_STA);
  esp_wifi_set_mac(WIFI_IF_STA, slaveCustomMac);

  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  if (!wm.autoConnect("SlaveHonda-Config", "honda1234")) {
    Serial.println("[WiFi] Config timeout — restarting");
    delay(500);
    ESP.restart();
  }
  Serial.print("[WiFi] Connected, IP: "); Serial.println(WiFi.localIP());

  // ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESP-NOW] Init failed — restarting");
    ESP.restart();
  }
  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);

  memset(&peerMaster, 0, sizeof(peerMaster));
  memcpy(peerMaster.peer_addr, masterCustomMac, 6);
  peerMaster.channel = 0;
  peerMaster.encrypt = false;
  if (esp_now_add_peer(&peerMaster) != ESP_OK)
    Serial.println("[ESP-NOW] Failed to add Master peer");
  else
    Serial.println("[ESP-NOW] Master peer added");

  // Web server
  webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    String html = String(INDEX_HTML);
    html.replace("%VER%",  FIRMWARE_VERSION);
    html.replace("%NAME%", FIRMWARE_NAME);
    html.replace("%IP%",   WiFi.localIP().toString());
    req->send(200, "text/html", html);
  });
  webServer.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "application/json", statusJson());
  });
  ElegantOTA.begin(&webServer);
  ElegantOTA.setAutoReboot(true);
  webServer.begin();

  Serial.print("[Web] http://"); Serial.println(WiFi.localIP());
  Serial.println("[Ready]");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
  ElegantOTA.loop();

  // Periodic status to master
  if (millis() - lastStatusSend >= STATUS_SEND_MS) {
    lastStatusSend = millis();
    sendStatus();
  }

  // Update running feedback continuously
  gblHondaRunning = !digitalRead(PIN_RUNNING_FB);

  // Heartbeat LED
  digitalWrite(PIN_LED_ONBOARD, (millis() / 500) % 2 == 0 ? HIGH : LOW);
  delay(50);
}
