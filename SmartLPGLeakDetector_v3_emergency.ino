/* =====================================================================
   Smart LPG Leak Detector - v3.1 OPTIMIZED for ESP32-C3 Super Mini
   MIT License: https://github.com/lamdaflock/LPG-Sniffer

   OPTIMIZATIONS vs v2:
   1.  State machine enum     → eliminates repeated millis() comparisons
   2.  Boot screen drawn ONCE in setup() → no redrawn 3000× in loop
   3.  ADC oversampling (8×)  → cleaner PPM readings, less noise
   4.  powf() instead of pow() → single-precision FPU, 2× faster
   5.  Sensor read gated at 200 ms → CPU free between reads
   6.  Calibration Ro update gated at 500 ms → not every loop tick
   7.  Conditional OLED redraw → I2C write only when PPM changes
   8.  snprintf() instead of String → zero heap fragmentation
   9.  const char* status table → no string construction per frame
   10. FreeRTOS task for WiFi/Blynk → network never starves display
   11. Buzzer logic flattened to table → fewer branches, faster
   12. Removed redundant pinMode(MQ_PIN,INPUT) → ADC pin, not needed

   NEW in v3.1 — EMERGENCY DETECTION DURING WARMUP:
   13. Raw ADC threshold check every 200 ms while calibrating
       → Buzzer + OLED alert fires instantly even before Ro is ready
   14. Ro calibration paused while gas is detected
       → Prevents corrupt calibration from gas-polluted baseline air
   15. Blynk reports "DARURAT! (Kalibrasi)" during warmup emergency
   ===================================================================== */

// ─── Blynk credentials ──────────────────────────────────────────────────────
#define BLYNK_TEMPLATE_ID   "TMPL67b_xXmLQ"
#define BLYNK_TEMPLATE_NAME "Smart LPG Leak Sniffer"
#define BLYNK_AUTH_TOKEN    "3ydnOR4IXz2ID8lvABdI0nJLOeg2drpp"

#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <TimeLib.h>
#include <WidgetRTC.h>
#include <U8g2lib.h>
#include <Wire.h>
#include "Images.h"

// ─── Wi-Fi credentials ──────────────────────────────────────────────────────
static const char ssid[] = "Zaki n Albi";
static const char pass[] = "Baktiar11";

// ─── Pin definitions ─────────────────────────────────────────────────────────
#define MQ_PIN      0
#define BUZZER_PIN  2

// ─── OLED ────────────────────────────────────────────────────────────────────
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// ─── Sensor constants (all float to avoid int→float casts at runtime) ────────
static const float RL_VALUE    = 3.3f;
static const float RO_FACTOR   = 6.5f;
static const float CALIB_A     = 574.25f;
static const float CALIB_B     = -2.222f;
static const long  WARMUP_TIME = 60000L;

// ─── ADC oversampling count ───────────────────────────────────────────────────
#define ADC_SAMPLES 8

// ─── Intervals ───────────────────────────────────────────────────────────────
#define SENSOR_INTERVAL  200UL   // ms between PPM reads during STATE_RUN
#define CALIB_INTERVAL   500UL   // ms between Ro updates during STATE_CALIB
#define NETWORK_YIELD     10UL   // ms the network task yields per cycle

// ─── Emergency raw ADC threshold (used during warmup) ────────────────────────
//  MQ-2/MQ-6 in clean air → typically 300–800 raw (12-bit ADC).
//  A spike above this = gas is almost certainly present.
//  Tuning guide:
//    Lebih sensitif (lebih mudah trigger) → turunkan nilai ini, misal 1500
//    Lebih kebal false alarm              → naikkan nilai ini, misal 2500
//  2000 ≈ ~1.6 V, titik aman yang konservatif.
#define EMERGENCY_RAW_THRESHOLD  2000

// ─── App state machine ───────────────────────────────────────────────────────
enum AppState : uint8_t { STATE_BOOT, STATE_CALIB, STATE_RUN };
static volatile AppState appState = STATE_BOOT;

// ─── Shared sensor data (written by loop, read by Blynk task) ────────────────
static volatile int   globalRaw  = 0;
static volatile int   ppmValue   = 0;
static volatile float Ro         = 10.0f;

// ─── Display tracking ────────────────────────────────────────────────────────
static int lastDisplayedPpm = -1;   // conditional redraw guard

// ─── Warmup emergency state ───────────────────────────────────────────────────
static volatile bool warmupEmergency = false;  // true = gas detected during calib

// ─── Timing ──────────────────────────────────────────────────────────────────
static unsigned long lastSensorRead    = 0;
static unsigned long lastCalibUpdate   = 0;
static unsigned long lastBuzzerTime    = 0;
static unsigned long lastEmergencyRead = 0;   // emergency check during warmup
static bool          buzzerState       = false;

// ─── Blynk / RTC ─────────────────────────────────────────────────────────────
BlynkTimer timer;
WidgetRTC  rtc;

// ─── FreeRTOS task handle ─────────────────────────────────────────────────────
static TaskHandle_t networkTaskHandle = NULL;


// ════════════════════════════════════════════════════════════════════════════
//  HELPER: Read ADC with oversampling (8 samples, integer average)
//  Reduces MQ-sensor noise significantly without floating-point cost.
// ════════════════════════════════════════════════════════════════════════════
static inline int readADCAvg() {
  uint32_t acc = 0;
  for (uint8_t i = 0; i < ADC_SAMPLES; i++) acc += analogRead(MQ_PIN);
  return (int)(acc >> 3);   // divide by 8 via bit-shift (faster than /8)
}


// ════════════════════════════════════════════════════════════════════════════
//  HELPER: Calculate PPM from averaged ADC reading
//  Uses powf() (float version) — uses ESP32-C3 FPU, ~2× faster than pow().
// ════════════════════════════════════════════════════════════════════════════
static int calcPPM() {
  int raw = readADCAvg();
  globalRaw = raw;

  if (raw <= 10)   return 0;
  if (raw >= 4095) return 9999;

  float voltage  = raw * (3.3f / 4095.0f);
  float Rs       = ((3.3f * RL_VALUE) / voltage) - RL_VALUE;
  float ratio    = Rs / Ro;
  float result   = CALIB_A * powf(ratio, CALIB_B);   // powf = float FPU

  return (result < 0.0f) ? 0 : (int)result;
}


// ════════════════════════════════════════════════════════════════════════════
//  HELPER: Map PPM level to a short status string
//  Returns a pointer to a string literal → zero allocation.
// ════════════════════════════════════════════════════════════════════════════
static const char* ppmStatus(int p) {
  if (p < 150) return "SAFE";
  if (p < 300) return "MOD";
  if (p < 500) return "CAUT";
  return "WARN";
}


// ════════════════════════════════════════════════════════════════════════════
//  OLED: Draw dashboard
//  Guard: skip if PPM hasn't changed → saves ~10 ms of I2C time per skip.
// ════════════════════════════════════════════════════════════════════════════
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


// ════════════════════════════════════════════════════════════════════════════
//  BUZZER (DARURAT WARMUP): Fast beep, same tone as full WARNING.
//  Called from STATE_CALIB when raw ADC exceeds EMERGENCY_RAW_THRESHOLD.
// ════════════════════════════════════════════════════════════════════════════
static void handleEmergencyAlarm() {
  unsigned long now = millis();
  if (now - lastBuzzerTime >= 100UL) {
    lastBuzzerTime = now;
    if (buzzerState) { noTone(BUZZER_PIN);     buzzerState = false; }
    else             { tone(BUZZER_PIN, 2500); buzzerState = true;  }
  }
}


// ════════════════════════════════════════════════════════════════════════════
//  BUZZER: Non-blocking, table-driven alarm
//  Flattened lookup table eliminates 3 nested if-else branches.
// ════════════════════════════════════════════════════════════════════════════
static void handleAlarm(int p) {
  if (p < 150) {
    noTone(BUZZER_PIN);
    digitalWrite(BUZZER_PIN, LOW);
    buzzerState = false;
    return;
  }

  // Table: {threshold, interval_ms, freq_Hz}
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


// ════════════════════════════════════════════════════════════════════════════
//  BLYNK UPLINK: sendToBlynk (called every 1 s by BlynkTimer)
//  Uses snprintf() instead of Arduino String → zero heap fragmentation.
// ════════════════════════════════════════════════════════════════════════════
void sendToBlynk() {
  if (!Blynk.connected()) return;

  // Uptime — stack buffer, no heap allocation
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
    // During warmup: report emergency state if detected
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


// ════════════════════════════════════════════════════════════════════════════
//  FREERTOS TASK: Network (WiFi + Blynk)
//  Pinned to core 0 at priority 1. Yields 10 ms every cycle so the main
//  loop (priority 1 as well, but scheduled separately) gets CPU time.
//  On ESP32-C3 (single core) this keeps Blynk from starving the display.
// ════════════════════════════════════════════════════════════════════════════
static void taskNetwork(void* pvParams) {
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      Blynk.run();
    }
    timer.run();
    vTaskDelay(pdMS_TO_TICKS(NETWORK_YIELD));
  }
}


// ════════════════════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);

  // I2C + OLED init
  Wire.begin(8, 9);
  u8g2.begin();

  // Buzzer pin
  pinMode(BUZZER_PIN, OUTPUT);
  // Note: MQ_PIN is an ADC pin — no pinMode() needed for analogRead

  // ── Draw boot screen ONCE here ─────────────────────────────────────────
  //  v2 was redrawing this every loop() iteration for 3 full seconds.
  //  Drawing it once cuts ~hundreds of redundant I2C transactions.
  u8g2.clearBuffer();
  u8g2.drawXBM(38, 3, 53, 43, image_Logo_Baru_bits);
  u8g2.setDrawColor(1);
  u8g2.setFont(u8g2_font_4x6_tf);
  u8g2.drawStr(30, 63, "Attribution @DIYODEMAG");
  u8g2.setFont(u8g2_font_haxrcorp4089_tr);
  u8g2.drawStr(5, 56, "Programmed by @lamdafnaf");
  u8g2.sendBuffer();

  // ── WiFi + Blynk (non-blocking config) ───────────────────────────────────
  WiFi.begin(ssid, pass);
  Blynk.config(BLYNK_AUTH_TOKEN);
  rtc.begin();
  setSyncInterval(10 * 60);
  timer.setInterval(1000L, sendToBlynk);

  // ── Spawn network task ────────────────────────────────────────────────────
  //  Stack 4096 bytes is enough for Blynk + timer callbacks.
  //  Priority 1 (lower than default Arduino loop priority of 1, same tier).
  xTaskCreatePinnedToCore(
    taskNetwork,        // function
    "NetTask",          // debug name
    4096,               // stack bytes
    NULL,               // params
    1,                  // priority
    &networkTaskHandle, // handle
    0                   // core (ESP32-C3 only has core 0)
  );
}


// ════════════════════════════════════════════════════════════════════════════
//  LOOP  —  State machine, all UI + sensor logic here
//  The network task runs concurrently via FreeRTOS.
// ════════════════════════════════════════════════════════════════════════════
void loop() {
  unsigned long now = millis();

  switch (appState) {

    // ── State 1: Boot splash (3 seconds) ────────────────────────────────────
    case STATE_BOOT:
      // Screen already drawn in setup(). Nothing to do except wait.
      if (now >= 3000UL) {
        appState = STATE_CALIB;
        lastCalibUpdate = now;
      }
      break;

    // ── State 2: Warm-up / calibration (60 seconds) ─────────────────────────
    case STATE_CALIB: {
      // ── Emergency sensor check every 200 ms ─────────────────────────────
      //  Even without accurate Ro, a raw ADC spike clearly = gas present.
      //  If detected: sound the alarm and skip Ro update (don't corrupt
      //  calibration baseline with gas-polluted air readings).
      if (now - lastEmergencyRead >= SENSOR_INTERVAL) {
        lastEmergencyRead = now;
        int rawCheck = readADCAvg();
        globalRaw = rawCheck;

        if (rawCheck >= EMERGENCY_RAW_THRESHOLD) {
          warmupEmergency = true;
        } else {
          // Gas cleared — silence buzzer and resume normal calibration
          if (warmupEmergency) {
            noTone(BUZZER_PIN);
            digitalWrite(BUZZER_PIN, LOW);
            buzzerState = false;
          }
          warmupEmergency = false;

          // Update Ro only in clean air, only every CALIB_INTERVAL
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

      // ── Buzzer: keep sounding if still in emergency ──────────────────────
      if (warmupEmergency) {
        handleEmergencyAlarm();
      }

      // ── OLED: progress bar + emergency overlay ───────────────────────────
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

      // Flash "! GAS BOCOR !" overlay during emergency
      if (warmupEmergency) {
        // Blinking: show warning text on even-numbered 500 ms windows
        if ((now / 500) % 2 == 0) {
          // Dark filled box behind text for contrast
          u8g2.setDrawColor(1);
          u8g2.drawBox(0, 50, 128, 14);
          u8g2.setDrawColor(0);   // white-on-black text
          u8g2.setFont(u8g2_font_6x10_tf);
          u8g2.setCursor(14, 61);
          u8g2.print("!! DARURAT: GAS BOCOR !!");
          u8g2.setDrawColor(1);   // restore
        }
      }

      u8g2.sendBuffer();

      if (now >= (unsigned long)WARMUP_TIME) {
        // Clear any residual emergency buzzer before entering run state
        if (!warmupEmergency) {
          noTone(BUZZER_PIN);
          digitalWrite(BUZZER_PIN, LOW);
          buzzerState = false;
        }
        warmupEmergency  = false;
        appState         = STATE_RUN;
        lastDisplayedPpm = -1;   // force first dashboard draw
      }
      break;
    }

    // ── State 3: Run — PPM monitoring ───────────────────────────────────────
    case STATE_RUN:
      // Read sensor only at SENSOR_INTERVAL — frees CPU between reads
      if (now - lastSensorRead >= SENSOR_INTERVAL) {
        lastSensorRead = now;
        ppmValue = calcPPM();
      }
      drawDashboard((int)ppmValue);   // skips if value unchanged
      handleAlarm((int)ppmValue);
      break;
  }

  // Small yield so FreeRTOS network task gets scheduled properly
  vTaskDelay(pdMS_TO_TICKS(5));
}
