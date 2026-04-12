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

// define腳位
#define CO2_VCC 4
#define FAN_VCC 2
#define Sensor_VCC 33
#define Button 12
#define SD_CS 32
#define S8_RX_PIN 16
#define S8_TX_PIN 17
#define RST_PIN -1

// OLED
#define OLED_ADDRESS 0x3C
SSD1306AsciiWire oled;

// 感測資料庫
RTC_DS3231 rtc;
BH1750 lightMeter;
SHT31 sht;
FS3000 fs3000;
HardwareSerial S8_serial(2);
S8_UART s8_sensor(S8_serial);

// define氧氣感測器(後來加入的)
#define Oxygen_IICAddress ADDRESS_3
#define OXYGEN_SAMPLE_COUNT 10
DFRobot_OxygenSensor oxygenSensor;

// SD 卡叫他datafile
File dataFile;

// 時間與模式變數
unsigned long lastUpdate = 0;
const unsigned long updateInterval = 2000;
bool systemPoweredOn = false;
bool passiveMode = false;
unsigned long lastAutoRun = 0;
const unsigned long autoInterval = 300000  ; // 5min測量一次
unsigned long autoStateTime = 0;
int autoState = 0;
bool autoRunning = false;
bool firstLoop = true;
void setup() {
  Serial.begin(115200);
  delay(2000); // 等一下拉

  pinMode(Sensor_VCC, OUTPUT);
  pinMode(FAN_VCC, OUTPUT);
  pinMode(CO2_VCC, OUTPUT);
  pinMode(Button, INPUT_PULLUP);

  digitalWrite(Sensor_VCC, LOW);
  digitalWrite(FAN_VCC, LOW);
  digitalWrite(CO2_VCC, LOW);
  Serial.println("系統啟動，設備關閉...");

  digitalWrite(Sensor_VCC, HIGH);
  Serial.println("Sensor VCC已開啟，準備開始初始化RTC...");

  // --- 加上這兩行！一定要有！！ ---
  Wire.begin();  // 初始化I2C
  if (!rtc.begin()) {
    Serial.println("RTC not found! 停止執行");
    while (1); // RTC沒找到直接卡住
  }
  // ---------------------------------

  Serial.println("RTC初始化完成，開始等待分鐘為5的倍數...");

  // --- 正確的等待5分鐘倍數流程 ---
  //等到5min倍數再進行運作
  while (true) {
    DateTime now = rtc.now();
    Serial.printf("目前時間: %02d:%02d:%02d\n", now.hour(), now.minute(), now.second());

    if (now.minute() % 5 == 0) {
      Serial.printf("現在時間 %02d:%02d，符合5分鐘倍數，開始運行!\n", now.hour(), now.minute());
      break;
    }
    delay(1000); // 每秒檢查一次
  }
}

void initializeDevices() {
  Serial.println("初始化所有設備...");
  Wire.begin();
  Wire.setClock(400000L);

  if (!rtc.begin()) {
    Serial.println("RTC not found!");
  }
  if (rtc.lostPower()) {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  if (!lightMeter.begin()) {
    Serial.println("BH1750 not detected!");
  }

  if (!sht.begin()) {
    Serial.println("SHT31 not detected!");
  }

  if (!fs3000.begin()) {
    Serial.println("FS3000 not detected!");
  }
  fs3000.setRange(AIRFLOW_RANGE_7_MPS);

  if (!oxygenSensor.begin(Oxygen_IICAddress)) {
    Serial.println("O2 Sensor not detected (I2C)");
  }

  S8_serial.begin(9600, SERIAL_8N1, S8_RX_PIN, S8_TX_PIN);

  if (!SD.begin(SD_CS)) {
    Serial.println("SD card initialization failed!");
  } else {
    Serial.println("SD Initialization done.");
  }

#if RST_PIN >= 0
  oled.begin(&Adafruit128x64, OLED_ADDRESS, RST_PIN); ㄎ
#else
  oled.begin(&Adafruit128x64, OLED_ADDRESS);
#endif

  oled.setFont(Adafruit5x7);
  oled.clear();
  oled.println(F("System Init"));
  delay(1000);
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

void readAndDisplayData(bool saveToSD = false) {
  DateTime now = rtc.now();
  sht.read();
  float lux = lightMeter.readLightLevel();
  float temperature = sht.getTemperature();
  float humidity = sht.getHumidity();
  float airflow = fs3000.readMetersPerSecond();
  float co2 = s8_sensor.get_co2();
  float o2 = oxygenSensor.getOxygenData(OXYGEN_SAMPLE_COUNT);

  if (co2 <= 0) {
    Serial.println("CO2 數據讀取錯誤，嘗試重新初始化...");
    restartCO2Sensor();
    co2 = s8_sensor.get_co2();
  }

  Serial.printf("Time: %02d:%02d:%02d | Lux: %.2f lx | Temp: %.1f C | Hum: %.0f%% | Wind: %.2f m/s | CO2: %.0f ppm | O2: %.2f %%vol\n",
                now.hour(), now.minute(), now.second(),
                lux, temperature, humidity, airflow, co2, o2);

  if (saveToSD) {
    logData(now, temperature, humidity, lux, airflow, co2, o2);
  }

  oled.clear();
  oled.setCursor(0, 0);
  oled.printf("Time: %02d:%02d:%02d\n", now.hour(), now.minute(), now.second());
  oled.printf("Lux: %.2f lx\n", lux);
  oled.printf("Temp: %.1f C\n", temperature);
  oled.printf("Hum: %.0f %%\n", humidity);
  oled.printf("Wind: %.2f m/s\n", airflow);
  oled.printf("CO2: %.0f ppm\n", co2);
  oled.printf("O2: %.2f %%\n", o2);
}

void logData(DateTime now, float temperature, float humidity, float lux, float airflow, float co2, float o2) {
  dataFile = SD.open("/data.txt", FILE_APPEND);
  if (dataFile) {
    dataFile.printf("%04d/%02d/%02d %02d:%02d:%02d, %.1f, %.1f, %.2f, %.2f, %.0f, %.2f\n",
                    now.year(), now.month(), now.day(),
                    now.hour(), now.minute(), now.second(),
                    temperature, humidity, lux, airflow, co2, o2);
    dataFile.flush();
    dataFile.close();
    Serial.println("Logged Data");
  } else {
    Serial.println("Error opening data file");
  }
}

void loop() {
  if (firstLoop) {
    lastAutoRun = millis();  // 這邊才正確設定！
    firstLoop = false;
  }
  // 被動模式：按鈕按下
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
      readAndDisplayData(false);  // 顯示不儲存
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

  // 主動模式：每小時執行一次
  if (millis() - lastAutoRun >= autoInterval && !autoRunning && !passiveMode) {
    autoRunning = true;
    autoState = 1;
    autoStateTime = millis();
    lastAutoRun = millis();
    Serial.println("啟動主動模式流程");
  }

  // 主動模式狀態流程
  if (autoRunning) {
    unsigned long now = millis();

    switch (autoState) {
      case 1:
        Serial.println("主動模式：開啟設備");
        digitalWrite(Sensor_VCC, HIGH);
        digitalWrite(FAN_VCC, HIGH);
        digitalWrite(CO2_VCC, HIGH);
        delay(500);
        initializeDevices();
        restartCO2Sensor();
        autoStateTime = now;
        autoState = 2;
        break;

      case 2:
        if (now - autoStateTime >= 10000) {
          Serial.println("主動模式：關閉風扇");
          digitalWrite(FAN_VCC, LOW);
          autoStateTime = now;
          autoState = 3;
        }
        break;

      case 3:
        if (now - autoStateTime >= 5000) {
          Serial.println("主動模式：00紀錄資料");
          oled.clear();
          oled.setCursor(0, 0);
          oled.println(F("Logging data..."));
          delay(1000);
          readAndDisplayData(true);
          autoStateTime = now;
          autoState = 4;
        }
        break;

      case 4:
        if (now - autoStateTime >= 1000) {
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
