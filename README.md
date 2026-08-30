# Plant Surrogate Project — ViabilityNode

> **Author:** Jabez Rafael Abella

An autonomous, hyper-efficient microclimate profiling platform for evaluating plant viability prior to planting and monitoring existing specimen health in real time. The node captures high-fidelity environmental data, pushes it to a **Supabase** backend via a **Next.js** ingestion route, and calculates predictive stress indicators — **Vapor Pressure Deficit (VPD)** and **Daily Light Integral (DLI)**.

---

## Current Status

The current firmware is a validated sensor test that reads all four sensor sources and prints telemetry to the serial monitor every five seconds. The soil sensor is powered only during its reading window via GPIO D1 to eliminate parasitic drain. Low-power RTC deep sleep has not been implemented yet — the `delay()` loop is a placeholder for the upcoming 30-minute sleep cycle.

---

## Hardware Architecture

Built around a low-power **RISC-V** core on a strict energy-harvesting power chain.

### Microcontroller
- **Seeed Studio XIAO ESP32-C6** — Sub-20µA deep sleep current

### Power System
```
5V/1W Mini Solar Panel → SD05CRMA MPPT Module → 1S BMS → 3200mAh 18650 Li-Ion Cell
```

### Sensors

| Sensor | Interface | Measurement |
|---|---|---|
| VEML7700 | I2C | 16-bit digital ambient light |
| AHT20 | I2C | Temperature & relative humidity *(SMD power LED desoldered)* |
| BMP280 | I2C | Barometric pressure |
| Capacitive Soil Moisture v1.2 | Analog (`A0`) | Soil saturation — GPIO `D1`-switched power |
| Battery Monitor | Analog (`A2` / `D2`) | Cell voltage via external 2:1 divider (2× 205 kΩ) → percentage |

### Pin Connections

| Function | Pin |
|---|---|
| I2C SDA | `D4` |
| I2C SCL | `D5` |
| Soil sensor power | `D1` |
| Soil sensor analog data | `A0` / `D0` |
| Battery voltage divider sense | `A2` / `D2` |

---

## Enclosure Design

Custom 3D-printed in **Fusion 360**, sliced in **OrcaSlicer** using UV-resistant **ASA or PETG**. Divided into four functional zones:

| Zone | Description |
|---|---|
| **The Core** | Vertical sealed chamber with internal sled stacking ESP32-C6, MPPT, and BMS to minimize X/Y footprint |
| **The Roof** | 15°-angled top lid with solar panel and flush acrylic window for the VEML7700 |
| **Micro Stevenson Screen** | Downward-louvered external balcony for the AHT20+BMP280, isolated from battery thermal mass |
| **The Floor** | Integrated M12 thread + compression nut to seal the soil sensor cable (replaces PG7 gland) |

---

## Firmware Duty Cycle

To achieve **30-day autonomy**, the firmware operates on a strict deep-sleep cycle:

1. **Wake** — RTC timer boots the microcontroller
2. **Power Up** — GPIO `D1` set `HIGH` to power soil sensor
3. **Read** — I2C and analog sensors polled
4. **Power Down** — GPIO `D1` set `LOW` immediately after reading
5. **Transmit** — Telemetry serialized to JSON and POSTed to the backend
6. **Sleep** — Device enters deep sleep for **30 minutes**

---

## Project Setup

- **Board:** Seeed Studio XIAO ESP32-C6
- **Framework:** Arduino
- **Build system:** PlatformIO (`pioarduino` Espressif32 platform)
- **Serial speed:** `115200`

Libraries are declared in `platformio.ini` and installed automatically:

- `Adafruit VEML7700 Library`
- `Adafruit AHTX0`
- `Adafruit BMP280 Library`

---

## Build & Flash

```bash
# Build
"$HOME/.platformio/penv/bin/pio" run -e seeed_xiao_esp32c6

# Upload
"$HOME/.platformio/penv/bin/pio" run -e seeed_xiao_esp32c6 -t upload

# Specify port explicitly
"$HOME/.platformio/penv/bin/pio" run -e seeed_xiao_esp32c6 -t upload --upload-port /dev/cu.usbmodemXXXX

# Serial monitor
"$HOME/.platformio/penv/bin/pio" device monitor -e seeed_xiao_esp32c6
```

Expected serial output: initialization messages followed by soil moisture (raw), illuminance (lux), temperature (°C), humidity (%), and pressure (hPa). If a sensor is not detected, a wiring warning is printed and the firmware continues.

---

## Data Analytics Engine

Raw telemetry flows into **Supabase PostgreSQL** and is visualized on a **Next.js** frontend (Vercel) computing three predictive metrics:

| Metric | Method | Output |
|---|---|---|
| **Daily Light Integral (DLI)** | 24-hour illuminance aggregation | Location classification: low-light / tropical / high-light |
| **Soil Drainage Velocity** | `-ΔMoisture / ΔTime` post-saturation | Identifies hypoxic vs. rapid-draining zones |
| **Vapor Pressure Deficit (VPD)** | Atmospheric transpiration calculation | High VPD → dehydration risk · Low VPD → fungal danger |

---

## Main Files

- [`src/main.cpp`](src/main.cpp) — Sensor-node firmware
- [`platformio.ini`](platformio.ini) — Board, framework, platform, monitor, and library configuration
- [`plan.md`](plan.md) — Full technical specification and implementation roadmap

---

## Next Steps

- [ ] Replace `delay()` sleep simulation with ESP32-C6 RTC deep sleep (target: sub-20µA idle).
- [ ] Add ArduinoJson to serialize telemetry into a structured JSON payload.
- [ ] Initialize Wi-Fi only after sensor acquisition to avoid current spikes during reads.
- [ ] POST payload to the Next.js ingestion route and confirm Supabase insertion.
- [ ] Calibrate soil moisture sensor — map raw ADC values to a 0–100% saturation scale.
- [ ] Implement DLI, Soil Drainage Velocity, and VPD calculations in the analytics engine.
- [ ] Add sensor error handling for invalid or out-of-range readings.
- [x] Add external 2:1 voltage divider on `D2/A2` to sense 18650 cell voltage (2× 205 kΩ, ~10.24 µA parasitic drain).
- [x] Implement `readBatteryVoltage()` with 16-sample ADC averaging and `calculateBatteryPercentage()` with linear interpolation (3.30 V–4.20 V).
- [x] Add `battery_v` and `battery_pct` columns to the Supabase `telemetry` table.
