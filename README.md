# Plant Surrogate Sensor Node

A PlatformIO test project for a Seeed Studio XIAO ESP32-C6 plant viability sensor node.

## Current Status

The current firmware is a working sensor test. It reads:

- Soil moisture from an analog sensor
- Ambient light from a VEML7700
- Temperature and humidity from an AHT20
- Atmospheric pressure from a BMP280

Readings are printed to the serial monitor every five seconds. The soil sensor is powered only during its reading to reduce power usage. The current loop uses a five-second delay as a sleep simulation; low-power sleep has not been implemented yet.

## Hardware Connections

| Function | Pin |
| --- | --- |
| I2C SDA | `D4` |
| I2C SCL | `D5` |
| Soil sensor power | `D1` |
| Soil sensor analog data | `A0` |

The VEML7700, AHT20, and BMP280 share the I2C bus. Confirm sensor power, ground, and I2C addresses before connecting hardware.

## Project Setup

This project uses:

- Board: Seeed Studio XIAO ESP32-C6
- Framework: Arduino
- Build system: PlatformIO
- Serial speed: `115200`

The project uses the stable `pioarduino` Espressif32 platform package because the standard PlatformIO board definition does not currently enable Arduino for this board.

Libraries are declared in `platformio.ini` and are installed automatically by PlatformIO:

- Adafruit VEML7700 Library
- Adafruit AHTX0
- Adafruit BMP280 Library

## Build

From the project directory, run:

```bash
"$HOME/.platformio/penv/bin/pio" run -e seeed_xiao_esp32c6
```

## Upload

Connect the XIAO ESP32-C6 over USB, then run:

```bash
"$HOME/.platformio/penv/bin/pio" run -e seeed_xiao_esp32c6 -t upload
```

To specify a port explicitly:

```bash
"$HOME/.platformio/penv/bin/pio" run -e seeed_xiao_esp32c6 -t upload --upload-port /dev/cu.usbmodemXXXX
```

## Serial Monitor

```bash
"$HOME/.platformio/penv/bin/pio" device monitor -e seeed_xiao_esp32c6
```

Expected output includes initialization messages followed by soil moisture, illuminance, temperature, humidity, and pressure readings. If a sensor is not detected, the firmware prints a wiring warning and continues running.

## Main Files

- `src/main.cpp` - sensor-node firmware
- `platformio.ini` - board, framework, platform, monitor, and library configuration

## Next Steps

- Replace the sleep simulation with ESP32-C6 low-power sleep.
- Calibrate the soil moisture sensor and convert raw values to a useful moisture scale.
- Add persistent logging or wireless data transfer.
- Add sensor error handling for invalid readings.
