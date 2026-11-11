// =============================================
// ТЕСТОВЫЙ СКЕТЧ ДЛЯ ПРОВЕРКИ ДАТЧИКОВ
// Quest Box — Robotics Lab
// =============================================
// Пины:
//   Звук (аналог)   — A15
//   DHT11 (темп/влаж) — 52
//   Датчик воды     — 53
//   LCD I2C         — SDA/SCL (20/21 на Mega)
// =============================================

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "DHT.h"

// --- Пины датчиков ---
#define SOUND_PIN   A15
#define DHTPIN      52
#define WATER_PIN   53
#define DHTTYPE     DHT11

// --- LCD ---
LiquidCrystal_I2C lcd(0x27, 16, 2);

// --- DHT ---
DHT dht(DHTPIN, DHTTYPE);

// --- Интервал вывода ---
const unsigned long INTERVAL = 1000; // мс
unsigned long lastPrint = 0;

void setup() {
  Serial.begin(9600);
  Serial.println("=== SENSOR TEST ===");
  Serial.println("Pins: Sound=A15  DHT11=52  Water=53");
  Serial.println("-------------------");

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("SENSOR TEST...");

  pinMode(WATER_PIN, INPUT);

  dht.begin();

  delay(2000);
  lcd.clear();
}

void loop() {
  unsigned long now = millis();
  if (now - lastPrint < INTERVAL) return;
  lastPrint = now;

  // --- Чтение ---
  int   sound   = analogRead(SOUND_PIN);
  float temp    = dht.readTemperature();
  float hum     = dht.readHumidity();
  int   water   = digitalRead(WATER_PIN); // HIGH = вода есть

  // --- Serial Monitor ---
  Serial.println("===================");
  Serial.print("Sound  (A15): "); Serial.print(sound);
  if (sound >= 500) Serial.println("  <- TRIGGERED");
  else              Serial.println("  (quiet)");

  if (isnan(temp) || isnan(hum)) {
    Serial.println("DHT11  (52) : ERROR — проверь подключение");
  } else {
    Serial.print("Temp   (52) : "); Serial.print(temp); Serial.println(" C");
    Serial.print("Humid  (52) : "); Serial.print(hum);  Serial.println(" %");
  }

  Serial.print("Water  (53) : ");
  Serial.println(water == HIGH ? "HIGH (есть вода)" : "LOW  (сухо)");

  // --- LCD ---
  lcd.clear();

  // Строка 0: звук и вода
  lcd.setCursor(0, 0);
  lcd.print("S:");
  lcd.print(sound);
  lcd.print(sound >= 500 ? "!" : " ");
  lcd.print(" W:");
  lcd.print(water == HIGH ? "WET" : "DRY");

  // Строка 1: температура и влажность
  lcd.setCursor(0, 1);
  if (isnan(temp) || isnan(hum)) {
    lcd.print("DHT ERR!");
  } else {
    lcd.print("T:");
    lcd.print((int)temp);
    lcd.print("C H:");
    lcd.print((int)hum);
    lcd.print("%");
  }
}
