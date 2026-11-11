/*
  ============================================================
   QUEST BOX 4 — обезвреживание бомбы в стиле KTANE
   Arduino Mega 2560. Правила — в «Руководстве по обезвреживанию».
  ============================================================
   Коды доступа:
     4091 ALPHA   8254 BRAVO   1337 CHARLIE  9021 DELTA
     5555 ECHO    6606 OMEGA   7392 ZETA
     0000 — режим проверки железа (провода, кнопки, микрофон, индикатор)
   Удержание D 3 секунды — сброс в меню (в игре и в режиме проверки).
   Ответы на все модули печатаются в Serial (9600) — для ведущего.
*/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>

// ================== ПОДКЛЮЧЕНИЕ ==================
LiquidCrystal_I2C lcd(0x27, 16, 2);

const byte ROWS = 4, COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {22, 23, 24, 25};
byte colPins[COLS] = {26, 27, 28, 29};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

const uint8_t BTN_PINS[5]  = {30, 32, 34, 36, 42};    // нажата = HIGH (внешние резисторы на землю)
const uint8_t LED_PINS[5]  = {31, 33, 35, 37, 39};
const uint8_t NUM_WIRES    = 6;
const uint8_t WIRE_PINS[6] = {43, 44, 45, 46, 47, 48}; // INPUT_PULLUP, вынут = HIGH
const uint8_t BUZZER       = 49;
const uint8_t SOUND_PIN    = A15;
const uint8_t SEG_PINS[8]  = {2, 3, 4, 5, 6, 7, 8, 9};  // a b c d e f g dp
const uint8_t DIG_PINS[4]  = {10, 11, 12, 13};          // разряд включён = LOW

// ================== НАСТРОЙКИ ==================
const int  SOUND_DELTA      = 150;  // насколько сигнал микрофона должен отклониться от фона (подбирается в режиме 0000)
const unsigned long CLAP_GAP_MS = 300;
const unsigned long BTN_DEBOUNCE_MS  = 25;
const unsigned long WIRE_DEBOUNCE_MS = 40;

struct Level {
  const char* code;
  const char* name;
  uint8_t  modules;
  uint16_t seconds;
  uint8_t  maxStrikes;
  uint8_t  echoStages;
  uint8_t  cipherLetters;
  uint8_t  needyMin;      // 0 = прослушки нет
  uint8_t  needyMax;
  bool     training;      // только Провода, Кнопка, Морзе, Шифр
};

const Level LEVELS[] = {
  {"4091", "ALPHA",   3, 360, 3, 3, 1,  0,  0, true },
  {"8254", "BRAVO",   4, 360, 3, 3, 1,  0,  0, false},
  {"1337", "CHARLIE", 5, 420, 3, 4, 1, 60, 90, false},
  {"9021", "DELTA",   6, 420, 3, 4, 2, 60, 90, false},
  {"5555", "ECHO",    7, 480, 3, 5, 2, 60, 90, false},
  {"6606", "OMEGA",   7, 390, 2, 5, 2, 40, 60, false},
  {"7392", "ZETA",    7, 330, 1, 5, 3, 40, 60, false},
};
const uint8_t LEVEL_COUNT = sizeof(LEVELS) / sizeof(LEVELS[0]);
const uint8_t LEVEL_TEST  = 254;
const uint8_t LEVEL_NONE  = 255;

enum ModuleId : uint8_t { M_WIRES, M_ECHO, M_BUTTON, M_MORSE, M_SYMBOLS, M_PASSWORD, M_CIPHER, M_COUNT };
const char* const MODULE_NAMES[M_COUNT] = {"WIRES", "ECHO", "BUTTON", "MORSE", "SYMBOLS", "PASSWORD", "CIPHER"};

// ================== СОСТОЯНИЕ ИГРЫ ==================
const Level* lvl = nullptr;
long timeLeftMs = 0;
unsigned long lastTickMs = 0;
unsigned long tickAcc = 0;
uint8_t strikes = 0, maxStrikes = 3;
bool gameRunning = false, testRunning = false;
bool exploded = false, resetRequested = false;
bool lcdDirty = true;
char serialNo[7];
uint8_t moduleIndex = 0, moduleCount = 0;
uint8_t moduleOrder[M_COUNT];
unsigned long seedBase = 0;

// прослушка
bool inNeedy = false;
uint8_t needySuppress = 0;
unsigned long nextNeedyAt = 0;

// ================== 7-СЕГМЕНТНЫЙ ИНДИКАТОР (по прерыванию) ==================
const uint8_t DIGITS[10] = {
  B11111100, B01100000, B11011010, B11110010, B01100110,
  B10110110, B10111110, B11100000, B11111110, B11110110
};
const uint8_t SEG_DP   = B00000001;
const uint8_t SEG_DASH = B00000010;

volatile uint8_t segBuf[4] = {0, 0, 0, 0};

enum DispMode : uint8_t { DISP_DASH, DISP_TIME, DISP_BOOM, DISP_ALL };
DispMode dispMode = DISP_DASH;
unsigned long flashUntil = 0;

ISR(TIMER1_COMPA_vect) {
  static uint8_t d = 0;
  for (uint8_t i = 0; i < 4; i++) digitalWrite(DIG_PINS[i], HIGH);
  uint8_t p = segBuf[d];
  for (uint8_t i = 0; i < 8; i++) digitalWrite(SEG_PINS[i], (p >> (7 - i)) & 1);
  digitalWrite(DIG_PINS[d], LOW);
  d = (d + 1) & 3;
}

void setupDisplayTimer() {
  noInterrupts();
  TCCR1A = 0;
  TCCR1B = 0;
  TCNT1  = 0;
  OCR1A  = 499;                                       // 16 МГц / 64 / 500 = 500 Гц → 125 Гц на разряд
  TCCR1B |= (1 << WGM12) | (1 << CS11) | (1 << CS10); // CTC, делитель 64
  TIMSK1 |= (1 << OCIE1A);
  interrupts();
}

void timerDigits(uint8_t out[4]) {
  long s = timeLeftMs / 1000;
  if (s < 0) s = 0;
  long mm = s / 60;
  if (mm > 99) mm = 99;
  long ss = s % 60;
  out[0] = mm / 10; out[1] = mm % 10; out[2] = ss / 10; out[3] = ss % 10;
}

void refreshDisplay() {
  uint8_t b[4];
  unsigned long now = millis();
  switch (dispMode) {
    case DISP_DASH:
      for (uint8_t i = 0; i < 4; i++) b[i] = SEG_DASH;
      break;
    case DISP_ALL:
      for (uint8_t i = 0; i < 4; i++) b[i] = 0xFF;
      break;
    case DISP_BOOM: {
      uint8_t p = ((now / 250) % 2) ? (DIGITS[8] | SEG_DP) : 0;
      for (uint8_t i = 0; i < 4; i++) b[i] = p;
      break;
    }
    case DISP_TIME: {
      if (now < flashUntil) {
        uint8_t p = ((now / 100) % 2) ? SEG_DASH : 0;
        for (uint8_t i = 0; i < 4; i++) b[i] = p;
        break;
      }
      uint8_t d[4];
      timerDigits(d);
      for (uint8_t i = 0; i < 4; i++) b[i] = DIGITS[d[i]];
      b[1] |= SEG_DP;                                  // точка второго разряда = двоеточие
      if (inNeedy && ((now / 250) % 2)) for (uint8_t i = 0; i < 4; i++) b[i] = 0;
      break;
    }
  }
  noInterrupts();
  for (uint8_t i = 0; i < 4; i++) segBuf[i] = b[i];
  interrupts();
}

// ================== ВВОД ==================
bool btnRaw[5], btnStable[5], btnPressEv[5], btnReleaseEv[5];
unsigned long btnChange[5];
bool wireRaw[NUM_WIRES], wireStable[NUM_WIRES], wirePullEv[NUM_WIRES];
unsigned long wireChange[NUM_WIRES];
char pendingKey = 0;
int soundBase = 512;
unsigned long micMuteUntil = 0;

void pollInputs() {
  unsigned long now = millis();
  for (uint8_t i = 0; i < 5; i++) {
    bool r = digitalRead(BTN_PINS[i]) == HIGH;
    if (r != btnRaw[i]) { btnRaw[i] = r; btnChange[i] = now; }
    else if (r != btnStable[i] && now - btnChange[i] >= BTN_DEBOUNCE_MS) {
      btnStable[i] = r;
      if (r) btnPressEv[i] = true; else btnReleaseEv[i] = true;
    }
  }
  for (uint8_t i = 0; i < NUM_WIRES; i++) {
    bool r = digitalRead(WIRE_PINS[i]) == HIGH;
    if (r != wireRaw[i]) { wireRaw[i] = r; wireChange[i] = now; }
    else if (r != wireStable[i] && now - wireChange[i] >= WIRE_DEBOUNCE_MS) {
      wireStable[i] = r;
      if (r) wirePullEv[i] = true;
    }
  }
  char k = keypad.getKey();
  if (k) pendingKey = k;
  if (gameRunning || testRunning) {
    for (uint8_t i = 0; i < LIST_MAX; i++) {
      if (keypad.key[i].kchar == 'D' && keypad.key[i].kstate == HOLD) resetRequested = true;
    }
  }
}

void clearEvents() {
  for (uint8_t i = 0; i < 5; i++) { btnPressEv[i] = false; btnReleaseEv[i] = false; }
  for (uint8_t i = 0; i < NUM_WIRES; i++) wirePullEv[i] = false;
  pendingKey = 0;
}

bool anyWirePulled() {
  for (uint8_t i = 0; i < NUM_WIRES; i++) if (wireStable[i]) return true;
  return false;
}

void initInputStates() {
  for (uint8_t i = 0; i < 5; i++) {
    btnRaw[i] = btnStable[i] = digitalRead(BTN_PINS[i]) == HIGH;
    btnChange[i] = millis();
  }
  for (uint8_t i = 0; i < NUM_WIRES; i++) {
    wireRaw[i] = wireStable[i] = digitalRead(WIRE_PINS[i]) == HIGH;
    wireChange[i] = millis();
  }
  clearEvents();
}

// ================== ЗВУК И МИКРОФОН ==================
void beep(unsigned int f, unsigned long d) {
  tone(BUZZER, f, d);
  micMuteUntil = millis() + d + 400;
}
void keyClick() { beep(1000, 30); }

void calibrateSound() {
  long sum = 0;
  for (uint8_t i = 0; i < 64; i++) { sum += analogRead(SOUND_PIN); delay(2); }
  soundBase = sum / 64;
}

bool soundHit() {
  if (millis() < micMuteUntil) return false;
  int v = analogRead(SOUND_PIN);
  return abs(v - soundBase) > SOUND_DELTA;
}

// ================== СВЕТОДИОДЫ И ЭКРАН ==================
void setLed(uint8_t i, bool on) { digitalWrite(LED_PINS[i], on ? HIGH : LOW); }
void allLedsOff() { for (uint8_t i = 0; i < 5; i++) setLed(i, false); }

void lcdLine(uint8_t row, const char* s) {
  char buf[17];
  uint8_t n = strlen(s);
  if (n > 16) n = 16;
  memcpy(buf, s, n);
  for (uint8_t i = n; i < 16; i++) buf[i] = ' ';
  buf[16] = 0;
  lcd.setCursor(0, row);
  lcd.print(buf);
}

void lcdTwo(const char* a, const char* b) { lcdLine(0, a); lcdLine(1, b); }

// ================== СЕРИЙНЫЙ НОМЕР ==================
uint8_t snLastDigit() { return serialNo[5] - '0'; }
bool snHasVowel() {
  for (uint8_t i = 0; i < 6; i++) {
    char c = serialNo[i];
    if (c == 'A' || c == 'E' || c == 'I' || c == 'U') return true;
  }
  return false;
}
void generateSerial() {
  const char chars[] = "ABCDEFGHIJKLMNPQRSTUVWXYZ0123456789";  // без O
  for (uint8_t i = 0; i < 5; i++) serialNo[i] = chars[random(35)];
  serialNo[5] = '0' + random(10);
  serialNo[6] = 0;
}

// ================== ИГРОВОЙ ЦИКЛ ==================
void runNeedy();

// Вызывается в каждом цикле ожидания. false = игра закончилась (взрыв или сброс).
bool service() {
  unsigned long now = millis();
  if (gameRunning) {
    unsigned long dt = now - lastTickMs;
    lastTickMs = now;
    tickAcc += dt * (4UL + strikes);        // каждая ошибка +25% к скорости
    timeLeftMs -= (long)(tickAcc / 4);
    tickAcc %= 4;
    if (timeLeftMs <= 0) { timeLeftMs = 0; exploded = true; }
  }
  refreshDisplay();
  pollInputs();
  if (exploded || resetRequested) return false;
  if (gameRunning && lvl && lvl->needyMin && !inNeedy && needySuppress == 0 && now >= nextNeedyAt) {
    runNeedy();
    if (exploded || resetRequested) return false;
  }
  return true;
}

bool waitMs(unsigned long ms) {
  unsigned long t = millis();
  while (millis() - t < ms) if (!service()) return false;
  return true;
}

void drawPassport() {
  char l0[17], l1[17];
  snprintf(l0, sizeof(l0), "SN:%s", serialNo);
  snprintf(l1, sizeof(l1), "ERR:%u/%u  M:%u/%u", strikes, maxStrikes, moduleIndex + 1, moduleCount);
  lcd.clear();
  lcdTwo(l0, l1);
}

void showPassport(unsigned long ms) {
  drawPassport();
  needySuppress++;
  waitMs(ms);
  needySuppress--;
  lcdDirty = true;
}

// Забирает нажатую клавишу. 'A' открывает паспорт, 'D' игнорируется (длинное D = сброс).
char takeKey(bool passportAllowed = true) {
  char k = pendingKey;
  pendingKey = 0;
  if (!k || k == 'D') return 0;
  if (passportAllowed && k == 'A') { showPassport(3000); return 0; }
  return k;
}

void strike() {
  strikes++;
  Serial.print(F("STRIKE ")); Serial.print(strikes); Serial.print('/'); Serial.println(maxStrikes);
  flashUntil = millis() + 700;
  allLedsOff();
  noTone(BUZZER);
  beep(180, 600);
  if (strikes >= maxStrikes) { exploded = true; return; }
  char l1[17];
  snprintf(l1, sizeof(l1), "ERROR %u/%u", strikes, maxStrikes);
  lcd.clear();
  lcdTwo("!!! STRIKE !!!", l1);
  needySuppress++;
  waitMs(900);
  needySuppress--;
  clearEvents();
  lcdDirty = true;
}

// ================== МОДУЛЬ 8: ПРОСЛУШКА ==================
void scheduleNeedy() {
  if (!lvl || !lvl->needyMin) return;
  nextNeedyAt = millis() + (unsigned long)random(lvl->needyMin, lvl->needyMax + 1) * 1000UL;
}

bool playNeedySignal(uint8_t type) {
  // 0: 3 коротких, 1: 2 коротких, 2: длинный, 3: короткий+длинный, 4: длинный+короткий
  const uint16_t S = 150, L = 600;
  uint16_t seq[3]; uint8_t n = 0;
  switch (type) {
    case 0: seq[0] = S; seq[1] = S; seq[2] = S; n = 3; break;
    case 1: seq[0] = S; seq[1] = S; n = 2; break;
    case 2: seq[0] = L; n = 1; break;
    case 3: seq[0] = S; seq[1] = L; n = 2; break;
    default: seq[0] = L; seq[1] = S; n = 2; break;
  }
  for (uint8_t i = 0; i < n; i++) {
    beep(1400, seq[i]);
    if (!waitMs(seq[i] + 200)) return false;
  }
  return true;
}

void runNeedy() {
  inNeedy = true;
  noTone(BUZZER);
  allLedsOff();
  uint8_t type = random(5);
  Serial.print(F("LISTEN signal type ")); Serial.println(type);

  lcd.clear();
  lcdTwo("LISTEN 10", "");
  if (!playNeedySignal(type)) { inNeedy = false; return; }

  unsigned long start = millis();
  unsigned long lastClap = 0;
  uint8_t count = 0;
  int8_t shownSec = -1;
  int8_t shownCount = -1;
  pendingKey = 0;
  while (millis() - start < 10000UL) {
    if (!service()) { inNeedy = false; return; }
    int8_t sec = 10 - (millis() - start) / 1000;
    if (sec != shownSec || count != shownCount) {
      char l0[17], l1[17];
      snprintf(l0, sizeof(l0), "LISTEN %2d", sec);
      snprintf(l1, sizeof(l1), "HEARD: %u", count);
      lcdTwo(l0, l1);
      shownSec = sec; shownCount = count;
    }
    char k = pendingKey; pendingKey = 0;
    if (k == 'B') { if (!playNeedySignal(type)) { inNeedy = false; return; } }
    if (soundHit() && millis() - lastClap > CLAP_GAP_MS) { count++; lastClap = millis(); }
  }

  uint8_t required;
  switch (type) {
    case 0: required = 1; break;
    case 1: required = 2; break;
    case 2: required = 0; break;
    case 3: required = (snLastDigit() % 2 == 0) ? 1 : 3; break;
    default: required = strikes + 1; break;
  }
  Serial.print(F("LISTEN heard ")); Serial.print(count); Serial.print(F(", need ")); Serial.println(required);
  if (count == required) beep(2000, 200);
  else strike();

  inNeedy = false;
  clearEvents();
  lcdDirty = true;
  scheduleNeedy();
}

// ================== МОДУЛЬ 1: ПРОВОДА ==================
// st: 0 = тёмный, 1 = горит, 2 = мигает. Возвращает номер провода 1..6.
uint8_t wiresAnswer(const uint8_t st[5]) {
  uint8_t dark = 0, blink = 0;
  for (uint8_t i = 0; i < 5; i++) { if (st[i] == 0) dark++; if (st[i] == 2) blink++; }
  if (dark == 0) return 6;
  if (blink == 0) { for (int8_t i = 4; i >= 0; i--) if (st[i] == 1) return i + 1; }
  if (blink == 1) {
    uint8_t idx = 0;
    for (uint8_t i = 0; i < 5; i++) if (st[i] == 2) idx = i;
    return (snLastDigit() % 2 == 1) ? idx + 1 : idx + 2;
  }
  if (blink == 2) return dark + 1;
  if (snHasVowel()) { for (uint8_t i = 0; i < 5; i++) if (st[i] == 0) return i + 1; }
  return strikes == 0 ? 2 : 4;
}

bool waitWiresConnected(const char* msg) {
  lcdDirty = true;
  while (anyWirePulled()) {
    if (!service()) { allLedsOff(); return false; }
    takeKey();
    if (lcdDirty) { lcd.clear(); lcdTwo("WIRES", msg); lcdDirty = false; }
  }
  clearEvents();
  return true;
}

bool modWires() {
  uint8_t st[5], dark;
  do {
    dark = 0;
    for (uint8_t i = 0; i < 5; i++) { st[i] = random(3); if (st[i] == 0) dark++; }
  } while (dark == 5);
  Serial.print(F("WIRES leds "));
  for (uint8_t i = 0; i < 5; i++) Serial.print(st[i] == 0 ? '.' : (st[i] == 1 ? 'O' : '*'));
  Serial.print(F(" -> cut ")); Serial.println(wiresAnswer(st));

  if (!waitWiresConnected("CONNECT WIRES")) return false;
  lcdDirty = true;
  while (true) {
    if (!service()) { allLedsOff(); return false; }
    takeKey();
    if (lcdDirty) { lcd.clear(); lcdTwo("WIRES", "CUT ONE WIRE"); lcdDirty = false; }
    bool blinkOn = (millis() / 250) % 2;
    for (uint8_t i = 0; i < 5; i++) setLed(i, st[i] == 1 || (st[i] == 2 && blinkOn));

    for (uint8_t w = 0; w < NUM_WIRES; w++) {
      if (!wirePullEv[w]) continue;
      wirePullEv[w] = false;
      uint8_t ans = wiresAnswer(st);
      allLedsOff();
      if (w + 1 == ans) {
        beep(2000, 300);
        return waitWiresConnected("RECONNECT WIRE");
      }
      strike();
      char msg[17];
      snprintf(msg, sizeof(msg), "RECONNECT %u", w + 1);
      if (!waitWiresConnected(msg)) return false;
      Serial.print(F("WIRES now cut ")); Serial.println(wiresAnswer(st));
      lcdDirty = true;
      break;
    }
  }
}

// ================== МОДУЛЬ 2: ЭХО ==================
// [есть гласная?0:1][ошибки 0/1/2+][вспыхнул 1..5] -> кнопка 1..5
const uint8_t ECHO_MAP[2][3][5] = {
  { {3, 5, 1, 2, 4}, {2, 4, 5, 1, 3}, {5, 1, 4, 3, 2} },   // есть гласная
  { {4, 1, 2, 5, 3}, {5, 3, 1, 2, 4}, {2, 4, 5, 3, 1} }    // нет гласных
};
const unsigned int ECHO_FREQ[5] = {440, 523, 659, 784, 988};

uint8_t echoExpect(uint8_t flashed) {   // flashed 0..4 -> индекс кнопки 0..4
  uint8_t s = strikes > 2 ? 2 : strikes;
  return ECHO_MAP[snHasVowel() ? 0 : 1][s][flashed] - 1;
}

bool echoPlay(const uint8_t* seq, uint8_t len) {
  allLedsOff();
  if (!waitMs(300)) return false;
  for (uint8_t i = 0; i < len; i++) {
    setLed(seq[i], true);
    beep(ECHO_FREQ[seq[i]], 300);
    if (!waitMs(350)) { allLedsOff(); return false; }
    setLed(seq[i], false);
    if (!waitMs(250)) return false;
  }
  for (uint8_t i = 0; i < 5; i++) { btnPressEv[i] = false; btnReleaseEv[i] = false; }
  return true;
}

bool modEcho() {
  uint8_t n = lvl->echoStages;
  if (n < 3) n = 3;
  if (n > 5) n = 5;
  uint8_t seq[5];
  for (uint8_t i = 0; i < n; i++) seq[i] = random(5);
  Serial.print(F("ECHO flashes "));
  for (uint8_t i = 0; i < n; i++) Serial.print(seq[i] + 1);
  Serial.print(F(" -> press (0 err) "));
  for (uint8_t i = 0; i < n; i++) Serial.print(echoExpect(seq[i]) + 1);
  Serial.println();

  clearEvents();
  for (uint8_t stage = 1; stage <= n; stage++) {
    char l1[17];
    snprintf(l1, sizeof(l1), "STAGE %u/%u", stage, n);
    lcd.clear(); lcdTwo("ECHO", l1); lcdDirty = false;
    if (!echoPlay(seq, stage)) return false;
    uint8_t pos = 0;
    unsigned long lastAct = millis();
    while (true) {
      if (!service()) { allLedsOff(); return false; }
      if (lcdDirty) { lcd.clear(); lcdTwo("ECHO", l1); lcdDirty = false; }
      char k = takeKey();
      if (k == 'B' || (pos == 0 && millis() - lastAct > 5000)) {
        if (!echoPlay(seq, stage)) return false;
        pos = 0;
        lastAct = millis();
        continue;
      }
      for (uint8_t i = 0; i < 5; i++) setLed(i, btnStable[i]);
      int8_t pressed = -1;
      for (uint8_t i = 0; i < 5; i++) if (btnPressEv[i]) { btnPressEv[i] = false; pressed = i; break; }
      if (pressed < 0) continue;
      lastAct = millis();
      if ((uint8_t)pressed == echoExpect(seq[pos])) {
        beep(ECHO_FREQ[pressed], 150);
        pos++;
        if (pos == stage) break;
      } else {
        strike();
        pos = 0;
        lastAct = millis() - 4000;   // повтор вспышек через ~1 с
      }
    }
    allLedsOff();
    if (!waitMs(500)) return false;
  }
  beep(2000, 300);
  return true;
}

// ================== МОДУЛЬ 3: КНОПКА ==================
const char* const BUTTON_LABELS[5] = {"ABORT", "DETONATE", "HOLD", "PRESS", "WAIT"};
const uint8_t RELEASE_DIGIT[4] = {4, 1, 5, 3};   // по числу загоревшихся светодиодов 1..4

bool buttonMustHold(uint8_t label, uint8_t num) {  // num 1..5
  if (label == 0 && (num == 1 || num == 5)) return true;
  if (label == 1 && strikes == 0) return false;
  if (num == snLastDigit()) return false;
  if (label == 2) return false;
  if (label == 4 && snHasVowel()) return true;
  if (label == 3 && strikes >= 1) return false;
  return true;
}

bool modButton() {
  uint8_t b = random(5), label = random(5);
  Serial.print(F("BUTTON #")); Serial.print(b + 1); Serial.print(' '); Serial.print(BUTTON_LABELS[label]);
  Serial.println(buttonMustHold(label, b + 1) ? F(" -> HOLD (0 err)") : F(" -> TAP (0 err)"));

  clearEvents();
  lcdDirty = true;
  bool holding = false, stripShown = false;
  uint8_t strip = 0;
  unsigned long downAt = 0;
  while (true) {
    if (!service()) { allLedsOff(); return false; }
    takeKey();
    if (lcdDirty) { lcd.clear(); lcdTwo("BUTTON", BUTTON_LABELS[label]); lcdDirty = false; }
    if (!holding) for (uint8_t i = 0; i < 5; i++) setLed(i, i == b);

    for (uint8_t i = 0; i < 5; i++) {
      if (!btnPressEv[i]) continue;
      btnPressEv[i] = false;
      if (i != b) { if (!holding) strike(); }
      else if (!holding) { holding = true; stripShown = false; downAt = millis(); }
    }

    if (holding && !stripShown && millis() - downAt >= 1000) {
      strip = random(1, 5);
      uint8_t others[4], n = 0;
      for (uint8_t i = 0; i < 5; i++) if (i != b) others[n++] = i;
      for (uint8_t i = 3; i > 0; i--) { uint8_t j = random(i + 1); uint8_t t = others[i]; others[i] = others[j]; others[j] = t; }
      for (uint8_t i = 0; i < strip; i++) setLed(others[i], true);
      stripShown = true;
      Serial.print(F("BUTTON strip ")); Serial.print(strip);
      Serial.print(F(" -> release on digit ")); Serial.println(RELEASE_DIGIT[strip - 1]);
    }

    if (holding && btnReleaseEv[b]) {
      btnReleaseEv[b] = false;
      holding = false;
      unsigned long dur = millis() - downAt;
      bool mustHold = buttonMustHold(label, b + 1);
      bool ok;
      if (dur < 1000) ok = !mustHold;
      else if (!mustHold || !stripShown) ok = false;
      else {
        uint8_t d[4];
        timerDigits(d);
        uint8_t target = RELEASE_DIGIT[strip - 1];
        ok = (d[0] == target || d[1] == target || d[2] == target || d[3] == target);
      }
      allLedsOff();
      if (ok) { beep(2000, 300); return true; }
      strike();
      stripShown = false;
      lcdDirty = true;
    }
  }
}

// ================== МОДУЛЬ 4: МОРЗЕ ==================
const char* const MORSE_WORDS[16] = {
  "BOLT", "BOMB", "BOOM", "CODE", "COIL", "CORE", "FIRE", "FUEL",
  "FUSE", "TANK", "TIDE", "TIME", "WIND", "WIRE", "WOLF", "ZERO"
};
const uint16_t MORSE_CODES[16] = {
  6134, 9025, 4786, 3517, 8042, 2961, 1248, 7851,
  3309, 6590, 8416, 2083, 1673, 7408, 5290, 5762
};
const char* const MORSE_TABLE[26] = {
  ".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---", "-.-", ".-..", "--",
  "-.", "---", ".--.", "--.-", ".-.", "...", "-", "..-", "...-", ".--", "-..-", "-.--", "--.."
};
const uint16_t MORSE_UNIT = 180;
const uint16_t MORSE_WORD_GAP = 2500;
const unsigned int MORSE_FREQ = 700;

bool modMorse() {
  uint8_t w = random(16);
  const char* word = MORSE_WORDS[w];
  char expected[5];
  snprintf(expected, sizeof(expected), "%04u", MORSE_CODES[w]);
  Serial.print(F("MORSE ")); Serial.print(word); Serial.print(F(" -> ")); Serial.println(expected);

  // временная шкала: чётные элементы — звук, нечётные — пауза
  uint16_t tl[40];
  uint8_t tlLen = 0;
  uint8_t wl = strlen(word);
  for (uint8_t li = 0; li < wl; li++) {
    const char* p = MORSE_TABLE[word[li] - 'A'];
    uint8_t pl = strlen(p);
    for (uint8_t si = 0; si < pl; si++) {
      tl[tlLen++] = (p[si] == '.') ? MORSE_UNIT : MORSE_UNIT * 3;
      if (si < pl - 1) tl[tlLen++] = MORSE_UNIT;
      else if (li < wl - 1) tl[tlLen++] = MORSE_UNIT * 3;
      else tl[tlLen++] = MORSE_WORD_GAP;
    }
  }

  char entry[5] = "";
  uint8_t len = 0;
  uint8_t seg = 0;
  unsigned long segStart = millis() + 800;   // небольшая пауза перед первым словом
  bool started = false;
  clearEvents();
  lcdDirty = true;

  while (true) {
    if (!service()) { noTone(BUZZER); allLedsOff(); return false; }
    if (lcdDirty) {
      char l1[17];
      snprintf(l1, sizeof(l1), "CODE: %s", entry);
      for (uint8_t i = len; i < 4; i++) strcat(l1, "_");
      lcd.clear(); lcdTwo("MORSE", l1);
      lcdDirty = false;
      noTone(BUZZER); setLed(2, false);
      seg = 0; started = false; segStart = millis() + 800;   // после паузы слово начинается заново
    }

    // воспроизведение
    unsigned long now = millis();
    if (now >= segStart) {
      if (!started) {
        started = true;
        tone(BUZZER, MORSE_FREQ); setLed(2, true);
        micMuteUntil = now + tl[0] + 400;
      } else if (now - segStart >= tl[seg]) {
        segStart += tl[seg];
        seg = (seg + 1) % tlLen;
        if (seg % 2 == 0) { tone(BUZZER, MORSE_FREQ); setLed(2, true); micMuteUntil = millis() + tl[seg] + 400; }
        else { noTone(BUZZER); setLed(2, false); }
      }
    }

    char k = takeKey();
    if (!k) continue;
    if (k == 'B') { lcdDirty = true; continue; }
    if (k >= '0' && k <= '9' && len < 4) {
      entry[len++] = k; entry[len] = 0;
      lcdDirty = true;
    } else if (k == '*') {
      len = 0; entry[0] = 0; lcdDirty = true;
    } else if (k == '#' && len == 4) {
      char exp[5];
      strcpy(exp, expected);
      if (strikes % 2 == 1) { for (uint8_t i = 0; i < 2; i++) { char t = exp[i]; exp[i] = exp[3 - i]; exp[3 - i] = t; } }
      noTone(BUZZER); setLed(2, false);
      if (strcmp(entry, exp) == 0) { beep(2000, 300); return true; }
      strike();
      len = 0; entry[0] = 0; lcdDirty = true;
    }
  }
}

// ================== МОДУЛЬ 5: СИМВОЛЫ ==================
// Ω Ж ♥ λ ¶ ▲ Ψ Ш ♪ ◆ π ¥ Я ✚ Σ ☼ ¿ Ю Ф
const uint8_t GLYPHS[19][8] PROGMEM = {
  {0x0E, 0x11, 0x11, 0x11, 0x0A, 0x1B, 0x00, 0x00}, // 0  Ω
  {0x15, 0x15, 0x0E, 0x04, 0x0E, 0x15, 0x15, 0x00}, // 1  Ж
  {0x00, 0x0A, 0x1F, 0x1F, 0x0E, 0x04, 0x00, 0x00}, // 2  ♥
  {0x10, 0x08, 0x08, 0x0C, 0x12, 0x12, 0x11, 0x00}, // 3  λ
  {0x0F, 0x1D, 0x1D, 0x0D, 0x05, 0x05, 0x05, 0x00}, // 4  ¶
  {0x00, 0x04, 0x04, 0x0E, 0x0E, 0x1F, 0x00, 0x00}, // 5  ▲
  {0x15, 0x15, 0x15, 0x0E, 0x04, 0x04, 0x04, 0x00}, // 6  Ψ
  {0x15, 0x15, 0x15, 0x15, 0x15, 0x1F, 0x00, 0x00}, // 7  Ш
  {0x04, 0x06, 0x05, 0x04, 0x0C, 0x1C, 0x08, 0x00}, // 8  ♪
  {0x00, 0x04, 0x0E, 0x1F, 0x0E, 0x04, 0x00, 0x00}, // 9  ◆
  {0x00, 0x1F, 0x0A, 0x0A, 0x0A, 0x09, 0x00, 0x00}, // 10 π
  {0x11, 0x0A, 0x04, 0x1F, 0x04, 0x1F, 0x04, 0x00}, // 11 ¥
  {0x0F, 0x11, 0x11, 0x0F, 0x05, 0x09, 0x11, 0x00}, // 12 Я
  {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00, 0x00}, // 13 ✚
  {0x1F, 0x10, 0x08, 0x04, 0x08, 0x10, 0x1F, 0x00}, // 14 Σ
  {0x15, 0x0E, 0x1B, 0x0E, 0x15, 0x00, 0x00, 0x00}, // 15 ☼
  {0x04, 0x00, 0x04, 0x08, 0x10, 0x11, 0x0E, 0x00}, // 16 ¿
  {0x12, 0x15, 0x15, 0x1D, 0x15, 0x15, 0x12, 0x00}, // 17 Ю
  {0x04, 0x0E, 0x15, 0x15, 0x0E, 0x04, 0x04, 0x00}  // 18 Ф
};
const uint8_t SYMBOL_COLUMNS[5][6] = {
  { 0,  1,  2,  3,  4,  5},   // Ω Ж ♥ λ ¶ ▲
  { 6,  3,  7,  8,  0,  9},   // Ψ λ Ш ♪ Ω ◆
  {10, 11,  8,  1, 12, 13},   // π ¥ ♪ Ж Я ✚
  {14,  9, 15, 16,  6, 11},   // Σ ◆ ☼ ¿ Ψ ¥
  {17,  4, 18, 15, 12, 10}    // Ю ¶ Ф ☼ Я π
};

bool modSymbols() {
  uint8_t c = random(5);
  uint8_t pos[6] = {0, 1, 2, 3, 4, 5};
  for (uint8_t i = 5; i > 0; i--) { uint8_t j = random(i + 1); uint8_t t = pos[i]; pos[i] = pos[j]; pos[j] = t; }
  // слоты A..D получают позиции pos[0..3] в столбце c
  for (uint8_t s = 0; s < 4; s++) {
    uint8_t g[8];
    memcpy_P(g, GLYPHS[SYMBOL_COLUMNS[c][pos[s]]], 8);
    lcd.createChar(s, g);
  }
  uint8_t order[4] = {0, 1, 2, 3};    // порядок слотов по позиции в столбце
  for (uint8_t i = 0; i < 4; i++)
    for (uint8_t j = i + 1; j < 4; j++)
      if (pos[order[j]] < pos[order[i]]) { uint8_t t = order[i]; order[i] = order[j]; order[j] = t; }
  Serial.print(F("SYMBOLS column ")); Serial.print(c + 1); Serial.print(F(" -> "));
  for (uint8_t i = 0; i < 4; i++) Serial.print((char)('A' + order[i]));
  Serial.println();

  bool done[4] = {false, false, false, false};
  uint8_t step = 0;
  clearEvents();
  lcdDirty = true;
  while (true) {
    if (!service()) return false;
    if (lcdDirty) {
      lcd.clear();
      lcdLine(0, "SYMBOLS");
      lcd.setCursor(0, 1);
      for (uint8_t s = 0; s < 4; s++) {
        lcd.write('A' + s); lcd.write(':');
        if (done[s]) lcd.write('-'); else lcd.write((uint8_t)s);
        if (s < 3) lcd.write(' ');
      }
      lcdDirty = false;
    }
    char k = takeKey(false);
    if (k < 'A' || k > 'D') continue;
    uint8_t s = k - 'A';
    if (done[s]) continue;
    if (s == order[step]) {
      keyClick();
      done[s] = true;
      step++;
      lcdDirty = true;
      if (step == 4) { beep(2000, 300); return true; }
    } else {
      strike();
    }
  }
}

// ================== МОДУЛЬ 6: ПАРОЛЬ ==================
const char PW_WORDS[30][6] = {
  "BLAST", "CABLE", "CLOCK", "MAGIC", "MOTOR", "PRIME",
  "GHOST", "LASER", "LEVER", "METAL", "PILOT", "SOLAR",
  "NOISE", "OCEAN", "PANEL", "PHASE", "POWER", "SPARK",
  "RADIO", "RELAY", "ROBOT", "SCOPE", "SHIFT", "STEEL",
  "STORM", "TIMER", "TORCH", "TRACE", "VALVE", "WATCH"
};

bool pwColumnHas(const char col[6], char c) {
  for (uint8_t i = 0; i < 6; i++) if (col[i] == c) return true;
  return false;
}

bool modPassword() {
  uint8_t t = random(30);
  char cols[5][6];
  for (uint16_t attempt = 0; attempt < 500; attempt++) {
    for (uint8_t p = 0; p < 5; p++) {
      cols[p][0] = PW_WORDS[t][p];
      uint8_t n = 1;
      while (n < 6) {
        char c = 'A' + random(26);
        bool dup = false;
        for (uint8_t i = 0; i < n; i++) if (cols[p][i] == c) dup = true;
        if (!dup) cols[p][n++] = c;
      }
      for (uint8_t i = 5; i > 0; i--) { uint8_t j = random(i + 1); char x = cols[p][i]; cols[p][i] = cols[p][j]; cols[p][j] = x; }
    }
    bool unique = true;
    for (uint8_t w = 0; w < 30 && unique; w++) {
      if (w == t) continue;
      bool formable = true;
      for (uint8_t p = 0; p < 5; p++) if (!pwColumnHas(cols[p], PW_WORDS[w][p])) { formable = false; break; }
      if (formable) unique = false;
    }
    if (unique) break;
  }
  Serial.print(F("PASSWORD -> ")); Serial.println(PW_WORDS[t]);

  uint8_t idx[5];
  for (uint8_t p = 0; p < 5; p++) idx[p] = random(6);
  clearEvents();
  lcdDirty = true;
  while (true) {
    if (!service()) return false;
    if (lcdDirty) {
      char l1[17] = "   ";
      for (uint8_t p = 0; p < 5; p++) { uint8_t L = strlen(l1); l1[L] = cols[p][idx[p]]; l1[L + 1] = ' '; l1[L + 2] = 0; }
      lcd.clear(); lcdTwo("PASSWORD", l1);
      lcdDirty = false;
    }
    char k = takeKey();
    if (k >= '1' && k <= '5') {
      uint8_t p = k - '1';
      idx[p] = (idx[p] + 1) % 6;
      keyClick();
      lcd.setCursor(3 + p * 2, 1);
      lcd.write(cols[p][idx[p]]);
    } else if (k == '#') {
      bool ok = true;
      for (uint8_t p = 0; p < 5; p++) if (cols[p][idx[p]] != PW_WORDS[t][p]) ok = false;
      if (ok) { beep(2000, 300); return true; }
      strike();
    }
  }
}

// ================== МОДУЛЬ 7: ШИФР ==================
void cipherApply(char d[4], char mode) {
  switch (mode) {
    case 'R': { char t = d[0]; d[0] = d[3]; d[3] = t; t = d[1]; d[1] = d[2]; d[2] = t; break; }
    case 'X': { char t = d[0]; d[0] = d[1]; d[1] = t; t = d[2]; d[2] = d[3]; d[3] = t; break; }
    case 'M': for (uint8_t i = 0; i < 4; i++) d[i] = '0' + (9 - (d[i] - '0')); break;
    case 'S': for (uint8_t i = 0; i < 4; i++) d[i] = '0' + ((d[i] - '0') + snLastDigit()) % 10; break;
    case 'P': for (uint8_t i = 0; i < 4; i++) d[i] = '0' + ((d[i] - '0') + strikes + 1) % 10; break;
  }
}

void cipherExpected(const char code[5], const char* modes, char out[5]) {
  memcpy(out, code, 5);
  for (uint8_t i = 0; modes[i]; i++) cipherApply(out, modes[i]);
}

bool modCipher() {
  char code[5];
  for (uint8_t i = 0; i < 4; i++) code[i] = '0' + random(10);
  code[4] = 0;
  uint8_t L = lvl->cipherLetters;
  if (L < 1) L = 1;
  if (L > 3) L = 3;
  char modes[4];
  const char MODE_CHARS[] = "RXMSP";
  for (uint8_t i = 0; i < L; i++) modes[i] = MODE_CHARS[random(5)];
  modes[L] = 0;
  char exp0[5];
  cipherExpected(code, modes, exp0);
  Serial.print(F("CIPHER ")); Serial.print(code); Serial.print(F(" MODE:")); Serial.print(modes);
  Serial.print(F(" -> (0 err) ")); Serial.println(exp0);

  char entry[5] = "";
  uint8_t len = 0;
  clearEvents();
  lcdDirty = true;
  while (true) {
    if (!service()) return false;
    if (lcdDirty) {
      char l0[17] = "CIPHER";
      for (uint8_t i = 0; i < 16 - 6 - 5 - L; i++) strcat(l0, " ");
      strcat(l0, "MODE:");
      strcat(l0, modes);
      char l1[17];
      snprintf(l1, sizeof(l1), "%s > %s", code, entry);
      for (uint8_t i = len; i < 4; i++) strcat(l1, "_");
      lcd.clear(); lcdTwo(l0, l1);
      lcdDirty = false;
    }
    char k = takeKey();
    if (!k) continue;
    if (k >= '0' && k <= '9' && len < 4) {
      entry[len++] = k; entry[len] = 0; keyClick(); lcdDirty = true;
    } else if (k == '*') {
      len = 0; entry[0] = 0; lcdDirty = true;
    } else if (k == '#' && len == 4) {
      char exp[5];
      cipherExpected(code, modes, exp);
      if (strcmp(entry, exp) == 0) { beep(2000, 300); return true; }
      strike();
      len = 0; entry[0] = 0; lcdDirty = true;
    }
  }
}

// ================== ЗАПУСК МОДУЛЕЙ ==================
bool runModule(uint8_t id) {
  switch (id) {
    case M_WIRES:    return modWires();
    case M_ECHO:     return modEcho();
    case M_BUTTON:   return modButton();
    case M_MORSE:    return modMorse();
    case M_SYMBOLS:  return modSymbols();
    case M_PASSWORD: return modPassword();
    case M_CIPHER:   return modCipher();
  }
  return true;
}

void pickModules() {
  uint8_t pool[M_COUNT];
  uint8_t n = 0;
  if (lvl->training) {
    pool[n++] = M_WIRES; pool[n++] = M_BUTTON; pool[n++] = M_MORSE; pool[n++] = M_CIPHER;
  } else {
    for (uint8_t i = 0; i < M_COUNT; i++) pool[n++] = i;
  }
  for (uint8_t i = n - 1; i > 0; i--) { uint8_t j = random(i + 1); uint8_t t = pool[i]; pool[i] = pool[j]; pool[j] = t; }
  moduleCount = lvl->modules < n ? lvl->modules : n;
  for (uint8_t i = 0; i < moduleCount; i++) moduleOrder[i] = pool[i];
}

void waitForHash() {
  pendingKey = 0;
  while (true) {
    refreshDisplay();
    char k = keypad.getKey();
    if (k == '#') return;
  }
}

void boomSequence() {
  dispMode = DISP_BOOM;
  allLedsOff();
  lcd.clear();
  lcdTwo("*** BOOM! ***", "BOMB EXPLODED");
  for (int f = 1000; f > 120; f -= 15) {
    tone(BUZZER, f);
    unsigned long t = millis();
    while (millis() - t < 20) refreshDisplay();
  }
  tone(BUZZER, 90, 1500);
  unsigned long t = millis();
  while (millis() - t < 2000) {
    refreshDisplay();
    for (uint8_t i = 0; i < 5; i++) setLed(i, (millis() / 150) % 2);
  }
  noTone(BUZZER);
  allLedsOff();
  lcdLine(1, "# - MENU");
}

void winSequence() {
  allLedsOff();
  uint8_t d[4];
  timerDigits(d);
  char l1[17];
  snprintf(l1, sizeof(l1), "TIME LEFT %u%u:%u%u", d[0], d[1], d[2], d[3]);
  lcd.clear();
  lcdTwo("SYSTEM DEFUSED!", l1);
  const unsigned int melody[] = {1047, 1319, 1568, 2093};
  for (uint8_t i = 0; i < 4; i++) {
    tone(BUZZER, melody[i], 180);
    unsigned long t = millis();
    while (millis() - t < 220) { refreshDisplay(); for (uint8_t j = 0; j < 5; j++) setLed(j, j <= i); }
  }
  unsigned long t = millis();
  while (millis() - t < 3000) refreshDisplay();
  allLedsOff();
  lcdLine(0, "DEFUSED  # MENU");
}

void runGame(uint8_t li) {
  lvl = &LEVELS[li];
  maxStrikes = lvl->maxStrikes;
  strikes = 0;
  exploded = false;
  resetRequested = false;
  inNeedy = false;
  needySuppress = 0;
  timeLeftMs = (long)lvl->seconds * 1000L;
  tickAcc = 0;
  generateSerial();
  pickModules();

  Serial.println(F("========================================"));
  Serial.print(F("LEVEL ")); Serial.print(lvl->name);
  Serial.print(F("  SN ")); Serial.print(serialNo);
  Serial.print(F("  vowel:")); Serial.print(snHasVowel() ? F("yes") : F("no"));
  Serial.print(F("  last digit:")); Serial.println(snLastDigit());
  Serial.print(F("MODULES: "));
  for (uint8_t i = 0; i < moduleCount; i++) { Serial.print(MODULE_NAMES[moduleOrder[i]]); Serial.print(' '); }
  Serial.println();

  initInputStates();
  calibrateSound();

  gameRunning = true;
  lastTickMs = millis();
  dispMode = DISP_TIME;
  scheduleNeedy();

  moduleIndex = 0;
  bool alive = true;
  showPassport(5000);
  if (exploded || resetRequested) alive = false;

  for (uint8_t m = 0; alive && m < moduleCount; m++) {
    moduleIndex = m;
    char l0[17];
    snprintf(l0, sizeof(l0), "MODULE %u/%u", m + 1, moduleCount);
    lcd.clear();
    lcdTwo(l0, MODULE_NAMES[moduleOrder[m]]);
    needySuppress++;
    alive = waitMs(1500);
    needySuppress--;
    if (!alive) break;
    clearEvents();
    alive = runModule(moduleOrder[m]);
    allLedsOff();
    noTone(BUZZER);
    if (alive) {
      Serial.print(F("SOLVED ")); Serial.print(MODULE_NAMES[moduleOrder[m]]);
      Serial.print(F("  time left ")); Serial.print(timeLeftMs / 1000); Serial.println(F(" s"));
    }
  }

  gameRunning = false;
  inNeedy = false;
  allLedsOff();
  noTone(BUZZER);

  if (resetRequested) {
    Serial.println(F("RESET by operator"));
    lcd.clear();
    lcdTwo("RESET", "");
    dispMode = DISP_DASH;
    unsigned long t = millis();
    while (millis() - t < 1000) refreshDisplay();
    resetRequested = false;
    return;
  }
  if (exploded) {
    Serial.println(timeLeftMs <= 0 ? F("EXPLODED: time out") : F("EXPLODED: too many strikes"));
    boomSequence();
  } else {
    Serial.print(F("DEFUSED, time left ")); Serial.print(timeLeftMs / 1000); Serial.println(F(" s"));
    winSequence();
  }
  waitForHash();
}

// ================== РЕЖИМ ПРОВЕРКИ (код 0000) ==================
void testMode() {
  testRunning = true;
  resetRequested = false;
  dispMode = DISP_ALL;
  initInputStates();
  calibrateSound();
  lcd.clear();
  char lastKey = '-';
  int peak = 0;
  unsigned long lastDraw = 0;
  while (true) {
    refreshDisplay();
    pollInputs();
    if (resetRequested) break;
    if (pendingKey) { lastKey = pendingKey; pendingKey = 0; beep(1000, 40); }
    for (uint8_t i = 0; i < 5; i++) setLed(i, btnStable[i]);
    int v = analogRead(SOUND_PIN);
    int dev = abs(v - soundBase);
    if (dev > peak) peak = dev;
    if (millis() - lastDraw > 250) {
      char l0[17], l1[17];
      char w[NUM_WIRES + 1], b[6];
      for (uint8_t i = 0; i < NUM_WIRES; i++) w[i] = wireStable[i] ? '0' : '1';   // 1 = провод на месте
      w[NUM_WIRES] = 0;
      for (uint8_t i = 0; i < 5; i++) b[i] = btnStable[i] ? '1' : '0';
      b[5] = 0;
      snprintf(l0, sizeof(l0), "W:%s B:%s", w, b);
      snprintf(l1, sizeof(l1), "S%4d P%4d %c%c", v, peak, lastKey, peak > SOUND_DELTA ? '!' : ' ');
      lcdTwo(l0, l1);
      peak = 0;
      lastDraw = millis();
    }
  }
  testRunning = false;
  resetRequested = false;
  allLedsOff();
  lcd.clear();
  lcdTwo("TEST END", "");
  unsigned long t = millis();
  while (millis() - t < 800) refreshDisplay();
}

// ================== МЕНЮ ==================
uint8_t selectLevel() {
  dispMode = DISP_DASH;
  allLedsOff();
  lcd.clear();
  lcdTwo("ACCESS CODE:", "CODE: ____");
  char entry[5] = "";
  uint8_t len = 0;
  while (true) {
    refreshDisplay();
    char k = keypad.getKey();
    if (!k) continue;
    if (k >= '0' && k <= '9' && len < 4) {
      entry[len++] = k; entry[len] = 0;
      tone(BUZZER, 1000, 60);
    } else if (k == '*') {
      len = 0; entry[0] = 0;
    } else if (k == '#' && len == 4) {
      randomSeed(seedBase ^ micros());
      if (strcmp(entry, "0000") == 0) return LEVEL_TEST;
      for (uint8_t i = 0; i < LEVEL_COUNT; i++) {
        if (strcmp(entry, LEVELS[i].code) == 0) {
          tone(BUZZER, 2000, 300);
          lcd.clear();
          lcdTwo("ACCESS GRANTED", LEVELS[i].name);
          unsigned long t = millis();
          while (millis() - t < 1500) refreshDisplay();
          return i;
        }
      }
      tone(BUZZER, 300, 500);
      lcdLine(1, "ACCESS DENIED");
      unsigned long t = millis();
      while (millis() - t < 1200) refreshDisplay();
      len = 0; entry[0] = 0;
    } else {
      continue;
    }
    char l1[17];
    snprintf(l1, sizeof(l1), "CODE: %s", entry);
    for (uint8_t i = len; i < 4; i++) strcat(l1, "_");
    lcdLine(1, l1);
  }
}

// ================== SETUP / LOOP ==================
void setup() {
  Serial.begin(9600);
  lcd.init();
  lcd.backlight();

  for (uint8_t i = 0; i < 8; i++) { pinMode(SEG_PINS[i], OUTPUT); digitalWrite(SEG_PINS[i], LOW); }
  for (uint8_t i = 0; i < 4; i++) { pinMode(DIG_PINS[i], OUTPUT); digitalWrite(DIG_PINS[i], HIGH); }
  for (uint8_t i = 0; i < 5; i++) {
    pinMode(BTN_PINS[i], INPUT);        // внешние резисторы на землю, нажата = HIGH
    pinMode(LED_PINS[i], OUTPUT);
    digitalWrite(LED_PINS[i], LOW);
  }
  for (uint8_t i = 0; i < NUM_WIRES; i++) pinMode(WIRE_PINS[i], INPUT_PULLUP);
  pinMode(BUZZER, OUTPUT);
  pinMode(A0, OUTPUT);                  // второй буззер из Quest BOX 2 — держим выключенным
  digitalWrite(A0, LOW);

  keypad.setHoldTime(3000);

  // зерно случайности со свободных аналоговых входов A1..A14
  for (uint8_t i = 0; i < 32; i++) seedBase = (seedBase << 1) ^ (analogRead(A1 + (i % 14)) & 0x3) ^ micros();
  randomSeed(seedBase);

  setupDisplayTimer();
  refreshDisplay();

  lcd.clear();
  lcdTwo("QUEST BOX 4", "SYSTEM READY");
  unsigned long t = millis();
  while (millis() - t < 1000) refreshDisplay();
  Serial.println(F("QUEST BOX 4 ready"));
}

void loop() {
  uint8_t li = selectLevel();
  if (li == LEVEL_TEST) testMode();
  else if (li != LEVEL_NONE) runGame(li);
}
