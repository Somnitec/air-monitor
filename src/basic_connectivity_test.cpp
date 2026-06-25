#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <Wire.h>
#include <driver/i2s.h>
#include "secrets.h"

// ---------------- Configuration ----------------

// I2C Pins
#define I2C_SDA 16
#define I2C_SCL 17


// I2S Pins
#define I2S_SD 33
#define I2S_WS 25
#define I2S_SCK 26

// Sensor Pins
#define HCHO_SENSOR_PIN 32
#define CO_SENSOR_PIN 39
#define SOIL_SENSOR_PIN 34

#define BATTERY_SENSING_PIN 34   // voltage divider to battery+; GPIO34 = ADC1_CH6 (input-only)

// Function Prototypes
void scanI2C();
void initI2S();
float readBatteryVoltage();

// ---------------- Setup ----------------

void setup()
{
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("================================");
    Serial.println(" ESP32 Connectivity Test");
    Serial.println("================================");

    // ---------- ADC ----------

    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);

    Serial.println("ADC configured");

    // ---------- I2C ----------

    Wire.setPins(I2C_SDA, I2C_SCL);
    Wire.begin();

    Serial.printf("I2C initialized on SDA=%d SCL=%d\n",
                  I2C_SDA,
                  I2C_SCL);

    scanI2C();

    // ---------- WiFi ----------

    Serial.printf("Connecting to %s", WIFI_SSID);

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long wifiStart = millis();

    while (WiFi.status() != WL_CONNECTED &&
           millis() - wifiStart < 20000)
    {
        delay(500);
        Serial.print(".");
    }

    Serial.println();

    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.println("WiFi connected");
        Serial.print("IP Address: ");
        Serial.println(WiFi.localIP());

        // ---------- OTA ----------

        ArduinoOTA.setHostname("esp32-sensor-node");

        ArduinoOTA.onStart([]()
                           {
            String type;

            if (ArduinoOTA.getCommand() == U_FLASH)
                type = "Sketch";
            else
                type = "Filesystem";

            Serial.println("OTA Start updating " + type); });

        ArduinoOTA.onEnd([]()
                         { Serial.println("\nOTA Finished"); });

        ArduinoOTA.onProgress([](unsigned int progress,
                                 unsigned int total)
                              { Serial.printf("OTA Progress: %u%%\r",
                                              progress * 100 / total); });

        ArduinoOTA.onError([](ota_error_t error)
                           {
            Serial.printf("OTA Error[%u]: ", error);

            switch (error)
            {
                case OTA_AUTH_ERROR:
                    Serial.println("Auth Failed");
                    break;

                case OTA_BEGIN_ERROR:
                    Serial.println("Begin Failed");
                    break;

                case OTA_CONNECT_ERROR:
                    Serial.println("Connect Failed");
                    break;

                case OTA_RECEIVE_ERROR:
                    Serial.println("Receive Failed");
                    break;

                case OTA_END_ERROR:
                    Serial.println("End Failed");
                    break;

                default:
                    Serial.println("Unknown Error");
            } });

        ArduinoOTA.begin();

        Serial.println("OTA Ready");
    }
    else
    {
        Serial.println("WiFi connection failed");
        Serial.println("OTA disabled");
    }

    // Initialize I2S
    initI2S();
}

// ---------------- Main Loop ----------------

void loop()
{
    ArduinoOTA.handle();

    int hcho_val = analogRead(HCHO_SENSOR_PIN);
    int co_val = analogRead(CO_SENSOR_PIN);
    int soil_val = analogRead(SOIL_SENSOR_PIN);

    float battery_voltage = readBatteryVoltage(); // Assuming the battery is connected to the default pin   


    Serial.printf(
        "HCHO=%4d | CO=%4d | Soil=%4d | Battery Voltage=%.2f V\n",
        hcho_val,
        co_val,
        soil_val,
        battery_voltage);

    // Keep OTA responsive during wait
    for (int i = 0; i < 200; i++)
    {
        ArduinoOTA.handle();
        delay(10);
    }

    // Read from I2S microphone
    int32_t sample;
    size_t bytes_read;
    esp_err_t err = i2s_read(I2S_NUM_0, (void *)&sample, sizeof(sample), &bytes_read, portMAX_DELAY);

    if (err == ESP_OK && bytes_read > 0)
    {
        Serial.printf("I2S Sample: %d\n", sample);
    }
}

// Function to read battery voltage
float readBatteryVoltage() {
    int adcValue = analogRead(BATTERY_SENSING_PIN); // Read the ADC value from the specified pin
    float voltage = analogReadMilliVolts(BATTERY_SENSING_PIN) / 1000.0f * 11.0f; // 10MΩ/1MΩ divider
    return voltage;
}

// ---------------- I2C Scanner ----------------

void scanI2C()
{
    byte error;
    int devicesFound = 0;

    Serial.println();
    Serial.println("Scanning I2C bus...");

    for (uint8_t address = 1; address < 127; address++)
    {
        Wire.beginTransmission(address);
        error = Wire.endTransmission();

        if (error == 0)
        {
            Serial.printf(
                "Found I2C device at 0x%02X\n",
                address);

            devicesFound++;
        }
        else if (error == 4)
        {
            Serial.printf(
                "Unknown error at 0x%02X\n",
                address);
        }
    }

    if (devicesFound == 0)
    {
        Serial.println("No I2C devices found.");
    }
    else
    {
        Serial.printf(
            "Scan complete. %d device(s) found.\n",
            devicesFound);
    }

    Serial.println();
}

// Initialize I2S
void initI2S()
{
    i2s_config_t i2sConfig = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = 44100,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_I2S_MSB,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 64,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0};

    i2s_pin_config_t pinConfig = {
        .bck_io_num = I2S_SCK,
        .ws_io_num = I2S_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_SD};

    i2s_driver_install(I2S_NUM_0, &i2sConfig, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &pinConfig);
}
