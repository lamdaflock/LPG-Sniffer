#include <U8g2lib.h>
#include <Wire.h>
#include "Images.h" 
// Last edited on 14/02/2026 18:37
// ==========================================
// --- KONFIGURASI BOARD (EDIT DISINI) ---
// ==========================================

// 1. PIN SENSOR (MQ_PIN)
// Wemos D1 R32     : Gunakan 36 (VP) atau 34/35 jika 36 error
// ESP32 C3 S.Mini  : Gunakan 0
#define MQ_PIN      0   

// 2. PIN BUZZER
// Wemos D1 R32     : 2
// ESP32 C3 S.Mini  : 2
#define BUZZER_PIN  2     

// ==========================================
// --- KONFIGURASI LAYAR ---
// ==========================================
// Gunakan driver yang sama persis dengan v.1 Anda agar tidak berantakan
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

// ==========================================
// --- VARIABEL ILMIAH (SCIENTIFIC MATH) ---
// ==========================================
const float RL_VALUE = 3.3;      // Resistor Beban Anda (3x 10k Paralel = 3.3k)
const float RO_FACTOR = 6.5;     // Konstanta udara bersih MQ-5
const float CALIB_A = 574.25;    // Konstanta Kurva Gas LPG (a)
const float CALIB_B = -2.222;    // Konstanta Kurva Gas LPG (b)

float Ro = 10.0;                 // Nilai Ro (akan otomatis dikalibrasi saat loading)
int ppm = 0;                     // Hasil akhir PPM

// Variabel Waktu
const long WARMUP_TIME = 60000;  // 60 Detik Loading
unsigned long timeStart;        

// Variabel Buzzer
unsigned long lastBuzzerTime = 0;
bool buzzerState = false;

void setup() {
  Serial.begin(115200);
  
  pinMode(MQ_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  
  // ==========================================
  // --- SETTING I2C BOARD (EDIT DISINI) ---
  // ==========================================
  // Wemos D1 R32     : Wire.begin(21, 22);
  // ESP32 C3 S.Mini  : Wire.begin(8, 9);
  Wire.begin(8, 9); 
  
  u8g2.begin();
  timeStart = millis();
}

void loop() {
  unsigned long currentMillis = millis();
  
  // --- SCREEN 1: BOOTING (0 - 3 Detik) ---
  if (currentMillis < 3000) {
    u8g2.clearBuffer();
    // Koordinat asli dari v.1 Anda (JANGAN DIUBAH)
    u8g2.drawXBM(38, 3, 53, 43, image_Logo_Baru_bits);
    
    u8g2.setDrawColor(1);
    u8g2.setFont(u8g2_font_4x6_tf);
    u8g2.drawStr(30, 63, "Attribution @DIYODEMAG"); 
    
    u8g2.setFont(u8g2_font_haxrcorp4089_tr); // Font kecil
    u8g2.drawStr(5, 56, "Programmed by @lamdafnaf"); 
    u8g2.sendBuffer();
  } 
  
  // --- SCREEN 2: LOADING & KALIBRASI (3 - 60 Detik) ---
  else if (currentMillis < WARMUP_TIME) {
    
    // >> LOGIKA KALIBRASI ILMIAH DISINI <<
    // Kita ambil sampel udara bersih selama loading berlangsung
    int val = analogRead(MQ_PIN);
    if(val > 0 && val < 4095) {
       // Konversi ADC ke Volt -> Hitung Rs -> Hitung Ro
       float volt = val * (3.3 / 4095.0);
       float rs_air = ((3.3 * RL_VALUE) / volt) - RL_VALUE;
       Ro = rs_air / RO_FACTOR; // Simpan Ro rata-rata
    }

    // Tampilan Visual (Tetap seperti v.1)
    int progress = map(currentMillis, 3000, WARMUP_TIME, 0, 100);
    u8g2.clearBuffer();
    u8g2.drawXBM(0, 0, 128, 64, image_Loading_Screen_4_bits);
    
    int barWidth = map(progress, 0, 100, 0, 75); 
    u8g2.setDrawColor(1);
    u8g2.drawBox(5, 34, barWidth, 10); 
    
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.setCursor(45, 30);
    u8g2.print(progress); u8g2.print("%");
    
    u8g2.sendBuffer();
  } 
  
  // --- SCREEN 3: DASHBOARD UTAMA (Setelah 60 Detik) ---
  else {
    // 1. Hitung PPM dengan Rumus Power Law
    ppm = getScientificPPM();
    
    // 2. Tampilkan Dashboard
    drawDashboard(ppm);
    
    // 3. Bunyikan Alarm
    handleAlarm(ppm);
  }
}

// --- RUMUS BARU: HITUNG PPM ILMIAH ---
int getScientificPPM() {
  int raw = analogRead(MQ_PIN);
  
  // Filter jika sensor error/putus
  if (raw <= 10) return 0;       
  if (raw >= 4095) return 9999; 

  // Langkah 1: Hitung Voltase
  float voltage = raw * (3.3 / 4095.0);
  
  // Langkah 2: Hitung Rs (Resistansi Sensor saat ini)
  float Rs = ((3.3 * RL_VALUE) / voltage) - RL_VALUE;
  
  // Langkah 3: Hitung Rasio
  float ratio = Rs / Ro;
  
  // Langkah 4: Rumus Power Law (PPM = a * ratio^b)
  float ppm_calc = CALIB_A * pow(ratio, CALIB_B);
  
  if (ppm_calc < 0) ppm_calc = 0;
  return (int)ppm_calc;
}

// --- TAMPILAN DASHBOARD ---
void drawDashboard(int gas_ppm) {
  u8g2.clearBuffer();
  // Gambar Background (Koordinat v.1)
  u8g2.drawXBM(0, 0, 129, 64, image_Dashboard_Main_bits);

  // ANGKA PPM
  u8g2.setFont(u8g2_font_ncenB12_tr);
  u8g2.setCursor(38, 28);
  u8g2.print(gas_ppm);

  // STATUS TEXT (4 KATEGORI: SAFE, MOD, CAUT, WARN)
  u8g2.setFont(u8g2_font_6x10_tf); // Font tebal
  String status;
  
  if (gas_ppm < 150) {
    status = "SAFE";
  } else if (gas_ppm < 300) {
    status = "MOD";   // Moderate
  } else if (gas_ppm < 500) {
    status = "CAUT";  // Caution
  } else {
    status = "WARN";  // Warning
  }
  
  u8g2.setCursor(40, 50);
  u8g2.print(status);

  // Debug Raw (Kecil di pojok)
  // u8g2.setFont(u8g2_font_4x6_tf);
  // u8g2.setCursor(100, 6);
  // u8g2.print(analogRead(MQ_PIN));

  u8g2.sendBuffer();
}

// --- LOGIKA ALARM (4 TINGKAT) ---
void handleAlarm(int gas_ppm) {
  unsigned long currentMillis = millis();

  // 1. SAFE (<150) -> MATI
  if (gas_ppm < 150) {
    noTone(BUZZER_PIN);
    digitalWrite(BUZZER_PIN, LOW);
    buzzerState = false;
  } 
  // 2. MODERATE (150-300) -> Pelan (2 detik sekali)
  else if (gas_ppm >= 150 && gas_ppm < 300) {
    if (currentMillis - lastBuzzerTime >= 1000) { 
      lastBuzzerTime = currentMillis;
      if (buzzerState) { noTone(BUZZER_PIN); buzzerState = false; } 
      else { tone(BUZZER_PIN, 500); buzzerState = true; }
    }
  }
  // 3. CAUTION (300-500) -> Sedang (0.5 detik sekali)
  else if (gas_ppm >= 300 && gas_ppm < 500) {
    if (currentMillis - lastBuzzerTime >= 250) { 
      lastBuzzerTime = currentMillis;
      if (buzzerState) { noTone(BUZZER_PIN); buzzerState = false; } 
      else { tone(BUZZER_PIN, 1000); buzzerState = true; }
    }
  }
  // 4. WARN (>500) -> Cepat (0.1 detik sekali)
  else {
    if (currentMillis - lastBuzzerTime >= 100) { 
      lastBuzzerTime = currentMillis;
      if (buzzerState) { noTone(BUZZER_PIN); buzzerState = false; } 
      else { tone(BUZZER_PIN, 2500); buzzerState = true; }
    }
  }
}