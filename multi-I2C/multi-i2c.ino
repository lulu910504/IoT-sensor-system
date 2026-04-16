#include <Wire.h>
#include "SSD1306Ascii.h"
#include "SSD1306AsciiWire.h"
#include <RTClib.h>
#include <BH1750.h>
#include "SHT31.h"
#include <s8_uart.h>
#include <SparkFun_FS3000_Arduino_Library.h>
#include <SD.h>
#include <SPI.h>
#include "DFRobot_OxygenSensor.h"

// ======================= 硬體腳位 =======================
#define CO2_VCC     4
#define FAN_VCC     2
#define Sensor_VCC 33
#define Button     12
#define SD_CS      32
#define S8_RX_PIN  16
#define S8_TX_PIN  17
#define RST_PIN   -1

// ======================= TCA9548A 設定 =======================
#define TCA_ADDR       0x70
#define TCA_CH_COMMON  0   // OLED、BH1750、SHT31、O2、RTC 等
#define TCA_CH_FS1     6   // 第一顆 FS3000
#define TCA_CH_FS2     7   // 第二顆 FS3000
static inline void tcaSelect(uint8_t ch) {
  Wire.beginTransmission(TCA_ADDR);
  Wire.write(1 << ch);
  Wire.endTransmission();
  delayMicroseconds(500);
}

// ======================= OLED =======================
#define OLED_ADDRESS 0x3C
SSD1306AsciiWire oled;

// ======================= 感測器物件 =======================
RTC_DS3231 rtc;
BH1750 lightMeter;
SHT31 sht;
FS3000 fs3000_1;  // FS3000 #1 on TCA_CH_FS1
FS3000 fs3000_2;  // FS3000 #2 on TCA_CH_FS2
HardwareSerial S8_serial(2);
S8_UART s8_sensor(S8_serial);
#define Oxygen_IICAddress ADDRESS_3
#define OXYGEN_SAMPLE_COUNT 10
DFRobot_OxygenSensor oxygenSensor;

// ======================= SD =======================
File dataFile;
bool sd_ok = false;
const char* LOG_PATH = "/data.txt";

// ======================= 狀態變數 =======================
unsigned long lastUpdate = 0;
const unsigned long updateInterval = 2000;   // 20s
bool systemPoweredOn = false;
bool passiveMode = false;
unsigned long lastAutoRun = 0;
const unsigned long autoInterval = 300000;    // 5min
unsigned long autoStateTime = 0;
int autoState = 0;
bool autoRunning = false;
bool firstLoop = true;

// ======================= 函式宣告 =======================
void initializeDevices();
void restartCO2Sensor();
void readAndDisplayData(bool saveToSD=false);
void logData(DateTime now, float temperature, float humidity, float lux,
             float airflow1, float airflow2, float co2, float o2);
bool ensureSDMounted();

// ======================= 初始化 =======================
void setup() {
  Serial.begin(115200);
  delay(2000);

  pinMode(Sensor_VCC, OUTPUT);
  pinMode(FAN_VCC, OUTPUT);
  pinMode(CO2_VCC, OUTPUT);
  pinMode(Button, INPUT_PULLUP);

  digitalWrite(Sensor_VCC, LOW);
  digitalWrite(FAN_VCC, LOW);
  digitalWrite(CO2_VCC, LOW);

  // 開啟感測器供電（若你希望開機就能立刻初始化）
  digitalWrite(Sensor_VCC, HIGH);

  Wire.begin();

  // RTC 初始化在公共通道
  tcaSelect(TCA_CH_COMMON);
  if (!rtc.begin()) {
    Serial.println("RTC not found! 停止執行");
    while (1);
  }
  if (rtc.lostPower()) {
    Serial.println("RTC lost power, set compile time.");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  Serial.println("setup() 完成，不等待 5 分鐘倍數，直接進入 loop()");
}

// ======================= 初始化所有設備（含 TCA/SD） =======================
void initializeDevices() {
  Serial.println("初始化所有設備...");
  Wire.begin();
  Wire.setClock(400000L);

  // 公共通道裝置
  tcaSelect(TCA_CH_COMMON);

  if (!rtc.begin()) {
    Serial.println("RTC not found!");
  }
  if (rtc.lostPower()) {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  if (!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
    Serial.println("BH1750 not detected!");
  }

  if (!sht.begin()) {
    Serial.println("SHT31 not detected!");
  }

  if (!oxygenSensor.begin(Oxygen_IICAddress)) {
    Serial.println("O2 Sensor not detected (I2C)");
  }

  // FS3000 #1
  tcaSelect(TCA_CH_FS1);
  if (!fs3000_1.begin()) {
    Serial.println("FS3000 #1 not detected!");
  } else {
    fs3000_1.setRange(AIRFLOW_RANGE_7_MPS);
  }

  // FS3000 #2
  tcaSelect(TCA_CH_FS2);
  if (!fs3000_2.begin()) {
    Serial.println("FS3000 #2 not detected!");
  } else {
    fs3000_2.setRange(AIRFLOW_RANGE_7_MPS);
  }

  // UART CO2
  S8_serial.begin(9600, SERIAL_8N1, S8_RX_PIN, S8_TX_PIN);

  // SD：掛載 + 建檔(含表頭)
  SPI.begin();                  // 預設 VSPI 腳位
  sd_ok = SD.begin(SD_CS);
  if (!sd_ok) {
    Serial.println("SD card initialization failed!（檢查接線/CS/供電）");
  } else {
    Serial.println("SD Initialization done.");
    if (!SD.exists(LOG_PATH)) {
      File f = SD.open(LOG_PATH, FILE_WRITE);
      if (f) {
        f.println("DateTime, Temp(C), RH(%), Lux, Wind1(m/s), Wind2(m/s), CO2(ppm), O2(%)");
        f.close();
        Serial.println("Created data.txt with header.");
      } else {
        Serial.println("Failed to create data.txt");
      }
    }
  }

  // OLED
  tcaSelect(TCA_CH_COMMON);
#if RST_PIN >= 0
  oled.begin(&Adafruit128x64, OLED_ADDRESS, RST_PIN);
#else
  oled.begin(&Adafruit128x64, OLED_ADDRESS);
#endif
  oled.setFont(Adafruit5x7);
  oled.clear();
  oled.println(F("System Init"));
  delay(800);
  oled.clear();

  Serial.println("所有設備初始化完成!");
}

void restartCO2Sensor() {
  Serial.println("重新啟動 CO2 感測器 UART...");
  S8_serial.end();
  delay(500);
  S8_serial.begin(9600, SERIAL_8N1, S8_RX_PIN, S8_TX_PIN);
  delay(2000);
}

// ======================= SD 確保掛載 =======================
bool ensureSDMounted() {
  if (sd_ok) return true;
  Serial.println("Re-trying SD.begin()...");
  SPI.begin();
  sd_ok = SD.begin(SD_CS);
  if (!sd_ok) Serial.println("SD still not ready.");
  return sd_ok;
}

// ======================= 讀取與顯示（含兩顆 FS3000） =======================
void readAndDisplayData(bool saveToSD) {
  // 公共通道：時間、溫濕度、光照、O2
  tcaSelect(TCA_CH_COMMON);
  DateTime now = rtc.now();

  sht.read();
  float lux = lightMeter.readLightLevel();
  float temperature = sht.getTemperature();
  float humidity = sht.getHumidity();

  // FS3000 #1
  tcaSelect(TCA_CH_FS1);
  float airflow1 = fs3000_1.readMetersPerSecond();

  // FS3000 #2
  tcaSelect(TCA_CH_FS2);
  float airflow2 = fs3000_2.readMetersPerSecond();

  // CO2 (UART)
  float co2 = s8_sensor.get_co2();
  if (co2 <= 0) {
    Serial.println("CO2 數據讀取錯誤，嘗試重新初始化...");
    restartCO2Sensor();
    co2 = s8_sensor.get_co2();
  }

  // O2
  tcaSelect(TCA_CH_COMMON);
  float o2 = oxygenSensor.getOxygenData(OXYGEN_SAMPLE_COUNT);

  Serial.printf("Time %02d:%02d:%02d | Lux %.2f lx | T %.1f C | RH %.0f %% | W1 %.2f m/s | W2 %.2f m/s | CO2 %.0f ppm | O2 %.2f %%\n",
                now.hour(), now.minute(), now.second(),
                lux, temperature, humidity, airflow1, airflow2, co2, o2);

  if (saveToSD) {
    logData(now, temperature, humidity, lux, airflow1, airflow2, co2, o2);
  }

  // OLED 顯示
  tcaSelect(TCA_CH_COMMON);
  oled.clear();
  oled.setCursor(0, 0);
  oled.printf("T:%02d:%02d:%02d\n", now.hour(), now.minute(), now.second());
  oled.printf("Lux: %.2f lx\n", lux);
  oled.printf("T/H: %.1fC %.0f%%\n", temperature, humidity);
  oled.printf("W1: %.2f  W2: %.2f\n", airflow1, airflow2);
  oled.printf("CO2: %.0f O2: %.2f\n", co2, o2);
}

// ======================= SD 紀錄 =======================
void logData(DateTime now, float temperature, float humidity, float lux,
             float airflow1, float airflow2, float co2, float o2) {
  if (!ensureSDMounted()) {
    Serial.println("Skip logging: SD not mounted.");
    return;
  }

  if (!SD.exists(LOG_PATH)) {
    File f0 = SD.open(LOG_PATH, FILE_WRITE);
    if (f0) {
      f0.println("DateTime, Temp(C), RH(%), Lux, Wind1(m/s), Wind2(m/s), CO2(ppm), O2(%)");
      f0.close();
    } else {
      Serial.println("Cannot create log file.");
      return;
    }
  }

  File f = SD.open(LOG_PATH, FILE_APPEND);
  if (f) {
    f.printf("%04d/%02d/%02d %02d:%02d:%02d, %.1f, %.1f, %.2f, %.2f, %.2f, %.0f, %.2f\r\n",
             now.year(), now.month(), now.day(),
             now.hour(), now.minute(), now.second(),
             temperature, humidity, lux, airflow1, airflow2, co2, o2);
    f.flush();
    f.close();
    Serial.println("Logged Data -> /data.txt");
  } else {
    Serial.println("Error opening data file (append).");
    File f2 = SD.open(LOG_PATH, FILE_WRITE);
    if (f2) {
      f2.printf("%04d/%02d/%02d %02d:%02d:%02d, %.1f, %.1f, %.2f, %.2f, %.2f, %.0f, %.2f\r\n",
                now.year(), now.month(), now.day(),
                now.hour(), now.minute(), now.second(),
                temperature, humidity, lux, airflow1, airflow2, co2, o2);
      f2.close();
      Serial.println("Append failed; WRITE fallback succeeded.");
    } else {
      Serial.println("WRITE fallback also failed.");
    }
  }
}

// ======================= 主迴圈 =======================
void loop() {
  if (firstLoop) {
    lastAutoRun = millis();
    firstLoop = false;
  }

  // 被動模式：按鍵按下即時顯示（不存檔）
  if (digitalRead(Button) == LOW) {
    if (!passiveMode) {
      Serial.println("進入被動模式");
      passiveMode = true;
      systemPoweredOn = true;
      digitalWrite(Sensor_VCC, HIGH);
      digitalWrite(FAN_VCC, HIGH);
      digitalWrite(CO2_VCC, HIGH);
      delay(500);
      initializeDevices();
      restartCO2Sensor();
    }

    if (millis() - lastUpdate >= updateInterval) {
      lastUpdate = millis();
      readAndDisplayData(false);
    }

  } else {
    if (passiveMode) {
      Serial.println("離開被動模式，關閉設備");
      passiveMode = false;
      systemPoweredOn = false;
      digitalWrite(Sensor_VCC, LOW);
      digitalWrite(FAN_VCC, LOW);
      digitalWrite(CO2_VCC, LOW);
    }
  }

  // 主動模式：每 5 分鐘自動量測與存檔
  if (millis() - lastAutoRun >= autoInterval && !autoRunning && !passiveMode) {
    autoRunning = true;
    autoState = 1;
    autoStateTime = millis();
    lastAutoRun = millis();
    Serial.println("啟動主動模式流程");
  }

  if (autoRunning) {
    unsigned long nowms = millis();

    switch (autoState) {
      case 1:
        Serial.println("主動模式：開啟設備");
        digitalWrite(Sensor_VCC, HIGH);
        digitalWrite(FAN_VCC, HIGH);
        digitalWrite(CO2_VCC, HIGH);
        delay(500);
        initializeDevices();
        restartCO2Sensor();
        autoStateTime = nowms;
        autoState = 2;
        break;

      case 2:
        if (nowms - autoStateTime >= 20000) {
          Serial.println("主動模式：關閉風扇");
          digitalWrite(FAN_VCC, LOW);
          autoStateTime = nowms;
          autoState = 3;
        }
        break;

      case 3:
        if (nowms - autoStateTime >= 5000) {
          Serial.println("主動模式：紀錄資料");
          tcaSelect(TCA_CH_COMMON);
          oled.clear();
          oled.setCursor(0, 0);
          oled.println(F("Logging data..."));
          delay(1000);
          readAndDisplayData(true); // 只存 SD
          autoStateTime = nowms;
          autoState = 4;
        }
        break;

      case 4:
        if (nowms - autoStateTime >= 1000) {
          Serial.println("主動模式：關閉電源");
          digitalWrite(Sensor_VCC, LOW);
          digitalWrite(FAN_VCC, LOW);
          digitalWrite(CO2_VCC, LOW);
          autoRunning = false;
          autoState = 0;
        }
        break;
    }
  }

  delay(100);
}
