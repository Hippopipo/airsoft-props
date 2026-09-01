#include <Arduino.h>
#include <LiquidCrystal.h>
#include <Keypad.h>
#include <EEPROM.h>

// ---------------------------------------------------------------------------
// Pin configuration — adjust these to match your actual wiring.
// ---------------------------------------------------------------------------

// 16x2 parallel LCD (4-bit mode)
const uint8_t LCD_RS = A0;
const uint8_t LCD_E  = A1;
const uint8_t LCD_D4 = A2;
const uint8_t LCD_D5 = A3;
const uint8_t LCD_D6 = A4;
const uint8_t LCD_D7 = A5;

// Team A / Team B buttons + LEDs (LEDs must be PWM-capable pins)
const uint8_t BTN_A_PIN = 2;
const uint8_t LED_A_PIN = 3;
const uint8_t BTN_B_PIN = 7;
const uint8_t LED_B_PIN = 6;

// Passive buzzer (driven with tone()). Repurposes the Serial TX pin since
// every other pin is committed to the LCD/keypad/buttons/LEDs — move this if
// you need Serial debug output during development.
const uint8_t BUZZER_PIN = 1;

// 4x4 matrix keypad
const byte KEYPAD_ROWS = 4;
const byte KEYPAD_COLS = 4;
char keypadKeys[KEYPAD_ROWS][KEYPAD_COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};
byte keypadRowPins[KEYPAD_ROWS] = {4, 5, 8, 9};
byte keypadColPins[KEYPAD_COLS] = {10, 11, 12, 13};

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------

const unsigned long BUTTON_DEBOUNCE_MS = 30;
const unsigned long CAPTURE_FLASH_MS = 400;     // LED/buzzer "captured" flash length
const int MIN_CAPTURE_SECONDS = 1;
const int MAX_CAPTURE_SECONDS = 60;
const int DEFAULT_CAPTURE_SECONDS = 5;
const char *DEFAULT_PIN = "1234";

// Buzzer tones
const unsigned int BEEP_FREQ_PROGRESS = 1000;
const unsigned int BEEP_DURATION_PROGRESS_MS = 60;
const unsigned int BEEP_FREQ_CAPTURED = 2000;
const unsigned int BEEP_DURATION_CAPTURED_MS = 450;

// ---------------------------------------------------------------------------
// EEPROM layout
// ---------------------------------------------------------------------------

const int EEPROM_MAGIC_ADDR = 0;
const uint8_t EEPROM_MAGIC_VAL = 0xA5;
const int EEPROM_CAPTURE_TIME_ADDR = 1; // 1 byte: seconds
const int EEPROM_PIN_ADDR = 2;          // 4 bytes: ASCII digits
const int EEPROM_LANG_ADDR = 6;         // 1 byte: Lang enum value

// ---------------------------------------------------------------------------
// Language / on-screen text
//
// The HD44780 controller's built-in character ROM doesn't reliably include
// Finnish ä/ö at normal ASCII positions, so Finnish strings intentionally
// drop the umlaut dots (e.g. "virittää" -> "virita") rather than risk
// garbled glyphs on hardware using the common "A00" ROM.
// ---------------------------------------------------------------------------

enum Lang { LANG_EN, LANG_FI };
Lang currentLang = LANG_EN;

enum StrId {
  STR_HINT_SETTINGS,
  STR_CAPTURING_FMT,
  STR_ENTER_PIN,
  STR_MENU_LINE0,
  STR_MENU_LINE1,
  STR_SET_TIME_TITLE,
  STR_SET_TIME_HINT,
  STR_COUNT
};

const char *const STRINGS_EN[STR_COUNT] = {
  "* for settings",
  "Capturing %c %3d%%",
  "Enter admin PIN:",
  "1)Time 2)Reset",
  "3)Lang #/*=Exit",
  "Capture time (s)",
  "1-60, # to save"
};

const char *const STRINGS_FI[STR_COUNT] = {
  "* asetukset",
  "Vallataan %c %3d%%",
  "Anna PIN-koodi:",
  "1)Aika 2)Nollaa",
  "3)Kieli #/*=pois",
  "Valtausaika (s)",
  "1-60, # tallenna"
};

const char *tr(StrId id) {
  return (currentLang == LANG_FI) ? STRINGS_FI[id] : STRINGS_EN[id];
}

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

LiquidCrystal lcd(LCD_RS, LCD_E, LCD_D4, LCD_D5, LCD_D6, LCD_D7);
Keypad keypad = Keypad(makeKeymap(keypadKeys), keypadRowPins, keypadColPins, KEYPAD_ROWS, KEYPAD_COLS);

unsigned long captureTimeMs = DEFAULT_CAPTURE_SECONDS * 1000UL;
char adminPin[5] = "1234"; // 4 digits + null terminator

struct Team {
  uint8_t buttonPin;
  uint8_t ledPin;
  int score = 0;

  bool rawState = HIGH;      // last raw button reading (active LOW)
  bool debouncedState = HIGH;
  unsigned long lastEdgeMs = 0;

  bool holding = false;
  unsigned long holdStartMs = 0;

  bool flashing = false;     // "captured" LED flash in progress
  unsigned long flashUntilMs = 0;
};

Team teamA = {BTN_A_PIN, LED_A_PIN};
Team teamB = {BTN_B_PIN, LED_B_PIN};

enum AppState {
  STATE_SCOREBOARD,
  STATE_ENTER_PIN,
  STATE_MENU,
  STATE_SET_CAPTURE_TIME
};

AppState appState = STATE_SCOREBOARD;
char entryBuffer[5];   // scratch buffer for PIN / capture-time entry
uint8_t entryLen = 0;

unsigned long nextProgressBeepMs = 0;
bool lcdNeedsRedraw = true;

// ---------------------------------------------------------------------------
// EEPROM helpers
// ---------------------------------------------------------------------------

void saveSettings() {
  EEPROM.update(EEPROM_MAGIC_ADDR, EEPROM_MAGIC_VAL);
  EEPROM.update(EEPROM_CAPTURE_TIME_ADDR, (uint8_t)(captureTimeMs / 1000UL));
  for (uint8_t i = 0; i < 4; i++) {
    EEPROM.update(EEPROM_PIN_ADDR + i, adminPin[i]);
  }
  EEPROM.update(EEPROM_LANG_ADDR, (uint8_t)currentLang);
}

void loadSettings() {
  if (EEPROM.read(EEPROM_MAGIC_ADDR) != EEPROM_MAGIC_VAL) {
    captureTimeMs = DEFAULT_CAPTURE_SECONDS * 1000UL;
    memcpy(adminPin, DEFAULT_PIN, 5);
    currentLang = LANG_EN;
    saveSettings();
    return;
  }

  uint8_t seconds = EEPROM.read(EEPROM_CAPTURE_TIME_ADDR);
  if (seconds < MIN_CAPTURE_SECONDS || seconds > MAX_CAPTURE_SECONDS) {
    seconds = DEFAULT_CAPTURE_SECONDS;
  }
  captureTimeMs = seconds * 1000UL;

  for (uint8_t i = 0; i < 4; i++) {
    char c = EEPROM.read(EEPROM_PIN_ADDR + i);
    adminPin[i] = (c >= '0' && c <= '9') ? c : DEFAULT_PIN[i];
  }
  adminPin[4] = '\0';

  uint8_t lang = EEPROM.read(EEPROM_LANG_ADDR);
  currentLang = (lang == LANG_FI) ? LANG_FI : LANG_EN;
}

// ---------------------------------------------------------------------------
// Team / capture logic
// ---------------------------------------------------------------------------

void resetScores() {
  teamA.score = 0;
  teamB.score = 0;
}

// Reads + debounces a team's button, returns true if pressed (active LOW).
void updateButton(Team &team) {
  bool reading = digitalRead(team.buttonPin);
  if (reading != team.rawState) {
    team.lastEdgeMs = millis();
    team.rawState = reading;
  }
  if (millis() - team.lastEdgeMs > BUTTON_DEBOUNCE_MS) {
    team.debouncedState = team.rawState;
  }
}

int captureProgressPercent(const Team &team) {
  if (!team.holding) return 0;
  unsigned long elapsed = millis() - team.holdStartMs;
  if (elapsed >= captureTimeMs) return 100;
  return (int)((elapsed * 100UL) / captureTimeMs);
}

void updateCapture(Team &team) {
  bool pressed = (team.debouncedState == LOW);

  if (team.flashing) {
    if (millis() >= team.flashUntilMs) {
      team.flashing = false;
      analogWrite(team.ledPin, 0);
      lcdNeedsRedraw = true;
    }
    return; // ignore button changes until the capture flash finishes
  }

  if (pressed && !team.holding) {
    team.holding = true;
    team.holdStartMs = millis();
    lcdNeedsRedraw = true;
  } else if (!pressed && team.holding) {
    team.holding = false;
    analogWrite(team.ledPin, 0);
    lcdNeedsRedraw = true;
  }

  if (team.holding) {
    unsigned long elapsed = millis() - team.holdStartMs;
    if (elapsed >= captureTimeMs) {
      // Captured!
      team.score++;
      team.holding = false;
      team.flashing = true;
      team.flashUntilMs = millis() + CAPTURE_FLASH_MS;
      analogWrite(team.ledPin, 255);
      tone(BUZZER_PIN, BEEP_FREQ_CAPTURED, BEEP_DURATION_CAPTURED_MS);
      lcdNeedsRedraw = true;
    } else {
      int brightness = map(elapsed, 0, captureTimeMs, 0, 255);
      analogWrite(team.ledPin, brightness);
    }
  }
}

// Drives the single buzzer for whichever team is currently closest to
// capturing (repeating beep, faster as progress increases).
void updateProgressBuzzer() {
  Team *active = nullptr;
  if (teamA.holding && teamB.holding) {
    active = (captureProgressPercent(teamA) >= captureProgressPercent(teamB)) ? &teamA : &teamB;
  } else if (teamA.holding) {
    active = &teamA;
  } else if (teamB.holding) {
    active = &teamB;
  }

  if (active == nullptr) {
    return;
  }

  if (millis() >= nextProgressBeepMs) {
    tone(BUZZER_PIN, BEEP_FREQ_PROGRESS, BEEP_DURATION_PROGRESS_MS);
    int percent = captureProgressPercent(*active);
    unsigned long interval = map(percent, 0, 100, 320, 90);
    nextProgressBeepMs = millis() + interval;
  }
}

// ---------------------------------------------------------------------------
// LCD rendering
// ---------------------------------------------------------------------------

void printPadded(const char *text, uint8_t width) {
  if (width > 16) width = 16;
  char buf[17];
  snprintf(buf, sizeof(buf), "%-*.*s", width, width, text);
  lcd.print(buf);
}

void renderScoreboard() {
  char line1[17];
  snprintf(line1, sizeof(line1), "A:%-3d    B:%-3d", teamA.score, teamB.score);
  lcd.setCursor(0, 0);
  printPadded(line1, 16);

  char line2[17];
  int progA = captureProgressPercent(teamA);
  int progB = captureProgressPercent(teamB);
  if (progA > 0 && progB > 0) {
    snprintf(line2, sizeof(line2), "A:%3d%% B:%3d%%", progA, progB);
  } else if (progA > 0) {
    snprintf(line2, sizeof(line2), tr(STR_CAPTURING_FMT), 'A', progA);
  } else if (progB > 0) {
    snprintf(line2, sizeof(line2), tr(STR_CAPTURING_FMT), 'B', progB);
  } else {
    snprintf(line2, sizeof(line2), "%s", tr(STR_HINT_SETTINGS));
  }
  lcd.setCursor(0, 1);
  printPadded(line2, 16);
}

void renderEnterPin() {
  lcd.setCursor(0, 0);
  printPadded(tr(STR_ENTER_PIN), 16);
  char masked[5] = "";
  for (uint8_t i = 0; i < entryLen; i++) masked[i] = '*';
  masked[entryLen] = '\0';
  lcd.setCursor(0, 1);
  printPadded(masked, 16);
}

void renderMenu() {
  lcd.setCursor(0, 0);
  printPadded(tr(STR_MENU_LINE0), 16);
  lcd.setCursor(0, 1);
  printPadded(tr(STR_MENU_LINE1), 16);
}

void renderSetCaptureTime() {
  lcd.setCursor(0, 0);
  printPadded(tr(STR_SET_TIME_TITLE), 16);
  lcd.setCursor(0, 1);
  printPadded(entryLen ? entryBuffer : tr(STR_SET_TIME_HINT), 16);
}

void render() {
  switch (appState) {
    case STATE_SCOREBOARD:        renderScoreboard();       break;
    case STATE_ENTER_PIN:         renderEnterPin();         break;
    case STATE_MENU:              renderMenu();             break;
    case STATE_SET_CAPTURE_TIME:  renderSetCaptureTime();   break;
  }
}

// ---------------------------------------------------------------------------
// Keypad / settings menu handling
// ---------------------------------------------------------------------------

void enterState(AppState next) {
  appState = next;
  entryLen = 0;
  entryBuffer[0] = '\0';
  lcdNeedsRedraw = true;
}

void handleKey(char key) {
  switch (appState) {
    case STATE_SCOREBOARD:
      if (key == '*') {
        enterState(STATE_ENTER_PIN);
      }
      break;

    case STATE_ENTER_PIN:
      if (key == '*') {
        enterState(STATE_SCOREBOARD);
      } else if (key == '#') {
        entryBuffer[entryLen] = '\0';
        if (entryLen == 4 && strcmp(entryBuffer, adminPin) == 0) {
          enterState(STATE_MENU);
        } else {
          enterState(STATE_SCOREBOARD); // wrong PIN -> bail out
        }
      } else if (isdigit(key) && entryLen < 4) {
        entryBuffer[entryLen++] = key;
        lcdNeedsRedraw = true;
      }
      break;

    case STATE_MENU:
      if (key == '1') {
        enterState(STATE_SET_CAPTURE_TIME);
      } else if (key == '2') {
        resetScores();
        lcdNeedsRedraw = true;
      } else if (key == '3') {
        currentLang = (currentLang == LANG_EN) ? LANG_FI : LANG_EN;
        saveSettings();
        lcdNeedsRedraw = true;
      } else if (key == '#' || key == '*') {
        enterState(STATE_SCOREBOARD);
      }
      break;

    case STATE_SET_CAPTURE_TIME:
      if (key == '*') {
        enterState(STATE_MENU);
      } else if (key == '#') {
        if (entryLen > 0) {
          int seconds = atoi(entryBuffer);
          if (seconds >= MIN_CAPTURE_SECONDS && seconds <= MAX_CAPTURE_SECONDS) {
            captureTimeMs = (unsigned long)seconds * 1000UL;
            saveSettings();
          }
        }
        enterState(STATE_SCOREBOARD);
      } else if (isdigit(key) && entryLen < 2) {
        entryBuffer[entryLen++] = key;
        entryBuffer[entryLen] = '\0';
        lcdNeedsRedraw = true;
      }
      break;
  }
}

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------

void setup() {
  pinMode(BTN_A_PIN, INPUT_PULLUP);
  pinMode(BTN_B_PIN, INPUT_PULLUP);
  pinMode(LED_A_PIN, OUTPUT);
  pinMode(LED_B_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  loadSettings();

  lcd.begin(16, 2);

  teamA.rawState = teamA.debouncedState = digitalRead(BTN_A_PIN);
  teamB.rawState = teamB.debouncedState = digitalRead(BTN_B_PIN);
}

void loop() {
  updateButton(teamA);
  updateButton(teamB);

  // Only track capture progress on the main scoreboard screen — settings
  // menu interaction takes priority over gameplay input.
  if (appState == STATE_SCOREBOARD) {
    updateCapture(teamA);
    updateCapture(teamB);
    updateProgressBuzzer();
  }

  char key = keypad.getKey();
  if (key) {
    handleKey(key);
  }

  if (lcdNeedsRedraw) {
    render();
    lcdNeedsRedraw = false;
  }

  // Scoreboard screen redraws continuously to keep the live progress
  // percentage current while a team is holding.
  if (appState == STATE_SCOREBOARD && (teamA.holding || teamB.holding)) {
    render();
  }
}
