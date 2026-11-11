#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <Servo.h>

// --- LCD ---
LiquidCrystal_I2C lcd(0x27, 16, 2);

// --- Клавиатура ---
const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {22, 23, 24, 25};
byte colPins[COLS] = {26, 27, 28, 29};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// --- Модуль 1: ультразвук и кнопка ---
#define TRIG 40
#define ECHO 41

// --- Модуль 2: Simon ---
int btn1=30, btn2=32, btn3=34, btn4=36, btn5=42;
int led1=31, led2=33, led3=35, led4=37, led5=39;
int buzzer2=49;

// --- Модуль 3: датчики ---
#define SOUND_A A15

// --- Модуль 5: семисегментный индикатор ---
const int segmentPins[8] = {2, 3, 4, 5, 6, 7, 8, 9};  
const int digitPins[4] = {10, 11, 12, 13};  
const int buzzerSeg = A0;

// Таблица цифр (0-9) для семисегментного индикатора (общий катод)
byte numbers[10] = {
  B11111100, // 0
  B01100000, // 1
  B11011010, // 2
  B11110010, // 3
  B01100110, // 4
  B10110110, // 5
  B10111110, // 6
  B11100000, // 7
  B11111110, // 8
  B11110110  // 9
};

int minutes = 3;
int seconds = 0;
unsigned long prevMillis = 0;
unsigned long prevBeep = 0;

// --- Модуль 6: сапёр ---
#define BUZZER6 49
int wirePins[6]={43,44,45,46,47,48};
#define YELLOW 43
#define RED    45
#define BLUE   47
#define FORBIDDEN1 44
#define FORBIDDEN2 46
#define FORBIDDEN3 48
int sequencePins[3] = {YELLOW, RED, BLUE};
bool usedWires[3] = {false, false, false};
int stepIndex = 0;
bool defused = false;
bool bombExploded = false;
unsigned long sapperStart = 0;
unsigned long timerDuration = 30000; // ms (30s)
unsigned long explodeTime = 0;
const unsigned long DEBOUNCE_MS = 50;
const unsigned long RESTART_DELAY = 10000;
bool sapperActive = false;

// --- Таймер и игра ---
unsigned long gameStart;
int timeLeft = 180;
int gameState = 0;

// --- Код для сейфа ---
int codeDigits[4] = {2, 4, 6, 1}; // будет переписан в setup в {1,3,5,7}
int collectedDigits = 0; // сколько цифр собрано
bool codeShown[4] = {false, false, false, false}; // была ли показана цифра
unsigned long codeShowTime = 0;
const int CODE_SHOW_DURATION = 3000; // 3 секунды показа цифры

// --- 7-seg мультиплекс ---
unsigned long last7SegMicros = 0;
int current7SegDigit = 0;
bool showFullCodeOn7Seg = false;
unsigned long fullCodeShowUntil = 0;

// вспомогательные для таймера
unsigned long lastSecondMillis = 0;

// --- Функции для семисегментного индикатора ---
void displayDigit(int digitIndex, int number, bool showDot = false) {
  // digitIndex: 0..3
  for (int i = 0; i < 4; i++) {
    // выключаем все разряды (активный низкий)
    digitalWrite(digitPins[i], HIGH);
  }
  // включаем выбранный разряд (активный низкий в твоей схеме)
  digitalWrite(digitPins[digitIndex], LOW);
  
  byte pattern = numbers[number];
  if (showDot) {
    pattern |= B00000001;
  }
  
  for (int i = 0; i < 8; i++) {
    digitalWrite(segmentPins[i], bitRead(pattern, 7 - i));
  }
}

// Показывает либо время (MM:SS), либо если showFullCodeOn7Seg - показывает код 1357 разрядами
void update7Segment() {
  // вызывается очень часто (каждые ~500-2000 микросекунд)
  unsigned long now = micros();
  if (now - last7SegMicros < 800) return; // ~0.8ms шаг — достаточно быстрый
  last7SegMicros = now;
  
  // Сначала выключаем все разряды
  for (int i = 0; i < 4; i++) {
    digitalWrite(digitPins[i], HIGH);
  }

  if (showFullCodeOn7Seg && millis() < fullCodeShowUntil) {
    // показать полный код 1357 на всех разрядах
    int codeToShow[4] = {codeDigits[0], codeDigits[1], codeDigits[2], codeDigits[3]};
    int num = codeToShow[current7SegDigit];
    bool dp = false;
    displayDigit(current7SegDigit, num, dp);
  } else if (showFullCodeOn7Seg && millis() >= fullCodeShowUntil) {
    // выключаем флаг показа кода
    showFullCodeOn7Seg = false;
  } else {
    // показываем timeLeft в формате MM:SS
    int t = timeLeft;
    if (t < 0) t = 0;
    int mm = t / 60;
    int ss = t % 60;

    int displayDigits[4];
    displayDigits[0] = mm / 10;
    displayDigits[1] = mm % 10;
    displayDigits[2] = ss / 10;
    displayDigits[3] = ss % 10;

    bool dp = (current7SegDigit == 1); // ставим точку как "двоеточие" между MM и SS on digit 1
    displayDigit(current7SegDigit, displayDigits[current7SegDigit], dp);
  }

  current7SegDigit = (current7SegDigit + 1) % 4;
}

void forceShowFullCodeOn7Seg(unsigned long duration_ms) {
  // включаем показ полного кода на дисплее (используется после каждой игры)
  showFullCodeOn7Seg = true;
  fullCodeShowUntil = millis() + duration_ms;
  // при включении сразу выставим collectedDigits чтобы update7Segment показывал правильные цифры
  current7SegDigit = 0;
}

// --- Показываем код 1357 на LCD и 7-seg и ждём 2 секунды ---
// void showCode1357AndPause() {
//   // LCD
//   lcd.clear();
//   lcd.setCursor(0,0);
//   lcd.print("CODE: ");
//   lcd.print(codeDigits[0]);
//   lcd.print(codeDigits[1]);
//   lcd.print(codeDigits[2]);
//   lcd.print(codeDigits[3]);

//   // 7-seg
//   forceShowFullCodeOn7Seg(2000);
//   unsigned long until = millis() + 2000;
//   while (millis() < until) {
//     update7Segment(); // keep 7-seg alive
//     // also keep keypad checks so cheats still possible during show (optional)
//     char k = keypad.getKey();
//     if (k) {
//       if (k=='C') { // skip module cheat still allowed
//         gameState++;
//       } else if (k=='D') {
//         restartGame();
//       }
//     }
//   }
// }

// --- Шумный текст (фича) ---
void noiseText() {
  lcd.clear();
  lcd.setCursor(0, 0);
  for (int i = 0; i < 16; i++) {
    char c = (char)random(33, 126);
    lcd.print(c);
  }
  lcd.setCursor(0, 1);
  for (int i = 0; i < 16; i++) {
    char c = (char)random(33, 126);
    lcd.print(c);
  }
}

// --- вспомогательные ---
void loseTime(int penalty){
  timeLeft -= penalty;
  if(timeLeft < 0) timeLeft = 0;
  tone(BUZZER6, 1000, 200);
}

void restartGame(){
  gameState = 0;
  timeLeft = 180;
  gameStart = millis();
  collectedDigits = 0;
  for (int i = 0; i < 4; i++) {
    codeShown[i] = false;
  }
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Game restarted!");
  unsigned long until = millis() + 1000;
  while (millis() < until) {
    update7Segment();
  }
}

// inline helper for wire read
inline bool isPulled(int pin) {
  return digitalRead(pin) == HIGH;
}

void startSapper() {
  defused = false;
  bombExploded = false;
  stepIndex = 0;
  for(int i=0; i<3; i++) usedWires[i] = false;
  sapperStart = millis();
  timerDuration = 30000;
  noTone(BUZZER6);
  sapperActive = true;
  
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("ДИЛИК ДИЛИК");
  lcd.setCursor(0,1);
  lcd.print("Uфцпаршщфцэпштуфплты");
}

// обработка ошибок в сапёре
void accelerateTimer() {
  unsigned long elapsed = millis() - sapperStart;
  unsigned long remaining = (timerDuration > elapsed) ? (timerDuration - elapsed) : 0;
  timerDuration = elapsed + remaining * 3 / 4;
  
  tone(BUZZER6, 300, 500);
  lcd.setCursor(0,1);
  lcd.print("WRONG WIRE! -25 S");
  unsigned long until = millis()+1000;
  while (millis() < until) {
    update7Segment();
  }
}

// взрыв
void explode() {
  bombExploded = true;
  explodeTime = millis();
  noTone(BUZZER6);
  
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("BOOM! EXPLODED!");
  lcd.setCursor(0,1);
  lcd.print("Restarting...");
  
  for (int i = 0; i < 6; ++i) {
    tone(BUZZER6, 200, 500);
    unsigned long until = millis()+500;
    while (millis() < until) update7Segment();
  }
  noTone(BUZZER6);
}

// win sapper
void win() {
  defused = true;
  noTone(BUZZER6);
  
  for (int i = 0; i < 4; ++i) {
    tone(BUZZER6, 2000, 200);
    unsigned long until = millis()+250;
    while (millis() < until) update7Segment();
  }
  noTone(BUZZER6);
}

// wait button (keeps 7-seg alive while waiting)
bool waitButton(int btn, int led, int toneVal) {
    while (true) {
        if (digitalRead(btn) == HIGH) {
            digitalWrite(led, HIGH);
            tone(buzzer2, toneVal, 300);
            unsigned long until = millis() + 300;
            while (millis() < until) update7Segment();
            digitalWrite(led, LOW);
            noTone(buzzer2);
            Serial.print("Button pressed: ");
            Serial.println(btn);
            return true;
        }
        // wrong press on other buttons -> penalty
        if ((btn != btn1 && digitalRead(btn1) == HIGH) ||
            (btn != btn2 && digitalRead(btn2) == HIGH) ||
            (btn != btn3 && digitalRead(btn3) == HIGH) ||
            (btn != btn4 && digitalRead(btn4) == HIGH)) {
            loseTime(25); // Штраф за неправильное действие
            return false;
        }
        // allow 7-seg update and keypad cheat checks while waiting
        update7Segment();
        char k = keypad.getKey();
        if (k) {
          if (k == 'C') { // cheat skip
            Serial.println("DEBUG: Cheat activated -> module skipped");
            lcd.clear();
            lcd.setCursor(0,0);
            lcd.print("CHEAT: Module");
            lcd.setCursor(0,1);
            lcd.print("skipped");
            unsigned long until2 = millis()+1000;
            while (millis() < until2) update7Segment();
            return true; // treat as success so sequence continues
          }
          if (k == 'D') {
            Serial.println("DEBUG: Game restart cheat");
            restartGame();
            return false;
          }
          if (k != 'A' && k != 'B' && k != 'D') {
            loseTime(25);
            lcd.clear();
            lcd.setCursor(0,0);
            lcd.print("Error!");
            lcd.setCursor(0,1);
            lcd.print("-25 seconds");
            unsigned long until2 = millis()+1000;
            while (millis() < until2) update7Segment();
          }
        }
    }
}

// --- GAME FUNCTIONS (перенесена логика, порядок тот же) ---

void showCodeDigit(int digitIndex) {
  // сохраняем прежнее поведение — показываем цифру на LCD и отмечаем как собранную
  if (collectedDigits < 4 && !codeShown[collectedDigits]) {
    codeShown[collectedDigits] = true;
    collectedDigits++;
    codeShowTime = millis();
    
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("CODE DIGIT: ");
    lcd.print(codeDigits[digitIndex]);
    lcd.setCursor(0, 1);
    lcd.print("Collected: ");
    lcd.print(collectedDigits);
    lcd.print("/4");
    
    tone(BUZZER6, 1500, 500);
    unsigned long until = millis()+500;
    while (millis() < until) update7Segment();
  }
}

void gameSimon() {
    lcd.setCursor(0,1);
    lcd.print("фцапфцп йфцепSimon");

    Serial.println(">> Жду нажатия BTN1 (пин 30)...");
    if (!waitButton(btn1, led1, 400)) { return; }
    Serial.println("   BTN1 OK!");
    Serial.println(">> Жду нажатия BTN2 (пин 32)...");
    if (!waitButton(btn2, led2, 500)) { return; }
    Serial.println("   BTN2 OK!");
    Serial.println(">> Жду нажатия BTN3 (пин 34)...");
    if (!waitButton(btn3, led3, 600)) { return; }
    Serial.println("   BTN3 OK!");
    Serial.println(">> Жду нажатия BTN4 (пин 36)...");
    if (!waitButton(btn4, led4, 700)) { return; }
    Serial.println("   BTN4 OK!");

    Serial.println("[Simon] ПРОЙДЕН! Получена цифра кода: 1");
    showCodeDigit(0); // Показываем первую цифру кода (1)
    unsigned long until = millis()+500;
    while (millis() < until) update7Segment();

    // после успешного прохождения показываем полный код 1357 и паузу 2 сек
    //showCode1357AndPause();

    Serial.println("------------------------------");
    Serial.println("[2/5] МОДУЛЬ: ЗВУК");
    Serial.println(">> Создай громкий звук! (хлопок, крик)");
    Serial.println("   Нужно: soundAnalog >= 500");
    gameState++;
}

void gameSensors() {
    static bool hintPrinted = false;
    if (!hintPrinted) {
      Serial.println(">> Датчик звука (A15) активен. Хлопни или крикни!");
      Serial.println("   Порог срабатывания: 500");
      hintPrinted = true;
    }

    lcd.setCursor(0,0);
    lcd.print("фцпфцпмфеп цуфепфц  Sensors");

    int soundAnalog = analogRead(SOUND_A);

    static unsigned long lastSoundPrint = 0;
    if (millis() - lastSoundPrint > 500) {
      Serial.print("   Sound = "); Serial.print(soundAnalog);
      Serial.println(soundAnalog >= 500 ? "  <- СРАБОТАЛ!" : "  (тихо)");
      lastSoundPrint = millis();
    }

    lcd.setCursor(0,1);
    lcd.print("Sound: "); lcd.print(soundAnalog);
    lcd.print("   ");

    digitalWrite(led3, soundAnalog >= 500 ? HIGH : LOW);
    if (soundAnalog >= 500){
      Serial.println("[Sound] ПРОЙДЕН! Получена цифра кода: 3");
      hintPrinted = false;
      showCodeDigit(1); // Показываем вторую цифру кода (3)
      Serial.println("------------------------------");
      Serial.println("[3/5] МОДУЛЬ: LED");
      Serial.println(">> Два светодиода мигают поочерёдно.");
      Serial.println("   Нажми BTN5 (пин 42) ТОЛЬКО когда горит LED4!");
      gameState++;
      return;
    }
    unsigned long until = millis()+1000;
    while (millis() < until) update7Segment();
}

void gameUltrasound() {
    // Два светодиода (led4, led5) мигают поочерёдно.
    // Нужно нажать btn5 пока горит led4 — иначе штраф -25 сек.

    const unsigned long LED_INTERVAL = 600; // мс на каждый светодиод

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Press when LED4!");
    lcd.setCursor(0, 1);
    lcd.print("Wait for it...");

    static unsigned long lastSwitch = 0;
    static bool led4Active = true; // true = горит led4, false = горит led5

    unsigned long now = millis();

    // Переключаем светодиоды по таймеру
    if (now - lastSwitch >= LED_INTERVAL) {
        lastSwitch = now;
        led4Active = !led4Active;
        digitalWrite(led4, led4Active ? HIGH : LOW);
        digitalWrite(led5, led4Active ? LOW  : HIGH);
        Serial.print("   LED: ");
        Serial.println(led4Active ? "LED4 горит <- жми BTN5!" : "LED5 горит   (жди...)");
    }

    // Проверяем кнопку
    if (digitalRead(btn5) == HIGH) {
        if (led4Active) {
            // Правильно — нажато когда горит led4
            digitalWrite(led4, HIGH);
            digitalWrite(led5, LOW);
            Serial.println("[LED] ПРОЙДЕН! Получена цифра кода: 5");
            showCodeDigit(2);
            Serial.println("------------------------------");
            Serial.println("[4/5] МОДУЛЬ: САПЁР");
            Serial.println(">> Вытащи провода в порядке: ЖЁЛТЫЙ -> КРАСНЫЙ -> СИНИЙ");
            Serial.println("   Запрещённые провода (пины 44,46,48) не трогать!");
            Serial.println("   Таймер: 30 секунд. Ошибка = таймер ускоряется.");
            gameState++;
        } else {
            // Неправильно — нажато когда горит led5
            loseTime(25);
            lcd.clear();
            lcd.setCursor(0, 0);
            lcd.print("WRONG! -25 sec");
            Serial.println("   ОШИБКА! Нажал когда горит LED5. Штраф -25 сек.");
            unsigned long until = millis() + 1000;
            while (millis() < until) update7Segment();
        }
        // Сбрасываем состояние для следующего захода
        lastSwitch = 0;
        led4Active = true;
        digitalWrite(led4, LOW);
        digitalWrite(led5, LOW);
        return;
    }

    update7Segment();
}

void gameSapper() {
    lcd.setCursor(0,0);
    lcd.print("фцпфцпфцп Sapper");
    if (!sapperActive) {
      startSapper();
    }
    updateSapper();

    // если defused became true inside updateSapper, updateSapper already increments state
    if (defused) {
      // показываем код 1357 и пауза 2 сек
    //  showCode1357AndPause();
    } else {
      // if stepIndex reached >=3 win() would be called inside updateSapper
      if (stepIndex >= 3 && defused) {
 //       showCode1357AndPause();
      }
    }
}

void gameSafe() {
    // запускаем ввод кода сейфа
    checkSafeCode();

    // если прошёл — show code and advance (внутри checkSafeCode() gameState++ при успехе)
    // а если не прошёл — checkSafeCode() уже снизил время / вернулся
}

// --- checkSafeCode (переписан без рекурсии) ---
void checkSafeCode() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("ПРИВЕТ КАК ДЕЛА?");
  lcd.setCursor(0, 1);
  lcd.print("# ТЕМУР МАДАМИМН");
  
  String enteredCode = "";
  unsigned long inputStart = millis();
  
  while (millis() - inputStart < 30000) { // 30 секунд на ввод
    update7Segment();
    
    char key = keypad.getKey();
    if (key) {
      if (key >= '0' && key <= '9') {
        if (enteredCode.length() < 4) {
          enteredCode += key;
          lcd.clear();
          lcd.setCursor(0, 0);
          lcd.print("s = SECRET: ");
          lcd.print(enteredCode);
          for (int i = enteredCode.length(); i < 4; i++) {
            lcd.print("_");
          }
          tone(BUZZER6, 1000, 100);
        }
      } else if (key == '#') {
        if (enteredCode.length() == 4) {
          int entered = enteredCode.toInt();
          int correctCode = 1357; // Статичный код
          
          if (entered == correctCode) {
            Serial.println("[Сейф] КОД ВЕРНЫЙ! ВСЕ МОДУЛИ ПРОЙДЕНЫ!");
            Serial.println("==============================");
            Serial.println("  БОМБА ОБЕЗВРЕЖЕНА! ВЫ ПОБЕДИЛИ!");
            Serial.println("==============================");
            lcd.clear();
            lcd.print("ФУПОЬТЛЗЭ");
            lcd.setCursor(0, 1);
            lcd.print("YOU WIN!");
            tone(BUZZER6, 2000, 1000);
            unsigned long until = millis()+3000;
            while (millis() < until) update7Segment();

            // show code 1357 after success too (consistent)
     //       showCode1357AndPause();

            gameState++; 
            return;
          } else {
            Serial.print("   Введён код: "); Serial.println(enteredCode);
            Serial.println("   НЕВЕРНЫЙ КОД! Штраф -30 сек. Попробуй снова.");
            lcd.clear();
            lcd.print("WRONG CODE!");
            lcd.setCursor(0, 1);
            lcd.print("Try again");
            tone(BUZZER6, 300, 1000);
            unsigned long until = millis()+2000;
            while (millis() < until) update7Segment();

            loseTime(30);
            return;
          }
        }
      } else if (key == '*') {
        // очищаем код и продолжаем ожидание (без рекурсии)
        enteredCode = "";
        lcd.clear();
        lcd.print("CODE CLEARED");
        unsigned long until = millis()+1000;
        while (millis() < until) update7Segment();
        // и продолжаем цикл ожидания
      }
      // Cheats allowed here too
      if (key == 'C') {
        Serial.println("DEBUG: Cheat activated -> module skipped");
        lcd.clear();
        lcd.setCursor(0,0);
        lcd.print("CHEAT: Module");
        lcd.setCursor(0,1);
        lcd.print("skipped");
        unsigned long until = millis()+1000;
        while (millis() < until) update7Segment();
        gameState++;
        return;
      }
      if (key == 'D') {
        restartGame();
        return;
      }
    }
    
    // Показываем подсказку на LCD
    if (millis() % 2000 < 1000) {
      lcd.setCursor(6, 1);
      lcd.print("    ");
    } else {
      lcd.setCursor(6, 1);
      lcd.print("####");
    }
    
    delay(50); // небольшая пауза, но update7Segment вызывается в начале цикла
  }
  
  lcd.clear();
  lcd.print("TIME OUT!");
  loseTime(20);
  unsigned long until = millis()+1000;
  while (millis() < until) update7Segment();
}

// --- Sapper logic (перенесено из оригинала, с update7Segment calls) ---
void updateSapper() {
  if (defused) {
    Serial.println("[Сапёр] ОБЕЗВРЕЖЕН! Получена цифра кода: 7");
    Serial.println("------------------------------");
    Serial.println("[5/5] МОДУЛЬ: СЕЙФ");
    Serial.println(">> Введи 4-значный код на клавиатуре и нажми #");
    Serial.println("   Подсказка: цифры кода получены из предыдущих модулей.");
    Serial.println("   * = очистить ввод");
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("ЩЭФРШЦПРШЩПЦФЩШЭРФЦП");
    unsigned long until = millis()+2000;
    while (millis() < until) update7Segment();
    gameState++;
    sapperActive = false;
    return;
  }

  if (bombExploded) {
    if (millis() - explodeTime >= RESTART_DELAY) {
      startSapper();
    }
    return;
  }

  unsigned long elapsed = millis() - sapperStart;
  if (elapsed >= timerDuration) {
    explode();
    return;
  }

  unsigned long remaining = (timerDuration - elapsed) / 1000;
  lcd.setCursor(0,0);
  lcd.print("Sapper: ");
  lcd.print(remaining);
  lcd.print("s  ");

  static int lastStep = -1;
  if (stepIndex != lastStep) {
    lastStep = stepIndex;
    if (stepIndex == 0) Serial.println(">> Шаг 1/3: вытащи ЖЁЛТЫЙ провод (пин 43)");
    if (stepIndex == 1) Serial.println(">> Шаг 2/3: вытащи КРАСНЫЙ провод (пин 45)");
    if (stepIndex == 2) Serial.println(">> Шаг 3/3: вытащи СИНИЙ провод (пин 47)");
  }

  static unsigned long lastTick = 0;
  int tickInterval = map((int)(timerDuration - elapsed), 0, (int)timerDuration, 100, 1000);
  if (millis() - lastTick >= (unsigned long)tickInterval) {
    tone(BUZZER6, 1600, 100);
    lastTick = millis();
  }

  bool rawY = isPulled(YELLOW) && !usedWires[0];
  bool rawR = isPulled(RED)    && !usedWires[1];
  bool rawB = isPulled(BLUE)   && !usedWires[2];

  bool forbidden1 = isPulled(FORBIDDEN1);
  bool forbidden2 = isPulled(FORBIDDEN2);
  bool forbidden3 = isPulled(FORBIDDEN3);

  if (forbidden1 || forbidden2 || forbidden3) {
    delay(DEBOUNCE_MS);
    if (isPulled(FORBIDDEN1) || isPulled(FORBIDDEN2) || isPulled(FORBIDDEN3)) {
      accelerateTimer();
      loseTime(25); // Штраф за неправильное действие
      return;
    }
  }

  if (!rawY && !rawR && !rawB) return;
  delay(DEBOUNCE_MS);

  bool confY = isPulled(YELLOW) && !usedWires[0];
  bool confR = isPulled(RED)    && !usedWires[1];
  bool confB = isPulled(BLUE)   && !usedWires[2];

  int confCount = (confY?1:0) + (confR?1:0) + (confB?1:0);
  int expectedPin = sequencePins[stepIndex];
  bool expectedPulled = 
      (expectedPin==YELLOW && confY) ||
      (expectedPin==RED    && confR) ||
      (expectedPin==BLUE   && confB);

  if (confCount > 1) { 
    explode(); 
    loseTime(25); // Штраф за неправильное действие
    return; 
  }
  
  if (expectedPulled && confCount==1) {
    unsigned long elapsedNow = millis() - sapperStart;
    if (stepIndex==0 && expectedPin==YELLOW) {
      unsigned long remain = (timerDuration > elapsedNow) ? (timerDuration - elapsedNow) : 0;
      timerDuration = elapsedNow + remain / 2;
      Serial.println("   (жёлтый провод ускорил таймер x2!)");
    }
    usedWires[stepIndex] = true;
    stepIndex++;
    Serial.print("   Шаг "); Serial.print(stepIndex); Serial.println("/3 OK!");

    tone(BUZZER6, 2000, 200);
    lcd.setCursor(0,1);
    lcd.print("Step ");
    lcd.print(stepIndex);
    lcd.print("/3 OK! ");
    
    unsigned long until = millis()+500;
    while (millis() < until) update7Segment();

    if (stepIndex >= 3) { win(); return; }
    return;
  }
  
  if (!expectedPulled && confCount==1) { 
    accelerateTimer(); 
    loseTime(25); // Штраф за неправильное действие
    return; 
  }
}

// --- Инициализация ---
void setup(){
  Serial.begin(9600);
  lcd.init();
  lcd.backlight();

  for (int i = 0; i < 8; i++) {
    pinMode(segmentPins[i], OUTPUT);
    digitalWrite(segmentPins[i], HIGH);
  }
  for (int i = 0; i < 4; i++) {
    pinMode(digitPins[i], OUTPUT);
    digitalWrite(digitPins[i], HIGH);
  }

  pinMode(TRIG, OUTPUT);
  pinMode(ECHO, INPUT);
  pinMode(btn5, INPUT);

  pinMode(btn1,INPUT);
  pinMode(btn2,INPUT);
  pinMode(btn3,INPUT);
  pinMode(btn4,INPUT);
  pinMode(led1,OUTPUT);
  pinMode(led2,OUTPUT);
  pinMode(led3,OUTPUT);
  pinMode(led4,OUTPUT);
  pinMode(led5,OUTPUT);
  pinMode(buzzer2,OUTPUT);

  pinMode(buzzerSeg,OUTPUT);

  pinMode(BUZZER6,OUTPUT);
  for(int i=0;i<6;i++) pinMode(wirePins[i],INPUT_PULLUP);

  gameStart = millis();
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Game started");
  unsigned long until = millis()+1000;
  while (millis() < until) update7Segment();

  Serial.println("==============================");
  Serial.println("  QUEST BOX — ИГРА НАЧАЛАСЬ!");
  Serial.println("==============================");
  Serial.println("Модули: 1.Simon  2.Sound  3.LED  4.Sapper  5.Safe");
  Serial.println("Читы: C = пропустить модуль, D = перезапуск");
  Serial.println("Время: 3 минуты. Ошибки = -25 сек.");
  Serial.println("------------------------------");
  Serial.println("[1/5] МОДУЛЬ: SIMON");
  Serial.println(">> Нажми 4 кнопки по порядку: BTN1 -> BTN2 -> BTN3 -> BTN4");

  // Статичный код 1357
  codeDigits[0] = 1;
  codeDigits[1] = 3;
  codeDigits[2] = 5;
  codeDigits[3] = 7;

  randomSeed(analogRead(0));
}

// --- Главный цикл ---
void loop(){
  unsigned long currentMillis = millis();
  
  // таймер игры (timeLeft) уменьшаем каждую секунду
  if (millis() - lastSecondMillis >= 1000) {
    lastSecondMillis = millis();
    if (timeLeft > 0) {
      timeLeft--;
      // Напоминание каждые 30 секунд
      if (timeLeft > 0 && timeLeft % 30 == 0) {
        Serial.print("[Таймер] Осталось: ");
        Serial.print(timeLeft / 60); Serial.print(":");
        if (timeLeft % 60 < 10) Serial.print("0");
        Serial.println(timeLeft % 60);
      }
    } else {
      // время вышло — показываем сообщение и останавливаемся
      lcd.clear();
      lcd.setCursor(0,0);
      lcd.print("TIME'S UP!");
      Serial.println("GAME OVER: Time's up");
      while (true) {
        update7Segment();
      }
    }
  }

  // семисегментник всегда обновляем (мультиплекс)
  update7Segment();

  // клавиши читов — глобальная проверка (также в waitButton и других местах)
  char key = keypad.getKey();
  if(key){
    Serial.print("Pressed: ");
    Serial.println(key);
    if(key == 'C'){
      Serial.println("DEBUG: Cheat activated -> module skipped");
      lcd.clear();
      lcd.setCursor(0,0);
      lcd.print("CHEAT: Module");
      lcd.setCursor(0,1);
      lcd.print("skipped");
      unsigned long until = millis()+1000;
      while (millis() < until) update7Segment();
      gameState++;
      return;
    }
    if(key == 'D'){
      Serial.println("DEBUG: Game restart cheat");
      restartGame();
      return;
    }
    if(key != 'A' && key != 'B' && key != 'D'){
      loseTime(25);
      lcd.clear();
      lcd.setCursor(0,0);
      lcd.print("Error!");
      lcd.setCursor(0,1);
      lcd.print("-25 seconds");
      unsigned long until = millis()+1000;
      while (millis() < until) update7Segment();
    }
  }

  // шумный текст время от времени (можно настроить частоту вывода)
  static unsigned long lastNoise = 0;
  if (millis() - lastNoise > 10000) { // каждые 10 сек показываем шум
    noiseText();
    unsigned long until = millis()+500;
    while (millis() < until) update7Segment();
    lastNoise = millis();
  }

  // последовательный переход по играм (порядок и логика соблюдены)
  switch(gameState){
    case 0:
      gameSimon();
      break;
    case 1:
      gameSensors();
      break;
    case 2:
      gameUltrasound();
      break;
    case 3:
      gameSapper();
      break;
    case 4:
      gameSafe();
      break;
    default:
      lcd.clear();
      lcd.print("BOMB DEFUSED!");
      Serial.println("WIN: All modules completed");
      while(true) {
        update7Segment();
      }
  }
}
