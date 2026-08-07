#include <Wire.h>
#include <BH1750.h>
#include <Adafruit_SHT31.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// --- Configuration ---
const char* ssid = "AGRI-MOTION";
const char* password = "agri1234";

const char* mqtt_server = "103.174.114.65"; 
const int mqtt_port = 1883;
const char* mqtt_client_id = "agrimotion-node-1a";
const char* mqtt_topic_pub = "agrimotion/device/node-1a/telemetry";

const unsigned long PUBLISH_INTERVAL = 10000;    // 10 detik
const unsigned long MODBUS_WINDOW = 400;         // Waktu tunggu respon sensor RS485

// --- Hardware Pins ---
// 1. MAX485 (RS485 NPK)
#define RE_DE_PIN 4   // Baris KUNING P4
#define RX2_PIN 16    // Baris KUNING P16 (RO MAX485)
#define TX2_PIN 17    // Baris KUNING P17 (DI MAX485)

// 2. I2C Bus 1 (BH1750)
#define I2C_SDA_1 21  // Baris KUNING P21
#define I2C_SCL_1 22  // Baris KUNING R22

// 3. I2C Bus 2 (SHT3x)
#define I2C_SDA_2 25  // Baris KUNING P25
#define I2C_SCL_2 26  // Baris KUNING P26

// 4. Capacitive Soil Moisture
#define SOIL_PIN 34   

// --- Sensor Calibration ---
const int SOIL_DRY = 3500;  // Nilai analog saat ditaruh di udara terbuka
const int SOIL_WET = 1200;  // Nilai analog saat dicelupkan ke air

// --- Objects ---
WiFiClient espClient;
PubSubClient mqttClient(espClient);
BH1750 lightMeter;
TwoWire I2C_SHT = TwoWire(1);
Adafruit_SHT31 sht31 = Adafruit_SHT31(&I2C_SHT);

bool isBH1750Ready = false;
bool isSHT31Ready = false;

// Frame Request NPK 7-in-1 Modbus
byte npkRequestFrame[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x07, 0x04, 0x08};

enum ModbusState { IDLE, WAITING_RESPONSE };
ModbusState modbusState = IDLE;

unsigned long lastPublishTime = 0;
unsigned long modbusRequestTime = 0;

// --- Variabel Data ---
float currentLux = 0.0;
float airTemp = 0.0;
float airHum = 0.0;
float analogSoilMoist = 0.0;
float rs485SoilMoist = 0.0; 
float rs485SoilTemp = 0.0; 
uint16_t currentEC = 0; 
float currentPH = 0.0; 
uint16_t currentN = 0; 
uint16_t currentP = 0; 
uint16_t currentK = 0; 

void publishData() {
  if (isBH1750Ready && lightMeter.measurementReady()) {
    currentLux = lightMeter.readLightLevel();
  }

  if (isSHT31Ready) {
    airTemp = sht31.readTemperature();
    airHum = sht31.readHumidity();
  }

  int rawAnalog = analogRead(SOIL_PIN);
  analogSoilMoist = map(rawAnalog, SOIL_DRY, SOIL_WET, 0, 100);
  if(analogSoilMoist < 0) analogSoilMoist = 0;
  if(analogSoilMoist > 100) analogSoilMoist = 100;

  float finalSoilMoisture = (rs485SoilMoist > 0) ? rs485SoilMoist : analogSoilMoist;

  StaticJsonDocument<256> doc; 
  doc["deviceId"] = "node-1a";
  doc["temperature"] = airTemp;
  doc["humidity"] = airHum;
  doc["soilMoisture"] = finalSoilMoisture;
  doc["ph"] = currentPH;
  doc["nitrogen"] = currentN;
  doc["phosphorus"] = currentP;
  doc["potassium"] = currentK;
  doc["lux"] = currentLux;

  char jsonBuffer[256];
  serializeJson(doc, jsonBuffer);

  Serial.println("\n--- DATA DEMPLOT AGRI-MOTION (ESP32 A) ---");
  Serial.print("Payload JSON: ");
  Serial.println(jsonBuffer);

  if (mqttClient.connected()) {
    mqttClient.publish(mqtt_topic_pub, jsonBuffer);
    Serial.println("[MQTT] Sukses terkirim ke VPS!");
  } else {
    Serial.println("[MQTT] Belum terhubung ke Broker.");
  }
}

void triggerModbusRead() {
  while(Serial2.available() > 0) Serial2.read(); // Bersihkan buffer

  digitalWrite(RE_DE_PIN, HIGH); 
  delay(10); 
  Serial2.write(npkRequestFrame, sizeof(npkRequestFrame));
  Serial2.flush(); 
  delayMicroseconds(500); 
  digitalWrite(RE_DE_PIN, LOW); 
  modbusState = WAITING_RESPONSE;
  modbusRequestTime = millis();
}

void processModbusResponse() {
  if (modbusState == WAITING_RESPONSE) {
    if (millis() - modbusRequestTime >= MODBUS_WINDOW) {
      int bytesAvailable = Serial2.available();
      if (bytesAvailable > 0) {
        byte responseBuffer[64];
        int len = 0;
        while (Serial2.available() && len < 64) {
          responseBuffer[len++] = Serial2.read();
        }

        if (len >= 19 && responseBuffer[0] == 0x01 && responseBuffer[1] == 0x03) {
          rs485SoilMoist = ((responseBuffer[3] << 8) | responseBuffer[4]) * 0.1;
          rs485SoilTemp = ((responseBuffer[5] << 8) | responseBuffer[6]) * 0.1;
          currentEC = ((responseBuffer[7] << 8) | responseBuffer[8]);
          currentPH = ((responseBuffer[9] << 8) | responseBuffer[10]) * 0.1;
          currentN = ((responseBuffer[11] << 8) | responseBuffer[12]);
          currentP = ((responseBuffer[13] << 8) | responseBuffer[14]);
          currentK = ((responseBuffer[15] << 8) | responseBuffer[16]);
        }
      } else {
        Serial.println("[Modbus] ⚠️ Timeout: Tidak ada balasan dari sensor NPK");
      }

      publishData();
      modbusState = IDLE;
      lastPublishTime = millis();
    }
  }
}

void setup() {
  pinMode(RE_DE_PIN, OUTPUT);
  digitalWrite(RE_DE_PIN, LOW);

  Serial.begin(115200);
  delay(2000); 

  Serial.println("\n=============================================");
  Serial.println(" AGRI-MOTION NODE 1A: BOOTING SYSTEM... ");
  Serial.println("=============================================");

  // Inisialisasi Modbus RS485
  Serial2.begin(4800, SERIAL_8N1, RX2_PIN, TX2_PIN);
  Serial.println("[System] Serial2 Modbus (4800 baud) OK!");

  // Inisialisasi I2C 1 (BH1750)
  Wire.begin(I2C_SDA_1, I2C_SCL_1);
  if (lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
    Serial.println("[BH1750] Sensor Cahaya OK!");
    isBH1750Ready = true;
  } else {
    Serial.println("[BH1750] Warning: Sensor Cahaya tidak terdeteksi!");
  }

  // Inisialisasi I2C 2 (SHT3x)
  I2C_SHT.begin(I2C_SDA_2, I2C_SCL_2);
  if (sht31.begin(0x44)) {
    Serial.println("[SHT31] Sensor Suhu Udara OK!");
    isSHT31Ready = true;
  } else {
    Serial.println("[SHT31] Warning: Sensor Suhu Udara tidak terdeteksi!");
  }

  // Inisialisasi Wi-Fi
  Serial.print("[Wi-Fi] Connecting to ");
  Serial.println(ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  // Set MQTT Server
  mqttClient.setServer(mqtt_server, mqtt_port);
  Serial.println("[System] Setup Selesai! Memulai loop operasional...\n");
  lastPublishTime = millis();
}

void loop() {
  // Pengelolaan Wi-Fi & MQTT 
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) {
      static unsigned long lastMqttRetry = 0;
      if (millis() - lastMqttRetry >= 5000) {
        lastMqttRetry = millis();
        Serial.print("[MQTT] Connecting to VPS...");
        if (mqttClient.connect(mqtt_client_id)) {
          Serial.println(" CONNECTED!");
        } else {
          Serial.print(" Failed, rc=");
          Serial.println(mqttClient.state());
        }
      }
    } else {
      mqttClient.loop();
    }
  }

  // Polling Sensor Rutin
  if (modbusState == IDLE) {
    if (millis() - lastPublishTime >= PUBLISH_INTERVAL) {
      triggerModbusRead();
    }
  } else {
    processModbusResponse();
  }
}