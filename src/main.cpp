#include <Arduino.h>
#include <Wire.h>
#include <SensirionI2cScd4x.h>
#include <BH1750.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME680.h>

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

uint32_t lastReadMs = 0;

void printScd4xError(const char* context, uint16_t error) {
  char errorMessage[64];
  errorToString(error, errorMessage, sizeof errorMessage);
  Serial.print("SCD-41 error (");
  Serial.print(context);
  Serial.print("): ");
  Serial.println(errorMessage);
}

void setup() {
  Serial.begin(115200);
  Wire.begin();

  scd4x.begin(Wire, SCD41_I2C_ADDR_62);
  uint16_t error = scd4x.startPeriodicMeasurement();
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

  bool scd4xDataReady = false;
  uint16_t error = scd4x.getDataReadyStatus(scd4xDataReady);
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
  Serial.print("Light: ");
  Serial.print(lux);
  Serial.println(" lx");

  if (!bme.performReading()) {
    Serial.println("BME680 read failed");
  } else {
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

  Serial.println();
}
