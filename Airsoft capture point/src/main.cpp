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

// Red team / yellow team buttons + LEDs (LEDs must be PWM-capable pins)
const uint8_t BTN_RED_PIN = 2;
const uint8_t LED_RED_PIN = 3;
const uint8_t BTN_YELLOW_PIN = 7;
const uint8_t LED_YELLOW_PIN = 6;

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

const int MIN_GOAL_SCORE = 1;
const int MAX_GOAL_SCORE = 99;
const int DEFAULT_GOAL_SCORE = 5; // first team to this many captures wins

// Buzzer tones
const unsigned int BEEP_FREQ_PROGRESS = 1000;
const unsigned int BEEP_DURATION_PROGRESS_MS = 60;
const unsigned int BEEP_FREQ_CAPTURED = 2000;
const unsigned int BEEP_DURATION_CAPTURED_MS = 450;
const unsigned int BEEP_FREQ_WIN = 2600;
const unsigned int BEEP_DURATION_WIN_MS = 700;

// ---------------------------------------------------------------------------
// EEPROM layout
// ---------------------------------------------------------------------------

const int EEPROM_MAGIC_ADDR = 0;
const uint8_t EEPROM_MAGIC_VAL = 0xA5;
const int EEPROM_CAPTURE_TIME_ADDR = 1; // 1 byte: seconds
const int EEPROM_PIN_ADDR = 2;          // 4 bytes: ASCII digits
                                        // byte 6 is free (e.g. for a saved language)
const int EEPROM_GOAL_ADDR = 7;         // 1 byte: goal score

// ---------------------------------------------------------------------------
// On-screen text
//
// All text lives in the STRINGS table, one row per language, so the prop can
// be translated without touching the rest of the code. To add a language:
//   1. Add it to the Lang enum, before LANG_COUNT (e.g. LANG_DE).
//   2. Add a matching row to STRINGS with one string per StrId, in the same
//      order as the English row. Each must fit the 16-column display, and
//      the %s / %d / %% placeholders must stay as they are. The scoreboard
//      needs both team names plus their scores on one line, so keep the
//      names short (together at most 10 characters).
//   3. Set currentLang to the new language.
// The LCD's built-in character set is ASCII plus Japanese katakana, so
// letters such as ä, ö, é or ß won't display; they'd need custom characters
// made with lcd.createChar() (8 slots at most).
// To switch language from the settings menu instead, add a menu entry that
// changes currentLang and save it to EEPROM byte 6.
// ---------------------------------------------------------------------------

enum Lang { LANG_EN, LANG_COUNT };
Lang currentLang = LANG_EN;

enum StrId {
  STR_TEAM_RED,
  STR_TEAM_YELLOW,
  STR_HINT_SETTINGS,
  STR_CAPTURING_FMT,
  STR_WIN_FMT,
  STR_ENTER_PIN,
  STR_MENU_LINE0,
  STR_MENU_LINE1,
  STR_SET_TIME_TITLE,
  STR_SET_TIME_HINT,
  STR_SET_GOAL_TITLE,
  STR_SET_GOAL_HINT,
  STR_COUNT
};

const char *const STRINGS[LANG_COUNT][STR_COUNT] = {
  { // LANG_EN
    "Red",
    "Yellow",
    "* for settings",
    "%s: %d%%",
    "%s wins!",
    "Enter admin PIN:",
    "1)Time 2)Reset",
    "3)Goal  #=Exit",
    "Capture time (s)",
    "1-60, # to save",
    "Goal score (pts)",
    "1-99, # to save"
  }
};

const char *tr(StrId id) {
  return STRINGS[currentLang][id];
}

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

LiquidCrystal lcd(LCD_RS, LCD_E, LCD_D4, LCD_D5, LCD_D6, LCD_D7);
Keypad keypad = Keypad(makeKeymap(keypadKeys), keypadRowPins, keypadColPins, KEYPAD_ROWS, KEYPAD_COLS);

unsigned long captureTimeMs = DEFAULT_CAPTURE_SECONDS * 1000UL;
char adminPin[5] = "1234"; // 4 digits + null terminator
int goalScore = DEFAULT_GOAL_SCORE;

struct Team {
  uint8_t buttonPin;
  uint8_t ledPin;
  StrId name;
  int score = 0;

  bool rawState = HIGH;      // last raw button reading (active LOW)
  bool debouncedState = HIGH;
  unsigned long lastEdgeMs = 0;

  bool holding = false;
  unsigned long holdStartMs = 0;

  bool flashing = false;     // "captured" LED flash in progress
  unsigned long flashUntilMs = 0;
};

Team teamRed = {BTN_RED_PIN, LED_RED_PIN, STR_TEAM_RED};
Team teamYellow = {BTN_YELLOW_PIN, LED_YELLOW_PIN, STR_TEAM_YELLOW};

bool gameOver = false;
const Team *winner = nullptr;

enum AppState {
  STATE_SCOREBOARD,
  STATE_ENTER_PIN,
  STATE_MENU,
  STATE_SET_CAPTURE_TIME,
  STATE_SET_GOAL
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
  EEPROM.update(EEPROM_GOAL_ADDR, (uint8_t)goalScore);
}

void loadSettings() {
  if (EEPROM.read(EEPROM_MAGIC_ADDR) != EEPROM_MAGIC_VAL) {
    captureTimeMs = DEFAULT_CAPTURE_SECONDS * 1000UL;
    memcpy(adminPin, DEFAULT_PIN, 5);
    goalScore = DEFAULT_GOAL_SCORE;
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

  uint8_t goal = EEPROM.read(EEPROM_GOAL_ADDR);
  goalScore = (goal >= MIN_GOAL_SCORE && goal <= MAX_GOAL_SCORE) ? goal : DEFAULT_GOAL_SCORE;
}

// ---------------------------------------------------------------------------
// Team / capture logic
// ---------------------------------------------------------------------------

// A hold is only valid while the scoreboard is live; a stale one would count
// the whole time away as hold time and score instantly on return.
void cancelHolds() {
  Team *teams[] = {&teamRed, &teamYellow};
  for (Team *t : teams) {
    if (t->holding) {
      t->holding = false;
      analogWrite(t->ledPin, 0);
    }
  }
}

void resetScores() {
  teamRed.score = 0;
  teamYellow.score = 0;
  teamRed.holding = teamYellow.holding = false;
  teamRed.flashing = teamYellow.flashing = false;
  gameOver = false;
  winner = nullptr;
  analogWrite(teamRed.ledPin, 0);
  analogWrite(teamYellow.ledPin, 0);
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
      lcdNeedsRedraw = true;

      if (team.score >= goalScore) {
        gameOver = true;
        winner = &team;
        cancelHolds();
        tone(BUZZER_PIN, BEEP_FREQ_WIN, BEEP_DURATION_WIN_MS);
      } else {
        tone(BUZZER_PIN, BEEP_FREQ_CAPTURED, BEEP_DURATION_CAPTURED_MS);
      }
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
  if (teamRed.holding && teamYellow.holding) {
    active = (captureProgressPercent(teamRed) >= captureProgressPercent(teamYellow)) ? &teamRed : &teamYellow;
  } else if (teamRed.holding) {
    active = &teamRed;
  } else if (teamYellow.holding) {
    active = &teamYellow;
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
  // Red's score on the left, yellow's on the right: "Red:2    Yellow:3"
  char left[17], right[17], line1[17];
  snprintf(left, sizeof(left), "%s:%d", tr(teamRed.name), teamRed.score);
  snprintf(right, sizeof(right), "%s:%d", tr(teamYellow.name), teamYellow.score);
  int gap = 16 - (int)strlen(left) - (int)strlen(right);
  snprintf(line1, sizeof(line1), "%s%*s%s", left, gap > 1 ? gap : 1, "", right);
  lcd.setCursor(0, 0);
  printPadded(line1, 16);

  char line2[17];
  if (gameOver) {
    snprintf(line2, sizeof(line2), tr(STR_WIN_FMT), tr(winner->name));
  } else {
    int progRed = captureProgressPercent(teamRed);
    int progYellow = captureProgressPercent(teamYellow);
    if (progRed > 0 && progYellow > 0) {
      // Both holding: only the names' first letters fit, e.g. "R: 42%  Y: 17%"
      snprintf(line2, sizeof(line2), "%c:%3d%%  %c:%3d%%",
               tr(teamRed.name)[0], progRed, tr(teamYellow.name)[0], progYellow);
    } else if (progRed > 0) {
      snprintf(line2, sizeof(line2), tr(STR_CAPTURING_FMT), tr(teamRed.name), progRed);
    } else if (progYellow > 0) {
      snprintf(line2, sizeof(line2), tr(STR_CAPTURING_FMT), tr(teamYellow.name), progYellow);
    } else {
      snprintf(line2, sizeof(line2), "%s", tr(STR_HINT_SETTINGS));
    }
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

void renderSetGoal() {
  lcd.setCursor(0, 0);
  printPadded(tr(STR_SET_GOAL_TITLE), 16);
  lcd.setCursor(0, 1);
  printPadded(entryLen ? entryBuffer : tr(STR_SET_GOAL_HINT), 16);
}

void render() {
  switch (appState) {
    case STATE_SCOREBOARD:        renderScoreboard();       break;
    case STATE_ENTER_PIN:         renderEnterPin();         break;
    case STATE_MENU:              renderMenu();             break;
    case STATE_SET_CAPTURE_TIME:  renderSetCaptureTime();   break;
    case STATE_SET_GOAL:          renderSetGoal();          break;
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
        cancelHolds();
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
        enterState(STATE_SET_GOAL);
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

    case STATE_SET_GOAL:
      if (key == '*') {
        enterState(STATE_MENU);
      } else if (key == '#') {
        if (entryLen > 0) {
          int pts = atoi(entryBuffer);
          if (pts >= MIN_GOAL_SCORE && pts <= MAX_GOAL_SCORE) {
            goalScore = pts;
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
  pinMode(BTN_RED_PIN, INPUT_PULLUP);
  pinMode(BTN_YELLOW_PIN, INPUT_PULLUP);
  pinMode(LED_RED_PIN, OUTPUT);
  pinMode(LED_YELLOW_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  loadSettings();

  lcd.begin(16, 2);

  teamRed.rawState = teamRed.debouncedState = digitalRead(BTN_RED_PIN);
  teamYellow.rawState = teamYellow.debouncedState = digitalRead(BTN_YELLOW_PIN);
}

void loop() {
  updateButton(teamRed);
  updateButton(teamYellow);

  // Only track capture progress on the main scoreboard screen — settings
  // menu interaction takes priority over gameplay input. Once a team has
  // hit the goal score, gameplay is frozen until a reset.
  if (appState == STATE_SCOREBOARD && !gameOver) {
    updateCapture(teamRed);
    updateCapture(teamYellow);
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
  if (appState == STATE_SCOREBOARD && (teamRed.holding || teamYellow.holding)) {
    render();
  }
}
