#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <EEPROM.h>

// ---------------------------------------------------------------------------
// Pin configuration — adjust these to match your actual wiring.
// ---------------------------------------------------------------------------

// Status LEDs (no buttons wired in this gamemode — LEDs are feedback only)
const uint8_t LED_A_PIN = 2;
const uint8_t LED_B_PIN = 3;

// Passive buzzer (driven with tone())
const uint8_t BUZZER_PIN = 9;

// 4x4 matrix keypad
const byte KEYPAD_ROWS = 4;
const byte KEYPAD_COLS = 4;
char keypadKeys[KEYPAD_ROWS][KEYPAD_COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};
byte keypadRowPins[KEYPAD_ROWS] = {4, 5, 6, 7};
byte keypadColPins[KEYPAD_COLS] = {8, 10, 11, 12};

// 20x4 I2C LCD. Wired to the Uno R4's dedicated SDA/SCL pins via Wire.begin()
// — no digital pin numbers needed here. If the display shows nothing, the
// most common fix is trying address 0x3F instead of 0x27.
const uint8_t LCD_I2C_ADDR = 0x27;
const uint8_t LCD_COLS = 20;
const uint8_t LCD_ROWS = 4;

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------

const int MIN_COUNTDOWN_SECONDS = 10;
const int MAX_COUNTDOWN_SECONDS = 600;
const int DEFAULT_COUNTDOWN_SECONDS = 90;
const char *DEFAULT_ARM_CODE = "1111";
const char *DEFAULT_DEFUSE_CODE = "2222";
const char *DEFAULT_ADMIN_PIN = "1234";

const unsigned long LED_ARMED_FLASH_MS = 400;   // sync flash rate while armed
const unsigned long LED_EXPLODE_FLASH_MS = 100; // rapid flash after exploding

const unsigned int BEEP_FREQ_TICK = 1200;
const unsigned int BEEP_DURATION_TICK_MS = 60;
const unsigned long TICK_INTERVAL_START_MS = 900; // tick rate right after arming
const unsigned long TICK_INTERVAL_END_MS = 150;   // tick rate just before detonation

const unsigned int BEEP_FREQ_ARMED_CONFIRM = 1500;
const unsigned int BEEP_FREQ_DEFUSED_CONFIRM = 2000;
const unsigned int BEEP_DURATION_CONFIRM_MS = 450;
const unsigned int BEEP_FREQ_WRONG = 300;
const unsigned int BEEP_DURATION_WRONG_MS = 200;

const unsigned int ALARM_FREQ_HIGH = 1800;
const unsigned int ALARM_FREQ_LOW = 900;
const unsigned long ALARM_TOGGLE_MS = 150;

const unsigned long TRANSIENT_MSG_MS = 1200; // how long "Wrong code" etc. stays up

// ---------------------------------------------------------------------------
// EEPROM layout
// ---------------------------------------------------------------------------

const int EEPROM_MAGIC_ADDR = 0;
const uint8_t EEPROM_MAGIC_VAL = 0xB5;
const int EEPROM_COUNTDOWN_ADDR = 1;   // 2 bytes: uint16_t seconds
const int EEPROM_ARM_CODE_ADDR = 3;    // 4 bytes
const int EEPROM_DEFUSE_CODE_ADDR = 7; // 4 bytes
const int EEPROM_ADMIN_PIN_ADDR = 11;  // 4 bytes
const int EEPROM_LANG_ADDR = 15;       // 1 byte: Lang enum value

// ---------------------------------------------------------------------------
// Language / on-screen text
//
// The HD44780 controller's built-in character ROM doesn't reliably include
// Finnish ä/ö at normal ASCII positions, so Finnish strings intentionally
// drop the umlaut dots (e.g. "virittää" -> "virita", "räjähti" -> "rajahti")
// rather than risk garbled glyphs on hardware using the common "A00" ROM.
// ---------------------------------------------------------------------------

enum Lang { LANG_EN, LANG_FI };
Lang currentLang = LANG_EN;

enum StrId {
  STR_BOMB_SAFE,
  STR_ENTER_ARM_CODE,
  STR_HINT_SETTINGS,
  STR_ARMED_TITLE,
  STR_TIME_FMT,
  STR_ENTER_DEFUSE_CODE,
  STR_HINT_DEFUSE_CLEAR,
  STR_DEFUSED_TITLE,
  STR_BOOM_TITLE,
  STR_BOOM_SUBTITLE,
  STR_ENTER_PIN,
  STR_HINT_PIN_CONFIRM_BACK,
  STR_MENU_LINE0,
  STR_MENU_LINE1,
  STR_MENU_LINE2,
  STR_MENU_LINE3,
  STR_SET_TIME_TITLE,
  STR_RANGE_FMT,
  STR_HINT_SAVE_CANCEL,
  STR_SET_ARM_TITLE,
  STR_SET_DEFUSE_TITLE,
  STR_ENTER_4_DIGITS,
  STR_WRONG_CODE,
  STR_COUNT
};

const char *const STRINGS_EN[STR_COUNT] = {
  "*** BOMB SAFE ***",
  "Enter the arm code:",
  "Press * for Settings",
  "**** BOMB ARMED ****",
  "Time left: %s",
  "Enter defuse code:",
  "# = Defuse * = Clear",
  "*** BOMB DEFUSED ***",
  "!!!!!! BOOM !!!!!!",
  "The bomb exploded!",
  "Enter the admin PIN:",
  "# = OK   * = Back",
  "1) Timer",
  "2) Arm code",
  "3) Defuse code",
  "4) Language #/*=Exit",
  "Set timer (seconds)",
  "Valid range: %d-%d",
  "# = Save  * = Cancel",
  "New arm code:",
  "New defuse code:",
  "Enter 4-digit code:",
  "Wrong code!"
};

const char *const STRINGS_FI[STR_COUNT] = {
  "** POMMI TURVASSA **",
  "Syota viritys koodi:",
  "Paina * asetuksiin",
  "* POMMI VIRITETTY *",
  "Aikaa jaljella %s",
  "Anna purkukoodi:",
  "#=Pura *=Tyhjenna",
  "** POMMI PURETTU **",
  "!!!!!! PAM !!!!!!",
  "Pommi on rajahtanyt!",
  "Anna PIN-koodi:",
  "# = OK  * = Takaisin",
  "1) Ajastin",
  "2) Virityskoodi",
  "3) Purkukoodi",
  "4) Kieli #/*=Poistu",
  "Aseta ajastin (s)",
  "Sallittu alue %d-%d",
  "#=Tallenna *=Peru",
  "Uusi virityskoodi:",
  "Uusi purkukoodi:",
  "Anna 4 numeroa:",
  "Vaara koodi"
};

const char *tr(StrId id) {
  return (currentLang == LANG_FI) ? STRINGS_FI[id] : STRINGS_EN[id];
}

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

LiquidCrystal_I2C lcd(LCD_I2C_ADDR, LCD_COLS, LCD_ROWS);
Keypad keypad = Keypad(makeKeymap(keypadKeys), keypadRowPins, keypadColPins, KEYPAD_ROWS, KEYPAD_COLS);

unsigned long countdownDurationMs;
char armCode[5];
char defuseCode[5];
char adminPin[5];

enum AppState {
  STATE_IDLE,
  STATE_ARMED,
  STATE_DEFUSED,
  STATE_EXPLODED,
  STATE_ENTER_ADMIN_PIN,
  STATE_MENU,
  STATE_SET_TIME,
  STATE_SET_ARM_CODE,
  STATE_SET_DEFUSE_CODE
};

AppState appState = STATE_IDLE;
AppState returnState = STATE_IDLE; // where to go back to if admin PIN entry is cancelled/wrong

char entryBuffer[5] = "";
uint8_t entryLen = 0;

unsigned long armedStartMs = 0;
unsigned long armedRemainingMs = 0;
unsigned long defusedRemainingMs = 0;

bool lcdNeedsRedraw = true;
int lastDisplayedSeconds = -1;

char transientMsg[LCD_COLS + 1] = "";
unsigned long transientUntilMs = 0;

unsigned long nextTickBeepMs = 0;
unsigned long ledFlashToggleMs = 0;
bool ledFlashOn = false;

unsigned long nextAlarmToggleMs = 0;
bool alarmToneHigh = false;

// ---------------------------------------------------------------------------
// EEPROM helpers
// ---------------------------------------------------------------------------

void saveSettings() {
  EEPROM.update(EEPROM_MAGIC_ADDR, EEPROM_MAGIC_VAL);
  uint16_t seconds = (uint16_t)(countdownDurationMs / 1000UL);
  EEPROM.put(EEPROM_COUNTDOWN_ADDR, seconds);
  for (uint8_t i = 0; i < 4; i++) EEPROM.update(EEPROM_ARM_CODE_ADDR + i, armCode[i]);
  for (uint8_t i = 0; i < 4; i++) EEPROM.update(EEPROM_DEFUSE_CODE_ADDR + i, defuseCode[i]);
  for (uint8_t i = 0; i < 4; i++) EEPROM.update(EEPROM_ADMIN_PIN_ADDR + i, adminPin[i]);
  EEPROM.update(EEPROM_LANG_ADDR, (uint8_t)currentLang);
}

void loadSettings() {
  if (EEPROM.read(EEPROM_MAGIC_ADDR) != EEPROM_MAGIC_VAL) {
    countdownDurationMs = DEFAULT_COUNTDOWN_SECONDS * 1000UL;
    memcpy(armCode, DEFAULT_ARM_CODE, 5);
    memcpy(defuseCode, DEFAULT_DEFUSE_CODE, 5);
    memcpy(adminPin, DEFAULT_ADMIN_PIN, 5);
    currentLang = LANG_EN;
    saveSettings();
    return;
  }

  uint16_t seconds;
  EEPROM.get(EEPROM_COUNTDOWN_ADDR, seconds);
  if (seconds < MIN_COUNTDOWN_SECONDS || seconds > MAX_COUNTDOWN_SECONDS) {
    seconds = DEFAULT_COUNTDOWN_SECONDS;
  }
  countdownDurationMs = (unsigned long)seconds * 1000UL;

  for (uint8_t i = 0; i < 4; i++) {
    char c = EEPROM.read(EEPROM_ARM_CODE_ADDR + i);
    armCode[i] = (c >= '0' && c <= '9') ? c : DEFAULT_ARM_CODE[i];
  }
  armCode[4] = '\0';

  for (uint8_t i = 0; i < 4; i++) {
    char c = EEPROM.read(EEPROM_DEFUSE_CODE_ADDR + i);
    defuseCode[i] = (c >= '0' && c <= '9') ? c : DEFAULT_DEFUSE_CODE[i];
  }
  defuseCode[4] = '\0';

  for (uint8_t i = 0; i < 4; i++) {
    char c = EEPROM.read(EEPROM_ADMIN_PIN_ADDR + i);
    adminPin[i] = (c >= '0' && c <= '9') ? c : DEFAULT_ADMIN_PIN[i];
  }
  adminPin[4] = '\0';

  uint8_t lang = EEPROM.read(EEPROM_LANG_ADDR);
  currentLang = (lang == LANG_FI) ? LANG_FI : LANG_EN;
}

// ---------------------------------------------------------------------------
// Small utilities
// ---------------------------------------------------------------------------

void setLeds(bool on) {
  digitalWrite(LED_A_PIN, on ? HIGH : LOW);
  digitalWrite(LED_B_PIN, on ? HIGH : LOW);
}

void formatMMSS(unsigned long ms, char *out, size_t outSize) {
  unsigned long totalSeconds = ms / 1000UL;
  unsigned int mm = totalSeconds / 60;
  unsigned int ss = totalSeconds % 60;
  snprintf(out, outSize, "%02u:%02u", mm, ss);
}

void maskedEntry(char *out, size_t outSize) {
  uint8_t i = 0;
  for (; i < entryLen && i + 1 < outSize; i++) out[i] = '*';
  out[i] = '\0';
}

void printLine(uint8_t row, const char *text) {
  char buf[LCD_COLS + 1];
  snprintf(buf, sizeof(buf), "%-*.*s", LCD_COLS, LCD_COLS, text);
  lcd.setCursor(0, row);
  lcd.print(buf);
}

// ---------------------------------------------------------------------------
// Transient status messages (e.g. "Wrong code")
// ---------------------------------------------------------------------------

void showTransient(const char *msg) {
  strncpy(transientMsg, msg, sizeof(transientMsg) - 1);
  transientMsg[sizeof(transientMsg) - 1] = '\0';
  transientUntilMs = millis() + TRANSIENT_MSG_MS;
  lcdNeedsRedraw = true;
}

bool transientActive() {
  return transientMsg[0] != '\0';
}

void updateTransient() {
  if (transientActive() && millis() >= transientUntilMs) {
    transientMsg[0] = '\0';
    lcdNeedsRedraw = true;
  }
}

// ---------------------------------------------------------------------------
// State transitions
// ---------------------------------------------------------------------------

void enterIdle() {
  appState = STATE_IDLE;
  entryLen = 0;
  entryBuffer[0] = '\0';
  setLeds(false);
  noTone(BUZZER_PIN);
  transientMsg[0] = '\0';
  lastDisplayedSeconds = -1;
  lcdNeedsRedraw = true;
}

void enterArmed() {
  appState = STATE_ARMED;
  entryLen = 0;
  entryBuffer[0] = '\0';
  armedStartMs = millis();
  armedRemainingMs = countdownDurationMs;
  lastDisplayedSeconds = -1;
  nextTickBeepMs = millis();
  ledFlashOn = false;
  ledFlashToggleMs = millis();
  tone(BUZZER_PIN, BEEP_FREQ_ARMED_CONFIRM, BEEP_DURATION_CONFIRM_MS);
  lcdNeedsRedraw = true;
}

void enterDefused() {
  appState = STATE_DEFUSED;
  entryLen = 0;
  entryBuffer[0] = '\0';
  defusedRemainingMs = armedRemainingMs;
  setLeds(true);
  tone(BUZZER_PIN, BEEP_FREQ_DEFUSED_CONFIRM, BEEP_DURATION_CONFIRM_MS);
  lcdNeedsRedraw = true;
}

void enterExploded() {
  appState = STATE_EXPLODED;
  entryLen = 0;
  entryBuffer[0] = '\0';
  nextAlarmToggleMs = millis();
  alarmToneHigh = false;
  ledFlashOn = false;
  ledFlashToggleMs = millis();
  lcdNeedsRedraw = true;
}

void enterAdminGate(AppState from) {
  returnState = from;
  appState = STATE_ENTER_ADMIN_PIN;
  entryLen = 0;
  entryBuffer[0] = '\0';
  lcdNeedsRedraw = true;
}

// ---------------------------------------------------------------------------
// Per-state background updates (countdown, LEDs, buzzer)
// ---------------------------------------------------------------------------

void updateArmed() {
  unsigned long elapsed = millis() - armedStartMs;
  armedRemainingMs = (elapsed >= countdownDurationMs) ? 0 : countdownDurationMs - elapsed;

  int currentSeconds = (int)(armedRemainingMs / 1000UL);
  if (currentSeconds != lastDisplayedSeconds) {
    lastDisplayedSeconds = currentSeconds;
    lcdNeedsRedraw = true;
  }

  if (armedRemainingMs == 0) {
    enterExploded();
    return;
  }

  if (millis() - ledFlashToggleMs >= LED_ARMED_FLASH_MS) {
    ledFlashToggleMs = millis();
    ledFlashOn = !ledFlashOn;
    setLeds(ledFlashOn);
  }

  if (millis() >= nextTickBeepMs) {
    tone(BUZZER_PIN, BEEP_FREQ_TICK, BEEP_DURATION_TICK_MS);
    unsigned long elapsedPercent = 100UL - ((armedRemainingMs * 100UL) / countdownDurationMs);
    unsigned long interval = map(elapsedPercent, 0, 100, TICK_INTERVAL_START_MS, TICK_INTERVAL_END_MS);
    nextTickBeepMs = millis() + interval;
  }
}

void updateExploded() {
  if (millis() - ledFlashToggleMs >= LED_EXPLODE_FLASH_MS) {
    ledFlashToggleMs = millis();
    ledFlashOn = !ledFlashOn;
    setLeds(ledFlashOn);
  }

  if (millis() >= nextAlarmToggleMs) {
    alarmToneHigh = !alarmToneHigh;
    tone(BUZZER_PIN, alarmToneHigh ? ALARM_FREQ_HIGH : ALARM_FREQ_LOW);
    nextAlarmToggleMs = millis() + ALARM_TOGGLE_MS;
  }
}

// ---------------------------------------------------------------------------
// LCD rendering
// ---------------------------------------------------------------------------

void renderIdle() {
  printLine(0, tr(STR_BOMB_SAFE));
  printLine(1, transientActive() ? transientMsg : tr(STR_ENTER_ARM_CODE));
  char masked[5];
  maskedEntry(masked, sizeof(masked));
  printLine(2, masked);
  printLine(3, tr(STR_HINT_SETTINGS));
}

void renderArmed() {
  printLine(0, tr(STR_ARMED_TITLE));
  char timeStr[6];
  formatMMSS(armedRemainingMs, timeStr, sizeof(timeStr));
  if (transientActive()) {
    printLine(1, transientMsg);
  } else {
    char line1[LCD_COLS + 1];
    snprintf(line1, sizeof(line1), tr(STR_TIME_FMT), timeStr);
    printLine(1, line1);
  }
  char masked[5];
  maskedEntry(masked, sizeof(masked));
  printLine(2, entryLen > 0 ? masked : tr(STR_ENTER_DEFUSE_CODE));
  printLine(3, tr(STR_HINT_DEFUSE_CLEAR));
}

void renderDefused() {
  printLine(0, tr(STR_DEFUSED_TITLE));
  char timeStr[6];
  formatMMSS(defusedRemainingMs, timeStr, sizeof(timeStr));
  char line1[LCD_COLS + 1];
  snprintf(line1, sizeof(line1), tr(STR_TIME_FMT), timeStr);
  printLine(1, line1);
  printLine(2, "");
  printLine(3, tr(STR_HINT_SETTINGS));
}

void renderExploded() {
  printLine(0, tr(STR_BOOM_TITLE));
  printLine(1, tr(STR_BOOM_SUBTITLE));
  printLine(2, "");
  printLine(3, tr(STR_HINT_SETTINGS));
}

void renderEnterAdminPin() {
  printLine(0, tr(STR_ENTER_PIN));
  char masked[5];
  maskedEntry(masked, sizeof(masked));
  printLine(1, masked);
  printLine(2, "");
  printLine(3, tr(STR_HINT_PIN_CONFIRM_BACK));
}

void renderMenu() {
  printLine(0, tr(STR_MENU_LINE0));
  printLine(1, tr(STR_MENU_LINE1));
  printLine(2, tr(STR_MENU_LINE2));
  printLine(3, tr(STR_MENU_LINE3));
}

void renderSetTime() {
  printLine(0, tr(STR_SET_TIME_TITLE));
  char range[LCD_COLS + 1];
  snprintf(range, sizeof(range), tr(STR_RANGE_FMT), MIN_COUNTDOWN_SECONDS, MAX_COUNTDOWN_SECONDS);
  printLine(1, range);
  printLine(2, entryLen > 0 ? entryBuffer : tr(STR_HINT_SAVE_CANCEL));
  printLine(3, "");
}

void renderSetArmCode() {
  printLine(0, tr(STR_SET_ARM_TITLE));
  printLine(1, tr(STR_ENTER_4_DIGITS));
  printLine(2, entryBuffer);
  printLine(3, tr(STR_HINT_SAVE_CANCEL));
}

void renderSetDefuseCode() {
  printLine(0, tr(STR_SET_DEFUSE_TITLE));
  printLine(1, tr(STR_ENTER_4_DIGITS));
  printLine(2, entryBuffer);
  printLine(3, tr(STR_HINT_SAVE_CANCEL));
}

void render() {
  switch (appState) {
    case STATE_IDLE:              renderIdle();            break;
    case STATE_ARMED:              renderArmed();           break;
    case STATE_DEFUSED:            renderDefused();         break;
    case STATE_EXPLODED:           renderExploded();        break;
    case STATE_ENTER_ADMIN_PIN:    renderEnterAdminPin();   break;
    case STATE_MENU:               renderMenu();            break;
    case STATE_SET_TIME:           renderSetTime();         break;
    case STATE_SET_ARM_CODE:       renderSetArmCode();      break;
    case STATE_SET_DEFUSE_CODE:    renderSetDefuseCode();   break;
  }
}

// ---------------------------------------------------------------------------
// Keypad handling
// ---------------------------------------------------------------------------

void handleKey(char key) {
  switch (appState) {
    case STATE_IDLE:
      if (isdigit(key)) {
        if (entryLen < 4) { entryBuffer[entryLen++] = key; entryBuffer[entryLen] = '\0'; lcdNeedsRedraw = true; }
      } else if (key == '#') {
        entryBuffer[entryLen] = '\0';
        bool ok = (entryLen == 4 && strcmp(entryBuffer, armCode) == 0);
        entryLen = 0;
        entryBuffer[0] = '\0';
        if (ok) {
          enterArmed();
        } else {
          tone(BUZZER_PIN, BEEP_FREQ_WRONG, BEEP_DURATION_WRONG_MS);
          showTransient(tr(STR_WRONG_CODE));
        }
      } else if (key == '*') {
        if (entryLen > 0) {
          entryLen = 0;
          entryBuffer[0] = '\0';
          lcdNeedsRedraw = true;
        } else {
          enterAdminGate(STATE_IDLE);
        }
      }
      break;

    case STATE_ARMED:
      if (isdigit(key)) {
        if (entryLen < 4) { entryBuffer[entryLen++] = key; entryBuffer[entryLen] = '\0'; lcdNeedsRedraw = true; }
      } else if (key == '#') {
        entryBuffer[entryLen] = '\0';
        bool ok = (entryLen == 4 && strcmp(entryBuffer, defuseCode) == 0);
        entryLen = 0;
        entryBuffer[0] = '\0';
        if (ok) {
          enterDefused();
        } else {
          tone(BUZZER_PIN, BEEP_FREQ_WRONG, BEEP_DURATION_WRONG_MS);
          showTransient(tr(STR_WRONG_CODE));
        }
      } else if (key == '*') {
        entryLen = 0;
        entryBuffer[0] = '\0';
        lcdNeedsRedraw = true;
      }
      break;

    case STATE_DEFUSED:
      if (key == '*') enterAdminGate(STATE_DEFUSED);
      break;

    case STATE_EXPLODED:
      if (key == '*') enterAdminGate(STATE_EXPLODED);
      break;

    case STATE_ENTER_ADMIN_PIN:
      if (isdigit(key)) {
        if (entryLen < 4) { entryBuffer[entryLen++] = key; entryBuffer[entryLen] = '\0'; lcdNeedsRedraw = true; }
      } else if (key == '#') {
        entryBuffer[entryLen] = '\0';
        bool ok = (entryLen == 4 && strcmp(entryBuffer, adminPin) == 0);
        entryLen = 0;
        entryBuffer[0] = '\0';
        appState = ok ? STATE_MENU : returnState;
        lcdNeedsRedraw = true;
      } else if (key == '*') {
        entryLen = 0;
        entryBuffer[0] = '\0';
        appState = returnState;
        lcdNeedsRedraw = true;
      }
      break;

    case STATE_MENU:
      if (key == '1') {
        appState = STATE_SET_TIME; entryLen = 0; entryBuffer[0] = '\0'; lcdNeedsRedraw = true;
      } else if (key == '2') {
        appState = STATE_SET_ARM_CODE; entryLen = 0; entryBuffer[0] = '\0'; lcdNeedsRedraw = true;
      } else if (key == '3') {
        appState = STATE_SET_DEFUSE_CODE; entryLen = 0; entryBuffer[0] = '\0'; lcdNeedsRedraw = true;
      } else if (key == '4') {
        currentLang = (currentLang == LANG_EN) ? LANG_FI : LANG_EN;
        saveSettings();
        lcdNeedsRedraw = true;
      } else if (key == '#' || key == '*') {
        saveSettings();
        enterIdle();
      }
      break;

    case STATE_SET_TIME:
      if (isdigit(key)) {
        if (entryLen < 3) { entryBuffer[entryLen++] = key; entryBuffer[entryLen] = '\0'; lcdNeedsRedraw = true; }
      } else if (key == '#') {
        if (entryLen > 0) {
          int seconds = atoi(entryBuffer);
          if (seconds >= MIN_COUNTDOWN_SECONDS && seconds <= MAX_COUNTDOWN_SECONDS) {
            countdownDurationMs = (unsigned long)seconds * 1000UL;
          }
        }
        appState = STATE_MENU; entryLen = 0; entryBuffer[0] = '\0'; lcdNeedsRedraw = true;
      } else if (key == '*') {
        appState = STATE_MENU; entryLen = 0; entryBuffer[0] = '\0'; lcdNeedsRedraw = true;
      }
      break;

    case STATE_SET_ARM_CODE:
      if (isdigit(key)) {
        if (entryLen < 4) { entryBuffer[entryLen++] = key; entryBuffer[entryLen] = '\0'; lcdNeedsRedraw = true; }
      } else if (key == '#') {
        if (entryLen == 4) memcpy(armCode, entryBuffer, 5);
        appState = STATE_MENU; entryLen = 0; entryBuffer[0] = '\0'; lcdNeedsRedraw = true;
      } else if (key == '*') {
        appState = STATE_MENU; entryLen = 0; entryBuffer[0] = '\0'; lcdNeedsRedraw = true;
      }
      break;

    case STATE_SET_DEFUSE_CODE:
      if (isdigit(key)) {
        if (entryLen < 4) { entryBuffer[entryLen++] = key; entryBuffer[entryLen] = '\0'; lcdNeedsRedraw = true; }
      } else if (key == '#') {
        if (entryLen == 4) memcpy(defuseCode, entryBuffer, 5);
        appState = STATE_MENU; entryLen = 0; entryBuffer[0] = '\0'; lcdNeedsRedraw = true;
      } else if (key == '*') {
        appState = STATE_MENU; entryLen = 0; entryBuffer[0] = '\0'; lcdNeedsRedraw = true;
      }
      break;
  }
}

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------

void setup() {
  pinMode(LED_A_PIN, OUTPUT);
  pinMode(LED_B_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  setLeds(false);

  loadSettings();

  Wire.begin();
  lcd.init();
  lcd.backlight();

  enterIdle();
}

void loop() {
  char key = keypad.getKey();
  if (key) {
    handleKey(key);
  }

  switch (appState) {
    case STATE_ARMED:    updateArmed();    break;
    case STATE_EXPLODED: updateExploded(); break;
    default: break;
  }

  updateTransient();

  if (lcdNeedsRedraw) {
    render();
    lcdNeedsRedraw = false;
  }
}
