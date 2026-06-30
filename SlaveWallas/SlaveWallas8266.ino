
/*
  ESP8266 Blink by Simon Peter
  Blink the blue LED on the ESP-01 module
  This example code is in the public domain

  The blue LED on the ESP-01 module is connected to GPIO1
  (which is also the TXD pin; so we cannot use Serial.print() at the same time)

  Note that this sketch uses LED_BUILTIN to find the pin with the internal LED
*/
#include <ESP8266WiFi.h>
#include <espnow.h>


#define WIFI false
#define WIFI_CHANNEL 5
// Pin usage
#define pinWallasStartLED 2
#define pinWallasStart 0
#define HW_HondaFB  0  // HW feedback enabled
#define DEBUG 0
#define HondaSendStatusTime 10000   // ms Slave send interval
unsigned long startTime=0;
// Structure example to receive data
// Must match the receiver structure
typedef struct struct_mastermessage {
  char a[32];
  bool HondaRunningFB;
  bool HondaIgnitionOn;
  bool HondaStart;
} struct_mastermessage;

// Create a struct_message called myData for master message to slave
struct_mastermessage myData;

// Structure example to send data
// Must match the receiver structure
typedef struct struct_slavemessage {
  char a[32];
  bool HondaIgnitionOn;
  bool HondaStarting;
  bool HondaRunning;
} struct_slavemessage;

// Create a struct_message called myData for master message to slave
struct_slavemessage myDataSlave;

bool gblWallasStartCommand= false;
bool gblCurrentWallasStartCommand= false;

//  Set legal MAC address; observe success on setting MAC in serioal monitor
uint8_t slaveCustomMac[] = {0x30, 0xAE, 0xA4, 0x1A, 0xAE, 0x30};  //Custom mac address for This Slave Device
uint8_t masterCustomMac[] = {0x30, 0xAE, 0xA4, 0x89, 0x92, 0x7A}; //Custom mac address for The Master Device 

byte cnt=0;


//
//  Set fixed MAC address and set channel to value WIFI_CHANNEL
//  Checks status with ESP_ERROR_CHECK 
//
//
// Init ESP Now with fallback
//
void InitESPNow() {
  
  WiFi.mode(WIFI_STA);
  Serial.print("ESPNow init: Mac this station:");Serial.println(WiFi.macAddress() );
 // WiFi.disconnect();
  if (esp_now_init() == 0) {
   Serial.println("ESPNow Init Success");
   esp_now_set_self_role(ESP_NOW_ROLE_SLAVE);
// Regiuster receive callback routine
   esp_now_register_recv_cb(OnDataRecv);
  }
  else {
    Serial.println("ESPNow Init Failed");
    // Retry InitESPNow, add a counte and then restart?
    // InitESPNow();
    // or Simply Restart
    ESP.restart();
  }
}
void setup() {
    uint8_t primaryChan;
  uint8_t secondChan = 0;
  // Set pin mode SlaveUnit
// Start out
  pinMode(pinWallasStartLED,OUTPUT);
  digitalWrite(pinWallasStartLED,LOW);  
  delay(100);
 // Start out
  pinMode(pinWallasStart,OUTPUT);
  digitalWrite(pinWallasStart,HIGH);  
  delay(100);
// start serial
  Serial.begin(74880);
// Delay 200 msec needed to become stable.....
  delay(200);
  for (int i = 0; i <= 100; i++) {
     Serial.println("Initializing.. ");
    delay(10);
  }
    //Set device in AP mode to begin with
// Version info
  Serial.println("Wallas8266 V4, 10/07/2024.. ");
  Serial.println("Wallas8266 V4, 10/07/2024.. ");
  Serial.println("Wallas8266 V4, 10/07/2024.. ");
  Serial.println("Wallas8266 V4, 10/07/2024.. ");
// Set channel 
// trick to set channel to WIFI_CHANNEL
  WiFi.disconnect(); 
  delay(200);
  Serial.println();
  WiFi.mode(WIFI_AP_STA); 
  Serial.print("Channel inital value: ");
  Serial.println(WiFi.channel());
  WiFi.softAP("fakeSSID","fakePasw", WIFI_CHANNEL, true); // hide true
  Serial.print("Channel set to: ");
  Serial.println(WiFi.channel());

  WiFi.mode(WIFI_STA);
// Set specific mac address
  Serial.print("[OLD] ESP8266 Board MAC Address:  ");
  Serial.println(WiFi.macAddress());
  wifi_set_macaddr(STATION_IF, &slaveCustomMac[0]);
  Serial.print("[NEW] ESP8266 Board MAC Address:  ");
  Serial.println(WiFi.macAddress());

// Disconnect from an AP if it was previously connected
  WiFi.disconnect();
  delay(100);

//  espnow init
  InitESPNow();


}
// callback when data is recv from Master

void OnDataRecv(uint8_t *mac_addr, uint8_t *data, uint8_t data_len) {
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
           mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
  #ifdef DEBUG
    Serial.print("Last Packet Recv from: "); Serial.println(macStr);
    Serial.print("Last Packet Recv Data: "); Serial.println(*data);
    Serial.println("");
  #endif
  int indata= *data;
  memcpy(&myData, data, sizeof(myData));
  #ifdef DEBUG 
    Serial.print("Decoded data: "); 
    Serial.println(indata);
    Serial.print("Bytes received: ");
    Serial.println(data_len);
    Serial.print("Char: ");
    Serial.println(myData.a);
    Serial.print("WallasStart: ");
    Serial.println(myData.HondaStart);
    Serial.println();
  #endif
  gblWallasStartCommand = myData.HondaStart;
  WallasMain();
}

void WallasMain()
{
//  if(gblWallasStartCommand != gblCurrentWallasStartCommand) 
//  {
    gblCurrentWallasStartCommand=  gblWallasStartCommand;
    if(!gblWallasStartCommand)
    {
      WallasStart();
    }
    else 
    {
      WallasStop();
    }
//  }
  }
void WallasStart()
{
  Serial.print("Wallas starting..");
  digitalWrite(pinWallasStartLED,HIGH);
  digitalWrite(pinWallasStart,HIGH);
  Serial.println("Wallas started..");
}

  void WallasStop()
{
      Serial.print("Wallas stop..");
       digitalWrite(pinWallasStartLED,LOW);
      digitalWrite(pinWallasStart,LOW);      
      Serial.println("Wallas stopped");
  }
// the loop function runs over and over again forever
void loop() {

  }
