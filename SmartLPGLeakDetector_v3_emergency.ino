/* =====================================================================
   MIT License: https://github.com/lamdaflock/LPG-Sniffer
   ===================================================================== */

#define BLYNK_TEMPLATE_ID   "ID Template"
#define BLYNK_TEMPLATE_NAME "Smart LPG Leak Sniffer"
#define BLYNK_AUTH_TOKEN    "Token Blynk"

#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <TimeLib.h>
#include <WidgetRTC.h>
#include <U8g2lib.h>
#include <Wire.h>
#include "Images.h"

static const char ssid[] = "SSID";
static const char pass[] = "Passwordnya";

#define MQ_PIN      0
#define BUZZER_PIN  2

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

static const float RL_VALUE    = 3.3f;
static const float RO_FACTOR   = 6.5f;
static const float CALIB_A     = 574.25f;
static const float CALIB_B     = -2.222f;
static const long  WARMUP_TIME = 60000L;

#define ADC_SAMPLES 8
#define SENSOR_INTERVAL  200UL   // ms between PPM reads during STATE_RUN
#define CALIB_INTERVAL   500UL   // ms between Ro updates during STATE_CALIB
#define NETWORK_YIELD     10UL   // ms the network task yields per cycle
#define EMERGENCY_RAW_THRESHOLD  2000

enum AppState : uint8_t { STATE_BOOT, STATE_CALIB, STATE_RUN };
static volatile AppState appState = STATE_BOOT;
static volatile int   globalRaw  = 0;
static volatile int   ppmValue   = 0;
static volatile float Ro         = 10.0f;
static int lastDisplayedPpm = -1;   // conditional redraw guard
static volatile bool warmupEmergency = false;  // true = gas detected during calib
static unsigned long lastSensorRead    = 0;
static unsigned long lastCalibUpdate   = 0;
static unsigned long lastBuzzerTime    = 0;
static unsigned long lastEmergencyRead = 0;   // emergency check during warmup
static bool          buzzerState       = false;
BlynkTimer timer;
WidgetRTC  rtc;
static TaskHandle_t networkTaskHandle = NULL;
static inline int readADCAvg() {
  uint32_t acc = 0;
  for (uint8_t i = 0; i < ADC_SAMPLES; i++) acc += analogRead(MQ_PIN);
  return (int)(acc >> 3);   // divide by 8 via bit-shift (faster than /8)
}

static int calcPPM() {
  int raw = readADCAvg();
  globalRaw = raw;

  if (raw <= 10)   return 0;
  if (raw >= 4095) return 9999;

  float voltage  = raw * (3.3f / 4095.0f);
  float Rs       = ((3.3f * RL_VALUE) / voltage) - RL_VALUE;
  float ratio    = Rs / Ro;
  float result   = CALIB_A * powf(ratio, CALIB_B);

  return (result < 0.0f) ? 0 : (int)result;
}

static const char* ppmStatus(int p) {
  if (p < 150) return "SAFE";
  if (p < 300) return "MOD";
  if (p < 500) return "CAUT";
  return "WARN";
}

static void drawDashboard(int p) {
  if (p == lastDisplayedPpm) return;    // ← conditional redraw guard
  lastDisplayedPpm = p;

  u8g2.clearBuffer();
  u8g2.drawXBM(0, 0, 129, 64, image_Dashboard_Main_bits);
  u8g2.setFont(u8g2_font_ncenB12_tr);
  u8g2.setCursor(38, 28);
  u8g2.print(p);
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.setCursor(40, 50);
  u8g2.print(ppmStatus(p));
  u8g2.sendBuffer();
}

static void handleEmergencyAlarm() {
  unsigned long now = millis();
  if (now - lastBuzzerTime >= 100UL) {
    lastBuzzerTime = now;
    if (buzzerState) { noTone(BUZZER_PIN);     buzzerState = false; }
    else             { tone(BUZZER_PIN, 2500); buzzerState = true;  }
  }
}

static void handleAlarm(int p) {
  if (p < 150) {
    noTone(BUZZER_PIN);
    digitalWrite(BUZZER_PIN, LOW);
    buzzerState = false;
    return;
  }

  static const uint16_t alarmTable[3][3] = {
    { 300, 1000,  500 },    // Moderate: slow beep
    { 500,  250, 1000 },    // Caution:  medium beep
    {9999,  100, 2500 },    // Warning:  fast loud beep
  };

  unsigned long interval = 100;
  int           freq     = 2500;
  for (uint8_t i = 0; i < 3; i++) {
    if (p < alarmTable[i][0]) {
      interval = alarmTable[i][1];
      freq     = alarmTable[i][2];
      break;
    }
  }

  unsigned long now = millis();
  if (now - lastBuzzerTime >= interval) {
    lastBuzzerTime = now;
    if (buzzerState) { noTone(BUZZER_PIN); buzzerState = false; }
    else             { tone(BUZZER_PIN, freq); buzzerState = true; }
  }
}

void sendToBlynk() {
  if (!Blynk.connected()) return;

  char uptimeBuf[16];
  long totalSec = millis() / 1000;
  snprintf(uptimeBuf, sizeof(uptimeBuf), "%ldJ %ldM",
           totalSec / 3600, (totalSec % 3600) / 60);

  Blynk.virtualWrite(V1, uptimeBuf);
  Blynk.virtualWrite(V0, (int)globalRaw);

  if (millis() >= (unsigned long)WARMUP_TIME) {
    int p = (int)ppmValue;
    Blynk.virtualWrite(V4, p);

    const char* s;
    bool isLeak = false;
    if      (p < 150) s = "Safe";
    else if (p < 300) s = "Moderate";
    else if (p < 500) s = "CAUTION";
    else              { s = "WARNING"; isLeak = true; }

    Blynk.virtualWrite(V3, s);
    if (isLeak) Blynk.logEvent("gas_leak", "PERINGATAN! Gas LPG Bocor!");
  } else {
    if (warmupEmergency) {
      Blynk.virtualWrite(V3, "DARURAT! (Kalibrasi)");
      Blynk.virtualWrite(V0, (int)globalRaw);
      Blynk.logEvent("gas_leak", "DARURAT! Gas Terdeteksi Saat Kalibrasi!");
    } else {
      Blynk.virtualWrite(V3, "Kalibrasi...");
      Blynk.virtualWrite(V4, 0);
    }
  }
}

static void taskNetwork(void* pvParams) {
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      Blynk.run();
    }
    timer.run();
    vTaskDelay(pdMS_TO_TICKS(NETWORK_YIELD));
  }
}

void setup() {
  Serial.begin(115200);
  Wire.begin(8, 9);
  u8g2.begin();
  pinMode(BUZZER_PIN, OUTPUT);
   
  u8g2.clearBuffer();
  u8g2.drawXBM(38, 3, 53, 43, image_Logo_Baru_bits);
  u8g2.setDrawColor(1);
  u8g2.setFont(u8g2_font_4x6_tf);
  u8g2.drawStr(30, 63, "Attribution @DIYODEMAG");
  u8g2.setFont(u8g2_font_haxrcorp4089_tr);
  u8g2.drawStr(5, 56, "Programmed by @lamdafnaf");
  u8g2.sendBuffer();

  WiFi.begin(ssid, pass);
  Blynk.config(BLYNK_AUTH_TOKEN);
  rtc.begin();
  setSyncInterval(10 * 60);
  timer.setInterval(1000L, sendToBlynk);

  xTaskCreatePinnedToCore(
    taskNetwork,        
    "NetTask",          
    4096,              
    NULL,              
    1,                 
    &networkTaskHandle, 
    0                  
  );
}

void loop() {
  unsigned long now = millis();

  switch (appState) {
    case STATE_BOOT:
      if (now >= 3000UL) {
        appState = STATE_CALIB;
        lastCalibUpdate = now;
      }
      break;
    case STATE_CALIB: {
      if (now - lastEmergencyRead >= SENSOR_INTERVAL) {
        lastEmergencyRead = now;
        int rawCheck = readADCAvg();
        globalRaw = rawCheck;

        if (rawCheck >= EMERGENCY_RAW_THRESHOLD) {
          warmupEmergency = true;
        } else {
          if (warmupEmergency) {
            noTone(BUZZER_PIN);
            digitalWrite(BUZZER_PIN, LOW);
            buzzerState = false;
          }
          warmupEmergency = false;
          if (now - lastCalibUpdate >= CALIB_INTERVAL) {
            lastCalibUpdate = now;
            if (rawCheck > 0 && rawCheck < 4095) {
              float volt   = rawCheck * (3.3f / 4095.0f);
              float rs_air = ((3.3f * RL_VALUE) / volt) - RL_VALUE;
              Ro = rs_air / RO_FACTOR;
            }
          }
        }
      }

      if (warmupEmergency) {
        handleEmergencyAlarm();
      }

      int progress = (int)map((long)now, 3000L, WARMUP_TIME, 0L, 100L);
      progress = constrain(progress, 0, 100);

      u8g2.clearBuffer();
      u8g2.drawXBM(0, 0, 128, 64, image_Loading_Screen_4_bits);
      u8g2.setDrawColor(1);
      u8g2.drawBox(5, 34, map(progress, 0, 100, 0, 75), 10);
      u8g2.setFont(u8g2_font_6x10_tf);
      u8g2.setCursor(45, 30);
      u8g2.print(progress);
      u8g2.print('%');

      if (warmupEmergency) {
        if ((now / 500) % 2 == 0) {
          u8g2.setDrawColor(1);
          u8g2.drawBox(0, 50, 128, 14);
          u8g2.setDrawColor(0);  
          u8g2.setFont(u8g2_font_6x10_tf);
          u8g2.setCursor(14, 61);
          u8g2.print("!! DARURAT: GAS BOCOR !!");
          u8g2.setDrawColor(1);
        }
      }

      u8g2.sendBuffer();

      if (now >= (unsigned long)WARMUP_TIME) {
        if (!warmupEmergency) {
          noTone(BUZZER_PIN);
          digitalWrite(BUZZER_PIN, LOW);
          buzzerState = false;
        }
        warmupEmergency  = false;
        appState         = STATE_RUN;
        lastDisplayedPpm = -1;
      }
      break;
    }
  
    case STATE_RUN:
      if (now - lastSensorRead >= SENSOR_INTERVAL) {
        lastSensorRead = now;
        ppmValue = calcPPM();
      }
      drawDashboard((int)ppmValue);  
      handleAlarm((int)ppmValue);
      break;
  }

  vTaskDelay(pdMS_TO_TICKS(5));
}
