# Final-Project-
This repo is the Final Project Part 3 for CPE 301.1001 Embedded Systems Design.

---

## Smart Embedded Door Security System

A password-protected embedded security system built on the **Arduino Mega 2560** for CPE 301 – Embedded Systems Design. The system monitors for unauthorized access attempts using a 4x4 keypad, provides real-time feedback through a 16x2 I2C LCD display, and responds with visual LED indicators and an audible buzzer alarm.

---

## Team
- Chris Thomas

---

## Hardware Components

| Component | Description |
|---|---|
| Arduino Mega 2560 | ELEGOO Mega 2560 R3 microcontroller |
| 16x2 LCD + I2C Backpack | LCD1602 with PCF8574 adapter (address 0x27) |
| 4x4 Membrane Keypad | 16-key matrix keypad |
| Green LED | Unlocked / ACTIVE state indicator |
| Red LED | OFF / ALARM state indicator |
| Yellow LED | Armed / IDLE state indicator |
| White LED | Flashlight — active when system is armed |
| Active Buzzer | Alarm actuator |
| Push Buttons × 3 | ON/ARM (pin 32), Inside toggle (pin 30), OFF/Reset (pin 31) |
| 220Ω Resistors × 4 | Current limiting for LEDs |
| Breadboard + Jumpers | Full-size breadboard |

---

## Pin Connections

| Component | Pin |
|---|---|
| LCD SDA | 20 |
| LCD SCL | 21 |
| Keypad Rows | 43, 42, 41, 40 |
| Keypad Cols | 37, 38, 39, 36 |
| Green LED | 22 |
| Red LED | 23 |
| Yellow LED | 24 |
| White LED | 25 |
| Buzzer | 26 |
| Button 1 — ON/ARM | 32 |
| Button 2 — Inside toggle | 30 |
| Button 3 — OFF/Reset | 31 |

---

## State Machine

The system operates across four states:

| State | LED | Description |
|---|---|---|
| **OFF** | Red | System inactive. Press Button 1 to arm. |
| **IDLE** | Yellow + White | Armed and waiting for passcode entry. |
| **ACTIVE** | Green (flashing) | Unlocked. Press Button 2 to re-lock. |
| **ERROR / ALARM** | Red + Buzzer | Triggered after 4 failed attempts. |

---

## How to Use

1. Power on → splash screen displays **"ACV Buzio"** for 5 seconds → enters **OFF** state
2. Press **Button 1** to arm the system → enters **IDLE** state
3. Enter passcode `1231` on the keypad, press `#` to submit
   - `*` = backspace
4. Correct code → **ACTIVE** state (unlocked, green LED flashes)
5. Press **Button 2** from inside to lock/unlock without a code
6. 4 consecutive wrong attempts → **ALARM** state (buzzer + red LED)
7. Disarm alarm by entering correct code + `#`, or pressing **Button 2 three times** within 1.5 seconds
8. Press **Button 3** at any time to return to **OFF**

---

## Libraries Required

Install via Arduino IDE Library Manager:
- `LiquidCrystal_I2C` by Frank de Brabander
- `Keypad` by Mark Stanley and Alexander Brevig
- `Serial` library functions

UART communication is implemented directly via `UBRR0`, `UCSR0B`, `UCSR0C`, and `UDR0` registers. All timing uses `millis()`.
