/*
  ============================================================
   QUEST BOX — версия для ICT Week (упрощённая)
   Arduino Mega 2560. Правила — вкладка «ICT Week» руководства.
  ============================================================
   3 случайных модуля из 5 (Провода, Эхо, Кнопка, Символы, Шифр),
   4 минуты, 3 ошибки = взрыв. Без серийного номера и прослушки.

   Стартовый экран:  #  — начать игру
                     C  — режим проверки железа (в нём B сбрасывает рекорд)
   В игре: удержание D 3 секунды — сброс на стартовый экран.
   Лучшее оставшееся время хранится в EEPROM и показывается на старте.
   Ответы на модули печатаются в Serial (9600).
*/

#include <Wire.h>
#include <EEPROM.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>

// ================== ПОДКЛЮЧЕНИЕ (как в Quest BOX 2) ==================
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

// Порядок = номер на корпусе. Если в EEPROM есть карта из мастера 8888 (прошивка QuestBox4) — берётся она.
uint8_t BTN_PINS[5]  = {30, 32, 34, 36, 42};    // нажата = HIGH
uint8_t LED_PINS[5]  = {31, 33, 35, 37, 39};
const uint8_t NUM_WIRES    = 6;
uint8_t WIRE_PINS[6] = {43, 44, 45, 46, 47, 48}; // INPUT_PULLUP, вынут = HIGH
const uint8_t BUZZER       = 49;
const uint8_t SEG_PINS[8]  = {2, 3, 4, 5, 6, 7, 8, 9};
const uint8_t DIG_PINS[4]  = {10, 11, 12, 13};

// ================== НАСТРОЙКИ ИГРЫ ==================
const uint16_t GAME_SECONDS   = 240;
const uint8_t  MAX_STRIKES    = 3;
const uint8_t  GAME_MODULES   = 3;
const uint8_t  ECHO_STAGES    = 3;
const unsigned long END_SCREEN_MS = 20000;   // через 20 с после финала — обратно на старт

enum ModuleId : uint8_t { M_WIRES, M_ECHO, M_BUTTON, M_SYMBOLS, M_CIPHER, M_COUNT };
const char* const MODULE_NAMES[M_COUNT] = {"WIRES", "ECHO", "BUTTON", "SYMBOLS", "CIPHER"};

// ================== СОСТОЯНИЕ ==================
long timeLeftMs = 0;
unsigned long lastTickMs = 0, tickAcc = 0;
uint8_t strikes = 0;
bool gameRunning = false, testRunning = false, exploded = false, resetRequested = false;
bool lcdDirty = true;
uint8_t moduleOrder[M_COUNT];
unsigned long seedBase = 0;

// ================== РЕКОРД В EEPROM ==================
const int EE_MAGIC_ADDR = 0, EE_BEST_ADDR = 1;
const uint8_t EE_MAGIC = 0x5A;

uint16_t loadBest() {
  if (EEPROM.read(EE_MAGIC_ADDR) != EE_MAGIC) return 0;
  uint16_t v;
  EEPROM.get(EE_BEST_ADDR, v);
  return v;
}
void saveBest(uint16_t s) {
  EEPROM.update(EE_MAGIC_ADDR, EE_MAGIC);
  EEPROM.put(EE_BEST_ADDR, s);
}
void clearBest() { EEPROM.update(EE_MAGIC_ADDR, 0); }

// Карта пинов, записанная мастером нумерации QuestBox4 (адреса 32–48)
const int PINMAP_ADDR = 32;
const uint8_t PINMAP_MAGIC = 0xA7;
bool samePinSet(const uint8_t* a, const uint8_t* def, uint8_t n) {
  for (uint8_t i = 0; i < n; i++) {
    uint8_t cnt = 0;
    for (uint8_t j = 0; j < n; j++) if (a[j] == def[i]) cnt++;
    if (cnt != 1) return false;
  }
  return true;
}
void loadPinMap() {
  if (EEPROM.read(PINMAP_ADDR) != PINMAP_MAGIC) return;
  uint8_t l[5], b[5], w[6];
  for (uint8_t i = 0; i < 5; i++) { l[i] = EEPROM.read(PINMAP_ADDR + 1 + i); b[i] = EEPROM.read(PINMAP_ADDR + 6 + i); }
  for (uint8_t i = 0; i < 6; i++) w[i] = EEPROM.read(PINMAP_ADDR + 11 + i);
  if (!samePinSet(l, LED_PINS, 5) || !samePinSet(b, BTN_PINS, 5) || !samePinSet(w, WIRE_PINS, 6)) return;
  memcpy(LED_PINS, l, 5); memcpy(BTN_PINS, b, 5); memcpy(WIRE_PINS, w, 6);
  Serial.println(F("PIN MAP loaded from EEPROM"));
}

// ================== 7-СЕГМЕНТНЫЙ ИНДИКАТОР ==================
const uint8_t DIGITS[10] = {
  B11111100, B01100000, B11011010, B11110010, B01100110,
  B10110110, B10111110, B11100000, B11111110, B11110110
};
const uint8_t SEG_DP = B00000001, SEG_DASH = B00000010;
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
  TCCR1A = 0; TCCR1B = 0; TCNT1 = 0;
  OCR1A = 499;
  TCCR1B |= (1 << WGM12) | (1 << CS11) | (1 << CS10);
  TIMSK1 |= (1 << OCIE1A);
  interrupts();
}

void timerDigits(uint8_t out[4]) {
  long s = timeLeftMs / 1000;
  if (s < 0) s = 0;
  long mm = s / 60; if (mm > 99) mm = 99;
  long ss = s % 60;
  out[0] = mm / 10; out[1] = mm % 10; out[2] = ss / 10; out[3] = ss % 10;
}

void refreshDisplay() {
  uint8_t b[4];
  unsigned long now = millis();
  switch (dispMode) {
    case DISP_DASH: for (uint8_t i = 0; i < 4; i++) b[i] = SEG_DASH; break;
    case DISP_ALL:  for (uint8_t i = 0; i < 4; i++) b[i] = 0xFF; break;
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
      b[1] |= SEG_DP;
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

void pollInputs() {
  unsigned long now = millis();
  for (uint8_t i = 0; i < 5; i++) {
    bool r = digitalRead(BTN_PINS[i]) == HIGH;
    if (r != btnRaw[i]) { btnRaw[i] = r; btnChange[i] = now; }
    else if (r != btnStable[i] && now - btnChange[i] >= 25) {
      btnStable[i] = r;
      if (r) btnPressEv[i] = true; else btnReleaseEv[i] = true;
    }
  }
  for (uint8_t i = 0; i < NUM_WIRES; i++) {
    bool r = digitalRead(WIRE_PINS[i]) == HIGH;
    if (r != wireRaw[i]) { wireRaw[i] = r; wireChange[i] = now; }
    else if (r != wireStable[i] && now - wireChange[i] >= 40) {
      wireStable[i] = r;
      if (r) wirePullEv[i] = true;
    }
  }
  char k = keypad.getKey();
  if (k) pendingKey = k;
  if (gameRunning || testRunning) {
    for (uint8_t i = 0; i < LIST_MAX; i++)
      if (keypad.key[i].kchar == 'D' && keypad.key[i].kstate == HOLD) resetRequested = true;
  }
}

void clearEvents() {
  for (uint8_t i = 0; i < 5; i++) { btnPressEv[i] = false; btnReleaseEv[i] = false; }
  for (uint8_t i = 0; i < NUM_WIRES; i++) wirePullEv[i] = false;
  pendingKey = 0;
}

void initInputStates() {
  for (uint8_t i = 0; i < 5; i++) { btnRaw[i] = btnStable[i] = digitalRead(BTN_PINS[i]) == HIGH; btnChange[i] = millis(); }
  for (uint8_t i = 0; i < NUM_WIRES; i++) { wireRaw[i] = wireStable[i] = digitalRead(WIRE_PINS[i]) == HIGH; wireChange[i] = millis(); }
  clearEvents();
}

bool anyWirePulled() {
  for (uint8_t i = 0; i < NUM_WIRES; i++) if (wireStable[i]) return true;
  return false;
}

// ================== ВЫВОД ==================
void beep(unsigned int f, unsigned long d) { tone(BUZZER, f, d); }
void keyClick() { beep(1000, 30); }
void setLed(uint8_t i, bool on) { digitalWrite(LED_PINS[i], on ? HIGH : LOW); }
void allLedsOff() { for (uint8_t i = 0; i < 5; i++) setLed(i, false); }

void lcdLine(uint8_t row, const char* s) {
  char buf[17];
  uint8_t n = strlen(s); if (n > 16) n = 16;
  memcpy(buf, s, n);
  for (uint8_t i = n; i < 16; i++) buf[i] = ' ';
  buf[16] = 0;
  lcd.setCursor(0, row);
  lcd.print(buf);
}
void lcdTwo(const char* a, const char* b) { lcdLine(0, a); lcdLine(1, b); }

void formatTime(uint16_t s, char out[6]) {
  snprintf(out, 6, "%02u:%02u", (unsigned)(s / 60) % 100, (unsigned)(s % 60));
}

// ================== ИГРОВОЙ ЦИКЛ ==================
bool service() {
  unsigned long now = millis();
  if (gameRunning) {
    unsigned long dt = now - lastTickMs;
    lastTickMs = now;
    tickAcc += dt * (4UL + strikes);
    timeLeftMs -= (long)(tickAcc / 4);
    tickAcc %= 4;
    if (timeLeftMs <= 0) { timeLeftMs = 0; exploded = true; }
  }
  refreshDisplay();
  pollInputs();
  return !(exploded || resetRequested);
}

bool waitMs(unsigned long ms) {
  unsigned long t = millis();
  while (millis() - t < ms) if (!service()) return false;
  return true;
}

char takeKey() {
  char k = pendingKey;
  pendingKey = 0;
  return (k == 'D') ? 0 : k;
}

void strike() {
  strikes++;
  Serial.print(F("STRIKE ")); Serial.println(strikes);
  flashUntil = millis() + 700;
  allLedsOff();
  noTone(BUZZER);
  beep(180, 600);
  if (strikes >= MAX_STRIKES) { exploded = true; return; }
  char l1[17];
  snprintf(l1, sizeof(l1), "ERROR %u/%u", strikes, MAX_STRIKES);
  lcd.clear();
  lcdTwo("!!! STRIKE !!!", l1);
  waitMs(900);
  clearEvents();
  lcdDirty = true;
}

// ================== ПРОВОДА ==================
// st: 0 тёмный, 1 горит, 2 мигает (мигающий максимум один)
uint8_t wiresAnswer(const uint8_t st[5]) {
  uint8_t lit = 0;
  for (uint8_t i = 0; i < 5; i++) if (st[i] == 2) return i + 1;
  for (uint8_t i = 0; i < 5; i++) if (st[i] == 1) lit++;
  return lit == 5 ? 6 : lit;
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
  uint8_t st[5], lit;
  do {
    lit = 0;
    for (uint8_t i = 0; i < 5; i++) { st[i] = random(2); lit += st[i]; }
  } while (lit == 0);
  if (random(3) == 0) st[random(5)] = 2;       // в трети случаев один светодиод мигает
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
      allLedsOff();
      if (w + 1 == wiresAnswer(st)) { beep(2000, 300); return waitWiresConnected("RECONNECT WIRE"); }
      strike();
      char msg[17];
      snprintf(msg, sizeof(msg), "RECONNECT %u", w + 1);
      if (!waitWiresConnected(msg)) return false;
      lcdDirty = true;
      break;
    }
  }
}

// ================== ЭХО ==================
const uint8_t ECHO_MAP[5] = {3, 5, 1, 2, 4};
const unsigned int ECHO_FREQ[5] = {440, 523, 659, 784, 988};

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
  uint8_t seq[ECHO_STAGES];
  for (uint8_t i = 0; i < ECHO_STAGES; i++) seq[i] = random(5);
  Serial.print(F("ECHO flashes "));
  for (uint8_t i = 0; i < ECHO_STAGES; i++) Serial.print(seq[i] + 1);
  Serial.print(F(" -> press "));
  for (uint8_t i = 0; i < ECHO_STAGES; i++) Serial.print(ECHO_MAP[seq[i]]);
  Serial.println();

  clearEvents();
  for (uint8_t stage = 1; stage <= ECHO_STAGES; stage++) {
    char l1[17];
    snprintf(l1, sizeof(l1), "STAGE %u/%u", stage, ECHO_STAGES);
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
        pos = 0; lastAct = millis();
        continue;
      }
      for (uint8_t i = 0; i < 5; i++) setLed(i, btnStable[i]);
      int8_t pressed = -1;
      for (uint8_t i = 0; i < 5; i++) if (btnPressEv[i]) { btnPressEv[i] = false; pressed = i; break; }
      if (pressed < 0) continue;
      lastAct = millis();
      if (pressed + 1 == ECHO_MAP[seq[pos]]) {
        beep(ECHO_FREQ[pressed], 150);
        if (++pos == stage) break;
      } else {
        strike();
        pos = 0;
        lastAct = millis() - 4000;
      }
    }
    allLedsOff();
    if (!waitMs(500)) return false;
  }
  beep(2000, 300);
  return true;
}

// ================== КНОПКА ==================
const char* const BUTTON_LABELS[3] = {"PRESS", "HOLD", "ABORT"};

bool buttonMustHold(uint8_t label, uint8_t num) {
  if (label == 0) return false;
  if (label == 1) return true;
  return num == 5;
}

bool modButton() {
  uint8_t b = random(5), label = random(3);
  Serial.print(F("BUTTON #")); Serial.print(b + 1); Serial.print(' '); Serial.print(BUTTON_LABELS[label]);
  Serial.println(buttonMustHold(label, b + 1) ? F(" -> HOLD") : F(" -> TAP"));

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
      Serial.print(F("BUTTON release on digit ")); Serial.println(strip);
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
        ok = (d[0] == strip || d[1] == strip || d[2] == strip || d[3] == strip);
      }
      allLedsOff();
      if (ok) { beep(2000, 300); return true; }
      strike();
      stripShown = false;
      lcdDirty = true;
    }
  }
}

// ================== СИМВОЛЫ ==================
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
  { 0,  1,  2,  3,  4,  5},
  { 6,  3,  7,  8,  0,  9},
  {10, 11,  8,  1, 12, 13},
  {14,  9, 15, 16,  6, 11},
  {17,  4, 18, 15, 12, 10}
};

bool modSymbols() {
  uint8_t c = random(5);
  uint8_t pos[6] = {0, 1, 2, 3, 4, 5};
  for (uint8_t i = 5; i > 0; i--) { uint8_t j = random(i + 1); uint8_t t = pos[i]; pos[i] = pos[j]; pos[j] = t; }
  for (uint8_t s = 0; s < 4; s++) {
    uint8_t g[8];
    memcpy_P(g, GLYPHS[SYMBOL_COLUMNS[c][pos[s]]], 8);
    lcd.createChar(s, g);
  }
  uint8_t order[4] = {0, 1, 2, 3};
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
    char k = takeKey();
    if (k < 'A' || k > 'D') continue;
    uint8_t s = k - 'A';
    if (done[s]) continue;
    if (s == order[step]) {
      keyClick();
      done[s] = true;
      lcdDirty = true;
      if (++step == 4) { beep(2000, 300); return true; }
    } else {
      strike();
    }
  }
}

// ================== ШИФР ==================
void cipherExpected(const char code[5], char mode, char out[5]) {
  memcpy(out, code, 5);
  switch (mode) {
    case 'R': { char t = out[0]; out[0] = out[3]; out[3] = t; t = out[1]; out[1] = out[2]; out[2] = t; break; }
    case 'X': { char t = out[0]; out[0] = out[1]; out[1] = t; t = out[2]; out[2] = out[3]; out[3] = t; break; }
    case 'M': for (uint8_t i = 0; i < 4; i++) out[i] = '0' + (9 - (out[i] - '0')); break;
  }
}

bool modCipher() {
  char code[5];
  for (uint8_t i = 0; i < 4; i++) code[i] = '0' + random(10);
  code[4] = 0;
  const char MODES[] = "RMX";
  char mode = MODES[random(3)];
  char exp[5];
  cipherExpected(code, mode, exp);
  Serial.print(F("CIPHER ")); Serial.print(code); Serial.print(F(" MODE:")); Serial.print(mode);
  Serial.print(F(" -> ")); Serial.println(exp);

  char entry[5] = "";
  uint8_t len = 0;
  clearEvents();
  lcdDirty = true;
  while (true) {
    if (!service()) return false;
    if (lcdDirty) {
      char l0[17], l1[17];
      snprintf(l0, sizeof(l0), "CIPHER    MODE:%c", mode);
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
      if (strcmp(entry, exp) == 0) { beep(2000, 300); return true; }
      strike();
      len = 0; entry[0] = 0; lcdDirty = true;
    }
  }
}

// ================== ИГРА ==================
bool runModule(uint8_t id) {
  switch (id) {
    case M_WIRES:   return modWires();
    case M_ECHO:    return modEcho();
    case M_BUTTON:  return modButton();
    case M_SYMBOLS: return modSymbols();
    case M_CIPHER:  return modCipher();
  }
  return true;
}

// ждёт # или таймаут; true = нажали #
bool waitHashOrTimeout(unsigned long ms) {
  unsigned long t = millis();
  while (millis() - t < ms) {
    refreshDisplay();
    if (keypad.getKey() == '#') return true;
  }
  return false;
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
  while (millis() - t < 2000) { refreshDisplay(); for (uint8_t i = 0; i < 5; i++) setLed(i, (millis() / 150) % 2); }
  noTone(BUZZER);
  allLedsOff();
  lcdLine(1, "TRY AGAIN!  #");
}

void winSequence() {
  allLedsOff();
  uint16_t left = timeLeftMs / 1000;
  uint16_t best = loadBest();
  bool record = left > best;
  if (record) saveBest(left);
  char t[6], l1[17];
  formatTime(left, t);
  snprintf(l1, sizeof(l1), record ? "NEW RECORD %s" : "TIME LEFT %s", t);
  lcd.clear();
  lcdTwo("SYSTEM DEFUSED!", l1);
  const unsigned int melody[] = {1047, 1319, 1568, 2093};
  for (uint8_t i = 0; i < 4; i++) {
    tone(BUZZER, melody[i], 180);
    unsigned long t0 = millis();
    while (millis() - t0 < 220) { refreshDisplay(); for (uint8_t j = 0; j < 5; j++) setLed(j, j <= i); }
  }
  if (record) {
    unsigned long t0 = millis();
    while (millis() - t0 < 2500) { refreshDisplay(); for (uint8_t j = 0; j < 5; j++) setLed(j, ((millis() / 120) % 5) == j); }
  }
  allLedsOff();
}

void runGame() {
  strikes = 0;
  exploded = false;
  resetRequested = false;
  timeLeftMs = (long)GAME_SECONDS * 1000L;
  tickAcc = 0;

  uint8_t pool[M_COUNT];
  for (uint8_t i = 0; i < M_COUNT; i++) pool[i] = i;
  for (uint8_t i = M_COUNT - 1; i > 0; i--) { uint8_t j = random(i + 1); uint8_t t = pool[i]; pool[i] = pool[j]; pool[j] = t; }
  for (uint8_t i = 0; i < GAME_MODULES; i++) moduleOrder[i] = pool[i];

  Serial.println(F("========================================"));
  Serial.print(F("ICT GAME: "));
  for (uint8_t i = 0; i < GAME_MODULES; i++) { Serial.print(MODULE_NAMES[moduleOrder[i]]); Serial.print(' '); }
  Serial.println();

  initInputStates();
  lcd.clear();
  lcdTwo("DEFUSE THE BOMB!", "3 MODULES 3 ERR");
  dispMode = DISP_TIME;
  {
    unsigned long t = millis();
    while (millis() - t < 2000) refreshDisplay();
  }

  gameRunning = true;
  lastTickMs = millis();
  bool alive = true;
  for (uint8_t m = 0; alive && m < GAME_MODULES; m++) {
    char l0[17];
    snprintf(l0, sizeof(l0), "MODULE %u/%u", m + 1, GAME_MODULES);
    lcd.clear();
    lcdTwo(l0, MODULE_NAMES[moduleOrder[m]]);
    alive = waitMs(1200);
    if (!alive) break;
    clearEvents();
    alive = runModule(moduleOrder[m]);
    allLedsOff();
    noTone(BUZZER);
  }
  gameRunning = false;
  allLedsOff();
  noTone(BUZZER);

  if (resetRequested) {
    resetRequested = false;
    lcd.clear(); lcdTwo("RESET", "");
    dispMode = DISP_DASH;
    unsigned long t = millis();
    while (millis() - t < 800) refreshDisplay();
    return;
  }
  if (exploded) { Serial.println(F("EXPLODED")); boomSequence(); }
  else { Serial.print(F("DEFUSED, left ")); Serial.println(timeLeftMs / 1000); winSequence(); }
  waitHashOrTimeout(END_SCREEN_MS);
}

// ================== ПРОВЕРКА ЖЕЛЕЗА (C на старте) ==================
void testMode() {
  testRunning = true;
  resetRequested = false;
  dispMode = DISP_ALL;
  initInputStates();
  lcd.clear();
  char lastKey = '-';
  unsigned long lastDraw = 0;
  while (true) {
    refreshDisplay();
    pollInputs();
    if (resetRequested) break;
    if (pendingKey) {
      lastKey = pendingKey; pendingKey = 0; beep(1000, 40);
      if (lastKey == 'B') {
        clearBest();
        lcdTwo("BEST RESET", "");
        unsigned long t = millis();
        while (millis() - t < 800) refreshDisplay();
      }
    }
    for (uint8_t i = 0; i < 5; i++) setLed(i, btnStable[i]);
    if (millis() - lastDraw > 250) {
      char l0[17], l1[17], w[NUM_WIRES + 1], b[6];
      for (uint8_t i = 0; i < NUM_WIRES; i++) w[i] = wireStable[i] ? '0' : '1';
      w[NUM_WIRES] = 0;
      for (uint8_t i = 0; i < 5; i++) b[i] = btnStable[i] ? '1' : '0';
      b[5] = 0;
      snprintf(l0, sizeof(l0), "W:%s B:%s", w, b);
      snprintf(l1, sizeof(l1), "KEY:%c  HOLD D=EX", lastKey);
      lcdTwo(l0, l1);
      lastDraw = millis();
    }
  }
  testRunning = false;
  resetRequested = false;
  allLedsOff();
}

// ================== СТАРТОВЫЙ ЭКРАН ==================
// Возвращает 'G' — игра, 'C' — проверка
char startScreen() {
  dispMode = DISP_DASH;
  allLedsOff();
  char t[6], l1[17];
  uint16_t best = loadBest();
  if (best) { formatTime(best, t); snprintf(l1, sizeof(l1), "BEST %s  # GO", t); }
  else snprintf(l1, sizeof(l1), "BEST --:--  # GO");
  lcd.clear();
  lcdTwo("BOMB DEFUSAL", l1);
  while (true) {
    refreshDisplay();
    setLed((millis() / 300) % 5, true);                    // «бегущий огонь», пока ждём игроков
    for (uint8_t i = 0; i < 5; i++) if (i != (millis() / 300) % 5) setLed(i, false);
    char k = keypad.getKey();
    if (k == '#') { randomSeed(seedBase ^ micros()); tone(BUZZER, 2000, 200); return 'G'; }
    if (k == 'C') return 'C';
  }
}

void setup() {
  Serial.begin(9600);
  loadPinMap();
  lcd.init();
  lcd.backlight();
  for (uint8_t i = 0; i < 8; i++) { pinMode(SEG_PINS[i], OUTPUT); digitalWrite(SEG_PINS[i], LOW); }
  for (uint8_t i = 0; i < 4; i++) { pinMode(DIG_PINS[i], OUTPUT); digitalWrite(DIG_PINS[i], HIGH); }
  for (uint8_t i = 0; i < 5; i++) { pinMode(BTN_PINS[i], INPUT); pinMode(LED_PINS[i], OUTPUT); digitalWrite(LED_PINS[i], LOW); }
  for (uint8_t i = 0; i < NUM_WIRES; i++) pinMode(WIRE_PINS[i], INPUT_PULLUP);
  pinMode(BUZZER, OUTPUT);
  pinMode(A0, OUTPUT);           // второй буззер Quest BOX 2 — выключен
  digitalWrite(A0, LOW);
  keypad.setHoldTime(3000);
  for (uint8_t i = 0; i < 32; i++) seedBase = (seedBase << 1) ^ (analogRead(A1 + (i % 14)) & 0x3) ^ micros();
  randomSeed(seedBase);
  setupDisplayTimer();
  Serial.println(F("QUEST BOX ICT ready"));
}

void loop() {
  char c = startScreen();
  if (c == 'C') testMode();
  else runGame();
}
