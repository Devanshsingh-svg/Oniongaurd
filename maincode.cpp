#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <DHT.h>
#include <SPIFFS.h>
#include <esp_task_wdt.h>
#include <time.h>
#include <cmath>
#include <vector>

// ==========================================
// HARDWARE PIN DEFINITIONS & RELAY LOGIC
// ==========================================
#define DHTPIN_PRIMARY   4
#define DHTPIN_BACKUP    16
#define DHTTYPE          DHT22
#define MQ135_PIN        34

#define RELAY_FAN        25
#define RELAY_CHILLER    26
#define RELAY_DAMPER     27

#define RELAY_ON         LOW
#define RELAY_OFF        HIGH

#define TWDT_TIMEOUT_MS  15000

// ==========================================
// THREAD-SAFE DATA STRUCTURES
// ==========================================
struct TelemetryData {
  float temp;
  float humidity;
  int co2Ppm;
  float heatStressScore;
  float tempRateOfChange;
  time_t timestamp;
};

struct SystemThresholds {
  float tempMax;
  float tempMin;
  int co2Max;
  int co2Min;
};

// ==========================================
// FREERTOS MEMORY ALLOCATION (Dynamic)
// ==========================================
TaskHandle_t xSensorTaskHandle;
TaskHandle_t xNetworkTaskHandle;

QueueHandle_t     telemetryQueue;
SemaphoreHandle_t thresholdMutex;

// ==========================================
// SYSTEM OBJECTS & CONSTANTS
// ==========================================
Preferences preferences;
DHT dhtPrimary(DHTPIN_PRIMARY, DHTTYPE);
DHT dhtBackup(DHTPIN_BACKUP, DHTTYPE);

WiFiClient espClient;
PubSubClient mqtt(espClient);

const char* ssid            = "Wokwi-GUEST";
const char* password        = "";
const char* mqtt_server     = "broker.hivemq.com";
const char* telemetry_topic = "onion_storage/room_1/telemetry";
const char* config_topic    = "onion_storage/room_1/config";
const char* CACHE_FILE      = "/offline_telemetry.csv";

// NTP Server Configuration
const char* ntpServer       = "pool.ntp.org";
const long  gmtOffset_sec   = 19800;
const int   daylightOffset  = 0;

SystemThresholds activeThresholds;

bool isStorageReady = false;

// Chiller Short-Cycle Safeguard
const uint32_t CHILLER_SHORT_CYCLE_MS = 10000;
uint32_t lastChillerToggleTime = 0;

// Predictive Rate-of-Change & Stress State
float accumulatedHeatStress = 0.0;
float tempHistory[5]        = {25.0, 25.0, 25.0, 25.0, 25.0};
uint8_t tempHistIndex       = 0;

// Delta-Encoding Last Values
float lastPubTemp = -999.0;
float lastPubHum  = -999.0;
int   lastPubCO2  = -999;
uint32_t lastHeartbeatTime = 0;
const uint32_t HEARTBEAT_INTERVAL_MS = 300000;

// ==========================================
// 1. SENSOR FILTERING & HARDWARE BOUNDS GUARD
// ==========================================
int getSmoothedCO2() {
  static const int SAMPLES = 10;
  static int readings[SAMPLES] = {0};
  static int readIndex = 0;
  static long total = 0;
  static int sampleCount = 0;

  total -= readings[readIndex];
  readings[readIndex] = analogRead(MQ135_PIN);
  total += readings[readIndex];
  readIndex = (readIndex + 1) % SAMPLES;
  if (sampleCount < SAMPLES) {
    sampleCount++;
  }

  return map(total / sampleCount, 0, 4095, 400, 2000);
}

bool isPhysicallyRealistic(float t) {
  return (t >= -10.0f && t <= 70.0f);
}

float getValidatedTemperature() {
  float primary = dhtPrimary.readTemperature();
  float backup  = dhtBackup.readTemperature();
  
  // Allow sensor time to recover between reads
  delay(100);

  bool primaryValid = !isnan(primary) && isPhysicallyRealistic(primary);
  bool backupValid  = !isnan(backup)  && isPhysicallyRealistic(backup);

  if (primaryValid && backupValid) {
    if (fabs(primary - backup) > 3.0f) {
      Serial.printf("[SENSOR FAULT] Divergence! Pri: %.1f°C | Bak: %.1f°C\n", primary, backup);
      return primary;
    }
    return (primary + backup) / 2.0f;
  }

  if (primaryValid) return primary;
  if (backupValid)  return backup;

  Serial.println("[HARDWARE FAULT] Sensor disconnected or hardware damaged!");
  return -999.0f;
}

// ==========================================
// 2. PREDICTIVE RATE-OF-CHANGE (dT/dt)
// ==========================================
float calculateTempRateOfChange(float currentTemp) {
  static bool initialized = false;

  if (!initialized) {
    for (int i = 0; i < 5; i++) {
      tempHistory[i] = currentTemp;
    }
    initialized = true;
    return 0.0f;
  }

  tempHistory[tempHistIndex] = currentTemp;
  tempHistIndex = (tempHistIndex + 1) % 5;

  float oldestTemp = tempHistory[tempHistIndex];
  float deltaTemp  = currentTemp - oldestTemp;

  return deltaTemp * 6.0f; 
}

// ==========================================
// 3. NTP REAL-WORLD TIMESTAMPING
// ==========================================
time_t getEpochTime() {
  time_t now;
  time(&now);
  return now;
}

// ==========================================
// STORE-AND-FORWARD (SPIFFS FLASH)
// ==========================================
void cacheTelemetryOffline(const TelemetryData& data) {
  if (!isStorageReady) return;

  File file = SPIFFS.open(CACHE_FILE, FILE_APPEND);
  if (file) {
    int written = file.printf("%lld,%.1f,%.1f,%d,%.2f\n", 
                (long long)data.timestamp, data.temp, data.humidity, data.co2Ppm, data.heatStressScore);
    file.close();
    if (written > 0) {
      Serial.println("[OFFLINE CACHE] Saved timestamped record to SPIFFS.");
    } else {
      Serial.println("[OFFLINE CACHE ERROR] Failed to write to SPIFFS!");
    }
  } else {
    Serial.println("[OFFLINE CACHE ERROR] Could not open cache file!");
  }
}

void flushOfflineBufferToCloud() {
  if (!isStorageReady || !SPIFFS.exists(CACHE_FILE)) return;

  File file = SPIFFS.open(CACHE_FILE, FILE_READ);
  if (!file) return;

  Serial.println("[STORE & FORWARD] Flushing timestamped backlog to Cloud...");
  
  const char* TEMP_CACHE_FILE = "/temp_backlog.csv";
  int linesFlushed = 0;
  bool publishFailed = false;
  File tempFile;

  while (file.available()) {
    esp_task_wdt_reset(); // Prevent watchdog timeout during long backlog syncs
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    if (!publishFailed) {
      if (mqtt.publish("onion_storage/room_1/backlog", line.c_str())) {
        linesFlushed++;
        vTaskDelay(pdMS_TO_TICKS(50));
        continue;
      } else {
        Serial.println("[STORE & FORWARD ERROR] Publish failed, saving remaining backlog.");
        publishFailed = true;
        tempFile = SPIFFS.open(TEMP_CACHE_FILE, FILE_WRITE);
      }
    }

    // Save un-flushed records to temp file if publish failed
    if (tempFile) {
      tempFile.println(line);
    }
  }
  file.close();
  if (tempFile) {
    tempFile.close();
  }

  if (publishFailed) {
    SPIFFS.remove(CACHE_FILE);
    if (SPIFFS.exists(TEMP_CACHE_FILE)) {
      SPIFFS.rename(TEMP_CACHE_FILE, CACHE_FILE);
    }
  } else {
    SPIFFS.remove(CACHE_FILE);
    if (SPIFFS.exists(TEMP_CACHE_FILE)) {
      SPIFFS.remove(TEMP_CACHE_FILE);
    }
  }

  Serial.printf("[STORE & FORWARD] Backlog sync status: %d records flushed.\n", linesFlushed);
}

// ==========================================
// CORE 1: SENSOR, SAFETY & PREDICTIVE TASK
// ==========================================
void TaskSensorsAndControl(void *pvParameters) {
  esp_task_wdt_add(NULL);

  TickType_t xLastWakeTime = xTaskGetTickCount();
  const TickType_t xFrequency = pdMS_TO_TICKS(2000);

  static bool fanState = false;

  for (;;) {
    vTaskDelayUntil(&xLastWakeTime, xFrequency);
    esp_task_wdt_reset();

    float temp = getValidatedTemperature();
    float hum  = dhtPrimary.readHumidity();
    if (isnan(hum)) {
      hum = dhtBackup.readHumidity(); // Try backup sensor
    }

    if (temp == -999.0f) {
      continue;
    }

    if (isnan(hum)) {
      hum = 65.0f; // Safe neutral fallback if both sensors fail humidity read
    }

    int co2 = getSmoothedCO2();

    // Continuous Warning Alerts
    if (temp > 35.0f) {
      Serial.printf("[WARNING ALERT] EXTREME TEMPERATURE DETECTED: %.1f°C! (Max Safe: 30°C)\n", temp);
    }
    if (hum > 75.0f) {
      Serial.printf("[WARNING ALERT] HIGH HUMIDITY DETECTED: %.1f%%! (Risk of rot/sprouting, Target: 65-70%%)\n", hum);
    }

    float rateOfChange = calculateTempRateOfChange(temp);

    if (temp > 30.0f) {
      accumulatedHeatStress += (temp - 30.0f) * (2.0f / 3600.0f);
    } else if (accumulatedHeatStress > 0.0f) {
      accumulatedHeatStress = fmax(0.0f, accumulatedHeatStress - 0.05f);
    }

    SystemThresholds currentCfg;
    if (xSemaphoreTake(thresholdMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      currentCfg = activeThresholds;
      xSemaphoreGive(thresholdMutex);
    } else {
      continue;
    }

    // ==========================================
    // REFRIGERATION CONTROL (CHILLER) - TEMPERATURE ONLY
    // ==========================================
    bool predictiveTempTrigger = (temp > currentCfg.tempMax) || 
                                  (temp > (currentCfg.tempMax - 1.0f) && rateOfChange > 1.5f);

    uint32_t now = millis();
    bool chillerCurrentState = (digitalRead(RELAY_CHILLER) == RELAY_ON);

    if (predictiveTempTrigger && !chillerCurrentState) {
      if (now - lastChillerToggleTime >= CHILLER_SHORT_CYCLE_MS) {
        digitalWrite(RELAY_CHILLER, RELAY_ON);
        lastChillerToggleTime = now;
        Serial.println("[RELAY] Chiller -> PREDICTIVE ACTIVATION ON");
      }
    } else if (temp <= currentCfg.tempMin && chillerCurrentState) {
      if (now - lastChillerToggleTime >= CHILLER_SHORT_CYCLE_MS) {
        digitalWrite(RELAY_CHILLER, RELAY_OFF);
        lastChillerToggleTime = now;
        Serial.println("[RELAY] Chiller -> OFF");
      }
    }

    // ==========================================
    // VENTILATION CONTROL (FAN & DAMPER) - CO2 & HUMIDITY ONLY
    // ==========================================
    bool co2High = (co2 > currentCfg.co2Max);
    bool humHigh = (hum > 72.0f);

    bool co2Safe = (co2 <= currentCfg.co2Min);
    bool humSafe = (hum <= 68.0f);

    if (co2High || humHigh) {
      if (!fanState) {
        fanState = true;
        Serial.printf("[RELAY] Fan ON -> Trigger: %s %s\n", 
                      humHigh ? "[HUMIDITY > 72%]" : "", 
                      co2High ? "[CO2 HIGH]" : "");
      }
    } else if (co2Safe && humSafe) {
      if (fanState) {
        fanState = false;
        Serial.println("[RELAY] Fan OFF -> Humidity & CO2 within safe range.");
      }
    }

    digitalWrite(RELAY_FAN, fanState ? RELAY_ON : RELAY_OFF);
    digitalWrite(RELAY_DAMPER, fanState ? RELAY_ON : RELAY_OFF);

    Serial.printf("[LOG] T:%.1f°C (dT/dt: %+.1f°C/m) | H:%.1f%% | CO2:%dppm | Fan:%s | Chiller:%s\n",
                  temp, rateOfChange, hum, co2,
                  fanState ? "ON" : "OFF",
                  (digitalRead(RELAY_CHILLER) == RELAY_ON) ? "ON" : "OFF");

    TelemetryData data = { temp, hum, co2, accumulatedHeatStress, rateOfChange, getEpochTime() };
    if (xQueueSend(telemetryQueue, &data, 0) != pdTRUE) {
      cacheTelemetryOffline(data);
    }
  }
}

// ==========================================
// CORE 0: NETWORK & DELTA-ENCODING TASK
// ==========================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  DynamicJsonDocument doc(512);
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.printf("[JSON] deserializeJson failed: %s\n", err.c_str());
    return;
  }

  if (!doc["tempMax"].isNull() || !doc["co2Max"].isNull()) {
    SystemThresholds newThresholds;
    
    // Copy current values first
    if (xSemaphoreTake(thresholdMutex, portMAX_DELAY) == pdTRUE) {
      newThresholds = activeThresholds;
      xSemaphoreGive(thresholdMutex);
    } else {
      return;
    }
    
    // Update with new values
    if (!doc["tempMax"].isNull()) newThresholds.tempMax = doc["tempMax"].as<float>();
    if (!doc["tempMin"].isNull()) newThresholds.tempMin = doc["tempMin"].as<float>();
    if (!doc["co2Max"].isNull())  newThresholds.co2Max  = doc["co2Max"].as<int>();
    if (!doc["co2Min"].isNull())  newThresholds.co2Min  = doc["co2Min"].as<int>();

    // Apply under mutex
    if (xSemaphoreTake(thresholdMutex, portMAX_DELAY) == pdTRUE) {
      activeThresholds = newThresholds;
      xSemaphoreGive(thresholdMutex);
    }
    
    // Write to NVS (outside mutex to avoid blocking)
    preferences.begin("onion_cfg", false);
    preferences.putFloat("tempMax", newThresholds.tempMax);
    preferences.putFloat("tempMin", newThresholds.tempMin);
    preferences.putInt("co2Max", newThresholds.co2Max);
    preferences.putInt("co2Min", newThresholds.co2Min);
    preferences.end();

    Serial.println("[NVS] Remote threshold settings updated.");
  }
}

void TaskNetwork(void *pvParameters) {
  esp_task_wdt_add(NULL);

  WiFi.begin(ssid, password);
  
  mqtt.setServer(mqtt_server, 1883);  // Non-TLS port 1883 with WiFiClient
  mqtt.setBufferSize(512);             // Support payloads up to 512 bytes
  mqtt.setCallback(mqttCallback);

  TelemetryData rxData;
  bool wasDisconnected = true;

  for (;;) {
    esp_task_wdt_reset();

    if (WiFi.status() != WL_CONNECTED) {
      wasDisconnected = true;
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    static bool ntpSynced = false;
    if (!ntpSynced) {
      configTime(gmtOffset_sec, daylightOffset, ntpServer);
      ntpSynced = true;
      Serial.println("[NTP] Time synchronized.");
    }

    if (!mqtt.connected()) {
      wasDisconnected = true;
      String clientId = "ESP32-Onion-" + String(random(0x10000), HEX);
      if (mqtt.connect(clientId.c_str())) {
        mqtt.subscribe(config_topic);
        Serial.println("[NETWORK] MQTT Connected!");
      } else {
        Serial.printf("[NETWORK] MQTT connect failed, rc=%d\n", mqtt.state());
        vTaskDelay(pdMS_TO_TICKS(3000));
        continue;
      }
    }

    mqtt.loop();

    if (wasDisconnected) {
      flushOfflineBufferToCloud();
      wasDisconnected = false;
    }

    if (xQueueReceive(telemetryQueue, &rxData, pdMS_TO_TICKS(100)) == pdTRUE) {
      uint32_t now = millis();
      
      bool tempShifted = fabs(rxData.temp - lastPubTemp) >= 0.3f;
      bool humShifted  = fabs(rxData.humidity - lastPubHum) >= 1.0f;
      bool co2Shifted  = abs(rxData.co2Ppm - lastPubCO2) >= 25;
      bool heartbeat   = (now - lastHeartbeatTime >= HEARTBEAT_INTERVAL_MS);

      if (tempShifted || humShifted || co2Shifted || heartbeat) {
        char jsonBuf[200];
        snprintf(jsonBuf, sizeof(jsonBuf), 
                 "{\"ts\":%lld,\"t\":%.1f,\"h\":%.1f,\"c\":%d,\"roc\":%.1f,\"s\":%.2f}", 
                 (long long)rxData.timestamp, rxData.temp, rxData.humidity, rxData.co2Ppm, 
                 rxData.tempRateOfChange, rxData.heatStressScore);

        if (mqtt.publish(telemetry_topic, jsonBuf)) {
          lastPubTemp = rxData.temp;
          lastPubHum  = rxData.humidity;
          lastPubCO2  = rxData.co2Ppm;
          lastHeartbeatTime = now;
          Serial.println("[MQTT] Delta-Encoded telemetry published!");
        } else {
          Serial.println("[MQTT ERROR] Publish failed, caching offline.");
          cacheTelemetryOffline(rxData);
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ==========================================
// SYSTEM INITIALIZATION
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== ESP32 Onion Storage Industrial Engine Starting ===");
  Serial.printf("[SYSTEM] CPU Clock: %d MHz\n", getCpuFrequencyMhz());

  dhtPrimary.begin();
  dhtBackup.begin();

  if (!SPIFFS.begin(true)) {  // Auto-format on fail if first boot
    Serial.println("[SYSTEM] SPIFFS Offline Flash Caching disabled.");
    isStorageReady = false;
  } else {
    Serial.println("[SYSTEM] SPIFFS Flash Ready.");
    isStorageReady = true;
  }

  pinMode(RELAY_FAN, OUTPUT);
  pinMode(RELAY_CHILLER, OUTPUT);
  pinMode(RELAY_DAMPER, OUTPUT);

  digitalWrite(RELAY_FAN, RELAY_OFF);
  digitalWrite(RELAY_CHILLER, RELAY_OFF);
  digitalWrite(RELAY_DAMPER, RELAY_OFF);

  // Initialize Task Watchdog Timer with cross-ESP-IDF version compatibility
#if defined(ESP_IDF_VERSION_MAJOR) && ESP_IDF_VERSION_MAJOR >= 5
  esp_task_wdt_config_t twdt_config = {
    .timeout_ms = TWDT_TIMEOUT_MS,
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
    .trigger_panic = true
  };
  esp_task_wdt_init(&twdt_config);
#else
  esp_task_wdt_init(TWDT_TIMEOUT_MS / 1000, true);
#endif
  Serial.println("[TWDT] Task Watchdog Timer initialized.");

  telemetryQueue = xQueueCreate(10, sizeof(TelemetryData));
  thresholdMutex = xSemaphoreCreateMutex();

  preferences.begin("onion_cfg", true);
  activeThresholds.tempMax = preferences.getFloat("tempMax", 30.0);
  activeThresholds.tempMin = preferences.getFloat("tempMin", 28.0);
  activeThresholds.co2Max  = preferences.getInt("co2Max", 1000);
  activeThresholds.co2Min  = preferences.getInt("co2Min", 800);
  preferences.end();
  
  Serial.printf("[CONFIG] Loaded thresholds: T[%.1f-%.1f] CO2[%d-%d]\n",
                activeThresholds.tempMax, activeThresholds.tempMin,
                activeThresholds.co2Max, activeThresholds.co2Min);

  xTaskCreatePinnedToCore(
    TaskSensorsAndControl,
    "SensorsTask",
    4096,
    NULL,
    2,
    &xSensorTaskHandle,
    1  // Core 1
  );
  Serial.println("[TASK] Sensor task created on Core 1.");

  xTaskCreatePinnedToCore(
    TaskNetwork,
    "NetworkTask",
    8192,
    NULL,
    1,
    &xNetworkTaskHandle,
    0  // Core 0
  );
  Serial.println("[TASK] Network task created on Core 0.");
}

void loop() {
  // Main task deleted - all work done in FreeRTOS tasks
  vTaskDelete(NULL);
}
