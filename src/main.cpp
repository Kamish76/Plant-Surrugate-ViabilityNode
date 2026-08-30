// ══════════════════════════════════════════════════════════════════════════════
//  Plant Surrogate — ViabilityNode Firmware
//  Author : Jabez Rafael Abella
//
//  Duty cycle (runs entirely inside setup(); loop() is never reached):
//    1. Boot
//    2. Read all sensors
//    3. Push reading into NVS offline queue (always succeeds)
//    4. Attempt WiFi connection (timeout-bounded, 10 s max)
//       ├─ Connected → flush entire NVS queue to backend, delete on ACK
//       └─ Failed    → leave queue intact, proceed to sleep
//    5. Disable WiFi radio
//    6. Enter deep sleep for SLEEP_INTERVAL_S seconds
//
//  Offline resilience:
//    NVS namespace "psq" holds up to QUEUE_MAX_SLOTS readings (48 = 24 h at
//    30-min intervals). When full, the oldest slot is overwritten so fresh
//    data is never dropped. Entries survive deep sleep and power cycles.
//    A reading is deleted from NVS only after the server returns HTTP 200.
// ══════════════════════════════════════════════════════════════════════════════

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>       // NVS wrapper (part of ESP32 Arduino core)
#include <ArduinoJson.h>
#include <Adafruit_VEML7700.h>
#include <Adafruit_AHTX0.h>
#include <Adafruit_BMP280.h>
#include "secrets.h"           // WIFI_SSID, WIFI_PASSWORD, BACKEND_URL, DEVICE_ID

// ── Pin Definitions ───────────────────────────────────────────────────────────
#define SDA_PIN          D4
#define SCL_PIN          D5
#define SOIL_POWER_PIN   D1    // GPIO switch for soil sensor power
#define SOIL_DATA_PIN    A0    // Capacitive soil moisture signal
#define BATTERY_PIN      D2    // Voltage divider sense tap (2:1, 2× 205 kΩ)

// ── Battery Voltage Divider ───────────────────────────────────────────────────
// Two matched 205 kΩ (SMD 0603, marked 30D) in series from BAT(+) to GND.
// Sense tap is the midpoint → pin voltage = battery_voltage / 2.
// Total impedance 410 kΩ → ~10 µA parasitic drain (< 0.25 mAh/day).
#define R1 205000.0F
#define R2 205000.0F

// ── Timing ────────────────────────────────────────────────────────────────────
#define SLEEP_INTERVAL_S   (30 * 60)   // 30 minutes in seconds
#define WIFI_TIMEOUT_MS    10000        // Max time to wait for WiFi (10 s)

// ── NVS Offline Queue ─────────────────────────────────────────────────────────
#define NVS_NAMESPACE      "psq"        // plant surrogate queue
#define QUEUE_MAX_SLOTS    48           // 48 readings x 30 min = 24 h of coverage

// ── Sensor Objects ────────────────────────────────────────────────────────────
Adafruit_VEML7700 veml = Adafruit_VEML7700();
Adafruit_AHTX0    aht;
Adafruit_BMP280   bmp;
Preferences       prefs;               // NVS handle

// ─────────────────────────────────────────────────────────────────────────────
//  Data Types
// ─────────────────────────────────────────────────────────────────────────────

struct SensorReading {
  float   illuminance_lux;
  float   temperature_c;
  float   humidity_rh;
  float   pressure_hpa;
  int     soil_moisture_raw;
  float   battery_v;
  int     battery_pct;
  // Uptime in seconds from boot (used for intra-flush ordering).
  // The server stamps recorded_at via DEFAULT NOW() — this is supplementary.
  unsigned long uptime_s;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Battery Reader
// ─────────────────────────────────────────────────────────────────────────────

// 16-sample rolling average suppresses RF-transmission ripple and ADC noise
// (no external filter capacitor on the divider).
float readBatteryVoltage() {
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  uint32_t rawSum = 0;
  for (int i = 0; i < 16; i++) {
    rawSum += analogRead(BATTERY_PIN);
    delay(1);
  }
  float rawAvg   = rawSum / 16.0F;
  float pinV     = (rawAvg / 4095.0F) * 3.3F;
  float batteryV = pinV * ((R1 + R2) / R2);
  return batteryV;
}

// Linear interpolation across the 1S discharge range (3.30 V - 4.20 V).
int calculateBatteryPercentage(float voltage) {
  if (voltage >= 4.20F) return 100;
  if (voltage <= 3.30F) return 0;
  int pct = (int)(((voltage - 3.30F) / (4.20F - 3.30F)) * 100.0F);
  return constrain(pct, 0, 100);
}

// ─────────────────────────────────────────────────────────────────────────────
//  JSON Serialiser
// ─────────────────────────────────────────────────────────────────────────────

// Serialise a SensorReading into a compact JSON string.
// Returns the number of characters written (0 = overflow — buffer too small).
size_t serialiseReading(const SensorReading& r, char* buf, size_t bufLen) {
  JsonDocument doc;
  doc["device_id"]         = DEVICE_ID;
  doc["illuminance_lux"]   = round(r.illuminance_lux   * 100) / 100.0;
  doc["temperature_c"]     = round(r.temperature_c     * 100) / 100.0;
  doc["humidity_rh"]       = round(r.humidity_rh       * 100) / 100.0;
  doc["pressure_hpa"]      = round(r.pressure_hpa      * 100) / 100.0;
  doc["soil_moisture_raw"] = r.soil_moisture_raw;
  doc["battery_v"]         = round(r.battery_v         * 100) / 100.0;
  doc["battery_pct"]       = r.battery_pct;
  doc["node_uptime_s"]     = r.uptime_s;
  return serializeJson(doc, buf, bufLen);
}

// ─────────────────────────────────────────────────────────────────────────────
//  NVS Queue  (namespace "psq")
//
//  Layout inside NVS:
//    "head"        uint32  — next write slot index (0-47, circular)
//    "count"       uint32  — number of entries currently stored
//    "r_00"-"r_47" string  — JSON blob per slot (absent key = empty slot)
// ─────────────────────────────────────────────────────────────────────────────

namespace NVSQueue {

  // Format a slot key like "r_07".
  void slotKey(uint32_t idx, char* out) {
    snprintf(out, 6, "r_%02u", idx % QUEUE_MAX_SLOTS);
  }

  // Persist a new reading into the circular NVS queue.
  void push(const SensorReading& reading) {
    prefs.begin(NVS_NAMESPACE, false);   // read-write

    uint32_t head  = prefs.getUInt("head",  0);
    uint32_t count = prefs.getUInt("count", 0);

    // Serialise reading to a JSON blob.
    char json[384];
    size_t written = serialiseReading(reading, json, sizeof(json));
    if (written == 0) {
      Serial.println("[NVS] ERROR: JSON too large for buffer — reading dropped.");
      prefs.end();
      return;
    }

    // Write to the current head slot (overwrites oldest when full).
    char key[6];
    slotKey(head, key);
    prefs.putString(key, json);

    // Advance head; if the queue was already full, count stays at MAX.
    head = (head + 1) % QUEUE_MAX_SLOTS;
    prefs.putUInt("head", head);
    if (count < QUEUE_MAX_SLOTS) {
      prefs.putUInt("count", count + 1);
    }

    Serial.printf("[NVS] Queued reading. Slots used: %u / %u\n",
                  (count < QUEUE_MAX_SLOTS) ? count + 1 : QUEUE_MAX_SLOTS,
                  (uint32_t)QUEUE_MAX_SLOTS);
    prefs.end();
  }

  // Attempt to POST every stored reading to the backend.
  // Each entry is deleted from NVS only after the server returns HTTP 200/201.
  // Returns the number of entries successfully delivered.
  int flush() {
    prefs.begin(NVS_NAMESPACE, false);

    uint32_t count = prefs.getUInt("count", 0);
    uint32_t head  = prefs.getUInt("head",  0);

    if (count == 0) {
      Serial.println("[NVS] Queue empty — nothing to flush.");
      prefs.end();
      return 0;
    }

    Serial.printf("[NVS] Flushing %u reading(s) to backend...\n", count);

    // The oldest slot is at (head - count + QUEUE_MAX_SLOTS) % QUEUE_MAX_SLOTS.
    uint32_t tail = (head + QUEUE_MAX_SLOTS - count) % QUEUE_MAX_SLOTS;

    int delivered = 0;
    HTTPClient http;
    http.setTimeout(8000);   // 8 s per POST — generous but bounded

    for (uint32_t i = 0; i < count; i++) {
      uint32_t slot = (tail + i) % QUEUE_MAX_SLOTS;
      char key[6];
      slotKey(slot, key);

      String json = prefs.getString(key, "");
      if (json.isEmpty()) continue;   // slot already cleared

      Serial.printf("[NVS] POST slot %u\n", slot);

      http.begin(BACKEND_URL);
      http.addHeader("Content-Type", "application/json");

      int httpCode = http.POST(json);
      http.end();

      if (httpCode == 200 || httpCode == 201) {
        // Confirmed delivery — remove from NVS.
        prefs.remove(key);
        delivered++;
        Serial.printf("[NVS] Slot %u delivered (HTTP %d). Deleted.\n",
                      slot, httpCode);
      } else {
        // Non-2xx or network error — leave slot intact for next cycle.
        Serial.printf("[NVS] Slot %u failed (HTTP %d). Will retry next cycle.\n",
                      slot, httpCode);
      }
    }

    // Recompute count and re-anchor head to reflect remaining entries.
    uint32_t remaining = 0;
    uint32_t newHead   = 0;
    for (uint32_t s = 0; s < QUEUE_MAX_SLOTS; s++) {
      char k[6];
      slotKey(s, k);
      if (prefs.getString(k, "").length() > 0) {
        newHead = (s + 1) % QUEUE_MAX_SLOTS;
        remaining++;
      }
    }
    prefs.putUInt("count", remaining);
    prefs.putUInt("head",  newHead);

    Serial.printf("[NVS] Flush complete. Delivered: %d. Remaining: %u.\n",
                  delivered, remaining);
    prefs.end();
    return delivered;
  }

}  // namespace NVSQueue

// ─────────────────────────────────────────────────────────────────────────────
//  WiFi Helper
// ─────────────────────────────────────────────────────────────────────────────

// Attempt connection, blocking for up to WIFI_TIMEOUT_MS.
// Returns true on success.
bool connectWiFi() {
  Serial.printf("[WiFi] Connecting to \"%s\"...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - t0 >= WIFI_TIMEOUT_MS) {
      Serial.println("[WiFi] Timeout — offline. Data queued for next cycle.");
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      return false;
    }
    delay(200);
  }

  Serial.printf("[WiFi] Connected. IP: %s\n",
                WiFi.localIP().toString().c_str());
  return true;
}

void disableWiFi() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.println("[WiFi] Radio disabled.");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Sensor Acquisition
// ─────────────────────────────────────────────────────────────────────────────

SensorReading acquireSensors() {
  SensorReading r = {};

  Serial.println("[Sensors] Acquiring readings...");

  // ── Soil Moisture ─────────────────────────────────────────────────────────
  // Brief power pulse: eliminate parasitic drain between readings.
  digitalWrite(SOIL_POWER_PIN, HIGH);
  delay(100);                              // stabilisation delay
  r.soil_moisture_raw = analogRead(SOIL_DATA_PIN);
  digitalWrite(SOIL_POWER_PIN, LOW);

  // ── VEML7700 (Ambient Light) ──────────────────────────────────────────────
  r.illuminance_lux = veml.readLux();

  // ── AHT20 (Temperature & Humidity) ───────────────────────────────────────
  sensors_event_t humEvt, tempEvt;
  aht.getEvent(&humEvt, &tempEvt);
  r.temperature_c = tempEvt.temperature;
  r.humidity_rh   = humEvt.relative_humidity;

  // ── BMP280 (Barometric Pressure) ─────────────────────────────────────────
  r.pressure_hpa = bmp.readPressure() / 100.0F;   // Pa -> hPa

  // ── Battery ───────────────────────────────────────────────────────────────
  r.battery_v   = readBatteryVoltage();
  r.battery_pct = calculateBatteryPercentage(r.battery_v);

  // ── Boot Uptime ───────────────────────────────────────────────────────────
  r.uptime_s = millis() / 1000UL;

  // ── Serial Diagnostic ─────────────────────────────────────────────────────
  Serial.printf("  Soil Moisture (Raw): %d\n",     r.soil_moisture_raw);
  Serial.printf("  Illuminance (Lux):   %.2f\n",   r.illuminance_lux);
  Serial.printf("  Temperature (C):     %.2f\n",   r.temperature_c);
  Serial.printf("  Humidity (%%RH):      %.2f\n",  r.humidity_rh);
  Serial.printf("  Pressure (hPa):      %.2f\n",   r.pressure_hpa);
  Serial.printf("  Battery Voltage (V): %.2f\n",   r.battery_v);
  Serial.printf("  Battery Level (%%):   %d\n",    r.battery_pct);

  return r;
}

// ─────────────────────────────────────────────────────────────────────────────
//  setup() — Full duty-cycle entry point
//  (loop() is never reached; the device sleeps and reboots.)
// ─────────────────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(200);   // allow USB-Serial to enumerate before printing

  Serial.println("\n======================================");
  Serial.println("  Plant Surrogate -- ViabilityNode Boot");
  Serial.println("======================================");

  // ── I2C Bus Init ──────────────────────────────────────────────────────────
  Wire.begin(SDA_PIN, SCL_PIN);

  // ── Soil Sensor Power Pin ─────────────────────────────────────────────────
  pinMode(SOIL_POWER_PIN, OUTPUT);
  digitalWrite(SOIL_POWER_PIN, LOW);   // off until acquisition

  // ── Sensor Init ───────────────────────────────────────────────────────────
  // Non-fatal: if a sensor is missing we still proceed with zeroed values
  // so the queue push and sleep cycle remain uninterrupted.
  if (!veml.begin()) Serial.println("[WARN] VEML7700 not found.");
  if (!aht.begin())  Serial.println("[WARN] AHT20 not found.");
  if (!bmp.begin())  Serial.println("[WARN] BMP280 not found.");

  // ─────────────────────────────────────────────────────────────────────────
  //  STEP 1: Read all sensors
  // ─────────────────────────────────────────────────────────────────────────
  SensorReading reading = acquireSensors();

  // ─────────────────────────────────────────────────────────────────────────
  //  STEP 2: Push to NVS offline queue (always — regardless of connectivity)
  // ─────────────────────────────────────────────────────────────────────────
  NVSQueue::push(reading);

  // ─────────────────────────────────────────────────────────────────────────
  //  STEP 3: Attempt WiFi connection and flush the queue
  // ─────────────────────────────────────────────────────────────────────────
  if (connectWiFi()) {
    NVSQueue::flush();
    disableWiFi();
  }

  // ─────────────────────────────────────────────────────────────────────────
  //  STEP 4: Deep sleep until next interval
  // ─────────────────────────────────────────────────────────────────────────
  uint64_t sleepUs = (uint64_t)SLEEP_INTERVAL_S * 1000000ULL;
  Serial.printf("\n[Sleep] Entering deep sleep for %d min...\n",
                SLEEP_INTERVAL_S / 60);
  Serial.flush();

  esp_sleep_enable_timer_wakeup(sleepUs);
  esp_deep_sleep_start();

  // Unreachable — deep sleep reboots the chip.
}

// ─────────────────────────────────────────────────────────────────────────────
//  loop() — intentionally empty; all work happens in setup()
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  // Never reached. The device deep-sleeps at the end of setup() and reboots
  // on the next RTC wake event, running setup() again from scratch.
}
