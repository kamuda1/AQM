#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <InfluxDbClient.h>
#include <SensirionI2cScd4x.h>
#include <BH1750.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME680.h>

// WiFi + InfluxDB credentials live in include/secrets.h (gitignored).
// Copy include/secrets.h.example to include/secrets.h and fill it in.
#include "secrets.h"

// Which physical board this is. Set per-board by the PlatformIO environment
// (see platformio.ini: `comfort`, `mushroom`, ...) so one firmware/secrets
// pair can flash multiple boards, each writing under its own InfluxDB tag.
// The fallback keeps a bare `pio run` (no env flag) building.
#ifndef DEVICE_NAME
#define DEVICE_NAME "esp32-aqm"
#endif

// The Sensirion SCD4x driver returns 0 on success but only defines NO_ERROR
// internally; its example sketches expect callers to declare it themselves.
#define NO_ERROR 0

// BME680 breakouts ship with either address depending on the vendor.
#define BME680_I2C_ADDR 0x77

// SCD-41 only produces a new reading every ~5s, so there's no point
// polling any faster than that.
const uint32_t SENSOR_READ_INTERVAL_MS = 5000;

SensirionI2cScd4x scd4x;
BH1750 lightMeter;
Adafruit_BME680 bme;

// InfluxDB 2.x client. All readings are written to a single measurement
// ("air_quality") tagged with the device name so multiple sensors can share
// one bucket.
InfluxDBClient influx(INFLUXDB_URL, INFLUXDB_ORG, INFLUXDB_BUCKET, INFLUXDB_TOKEN);

uint32_t lastReadMs = 0;

void printScd4xError(const char* context, uint16_t error) {
  char errorMessage[64];
  errorToString(error, errorMessage, sizeof errorMessage);
  Serial.print("SCD-41 error (");
  Serial.print(context);
  Serial.print("): ");
  Serial.println(errorMessage);
}

void connectWiFi() {
  Serial.print("Connecting to WiFi ");
  Serial.print(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  // The board runs unattended off USB power, so keep the radio awake (modem
  // sleep drops the occasional write) and let the SDK reconnect on its own.
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  // Block until associated; the sensors don't produce useful data for the
  // first few seconds anyway, so there's nothing to lose by waiting here.
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.print(" connected, IP: ");
  Serial.println(WiFi.localIP());
}

// Called every cycle. If the link dropped, try to bring it back within a
// bounded window so sensor reads and serial output keep flowing regardless.
void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }
  Serial.println("WiFi disconnected, reconnecting...");
  WiFi.reconnect();
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(250);
  }
}

void setup() {
  Serial.begin(115200);
  Wire.begin();

  connectWiFi();

  if (influx.validateConnection()) {
    Serial.print("Connected to InfluxDB: ");
    Serial.println(influx.getServerUrl());
  } else {
    Serial.print("InfluxDB connection failed: ");
    Serial.println(influx.getLastErrorMessage());
  }

  scd4x.begin(Wire, SCD41_I2C_ADDR_62);
  delay(30);

  // `make flash` resets the ESP32 but not the SCD-41, so on a reflash the
  // sensor is often still running periodic measurement from the previous
  // session. In that state it rejects startPeriodicMeasurement(), and no CO2
  // is ever read. Bring it back to a known-idle state first (the driver's
  // stop/reinit calls include their required settling delays internally).
  uint16_t error = scd4x.wakeUp();
  if (error != NO_ERROR) {
    printScd4xError("wakeUp", error);
  }
  error = scd4x.stopPeriodicMeasurement();
  if (error != NO_ERROR) {
    printScd4xError("stopPeriodicMeasurement", error);
  }
  error = scd4x.reinit();
  if (error != NO_ERROR) {
    printScd4xError("reinit", error);
  }

  // Diagnostic: prove the ESP32 can actually talk to the SCD-41 over I2C.
  // Must run while the sensor is idle (not in periodic measurement).
  uint64_t scd4xSerial = 0;
  error = scd4x.getSerialNumber(scd4xSerial);
  if (error != NO_ERROR) {
    printScd4xError("getSerialNumber", error);
    Serial.println("SCD-41 not responding on I2C -- check wiring/pinout");
  } else {
    Serial.print("SCD-41 serial: 0x");
    Serial.print((uint32_t)(scd4xSerial >> 32), HEX);
    Serial.println((uint32_t)(scd4xSerial & 0xFFFFFFFF), HEX);
  }

  error = scd4x.startPeriodicMeasurement();
  if (error != NO_ERROR) {
    printScd4xError("startPeriodicMeasurement", error);
  }

  if (!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
    Serial.println("BH1750 not found");
  }

  if (!bme.begin(BME680_I2C_ADDR)) {
    Serial.println("BME680 not found");
  }
  bme.setTemperatureOversampling(BME680_OS_8X);
  bme.setHumidityOversampling(BME680_OS_2X);
  bme.setPressureOversampling(BME680_OS_4X);
  bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
  bme.setGasHeater(320, 150); // 320C for 150ms
}

void loop() {
  uint32_t now = millis();
  if (now - lastReadMs < SENSOR_READ_INTERVAL_MS) {
    return;
  }
  lastReadMs = now;

  ensureWiFi();

  // Accumulate this cycle's readings into a single point. Fields are only
  // added when their sensor read succeeds, so a failing sensor drops out of
  // the time series rather than writing a bogus zero.
  Point reading("air_quality");
  reading.addTag("device", DEVICE_NAME);

  // The SCD-41 produces a new sample every ~5s. Polling once per 5s cycle
  // phase-locks with that cadence and can miss the ready window on every
  // cycle, so actively wait (bounded) for the sample to become ready.
  bool scd4xDataReady = false;
  uint16_t error = NO_ERROR;
  for (uint8_t attempt = 0; attempt < 20; attempt++) {
    error = scd4x.getDataReadyStatus(scd4xDataReady);
    if (error != NO_ERROR || scd4xDataReady) {
      break;
    }
    delay(100);
  }
  if (error != NO_ERROR) {
    printScd4xError("getDataReadyStatus", error);
  } else if (scd4xDataReady) {
    uint16_t co2 = 0;
    float scd4xTemperature = 0.0f;
    float scd4xHumidity = 0.0f;
    error = scd4x.readMeasurement(co2, scd4xTemperature, scd4xHumidity);
    if (error != NO_ERROR) {
      printScd4xError("readMeasurement", error);
    } else {
      reading.addField("co2", co2);
      reading.addField("scd4x_temperature", scd4xTemperature);
      reading.addField("scd4x_humidity", scd4xHumidity);
      Serial.print("CO2: ");
      Serial.print(co2);
      Serial.print(" ppm, Temp: ");
      Serial.print(scd4xTemperature);
      Serial.print(" C, RH: ");
      Serial.print(scd4xHumidity);
      Serial.println(" %");
    }
  }

  float lux = lightMeter.readLightLevel();
  if (lux >= 0.0f) {
    reading.addField("light", lux);
  }
  Serial.print("Light: ");
  Serial.print(lux);
  Serial.println(" lx");

  if (!bme.performReading()) {
    Serial.println("BME680 read failed");
  } else {
    reading.addField("bme_temperature", bme.temperature);
    reading.addField("bme_humidity", bme.humidity);
    reading.addField("pressure", bme.pressure / 100.0);
    reading.addField("gas_resistance", bme.gas_resistance / 1000.0);
    Serial.print("BME680 Temp: ");
    Serial.print(bme.temperature);
    Serial.print(" C, RH: ");
    Serial.print(bme.humidity);
    Serial.print(" %, Pressure: ");
    Serial.print(bme.pressure / 100.0);
    Serial.print(" hPa, Gas: ");
    Serial.print(bme.gas_resistance / 1000.0);
    Serial.println(" kOhm");
  }

  // Only write if at least one sensor produced a field this cycle.
  if (reading.hasFields()) {
    if (!influx.writePoint(reading)) {
      Serial.print("InfluxDB write failed: ");
      Serial.println(influx.getLastErrorMessage());
    }
  }

  Serial.println();
}
