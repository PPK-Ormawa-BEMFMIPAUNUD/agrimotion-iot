#include <WiFi.h>
#include <PubSubClient.h>
#include <esp_now.h>

// ==========================================
// 1. KONFIGURASI WIFI & MQTT SERVER
// ==========================================
const char* ssid = "AGRI-MOTION";         
const char* password = "agri1234"; 
const char* mqtt_server = "103.174.114.65"; 
const int mqtt_port = 1883;
const char* mqtt_client_id = "agrimotion-esp-a";
const char* mqtt_topic_sub = "agrimotion/device/pumps/cmd";    
const char* topic_status   = "agrimotion/device/pumps/status"; 

WiFiClient espClient;
PubSubClient mqttClient(espClient);
unsigned long lastMqttRetry = 0;

// ==========================================
// 2. PEMETAAN PIN & VARIABEL
// ==========================================
const int PIN_TRIFOO_AIR = 32; 
const int PIN_TRIFOO_D1  = 33; 
const int PIN_TRIFOO_D2  = 25; 
const int PIN_TRIFOO_D3  = 26; 

const int PIN_FLOAT_1 = 18; 
const int PIN_FLOAT_2 = 19; 

const int allPins[4] = {PIN_TRIFOO_AIR, PIN_TRIFOO_D1, PIN_TRIFOO_D2, PIN_TRIFOO_D3};
int activeZones = 0;

// Variabel Pengisian Jerigen (ESP-NOW)
bool checkRefill = false;
bool isRefilling = false;
unsigned long lastPingTime = 0;
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

void setup() {
  for (int i = 0; i < 4; i++) {
    digitalWrite(allPins[i], HIGH);
    pinMode(allPins[i], OUTPUT_OPEN_DRAIN);
    digitalWrite(allPins[i], HIGH);
    pinMode(allPins[i], OUTPUT);
  }

  pinMode(PIN_FLOAT_1, INPUT_PULLUP);
  pinMode(PIN_FLOAT_2, INPUT_PULLUP);

  Serial.begin(115200);
  delay(1000);
  Serial.println("\n[ESP A] SISTEM PENGENDALI POMPA TRIFOO BOOTING...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  
  Serial.print("\n[ESP A] Terhubung ke WiFi. Channel WiFi: ");
  Serial.println(WiFi.channel());

  if (esp_now_init() != ESP_OK) {
    Serial.println("Error inisialisasi ESP-NOW");
  } else {
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, broadcastAddress, 6);
    peerInfo.channel = 0; 
    peerInfo.encrypt = false;
    esp_now_add_peer(&peerInfo);
  }

  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqttCallback);
}

void sendEspNow(String cmd) {
  esp_now_send(broadcastAddress, (uint8_t *)cmd.c_str(), cmd.length());
}

void startTrifoo(int pinDemplot, String namaDemplot) {
  Serial.println("[ESP A] Menunggu Valve terbuka (800ms)...");
  delay(800); 
  activeZones++;
  digitalWrite(PIN_TRIFOO_AIR, LOW); 
  delay(300);
  digitalWrite(pinDemplot, LOW);     
  Serial.println("[ESP A] " + namaDemplot + " & Master Air MENYALA");
}

void stopTrifoo(int pinDemplot, String namaDemplot) {
  Serial.println("[ESP A] Melakukan Flushing Pipa (3000ms)...");
  delay(3000); 
  digitalWrite(pinDemplot, HIGH); 
  activeZones--;
  
  if (activeZones <= 0) {
    digitalWrite(PIN_TRIFOO_AIR, HIGH); 
    activeZones = 0;
    checkRefill = true;
  }
  Serial.println("[ESP A] " + namaDemplot + " & Master Air MATI");
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String cmd = "";
  for (int i = 0; i < length; i++) { cmd += (char)payload[i]; }
  cmd.trim(); cmd.toUpperCase();
  Serial.println("[ESP A] CMD: " + cmd);

  // -- DEMPLOT 1 -- (Ditambah D1_AIR_ON & OFF)
  if      (cmd == "D1_PUPUK_ON"  || cmd == "D1_PESTI_ON"  || cmd == "D1_AIR_ON")  startTrifoo(PIN_TRIFOO_D1, "Trifoo D1");
  else if (cmd == "D1_PUPUK_OFF" || cmd == "D1_PESTI_OFF" || cmd == "D1_AIR_OFF") stopTrifoo(PIN_TRIFOO_D1, "Trifoo D1");
  
  // -- DEMPLOT 2 -- (Ditambah D2_AIR_ON & OFF)
  else if (cmd == "D2_PUPUK_ON"  || cmd == "D2_PESTI_ON"  || cmd == "D2_AIR_ON")  startTrifoo(PIN_TRIFOO_D2, "Trifoo D2");
  else if (cmd == "D2_PUPUK_OFF" || cmd == "D2_PESTI_OFF" || cmd == "D2_AIR_OFF") stopTrifoo(PIN_TRIFOO_D2, "Trifoo D2");
  
  // -- DEMPLOT 3 -- (Ditambah D3_AIR_ON & OFF)
  else if (cmd == "D3_PUPUK_ON"  || cmd == "D3_PESTI_ON"  || cmd == "D3_AIR_ON")  startTrifoo(PIN_TRIFOO_D3, "Trifoo D3");
  else if (cmd == "D3_PUPUK_OFF" || cmd == "D3_PESTI_OFF" || cmd == "D3_AIR_OFF") stopTrifoo(PIN_TRIFOO_D3, "Trifoo D3");
  
  // -- EMERGENCY --
  else if (cmd == "ALL_OFF") {
    for (int i = 0; i < 4; i++) digitalWrite(allPins[i], HIGH);
    activeZones = 0; checkRefill = true;
  }
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) {
      if (millis() - lastMqttRetry >= 5000) {
        lastMqttRetry = millis();
        if (mqttClient.connect(mqtt_client_id)) {
          mqttClient.subscribe(mqtt_topic_sub);
          mqttClient.publish(topic_status, "STATUS: ESP-A (Trifoo+Sensor) Siap");
        }
      }
    } else { mqttClient.loop(); }
  }

  bool isFull = (digitalRead(PIN_FLOAT_1) == LOW) && (digitalRead(PIN_FLOAT_2) == LOW);

  if (checkRefill && !isFull) { 
    isRefilling = true; checkRefill = false;
  }
  if (isFull && isRefilling) {  
    isRefilling = false; sendEspNow("CMD_PUMP_OFF");
    mqttClient.publish(topic_status, "STATUS: TANDON PENUH");
  }
  if (isRefilling && (millis() - lastPingTime > 2000)) {
    sendEspNow("CMD_PUMP_ON"); lastPingTime = millis();
  }
}