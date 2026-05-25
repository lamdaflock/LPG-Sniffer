/* =====================
   This software is licensed under the MIT License:
   https://github.com/lamdaflock/LPG-Sniffer
   ===================== */

#define BLYNK_TEMPLATE_ID "TMPL67b_xXmLQ"
#define BLYNK_TEMPLATE_NAME "Smart LPG Leak Sniffer"
#define BLYNK_AUTH_TOKEN "erkT2-8nqnK0HU2MEvUrtTCrcQnX5QK0"

#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <TimeLib.h>
#include <WidgetRTC.h>
#include <U8g2lib.h>
#include <Wire.h>
#include "Images.h" 

char ssid[] = "naf"; 
char pass[] = "naf11119"; 


#define MQ_PIN      0   
#define BUZZER_PIN  2     

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);
//U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

const float RL_VALUE = 3.3;
const float RO_FACTOR = 6.5;
const float CALIB_A = 574.25;
const float CALIB_B = -2.222;

// Threshold ADC mentah untuk deteksi darurat saat kalibrasi.
// Tidak bergantung Ro yang masih berubah saat warmup.
// Range ADC ESP32: 0-4095. Naikkan jika terlalu sensitif,
// turunkan jika kurang sensitif.
#define emergency 2950
float Ro = 10.0;
int ppm = 0;
const long WARMUP_TIME = 63000;  
unsigned long timeStart;
unsigned long lastBuzzerTime = 0;
bool buzzerState = false;
BlynkTimer timer;
WidgetRTC rtc;
int globalRaw = 0;

// Dashboard Blynk
void sendToBlynk() {
  if (!Blynk.connected()) return;

  long totalDetik = millis() / 1000;
  long jam = totalDetik / 3600;
  long menit = (totalDetik % 3600) / 60;
  String uptimeStr = String(jam) + "J " + String(menit) + "M";
  Blynk.virtualWrite(V1, uptimeStr);

  /*
  char timeStr[10];
  sprintf(timeStr, "%02d:%02d", hour(), minute());
  Blynk.virtualWrite(V2, timeStr);
  */
  Blynk.virtualWrite(V0, globalRaw);

  if (millis() >= WARMUP_TIME) {
    Blynk.virtualWrite(V4, ppm); 

    String statusBlynk;
    if (ppm < 150) {
      statusBlynk = "Safe";
    } else if (ppm < 300) {
      statusBlynk = "Moderate";
    } else if (ppm < 500) {
      statusBlynk = "CAUTION";
    } else {
      statusBlynk = "WARNING";
      Blynk.logEvent("gas_leak", "PERINGATAN! Gas LPG Bocor!"); 
    }
    Blynk.virtualWrite(V3, statusBlynk);
  } else {
    Blynk.virtualWrite(V3, "Kalibrasi...");
    Blynk.virtualWrite(V4, 0); 
  }
}

// Perhitungan PPM
int getPPM() {
  globalRaw = analogRead(MQ_PIN); 
  
  if (globalRaw <= 10) return 0;       
  if (globalRaw >= 4095) return 9999; 

  float voltage = globalRaw * (3.3 / 4095.0);
  float Rs = ((3.3 * RL_VALUE) / voltage) - RL_VALUE;
  float ratio = Rs / Ro;
  float ppm_calc = CALIB_A * pow(ratio, CALIB_B);
  
  if (ppm_calc < 0) ppm_calc = 0;
  return (int)ppm_calc;
}

// Tampilan LCD OLED
void drawDashboard(int gas_ppm) {
  u8g2.clearBuffer();
  u8g2.drawXBM(0, 0, 129, 64, image_dashboard);

  u8g2.setFontMode(1);
  u8g2.setDrawColor(2);

  // tampilan maksimal 999 agar tidak keluar lingkaran
  int display_ppm = (gas_ppm > 999) ? 999 : gas_ppm;

  // agar selalu tepat tengah di lingkaran
  u8g2.setFont(u8g2_font_ncenB12_tr);
  int txtW = u8g2.getStrWidth(String(display_ppm).c_str());
  int centerX = 20 + (52 / 2) - (txtW / 2); // lingkaran ~x8..x60
  u8g2.setCursor(centerX, 28);
  u8g2.print(display_ppm);

  u8g2.setFont(u8g2_font_6x10_tf); 
  String status;
  if (gas_ppm < 150) {
    status = "SAFE";
  } else if (gas_ppm < 300) {
    status = "MOD";   
  } else if (gas_ppm < 500) {
    status = "CAUT";  
  } else {
    status = "WARN";  
  }
  
  u8g2.setCursor(40, 50);
  u8g2.print(status);
  u8g2.sendBuffer();
}

// Buzzer
void handleAlarm(int gas_ppm) {
  unsigned long currentMillis = millis();
  
  if (gas_ppm < 150) {
    noTone(BUZZER_PIN);
    digitalWrite(BUZZER_PIN, LOW);
    buzzerState = false;
  } 
  else if (gas_ppm >= 150 && gas_ppm < 300) {
    if (currentMillis - lastBuzzerTime >= 1000) { 
      lastBuzzerTime = currentMillis;
      if (buzzerState) { noTone(BUZZER_PIN); buzzerState = false; } 
      else { tone(BUZZER_PIN, 3500); buzzerState = true; }
    }
  }
  else if (gas_ppm >= 300 && gas_ppm < 500) {
    if (currentMillis - lastBuzzerTime >= 250) { 
      lastBuzzerTime = currentMillis;
      if (buzzerState) { noTone(BUZZER_PIN); buzzerState = false; } 
      else { tone(BUZZER_PIN, 3750); buzzerState = true; }
    }
  }
  else {
    if (currentMillis - lastBuzzerTime >= 100) { 
      lastBuzzerTime = currentMillis;
      if (buzzerState) { noTone(BUZZER_PIN); buzzerState = false; } 
      else { tone(BUZZER_PIN, 4000); buzzerState = true; }
    }
  }
}

// Setup dan Loop
void setup() {
  Serial.begin(115200);
  
  //LCD OLED
  Wire.begin(8, 9); 
  u8g2.begin();
  
  //Pin Sensor dan Buzzer
  pinMode(MQ_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  //Koneksi ke Blynk IoT
  WiFi.begin(ssid, pass);
  Blynk.config(BLYNK_AUTH_TOKEN);
  
  rtc.begin();
  setSyncInterval(10 * 60); 
  timer.setInterval(1000L, sendToBlynk); 
  
  timeStart = millis();
}

void loop() {
  unsigned long currentMillis = millis();

  if (WiFi.status() == WL_CONNECTED) {
    Blynk.run();
  }
  timer.run();
  
  //Screen 1 : Booting
  if (currentMillis < 3000) {
    u8g2.clearBuffer();
    u8g2.drawXBM(38, 3, 53, 43, image_booting);
    u8g2.setDrawColor(1);
    u8g2.setFont(u8g2_font_4x6_tf);
    u8g2.drawStr(30, 63, "Attribution @DIYODEMAG"); 
    u8g2.setFont(u8g2_font_haxrcorp4089_tr); 
    u8g2.drawStr(5, 56, "Programmed by @lamdafnaf"); 
    u8g2.sendBuffer();
  } 
  
  //Screen 2 : Kalibrasi (60 detik)
  else if (currentMillis < WARMUP_TIME) {

    // 1. Baca ADC & Update Ro untuk kalibrasi
    globalRaw = analogRead(MQ_PIN);
    if (globalRaw > 0 && globalRaw < 4095) {
      float volt = globalRaw * (3.3 / 4095.0);
      float rs_air = ((3.3 * RL_VALUE) / volt) - RL_VALUE;
      Ro = rs_air / RO_FACTOR;
    }

    // 2. Deteksi Darurat menggunakan ADC mentah (BUKAN getPPM)
    // Cara ini tidak bergantung pada Ro yang masih berubah-ubah,
    // sehingga hasilnya konsisten dan tidak lompat-lompat.
    // ADC tinggi = tegangan tinggi = Rs rendah = gas terdeteksi.
    bool emergencyDetected = (globalRaw >= emergency);

    if (emergencyDetected) {
      handleAlarm(500); // perlakukan sebagai level WARN langsung
    } else {
      // Pastikan buzzer mati saat tidak ada gas
      noTone(BUZZER_PIN);
      digitalWrite(BUZZER_PIN, LOW);
      buzzerState = false;
    }

    // 3. Kalkulasi Countdown & Progress Bar
    int countdown = (WARMUP_TIME - currentMillis) / 1000;
    if (countdown < 0) countdown = 0;

    int barWidth = map(currentMillis, 3000, WARMUP_TIME, 0, 80);
    barWidth = constrain(barWidth, 0, 80);

    // 4. Gambar ke OLED
    u8g2.clearBuffer();
    u8g2.setBitmapMode(1);
    u8g2.setFontMode(1);

    if (emergencyDetected) {
      // --- Tampilan Darurat ---
      u8g2.setDrawColor(1);
      // Header berkedip setiap 300ms
      if ((currentMillis / 300) % 2 == 0) {
        u8g2.drawBox(0, 0, 128, 15);
        u8g2.setDrawColor(2);
      }
      u8g2.setFont(u8g2_font_6x13B_tr);
      u8g2.drawStr(2, 12, "!! GAS DETECTED !!");
      u8g2.setDrawColor(1);

      // Info ADC mentah sebagai petunjuk level
      u8g2.setFont(u8g2_font_6x10_tf);
      char rawBuf[20];
      sprintf(rawBuf, "RAW:%d-WARN!", globalRaw);
      u8g2.drawStr(2, 51, rawBuf);
      u8g2.setFont(u8g2_font_6x13B_tr);
      u8g2.drawStr(2, 62, "Sensor MQ-6");

    } else {
      // --- Tampilan Normal ---
      u8g2.setFont(u8g2_font_6x13B_tr);
      u8g2.drawStr(2, 14, "Auto-Calibrating");
      u8g2.drawStr(2, 51, "Sensor MQ-5");
      u8g2.setFont(u8g2_font_6x13O_tr);
      u8g2.drawStr(10, 62, "Warming Up");
    }

    // Frame progress bar & ikon (selalu tampil)
    u8g2.setDrawColor(1);
    u8g2.drawXBM(-10, 20, 109, 25, image_progress);
    u8g2.drawXBM(87, 20, 39, 43, image_gas);
    u8g2.drawBox(5, 28, barWidth, 8);

    // Countdown (XOR agar selalu kontras)
    u8g2.setDrawColor(2);
    u8g2.setFont(u8g2_font_6x10_tf);
    char cdBuf[6];
    sprintf(cdBuf, "%ds", countdown);
    u8g2.drawStr(37, 35, cdBuf);

    u8g2.sendBuffer();
  }
  
  //Screen 3: Dashboard PPM
  else {
    ppm = getPPM();
    drawDashboard(ppm);
    handleAlarm(ppm);
  }
}