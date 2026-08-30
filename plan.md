# Plant Surrogate Project — ViabilityNode

> **Author:** Jabez Rafael Abella

## Overview

The Plant Surrogate Project is an autonomous, hyper-efficient microclimate profiling platform designed to evaluate plant viability prior to planting and monitor existing specimen health in real time. By capturing high-fidelity environmental data and pushing it to a Supabase backend via a Next.js ingestion route, the node calculates predictive stress indicators like **Vapor Pressure Deficit (VPD)** and **Daily Light Integral (DLI)**.

---

## Hardware Architecture

The system is built around a low-power RISC-V core and a strict energy-harvesting architecture.

### Microcontroller
- **Seeed Studio XIAO ESP32-C6** — Sub-20µA deep sleep current.

### Power System
```
5V/1W Mini Solar Panel → SD05CRMA MPPT Module → 1S BMS → 3200mAh 18650 Li-Ion Cell
```

### Sensors

| Sensor | Interface | Measurement |
|---|---|---|
| VEML7700 | I2C | 16-bit digital ambient light |
| AHT20 | I2C | Temperature & relative humidity *(SMD power LED desoldered to eliminate parasitic drain)* |
| BMP280 | I2C | Barometric pressure |
| Capacitive Soil Moisture v1.2 | Analog (A0) | Soil saturation — powered dynamically via GPIO D1 to prevent parasitic drain between readings |

---

## Ultra-Compact Enclosure Design

The hardware is housed in a custom 3D-printed enclosure designed in **Fusion 360**, sliced in **OrcaSlicer** using UV-resistant **ASA or PETG**. The design prioritizes a minimal footprint while protecting electronics from heavy Eastern Visayas rain and maintaining sensor accuracy.

### The Core (Vertical Stacking)
The main sealed chamber groups the electronics vertically along the axis of the 18650 battery. A custom internal sled mounts the ESP32-C6, MPPT, and BMS back-to-back, drastically reducing the X/Y footprint.

### The Roof (Solar & Light)
The top lid is angled at **15°** to shed water and houses the solar panel. A flush-mounted clear acrylic window gives the VEML7700 an unobstructed view of the sky to prevent shadow-clipping.

### Micro Stevenson Screen (Climate)
A single-sided, downward-louvered balcony integrated into the exterior wall houses the AHT20+BMP280 module. This isolates the sensors from the thermal mass of the battery and greenhouse-heating of the main box, allowing the ambient airflow required for accurate VPD calculation.

### The Floor (Data Out)
Instead of a bulky off-the-shelf PG7 gland, the bottom plate features an **integrated M12 thread** and a custom compression nut to cleanly seal the capacitive soil moisture sensor cable.

---

## Firmware & Operational Duty Cycle

To achieve **30-day autonomy**, the ESP32-C6 firmware bypasses continuous `delay()` loops in favor of a strict deep-sleep duty cycle:

1. **Wake** — RTC timer boots the microcontroller.
2. **Power Up** — GPIO D1 is set `HIGH` to supply power to the analog soil sensor.
3. **Read** — I2C and analog sensors are polled.
4. **Power Down** — GPIO D1 is set `LOW` immediately after reading.
5. **Transmit** — Data is packaged into a JSON payload and POSTed to the backend.
6. **Sleep** — Device enters deep sleep for **30 minutes**.

### Base Hardware Test Code (`main.cpp`)

Validates hardware wiring and simulates the wake/sleep sensor polling sequence:

```cpp
#include <Wire.h>
#include <Adafruit_VEML7700.h>
#include <Adafruit_AHTX0.h>
#include <Adafruit_BMP280.h>

// Pin Definitions based on the Technical Specification
#define SDA_PIN D4
#define SCL_PIN D5
#define SOIL_POWER_PIN D1
#define SOIL_DATA_PIN A0

// Sensor Objects
Adafruit_VEML7700 veml = Adafruit_VEML7700();
Adafruit_AHTX0 aht;
Adafruit_BMP280 bmp;

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  Serial.println("\n--- Plant Viability Sensor Node Test ---");

  // Configure the Soil Sensor Power Pin
  pinMode(SOIL_POWER_PIN, OUTPUT);
  digitalWrite(SOIL_POWER_PIN, LOW); // Keep it off initially

  // Initialize I2C with the specific XIAO ESP32-C6 pins
  Wire.begin(SDA_PIN, SCL_PIN);

  // Initialize VEML7700
  if (!veml.begin()) {
    Serial.println("VEML7700 not found! Check wiring.");
  } else {
    Serial.println("VEML7700 initialized.");
  }

  // Initialize AHT20
  if (!aht.begin()) {
    Serial.println("AHT20 not found! Check wiring.");
  } else {
    Serial.println("AHT20 initialized.");
  }

  // Initialize BMP280
  if (!bmp.begin()) {
    Serial.println("BMP280 not found! Check wiring.");
  } else {
    Serial.println("BMP280 initialized.");
  }
}

void loop() {
  Serial.println("\n[Waking Sensors...]");

  // 1. Power up the analog soil sensor
  digitalWrite(SOIL_POWER_PIN, HIGH);
  delay(100); // Give the sensor a moment to stabilize

  // 2. Read Soil Moisture
  int soilMoistureRaw = analogRead(SOIL_DATA_PIN);

  // 3. Power down the soil sensor to save battery
  digitalWrite(SOIL_POWER_PIN, LOW);

  // 4. Read VEML7700 (Light)
  float lux = veml.readLux();

  // 5. Read AHT20 (Temp & Humidity)
  sensors_event_t humidity, temp;
  aht.getEvent(&humidity, &temp);

  // 6. Read BMP280 (Pressure)
  float pressure = bmp.readPressure() / 100.0F; // Convert Pa to hPa

  // 7. Output Data to Serial Monitor
  Serial.print("Soil Moisture (Raw): "); Serial.println(soilMoistureRaw);
  Serial.print("Illuminance (Lux):   "); Serial.println(lux);
  Serial.print("Temperature (C):     "); Serial.println(temp.temperature);
  Serial.print("Humidity (%):        "); Serial.println(humidity.relative_humidity);
  Serial.print("Pressure (hPa):      "); Serial.println(pressure);

  Serial.println("[Entering Sleep Simulation...]");
  delay(5000); // Wait 5 seconds before the next test loop
}
```

---

## Data Analytics Engine

Raw telemetry is processed via a **Supabase PostgreSQL** database and visualized on a **Next.js** frontend (deployed on Vercel) to calculate three primary predictive metrics:

### Daily Light Integral (DLI)
Aggregates 24-hour illuminance to classify the location for low-light foliage, general tropicals, or high-light plants.

### Soil Drainage Velocity
Calculates the moisture drop rate (`-ΔMoisture / ΔTime`) post-saturation to identify hypoxic, slow-draining soil versus rapid-draining zones.

### Vapor Pressure Deficit (VPD)
Measures atmospheric transpiration potential to issue early warnings for:
- **High VPD** → Dehydration risk
- **Low VPD** → Fungal stagnation danger

---

# Phase 1: Firmware Evolution

## Objective

Replace the continuous `delay()` loop with the ESP32 RTC wake cycle to achieve a deep-sleep current below 20 µA and support 30 days of battery autonomy.

## Sensor Duty Cycle

On each wake cycle:

1. Boot the microcontroller.
2. Set `D1` HIGH to power the capacitive soil sensor.
3. Allow the sensor to stabilize, then read all I2C and analog sensors.
4. Set `D1` LOW immediately after the soil reading to eliminate parasitic drain.

## JSON Payload

Add the ArduinoJson library and serialize the telemetry into a structured payload containing:

- Illuminance
- Air temperature
- Relative humidity
- Barometric pressure
- Soil saturation

## Radio Power Management

Wi-Fi wake-up can produce transient current spikes of up to 350 mA. Keep Wi-Fi disabled during sensor acquisition. Initialize it only when the device is ready to POST the JSON payload, then immediately call `esp_deep_sleep_start()` after the request completes.