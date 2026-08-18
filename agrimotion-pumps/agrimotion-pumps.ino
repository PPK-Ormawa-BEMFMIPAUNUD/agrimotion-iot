#include <WiFi.h>
#include <PubSubClient.h>

// ==========================================
// 1. KONFIGURASI WIFI & MQTT SERVER
// ==========================================
const char* ssid = "AGRI-MOTION";         
const char* password = "agri1234"; 

const char* mqtt_server = "broker.hivemq.com"; 
const int mqtt_port = 1883;
const char* mqtt_user = ""; 
const char* mqtt_password = "";

const char* topic_cmd = "agrimotion/demplot/cmd";       
const char* topic_status = "agrimotion/demplot/status"; 

WiFiClient espClient;
PubSubClient client(espClient);

// ==========================================
// 2. PEMETAAN PIN (16 CHANNEL RELAY)
// ==========================================
// --- Relay 1 (Pompa Pendorong Utama & Demplot) ---
const int PIN_TRIFOO_AIR = 32; // Master Air
const int PIN_TRIFOO_D1  = 33; 
const int PIN_TRIFOO_D2  = 25; 
const int PIN_TRIFOO_D3  = 26; 

// --- Relay 2 (Peristaltik D1 & D2) ---
const int PIN_PERI_PUPUK_D1 = 23; 
const int PIN_PERI_PESTI_D1 = 22; 
const int PIN_PERI_PUPUK_D2 = 21; 
const int PIN_PERI_PESTI_D2 = 19; 

// --- Relay 3 (Peristaltik D3 & Valve D1) ---
const int PIN_PERI_PUPUK_D3  = 4;  
const int PIN_PERI_PESTI_D3  = 27; 
const int PIN_VALVE_PUPUK_D1 = 14; 
const int PIN_VALVE_PESTI_D1 = 12; // PERHATIAN: Cabut pin ini saat upload program

// --- Relay 4 (Valve D2 & D3) ---
const int PIN_VALVE_PUPUK_D2 = 18; 
const int PIN_VALVE_PESTI_D2 = 5;  // PERHATIAN: Pin strapping, pastikan tidak membuat bootloop
const int PIN_VALVE_PUPUK_D3 = 17; 
const int PIN_VALVE_PESTI_D3 = 16; 

// Variabel untuk melacak berapa banyak demplot yang sedang aktif (agar Master Air tidak mati duluan)
int activeZones = 0;

void setup() {
  Serial.begin(115200);

  // ARRAY UNTUK SAFETY BOOT
  int allPins[] = {
    PIN_TRIFOO_AIR, PIN_TRIFOO_D1, PIN_TRIFOO_D2, PIN_TRIFOO_D3,
    PIN_PERI_PUPUK_D1, PIN_PERI_PESTI_D1, PIN_PERI_PUPUK_D2, PIN_PERI_PESTI_D2,
    PIN_PERI_PUPUK_D3, PIN_PERI_PESTI_D3, PIN_VALVE_PUPUK_D1, PIN_VALVE_PESTI_D1,
    PIN_VALVE_PUPUK_D2, PIN_VALVE_PESTI_D2, PIN_VALVE_PUPUK_D3, PIN_VALVE_PESTI_D3
  };

  // Set HIGH (Mati) sebelum pinMode agar relay tidak cetek saat boot
  for (int i = 0; i < 16; i++) {
    digitalWrite(allPins[i], HIGH);
    pinMode(allPins[i], OUTPUT);
  }

  setup_wifi();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);
}

void setup_wifi() {
  delay(10);
  Serial.println("\nMenghubungkan ke WiFi...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Terhubung! IP: " + WiFi.localIP().toString());
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Menghubungkan ke MQTT...");
    String clientId = "AgriMotion-ESP32-" + String(random(0xffff), HEX);
    if (client.connect(clientId.c_str(), mqtt_user, mqtt_password)) {
      Serial.println("Terhubung!");
      client.subscribe(topic_cmd);
      client.publish(topic_status, "SISTEM SIAP: Seluruh 3 Demplot Terkoneksi");
    } else {
      Serial.print("Gagal, rc=");
      Serial.print(client.state());
      Serial.println(" Coba 5 detik lagi...");
      delay(5000);
    }
  }
}

// ==========================================
// 3. FUNGSI LOGIKA PERPIPAAN
// ==========================================

void jalankanSistem(String namaSistem, int pinValve, int pinTrifooD, int pinPeri) {
  Serial.println("\n>> MENYALAKAN: " + namaSistem);
  
  // 1. Buka Solenoid Valve
  digitalWrite(pinValve, LOW);
  delay(500);
  
  // 2. Nyalakan Pompa Pendorong & Master
  activeZones++; // Tambah zona aktif
  digitalWrite(PIN_TRIFOO_AIR, LOW); // Master selalu ON jika ada zona aktif
  digitalWrite(pinTrifooD, LOW);
  delay(500);
  
  // 3. Mulai Injeksi Peristaltik
  digitalWrite(pinPeri, LOW);
  
  String statusMsg = "STATUS: " + namaSistem + " ON";
  client.publish(topic_status, statusMsg.c_str());
}

void matikanSistem(String namaSistem, int pinValve, int pinTrifooD, int pinPeri) {
  Serial.println("\n>> MEMATIKAN & FLUSHING: " + namaSistem);
  
  // 1. Hentikan Injeksi Peristaltik (Bilas Pipa dengan air murni)
  digitalWrite(pinPeri, HIGH);
  delay(3000); // Flushing singkat 3 detik
  
  // 2. Matikan Pompa Pendorong Demplot
  digitalWrite(pinTrifooD, HIGH);
  
  // Evaluasi Master Pompa Air
  activeZones--;
  if (activeZones <= 0) {
    digitalWrite(PIN_TRIFOO_AIR, HIGH); // Matikan master HANYA jika tidak ada demplot lain yang jalan
    activeZones = 0; // Reset ke 0 untuk mencegah error minus
  }
  delay(500);
  
  // 3. Tutup Solenoid Valve
  digitalWrite(pinValve, HIGH);
  
  String statusMsg = "STATUS: " + namaSistem + " OFF (Selesai Flushing)";
  client.publish(topic_status, statusMsg.c_str());
}

void matikanSemuaTotal() {
  Serial.println("\n>> EMERGENCY ALL OFF!");
  int allPins[] = {
    PIN_TRIFOO_AIR, PIN_TRIFOO_D1, PIN_TRIFOO_D2, PIN_TRIFOO_D3,
    PIN_PERI_PUPUK_D1, PIN_PERI_PESTI_D1, PIN_PERI_PUPUK_D2, PIN_PERI_PESTI_D2,
    PIN_PERI_PUPUK_D3, PIN_PERI_PESTI_D3, PIN_VALVE_PUPUK_D1, PIN_VALVE_PESTI_D1,
    PIN_VALVE_PUPUK_D2, PIN_VALVE_PESTI_D2, PIN_VALVE_PUPUK_D3, PIN_VALVE_PESTI_D3
  };
  for (int i = 0; i < 16; i++) {
    digitalWrite(allPins[i], HIGH);
  }
  activeZones = 0;
  client.publish(topic_status, "STATUS: EMERGENCY ALL OFF Dieksekusi!");
}

// ==========================================
// 4. ROUTER PERINTAH DARI MQTT (SERVER/APP)
// ==========================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String cmd = "";
  for (int i = 0; i < length; i++) { cmd += (char)payload[i]; }
  cmd.trim(); cmd.toUpperCase();
  Serial.println("[MQTT] Perintah: " + cmd);

  // -- DEMPLOT 1 --
  if      (cmd == "D1_PUPUK_ON")   jalankanSistem("Pupuk Demplot 1", PIN_VALVE_PUPUK_D1, PIN_TRIFOO_D1, PIN_PERI_PUPUK_D1);
  else if (cmd == "D1_PUPUK_OFF")  matikanSistem("Pupuk Demplot 1", PIN_VALVE_PUPUK_D1, PIN_TRIFOO_D1, PIN_PERI_PUPUK_D1);
  else if (cmd == "D1_PESTI_ON")   jalankanSistem("Pestisida Demplot 1", PIN_VALVE_PESTI_D1, PIN_TRIFOO_D1, PIN_PERI_PESTI_D1);
  else if (cmd == "D1_PESTI_OFF")  matikanSistem("Pestisida Demplot 1", PIN_VALVE_PESTI_D1, PIN_TRIFOO_D1, PIN_PERI_PESTI_D1);

  // -- DEMPLOT 2 --
  else if (cmd == "D2_PUPUK_ON")   jalankanSistem("Pupuk Demplot 2", PIN_VALVE_PUPUK_D2, PIN_TRIFOO_D2, PIN_PERI_PUPUK_D2);
  else if (cmd == "D2_PUPUK_OFF")  matikanSistem("Pupuk Demplot 2", PIN_VALVE_PUPUK_D2, PIN_TRIFOO_D2, PIN_PERI_PUPUK_D2);
  else if (cmd == "D2_PESTI_ON")   jalankanSistem("Pestisida Demplot 2", PIN_VALVE_PESTI_D2, PIN_TRIFOO_D2, PIN_PERI_PESTI_D2);
  else if (cmd == "D2_PESTI_OFF")  matikanSistem("Pestisida Demplot 2", PIN_VALVE_PESTI_D2, PIN_TRIFOO_D2, PIN_PERI_PESTI_D2);

  // -- DEMPLOT 3 --
  else if (cmd == "D3_PUPUK_ON")   jalankanSistem("Pupuk Demplot 3", PIN_VALVE_PUPUK_D3, PIN_TRIFOO_D3, PIN_PERI_PUPUK_D3);
  else if (cmd == "D3_PUPUK_OFF")  matikanSistem("Pupuk Demplot 3", PIN_VALVE_PUPUK_D3, PIN_TRIFOO_D3, PIN_PERI_PUPUK_D3);
  else if (cmd == "D3_PESTI_ON")   jalankanSistem("Pestisida Demplot 3", PIN_VALVE_PESTI_D3, PIN_TRIFOO_D3, PIN_PERI_PESTI_D3);
  else if (cmd == "D3_PESTI_OFF")  matikanSistem("Pestisida Demplot 3", PIN_VALVE_PESTI_D3, PIN_TRIFOO_D3, PIN_PERI_PESTI_D3);

  // -- EMERGENCY --
  else if (cmd == "ALL_OFF")       matikanSemuaTotal();
}

void loop() {
  if (!client.connected()) reconnect();
  client.loop();
}