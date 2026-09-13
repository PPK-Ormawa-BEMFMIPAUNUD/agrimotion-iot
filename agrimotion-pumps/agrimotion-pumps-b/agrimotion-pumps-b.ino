#include <WiFi.h>
#include <PubSubClient.h>

const char* ssid = "AGRI-MOTION";         
const char* password = "agri1234"; 
const char* mqtt_server = "103.174.114.65"; 
const int mqtt_port = 1883;
const char* mqtt_client_id = "agrimotion-esp-b";
const char* mqtt_topic_sub = "agrimotion/device/pumps/cmd";    
const char* topic_status   = "agrimotion/device/pumps/status"; 

WiFiClient espClient;
PubSubClient mqttClient(espClient);
unsigned long lastMqttRetry = 0;

const int PIN_PERI_1 = 32; const int PIN_PERI_2 = 33; 
const int PIN_PERI_3 = 25; const int PIN_PERI_4 = 26; 
const int PIN_PERI_5 = 23; const int PIN_PERI_6 = 22; 
const int PIN_VALVE_1 = 21; const int PIN_VALVE_2 = 19; 
const int PIN_VALVE_3 = 4;  const int PIN_VALVE_4 = 27; 
const int PIN_VALVE_5 = 14; const int PIN_VALVE_6 = 12; 

const int allPins[12] = {PIN_PERI_1, PIN_PERI_2, PIN_PERI_3, PIN_PERI_4, PIN_PERI_5, PIN_PERI_6, PIN_VALVE_1, PIN_VALVE_2, PIN_VALVE_3, PIN_VALVE_4, PIN_VALVE_5, PIN_VALVE_6};

// Variabel Pengaman Watchdog (3 Menit)
const unsigned long MAX_PUMP_TIMEOUT_MS = 180000;
unsigned long pumpStartTime = 0;
bool isAnyPumpRunning = false;

bool isAnyRelayActive() {
  for (int i = 0; i < 12; i++) {
    if (digitalRead(allPins[i]) == LOW) return true;
  }
  return false;
}

void turnOffAll() {
  for (int i = 0; i < 12; i++) digitalWrite(allPins[i], HIGH);
  isAnyPumpRunning = false;
}

void setup() {
  for (int i = 0; i < 12; i++) {
    digitalWrite(allPins[i], HIGH);
    pinMode(allPins[i], OUTPUT_OPEN_DRAIN);
    digitalWrite(allPins[i], HIGH);
    pinMode(allPins[i], OUTPUT);
  }
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqttCallback);
}

// ------------------------------------------
// Fungsi Pupuk / Pestisida (Valve + Peristaltik)
// ------------------------------------------
void startDosing(int pinValve, int pinPeri, String nama) {
  digitalWrite(pinValve, LOW); 
  delay(1600); 
  digitalWrite(pinPeri, LOW);  
}

void stopDosing(int pinValve, int pinPeri, String nama) {
  digitalWrite(pinPeri, HIGH); 
  delay(3800); 
  digitalWrite(pinValve, HIGH); 
}

// ------------------------------------------
// Fungsi Penyiraman Air Saja (Hanya Valve)
// ------------------------------------------
void startWaterOnly(int pinValve, String nama) {
  Serial.println("\n>> [ESP B] MULAI SEKUEN ON: " + nama);
  digitalWrite(pinValve, LOW); // Hanya buka Valve (tanpa menyalakan peristaltik)
  Serial.println("[ESP B] Valve TERBUKA (Hanya Air/Drip)");
  mqttClient.publish(topic_status, ("STATUS: " + nama + " AKTIF").c_str());
}

void stopWaterOnly(int pinValve, String nama) {
  Serial.println("\n>> [ESP B] MULAI SEKUEN OFF: " + nama);
  delay(3800); // Tunggu Pompa Trifoo mati & membuang tekanan (3.8 detik)
  digitalWrite(pinValve, HIGH); // Tutup Valve
  Serial.println("[ESP B] Valve TERTUTUP (Hanya Air/Drip)");
  mqttClient.publish(topic_status, ("STATUS: " + nama + " SELESAI").c_str());
}
// ------------------------------------------

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String cmd = "";
  for (int i = 0; i < length; i++) { cmd += (char)payload[i]; }
  cmd.trim(); cmd.toUpperCase();
  Serial.println("[ESP B] CMD: " + cmd);
  
  // -- DEMPLOT 1 --
  if      (cmd == "D1_PUPUK_ON")   startDosing(PIN_VALVE_1, PIN_PERI_1, "Pupuk D1");
  else if (cmd == "D1_PUPUK_OFF")  stopDosing(PIN_VALVE_1, PIN_PERI_1, "Pupuk D1");
  else if (cmd == "D1_PESTI_ON")   startDosing(PIN_VALVE_2, PIN_PERI_2, "Pestisida D1");
  else if (cmd == "D1_PESTI_OFF")  stopDosing(PIN_VALVE_2, PIN_PERI_2, "Pestisida D1");
  // Air D1 (Pakai Valve Pupuk D1: PIN_VALVE_1)
  else if (cmd == "D1_AIR_ON")     startWaterOnly(PIN_VALVE_1, "Siram Air D1");
  else if (cmd == "D1_AIR_OFF")    stopWaterOnly(PIN_VALVE_1, "Siram Air D1");

  // -- DEMPLOT 2 --
  else if (cmd == "D2_PUPUK_ON")   startDosing(PIN_VALVE_3, PIN_PERI_3, "Pupuk D2");
  else if (cmd == "D2_PUPUK_OFF")  stopDosing(PIN_VALVE_3, PIN_PERI_3, "Pupuk D2");
  else if (cmd == "D2_PESTI_ON")   startDosing(PIN_VALVE_4, PIN_PERI_4, "Pestisida D2");
  else if (cmd == "D2_PESTI_OFF")  stopDosing(PIN_VALVE_4, PIN_PERI_4, "Pestisida D2");
  // Air D2 (Pakai Valve Pupuk D2: PIN_VALVE_3)
  else if (cmd == "D2_AIR_ON")     startWaterOnly(PIN_VALVE_3, "Siram Air D2");
  else if (cmd == "D2_AIR_OFF")    stopWaterOnly(PIN_VALVE_3, "Siram Air D2");

  // -- DEMPLOT 3 --
  else if (cmd == "D3_PUPUK_ON")   startDosing(PIN_VALVE_5, PIN_PERI_5, "Pupuk D3");
  else if (cmd == "D3_PUPUK_OFF")  stopDosing(PIN_VALVE_5, PIN_PERI_5, "Pupuk D3");
  else if (cmd == "D3_PESTI_ON")   startDosing(PIN_VALVE_6, PIN_PERI_6, "Pestisida D3");
  else if (cmd == "D3_PESTI_OFF")  stopDosing(PIN_VALVE_6, PIN_PERI_6, "Pestisida D3");
  // Air D3 (Pakai Valve Pupuk D3: PIN_VALVE_5)
  else if (cmd == "D3_AIR_ON")     startWaterOnly(PIN_VALVE_5, "Siram Air D3");
  else if (cmd == "D3_AIR_OFF")    stopWaterOnly(PIN_VALVE_5, "Siram Air D3");

  // -- EMERGENCY --
  else if (cmd == "ALL_OFF") { 
    turnOffAll();
  }

  // Update Status Software Watchdog
  if (cmd.endsWith("_ON")) {
    pumpStartTime = millis();
    isAnyPumpRunning = true;
  } else if (cmd.endsWith("_OFF")) {
    if (!isAnyRelayActive()) {
      isAnyPumpRunning = false;
    }
  }
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) {
      if (millis() - lastMqttRetry >= 5000) {
        lastMqttRetry = millis();
        if (mqttClient.connect(mqtt_client_id)) {
          mqttClient.subscribe(mqtt_topic_sub);
        }
      }
    } else { mqttClient.loop(); }
  }

  // --- EMERGENCY HARD-TIMEOUT (SOFTWARE WATCHDOG) ---
  if (isAnyPumpRunning && (millis() - pumpStartTime >= MAX_PUMP_TIMEOUT_MS)) {
    Serial.println("\n[SAFETY EWS] ⚠️ HARD TIMEOUT (3 Menit) Tercapai! Mematikan seluruh pompa/valve otomatis...");
    turnOffAll();
    if (mqttClient.connected()) {
      mqttClient.publish(topic_status, "ALERT: [SAFETY EWS] HARD TIMEOUT (3 Menit) Tercapai! Seluruh valve & dosing dimatikan.");
    }
  }
}