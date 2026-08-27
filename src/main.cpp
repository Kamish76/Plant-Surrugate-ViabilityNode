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
