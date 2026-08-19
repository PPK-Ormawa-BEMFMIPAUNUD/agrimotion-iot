#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>

// ==========================================
// PENTING: SINKRONISASI KANAL WIFI!
// Ubah angka ini sesuai dengan "Channel WiFi" 
// yang tercetak di Serial Monitor ESP A.
// ==========================================
constexpr uint8_t WIFI_CHANNEL = 1; 

const int PIN_BILGE_PUMP = 32; // IN1 Relay 5

// Variabel Pengaman (Fail-Safe)
bool isPumpOn = false;
unsigned long lastHeartbeatTime = 0;
unsigned long pumpStartTime = 0;
const unsigned long TIMEOUT_HEARTBEAT = 5000;   // 5 Detik hilang sinyal -> Mati
const unsigned long MAX_RUN_TIME = 480000;      // 8 Menit batas maksimal hidup terus-terusan

// Callback saat data ESP-NOW masuk
void OnDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
  String cmd = "";
  for (int i = 0; i < len; i++) { cmd += (char)incomingData[i]; }
  
  if (cmd == "CMD_PUMP_ON") {
    lastHeartbeatTime = millis();
    if (!isPumpOn) {
      digitalWrite(PIN_BILGE_PUMP, LOW); // Pompa ON
      isPumpOn = true;
      pumpStartTime = millis();
      Serial.println("[ESP C] Sinyal ON diterima. Pompa Bilge MENYALA.");
    }
  } 
  else if (cmd == "CMD_PUMP_OFF") {
    digitalWrite(PIN_BILGE_PUMP, HIGH); // Pompa OFF
    isPumpOn = false;
    Serial.println("[ESP C] Sinyal OFF diterima. Pompa Bilge MATI.");
  }
}

void setup() {
  // SAFETY BOOT
  digitalWrite(PIN_BILGE_PUMP, HIGH);
  pinMode(PIN_BILGE_PUMP, OUTPUT_OPEN_DRAIN);
  digitalWrite(PIN_BILGE_PUMP, HIGH);
  pinMode(PIN_BILGE_PUMP, OUTPUT);

  Serial.begin(115200);
  delay(1000);
  Serial.println("\n[ESP C] SISTEM POMPA PARIT (BILGE) BOOTING...");

  // Konfigurasi WiFi Station (Wajib untuk ESP-NOW)
  WiFi.mode(WIFI_STA);
  
  // Memaksa ESP C berada di Channel yang sama dengan Modem/ESP A
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  // Inisialisasi ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error Inisialisasi ESP-NOW");
    return;
  }
  esp_now_register_recv_cb(OnDataRecv);
  Serial.println("[ESP C] Berhasil Setup ESP-NOW. Menunggu sinyal...");
}

void loop() {
  if (isPumpOn) {
    // FAIL-SAFE 1: Sinyal Terputus / Out of Range
    if (millis() - lastHeartbeatTime > TIMEOUT_HEARTBEAT) {
      digitalWrite(PIN_BILGE_PUMP, HIGH);
      isPumpOn = false;
      Serial.println("[ALARM] Sinyal Terputus > 5 Detik! Pompa dimatikan paksa.");
    }
    
    // FAIL-SAFE 2: Pompa menyala terlalu lama (Sensor rusak/nyangkut)
    if (millis() - pumpStartTime > MAX_RUN_TIME) {
      digitalWrite(PIN_BILGE_PUMP, HIGH);
      isPumpOn = false;
      Serial.println("[ALARM] Batas Waktu 8 Menit Tercapai! Pompa dimatikan paksa.");
    }
  }
}