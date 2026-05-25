#include <U8g2lib.h>
#include <Wire.h>
#include "Images.h" 
// Last edited on 14/02/2026 18:37

#define MQ_PIN      0   
#define BUZZER_PIN  2     

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

const float RL_VALUE = 3.3;
const float RO_FACTOR = 6.5;
const float CALIB_A = 574.25;
const float CALIB_B = -2.222;

float Ro = 10.0;
int ppm = 0;

const long WARMUP_TIME = 60000;
unsigned long timeStart;        

unsigned long lastBuzzerTime = 0;
bool buzzerState = false;

void setup() {
  Serial.begin(115200);
  
  pinMode(MQ_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  
  Wire.begin(8, 9);
  u8g2.begin();
  timeStart = millis();
}

void loop() {
  unsigned long currentMillis = millis();
  
  if (currentMillis < 3000) {
    u8g2.clearBuffer();
    u8g2.drawXBM(38, 3, 53, 43, image_Logo_Baru_bits);
    
    u8g2.setDrawColor(1);
    u8g2.setFont(u8g2_font_4x6_tf);
    u8g2.drawStr(30, 63, "Attribution @DIYODEMAG"); 
    
    u8g2.setFont(u8g2_font_haxrcorp4089_tr);
    u8g2.drawStr(5, 56, "Programmed by @lamdafnaf"); 
    u8g2.sendBuffer();
  } 
  
  else if (currentMillis < WARMUP_TIME) {
    int val = analogRead(MQ_PIN);
    if(val > 0 && val < 4095) {
       float volt = val * (3.3 / 4095.0);
       float rs_air = ((3.3 * RL_VALUE) / volt) - RL_VALUE;
       Ro = rs_air / RO_FACTOR;
    }
    
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
  
  else {
    ppm = getScientificPPM();
    drawDashboard(ppm);
    handleAlarm(ppm);
  }
}

int getScientificPPM() {
  int raw = analogRead(MQ_PIN);
  
  if (raw <= 10) return 0;       
  if (raw >= 4095) return 9999; 
  float voltage = raw * (3.3 / 4095.0);
  float Rs = ((3.3 * RL_VALUE) / voltage) - RL_VALUE;
  float ratio = Rs / Ro;
  float ppm_calc = CALIB_A * pow(ratio, CALIB_B);
  
  if (ppm_calc < 0) ppm_calc = 0;
  return (int)ppm_calc;
}

void drawDashboard(int gas_ppm) {
  u8g2.clearBuffer();
  u8g2.drawXBM(0, 0, 129, 64, image_Dashboard_Main_bits);

  u8g2.setFont(u8g2_font_ncenB12_tr);
  u8g2.setCursor(38, 28);
  u8g2.print(gas_ppm);

  u8g2.setFont(u8g2_font_6x10_tf);
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

  u8g2.sendBuffer();
}

void handleAlarm(int gas_ppm) {
  unsigned long currentMillis = millis();

  // Safe
  if (gas_ppm < 150) {
    noTone(BUZZER_PIN);
    digitalWrite(BUZZER_PIN, LOW);
    buzzerState = false;
  }
  // Moderate
  else if (gas_ppm >= 150 && gas_ppm < 300) {
    if (currentMillis - lastBuzzerTime >= 1000) { 
      lastBuzzerTime = currentMillis;
      if (buzzerState) { noTone(BUZZER_PIN); buzzerState = false; } 
      else { tone(BUZZER_PIN, 500); buzzerState = true; }
    }
  }
  // Caution
  else if (gas_ppm >= 300 && gas_ppm < 500) {
    if (currentMillis - lastBuzzerTime >= 250) { 
      lastBuzzerTime = currentMillis;
      if (buzzerState) { noTone(BUZZER_PIN); buzzerState = false; } 
      else { tone(BUZZER_PIN, 1000); buzzerState = true; }
    }
  }
  // Warning
  else {
    if (currentMillis - lastBuzzerTime >= 100) { 
      lastBuzzerTime = currentMillis;
      if (buzzerState) { noTone(BUZZER_PIN); buzzerState = false; } 
      else { tone(BUZZER_PIN, 2500); buzzerState = true; }
    }
  }
}
