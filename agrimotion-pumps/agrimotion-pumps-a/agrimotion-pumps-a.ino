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
const char* mqtt_topic_refill_cmd = "agrimotion/device/pumps/refill/cmd"; // Topic Relay ke Node-3A

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

// Konfigurasi Water Level Float Sensor Tunggal (Active-Low: Pelampung Naik/Penuh = LOW)
const int PIN_FLOAT_1 = 18; 
const int PIN_FLOAT_2 = 19; 
#define USE_SINGLE_FLOAT_SENSOR true
const int FLOAT_FULL_STATE = LOW;

const int allPins[4] = {PIN_TRIFOO_AIR, PIN_TRIFOO_D1, PIN_TRIFOO_D2, PIN_TRIFOO_D3};
int activeZones = 0;

// Variabel Pengaman Watchdog Pompa Utama (3 Menit)
const unsigned long MAX_PUMP_TIMEOUT_MS = 180000;
unsigned long pumpStartTime = 0;
bool isAnyPumpRunning = false;

// Variabel Pengisian Jerigen 10L (Auto-Refill & Timeout Safety)
bool checkRefill = false;
bool isRefilling = false;
bool isRefillTimeoutError = false; // Interlock Flag: Mengunci pompa jika terjadi gagal isi (Timeout)
unsigned long refillStartTime = 0;
unsigned long lastPingTime = 0;
const unsigned long MAX_REFILL_TIMEOUT_MS = 180000; // 3 Menit (180 detik) Max-Runtime Pengisian
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Variabel Sensor Debouncing (Filter riak air saat pengisian)
const unsigned long FLOAT_DEBOUNCE_MS = 1500; // Stabil minimal 1.5 detik
bool isWaterFullDebounced = false;
bool lastRawFloatState = false;
unsigned long lastFloatDebounceTime = 0;

bool readRawFloat() {
#if USE_SINGLE_FLOAT_SENSOR
  return (digitalRead(PIN_FLOAT_1) == FLOAT_FULL_STATE);
#else
  return (digitalRead(PIN_FLOAT_1) == FLOAT_FULL_STATE) && (digitalRead(PIN_FLOAT_2) == FLOAT_FULL_STATE);
#endif
}

void updateWaterLevelSensor() {
  bool currentRaw = readRawFloat();
  if (currentRaw != lastRawFloatState) {
    lastFloatDebounceTime = millis();
    lastRawFloatState = currentRaw;
  }
  if ((millis() - lastFloatDebounceTime) >= FLOAT_DEBOUNCE_MS) {
    if (currentRaw != isWaterFullDebounced) {
      isWaterFullDebounced = currentRaw;
      Serial.print("[ESP A] Status Sensor Tandon (Debounced): ");
      Serial.println(isWaterFullDebounced ? "PENUH (FULL)" : "KURANG/KOSONG (NOT FULL)");
    }
  }
}

bool isAnyRelayActive() {
  for (int i = 0; i < 4; i++) {
    if (digitalRead(allPins[i]) == LOW) return true;
  }
  return false;
}

void turnOffAll() {
  for (int i = 0; i < 4; i++) digitalWrite(allPins[i], HIGH);
  activeZones = 0; 
  checkRefill = true;
  isAnyPumpRunning = false;
}

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

  // Inisialisasi status debouncing sensor pelampung
  lastRawFloatState = readRawFloat();
  isWaterFullDebounced = lastRawFloatState;
}

void sendEspNow(String cmd) {
  esp_now_send(broadcastAddress, (uint8_t *)cmd.c_str(), cmd.length());
}

void sendRefillCommand(String cmd) {
  // 1. Kirim via MQTT ke ESP Node-3A (Relay/Jembatan ke Pumps-C)
  if (mqttClient.connected()) {
    mqttClient.publish(mqtt_topic_refill_cmd, cmd.c_str());
  }
  // 2. Redundansi: Kirim langsung via ESP-NOW jika dalam jangkauan
  sendEspNow(cmd);
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
  for (unsigned int i = 0; i < length; i++) { cmd += (char)payload[i]; }
  cmd.trim(); cmd.toUpperCase();
  Serial.println("[ESP A] CMD: " + cmd);

  // INTERLOCK SYSTEM: Cegah aktivasi pompa jika terjadi error gagal isi tandon (ERROR_TIMEOUT_REFILL)
  if (cmd.endsWith("_ON")) {
    if (isRefillTimeoutError) {
      Serial.println("[INTERLOCK] ⚠️ Perintah DITOLAK! Terjadi ERROR_TIMEOUT_REFILL. Air jerigen gagal terisi.");
      if (mqttClient.connected()) {
        mqttClient.publish(topic_status, "ALERT: [INTERLOCK] Perintah dibatalkan! Refill Timeout Error aktif. Harap periksa tandon.");
      }
      return; // Batalkan eksekusi, jangan nyalakan pompa!
    }
    pumpStartTime = millis();
    isAnyPumpRunning = true;
    checkRefill = true; // Setiap proses akan/sedang dijalankan, stok air diasumsikan berkurang
  }

  // -- DEMPLOT 1 -- (Ditambah D1_AIR_ON & OFF)
  if      (cmd == "D1_PUPUK_ON"  || cmd == "D1_PESTI_ON"  || cmd == "D1_AIR_ON")  startTrifoo(PIN_TRIFOO_D1, "Trifoo D1");
  else if (cmd == "D1_PUPUK_OFF" || cmd == "D1_PESTI_OFF" || cmd == "D1_AIR_OFF") stopTrifoo(PIN_TRIFOO_D1, "Trifoo D1");
  
  // -- DEMPLOT 2 -- (Ditambah D2_AIR_ON & OFF)
  else if (cmd == "D2_PUPUK_ON"  || cmd == "D2_PESTI_ON"  || cmd == "D2_AIR_ON")  startTrifoo(PIN_TRIFOO_D2, "Trifoo D2");
  else if (cmd == "D2_PUPUK_OFF" || cmd == "D2_PESTI_OFF" || cmd == "D2_AIR_OFF") stopTrifoo(PIN_TRIFOO_D2, "Trifoo D2");
  
  // -- DEMPLOT 3 -- (Ditambah D3_AIR_ON & OFF)
  else if (cmd == "D3_PUPUK_ON"  || cmd == "D3_PESTI_ON"  || cmd == "D3_AIR_ON")  startTrifoo(PIN_TRIFOO_D3, "Trifoo D3");
  else if (cmd == "D3_PUPUK_OFF" || cmd == "D3_PESTI_OFF" || cmd == "D3_AIR_OFF") stopTrifoo(PIN_TRIFOO_D3, "Trifoo D3");
  
  // -- EMERGENCY & RESET --
  else if (cmd == "ALL_OFF") {
    turnOffAll();
    isRefillTimeoutError = false; // Reset error saat darurat dimatikan manual
  }
  else if (cmd == "REFILL_RESET" || cmd == "RESET_REFILL_ERROR") {
    isRefillTimeoutError = false;
    checkRefill = true;
    Serial.println("[ESP A] ERROR_TIMEOUT_REFILL di-reset manual via MQTT.");
    if (mqttClient.connected()) {
      mqttClient.publish(topic_status, "STATUS: Refill error telah di-reset. Siap auto-refill.");
    }
  }

  // Update Status Software Watchdog untuk OFF
  if (cmd.endsWith("_OFF")) {
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
          mqttClient.publish(topic_status, "STATUS: ESP-A (Trifoo+Sensor) Siap");
        }
      }
    } else { mqttClient.loop(); }
  }

  // --- EMERGENCY HARD-TIMEOUT (SOFTWARE WATCHDOG POMPA UTAMA) ---
  if (isAnyPumpRunning && (millis() - pumpStartTime >= MAX_PUMP_TIMEOUT_MS)) {
    Serial.println("\n[SAFETY EWS] ⚠️ HARD TIMEOUT (3 Menit) Tercapai! Mematikan seluruh pompa/valve otomatis...");
    turnOffAll();
    if (mqttClient.connected()) {
      mqttClient.publish(topic_status, "ALERT: [SAFETY EWS] HARD TIMEOUT (3 Menit) Tercapai! Seluruh pompa dimatikan otomatis.");
    }
  }

  // --- KONTROL OTOMASI PENGISIAN AIR & SAFETY PROTECTION (JERIGEN 10L) ---
  // 1. Update bacaan sensor air dengan debouncing (1500 ms filter riak air)
  updateWaterLevelSensor();

  // 2. Evaluasi status tandon penuh
  if (isWaterFullDebounced) {
    if (isRefilling) {
      isRefilling = false;
      checkRefill = false;
      sendRefillCommand("CMD_PUMP_OFF");
      Serial.println("[ESP A] Jerigen PENUH (isFull=TRUE). Mengirim sinyal STOP ke bilge pump.");
      if (mqttClient.connected()) {
        mqttClient.publish(topic_status, "STATUS: TANDON PENUH (REFILL SELESAI)");
      }
    }
    // Jika tandon sudah penuh, pulihkan status interlock otomatis
    if (isRefillTimeoutError) {
      isRefillTimeoutError = false;
    }
  }

  // 3. Pemicu Pengisian Otomatis (Auto-Refill Trigger)
  // Dipicu jika checkRefill aktif atau air tidak penuh, sedang tidak mengisi, dan tidak ada error timeout
  if ((checkRefill || !isWaterFullDebounced) && !isWaterFullDebounced && !isRefilling && !isRefillTimeoutError) {
    isRefilling = true;
    checkRefill = false;
    refillStartTime = millis();
    lastPingTime = millis();
    sendRefillCommand("CMD_PUMP_ON");
    Serial.println("[ESP A] Stok air jerigen berkurang. Memulai Auto-Refill (Bilge Pump ON)...");
    if (mqttClient.connected()) {
      mqttClient.publish(topic_status, "STATUS: MEMULAI PENGISIAN AIR JERIGEN");
    }
  }

  // 4. Pengawasan Selama Proses Pengisian (Refilling Active)
  if (isRefilling) {
    // 4a. Calculated Timeout Protection (Max 3 Menit: Kapasitas 10L, Debit 5L/menit)
    if (millis() - refillStartTime >= MAX_REFILL_TIMEOUT_MS) {
      isRefilling = false;
      checkRefill = false;
      isRefillTimeoutError = true;
      sendRefillCommand("CMD_PUMP_OFF");
      Serial.println("\n[SAFETY EWS] ⚠️ ERROR_TIMEOUT_REFILL! Pengisian mencapai batas 3 menit.");
      Serial.println("[SAFETY EWS] Bilge pump dimatikan paksa & Interlock penyiraman diaktifkan!");
      if (mqttClient.connected()) {
        mqttClient.publish(topic_status, "ERROR_TIMEOUT_REFILL: Gagal mengisi jerigen dalam 3 menit! Periksa pasokan air/sensor.");
      }
    }
    // 4b. Periodik Heartbeat Ping (Setiap 2000 ms) agar watchdog di Pumps-C tidak cutoff
    else if (millis() - lastPingTime > 2000) {
      sendRefillCommand("CMD_PUMP_ON");
      lastPingTime = millis();
    }
  }
}