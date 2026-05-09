/*
 * Smart Embedded Door Security System
 * CPE 301 - Embedded Systems Design
 * Author: Chris Thomas
 *
 * ─── Hardware ────────────────────────────────────────────────────
 *  Arduino Mega 2560
 *  16×2 LCD1602 with I2C backpack → SDA=20, SCL=21, VCC=5V, GND=GND
 *                                    I2C address: 0x27
 *  4×4 Membrane Keypad       → Rows: 36-39  Cols: 40-43
 *  Green  LED (unlocked)     → Pin 22  (PA0)  + 220Ω → GND
 *  Red    LED (locked/alarm) → Pin 23  (PA1)  + 220Ω → GND
 *  Yellow LED (armed/idle)   → Pin 24  (PA2)  + 220Ω → GND
 *  Active Buzzer             → Pin 26  (PA4)  → GND
 *  Button 1 — ON / Arm       → Pin 32  (PC5, polled)      → GND
 *  Button 2 — Inside toggle  → Pin 30  (PC7)              → GND
 *  Button 3 — OFF / Reset    → Pin 31  (PC6)              → GND
 *  White LED (flashlight)    → Pin 25  (PA3)  + 220Ω → GND
 *                              sits above LCD module
 *
 * ─── Button 2 behavior ───────────────────────────────────────────
 *  IDLE  → 1 press  : unlock door (→ ACTIVE, green)
 *  ACTIVE → 1 press : re-lock     (→ IDLE,   yellow)
 *  ERROR → 3 quick presses (within 1.5s) : disarm alarm (→ IDLE)
 *
 * ─── Restrictions ────────────────────────────────────────────────
 *  NO pinMode, digitalRead, digitalWrite, delay, analogRead, Serial
 *  millis() for all timing
 */

#include <LiquidCrystal_I2C.h>    // Install: Library Manager → LiquidCrystal I2C by Frank de Brabander
#include <Keypad.h>               // Keypad library

// ─── LCD (I2C backpack — address 0x27) ──────────────────────────
// SDA=20, SCL=21  (hardware I2C on Mega)
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ─── Keypad ──────────────────────────────────────────────────────
const byte ROWS = 4, COLS = 4;
char keys[ROWS][COLS] = {
  {'3','2','1','A'},
  {'6','5','4','B'},
  {'9','8','7','C'},
  {'#','0','*','D'}
};
byte rowPins[ROWS] = {43, 42, 41, 40};
byte colPins[COLS] = {37, 38, 39, 36};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// ─── Password ────────────────────────────────────────────────────
const String CORRECT_PASSWORD = "1231";   // Unlock / disarm PIN
String enteredPassword = "";
int failedAttempts = 0;
const int MAX_ATTEMPTS = 4;

// ─── State machine ───────────────────────────────────────────────
enum class State { OFF, IDLE, ACTIVE, ERROR_ALARM };
volatile State currentState = State::OFF;

// ─── Port A bitmasks (LEDs + buzzer) — pins 22-26 ────────────────
#define GREEN_BIT   (1 << 0)   // PA0 — pin 22
#define RED_BIT     (1 << 1)   // PA1 — pin 23
#define YELLOW_BIT  (1 << 2)   // PA2 — pin 24
#define WHITE_BIT   (1 << 3)   // PA3 — pin 25  (flashlight, above LCD)
#define BUZZER_BIT  (1 << 4)   // PA4 — pin 26

// ─── Port C bitmasks (buttons) — pins 30-31 ──────────────────────
#define INSIDE_BTN_BIT  (1 << 7)  // PC7 — pin 30
#define OFF_BTN_BIT     (1 << 6)  // PC6 — pin 31
#define ON_BTN_BIT      (1 << 5)  // PC5 — pin 32

// ─── Interrupt flag ──────────────────────────────────────────────

// ─── Timing ──────────────────────────────────────────────────────
// Green LED flash (500ms on / 500ms off in ACTIVE state)
unsigned long greenFlashStart = 0;

// Inside button multi-press tracking (for 3-press alarm disarm)
int     insidePressCount      = 0;
unsigned long firstPressTime  = 0;
const unsigned long MULTI_PRESS_WINDOW = 1500UL; // 1.5 second window

// Button debounce
unsigned long lastInsidePress = 0;
unsigned long lastOffPress    = 0;
const unsigned long DEBOUNCE  = 80UL;

// ─── Register-level UART (replaces Serial) ───────────────────────
void uartInit(unsigned long baud) {
  uint16_t ubrr = F_CPU / 16 / baud - 1;
  UBRR0H = (uint8_t)(ubrr >> 8);
  UBRR0L = (uint8_t)(ubrr);
  UCSR0B = (1 << TXEN0);
  UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
}

void uartChar(char c) {
  while (!(UCSR0A & (1 << UDRE0)));
  UDR0 = c;
}

void uartStr(const char *s) {
  while (*s) uartChar(*s++);
}

void uartInt(int n) {
  if (n < 0) { uartChar('-'); n = -n; }
  if (n >= 10) uartInt(n / 10);
  uartChar('0' + n % 10);
}

// Log event to serial monitor
void log(const char *msg) {
  uartStr(msg);
  uartChar('\r'); uartChar('\n');
}

// ─── LED / Buzzer helpers (register-level) ───────────────────────
void allLedsOff() {
  PORTA &= ~(GREEN_BIT | RED_BIT | YELLOW_BIT | WHITE_BIT);
}

void setLed(uint8_t bit) {
  allLedsOff();
  PORTA |= bit;
}

void buzzerOn()  { PORTA |=  BUZZER_BIT; }
void buzzerOff() { PORTA &= ~BUZZER_BIT; }

// ─── Button reads (register-level, active LOW with pull-up) ──────
bool insideBtnDown() { return !(PINC & INSIDE_BTN_BIT); }
bool offBtnDown()    { return !(PINC & OFF_BTN_BIT);    }
bool onBtnDown()     { return !(PINC & ON_BTN_BIT);     }

// ─── LCD helpers ─────────────────────────────────────────────────
void lcdLine(uint8_t row, const char *text) {
  lcd.setCursor(0, row);
  lcd.print(text);
  // Pad to 16 chars to clear leftover characters
  uint8_t len = strlen(text);
  while (len < 16) { lcd.print(' '); len++; }
}

// ─── State entry functions ────────────────────────────────────────

void enterOff() {
  currentState = State::OFF;
  buzzerOff();
  setLed(RED_BIT);
  PORTA &= ~WHITE_BIT;   // flashlight OFF in OFF state
  lcdLine(0, "  SYSTEM  OFF   ");
  lcdLine(1, "Press ON button ");
  log("System OFF");
}

void enterIdle() {
  currentState = State::IDLE;
  buzzerOff();
  enteredPassword  = "";
  failedAttempts   = 0;
  insidePressCount = 0;
  setLed(YELLOW_BIT);
  PORTA |= WHITE_BIT;    // flashlight ON — system is armed and watching
  lcdLine(0, "Enter Passcode: ");
  lcdLine(1, "                ");
  log("IDLE state -- system armed");
}

void enterActive(bool fromButton) {
  currentState     = State::ACTIVE;
  greenFlashStart  = millis();
  insidePressCount = 0;
  buzzerOff();
  setLed(GREEN_BIT);
  PORTA |= WHITE_BIT;    // flashlight ON — illuminate entry area
  lcdLine(0, "*** UNLOCKED ***");
  lcdLine(1, fromButton ? "Inside btn open " : "Correct code!   ");
  if (fromButton) log("ACTIVE -- unlocked by inside button");
  else            log("ACTIVE -- unlocked by correct code");
}

void enterAlarm() {
  currentState     = State::ERROR_ALARM;
  insidePressCount = 0;
  buzzerOn();
  setLed(RED_BIT);
  PORTA &= ~WHITE_BIT;   // flashlight OFF — alarm state (red only)
  lcdLine(0, "!!! ALARM !!!   ");
  lcdLine(1, "Btn x3 or code  ");
  log("ERROR -- alarm triggered (4 failed attempts)");
}

// ─── Password entry (called from IDLE or ALARM states) ───────────
// Returns true if the user submitted something (correct or not)
bool handleKey(char key) {
  if (key == '#') {
    // ── Submit ──
    if (enteredPassword == CORRECT_PASSWORD) {
      failedAttempts = 0;
      log("Password correct");
      enterActive(false);
    } else {
      failedAttempts++;
      // Log the failure
      uartStr("Wrong password (attempt ");
      uartInt(failedAttempts);
      uartChar('/');
      uartInt(MAX_ATTEMPTS);
      uartStr(")\r\n");

      if (failedAttempts >= MAX_ATTEMPTS) {
        enterAlarm();
      } else {
        // Show INCORRECT briefly, then redraw entry screen
        lcdLine(0, "INCORRECT CODE  ");
        char remaining[17];
        remaining[0] = 'T'; remaining[1] = 'r'; remaining[2] = 'i';
        remaining[3] = 'e'; remaining[4] = 's'; remaining[5] = ' ';
        remaining[6] = 'l'; remaining[7] = 'e'; remaining[8] = 'f';
        remaining[9] = 't'; remaining[10] = ':'; remaining[11] = ' ';
        remaining[12] = '0' + (MAX_ATTEMPTS - failedAttempts);
        remaining[13] = ' '; remaining[14] = ' '; remaining[15] = ' ';
        remaining[16] = '\0';
        lcdLine(1, remaining);
        // Short visual pause with millis (no delay() allowed)
        unsigned long t = millis();
        while (millis() - t < 1800UL);
        enteredPassword = "";
        lcdLine(0, "Enter Passcode: ");
        lcdLine(1, "                ");
      }
    }
    enteredPassword = "";
    return true;

  } else if (key == '*') {
    // ── Backspace ──
    if (enteredPassword.length() > 0)
      enteredPassword.remove(enteredPassword.length() - 1);
    lcdLine(1, "                ");
    lcd.setCursor(0, 1);
    for (uint8_t i = 0; i < enteredPassword.length(); i++) lcd.print('*');

  } else {
    // ── Digit / letter ──
    if (enteredPassword.length() < 4) {
      enteredPassword += key;
      lcd.setCursor(0, 1);
      lcd.print("                ");
      lcd.setCursor(0, 1);
      for (uint8_t i = 0; i < enteredPassword.length(); i++) lcd.print('*');
    }
  }
  return false;
}

// ─── Setup ───────────────────────────────────────────────────────
void setup() {
  // Port A outputs: LEDs + buzzer (pins 22-26)
  DDRA  |=  (GREEN_BIT | RED_BIT | YELLOW_BIT | WHITE_BIT | BUZZER_BIT);
  PORTA &= ~(GREEN_BIT | RED_BIT | YELLOW_BIT | WHITE_BIT | BUZZER_BIT);

  // Port C inputs with pull-ups: all 3 buttons
  DDRC  &= ~(INSIDE_BTN_BIT | OFF_BTN_BIT | ON_BTN_BIT);
  PORTC |=  (INSIDE_BTN_BIT | OFF_BTN_BIT | ON_BTN_BIT);

  // UART
  uartInit(9600);

  // I2C (for LCD backpack) — settle time before interrupts
  Wire.begin();
  unsigned long i2cSettle = millis();
  while (millis() - i2cSettle < 100UL);

  // LCD — I2C backpack (init twice for reliability)
  lcd.init();
  lcd.init();
  lcd.backlight();

  log("System boot");

  // ── Splash screen: "ACV Buzio" for 5 seconds ──
  lcdLine(0, "   ACV  Buzio   ");
  lcdLine(1, "                ");
  log("Splash screen");
  unsigned long splashStart = millis();
  while (millis() - splashStart < 5000UL);

  // Enter OFF state first
  enterOff();

  sei();
}

// ─── Loop ────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  // ── ON button: arm system from OFF state (polled) ────────────
  if (currentState == State::OFF && onBtnDown()) {
    while (onBtnDown()); // wait for release
    log("ON button pressed -- arming");
    enterIdle();
    return;
  }

  // ── OFF button: return to OFF from any state ──────────────────
  if (currentState != State::OFF && offBtnDown()) {
    if (now - lastOffPress > DEBOUNCE) {
      lastOffPress = now;
      while (offBtnDown()); // wait for release
      enterOff();
      return;
    }
  }

  // ═══════════════════════════════════════════════════════════════
  switch (currentState) {

  // ── OFF ─────────────────────────────────────────────────────
  case State::OFF:
    // Waiting for ISR — nothing to do
    break;

  // ── IDLE ────────────────────────────────────────────────────
  case State::IDLE: {
    // Inside button pressed → unlock immediately (no code needed from inside)
    if (insideBtnDown() && now - lastInsidePress > DEBOUNCE) {
      lastInsidePress = now;
      while (insideBtnDown()); // wait for release
      enterActive(true);
      return;
    }

    // Keypad: any key feeds directly into passcode entry (screen already shows "Enter Passcode:")
    char key = keypad.getKey();
    if (key) handleKey(key);

    break;
  }

  // ── ACTIVE (unlocked) ────────────────────────────────────────
  case State::ACTIVE: {
    // Green LED 1 Hz flash
    unsigned long elapsed = now - greenFlashStart;
    if      (elapsed < 500)  PORTA |=  GREEN_BIT;
    else if (elapsed < 1000) PORTA &= ~GREEN_BIT;
    else    greenFlashStart = now;

    // Inside button pressed again → re-lock
    if (insideBtnDown() && now - lastInsidePress > DEBOUNCE) {
      lastInsidePress = now;
      while (insideBtnDown());
      log("Re-locked by inside button");
      enterIdle();
      return;
    }
    break;
  }

  // ── ERROR / ALARM ────────────────────────────────────────────
  case State::ERROR_ALARM: {
    // ── Option 1: correct code on keypad clears alarm ──────────
    char key = keypad.getKey();
    if (key == '#') {
      enteredPassword = "";
      lcdLine(0, "Enter code:     ");
      lcdLine(1, "                ");
      // Sub-loop
      while (currentState == State::ERROR_ALARM) {
        if (offBtnDown()) { enterOff(); return; }
        char k = keypad.getKey();
        if (k) {
          if (k == '#') {
            if (enteredPassword == CORRECT_PASSWORD) {
              failedAttempts = 0;
              log("Alarm cleared by correct code");
              enterIdle();
            } else {
              lcdLine(0, "!!! ALARM !!!   ");
              lcdLine(1, "Wrong code      ");
              unsigned long t2 = millis();
              while (millis() - t2 < 1500UL);
              enteredPassword = "";
              lcdLine(0, "Enter code:     ");
              lcdLine(1, "                ");
              log("Wrong code during alarm");
            }
            enteredPassword = "";
          } else if (k == '*') {
            if (enteredPassword.length() > 0)
              enteredPassword.remove(enteredPassword.length() - 1);
            lcd.setCursor(0, 1);
            lcd.print("                ");
            lcd.setCursor(0, 1);
            for (uint8_t i = 0; i < enteredPassword.length(); i++) lcd.print('*');
          } else {
            if (enteredPassword.length() < 4) {
              enteredPassword += k;
              lcd.setCursor(0, 1);
              lcd.print("                ");
              lcd.setCursor(0, 1);
              for (uint8_t i = 0; i < enteredPassword.length(); i++) lcd.print('*');
            }
          }
        }
      }
      return;
    }

    // ── Option 2: inside button pressed 3× within 1.5s ─────────
    if (insideBtnDown() && now - lastInsidePress > DEBOUNCE) {
      lastInsidePress = now;
      while (insideBtnDown()); // wait for release

      if (insidePressCount == 0) {
        firstPressTime = now;
        insidePressCount = 1;
      } else if (now - firstPressTime <= MULTI_PRESS_WINDOW) {
        insidePressCount++;
        if (insidePressCount >= 3) {
          failedAttempts   = 0;
          insidePressCount = 0;
          log("Alarm cleared by inside button (3 presses)");
          enterIdle();
          return;
        }
      } else {
        // Window expired — restart count
        firstPressTime   = now;
        insidePressCount = 1;
      }
    }

    // Reset multi-press count if window has elapsed without enough presses
    if (insidePressCount > 0 && now - firstPressTime > MULTI_PRESS_WINDOW) {
      insidePressCount = 0;
    }

    break;
  }

  } // end switch
}
