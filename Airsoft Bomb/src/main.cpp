#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <EEPROM.h>

// ---------------------------------------------------------------------------
// Pin configuration — adjust these to match your actual wiring.
// ---------------------------------------------------------------------------

// Status LEDs. Green = bomb not counting down; red = blinks with each
// countdown beep, like the Counter-Strike C4.
const uint8_t LED_RED_PIN = 2;
const uint8_t LED_GREEN_PIN = 3;

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
const uint8_t CODE_LEN = 6; // arming and disarming codes
const uint8_t PIN_LEN = 4;  // admin PIN
const char *DEFAULT_ARM_CODE = "111111";
const char *DEFAULT_DEFUSE_CODE = "222222";
const char *DEFAULT_ADMIN_PIN = "1234";

// Countdown modelled on the Counter-Strike C4: a short high beep with a red
// blink, the gap shrinking from 1 s to 0.1 s as time runs out, then one
// continuous tone for the final second.
const unsigned int BEEP_FREQ_TICK = 2900;
const unsigned int BEEP_DURATION_TICK_MS = 90;
const unsigned long RED_FLASH_MS = 90;
const unsigned long TICK_INTERVAL_START_MS = 1000;
const unsigned long TICK_INTERVAL_END_MS = 100;
const unsigned long FINAL_TONE_MS = 1000;
const unsigned int FINAL_TONE_FREQ = 3400;

const unsigned int KEY_CLICK_FREQ = 1800;   // keypad press while arming/disarming
const unsigned int KEY_CLICK_MS = 35;
const unsigned int BEEP_FREQ_WRONG = 300;
const unsigned int BEEP_DURATION_WRONG_MS = 200;

// After a wrong code the message stays up and the keypad is locked this long.
const unsigned long WRONG_CODE_COOLDOWN_MS = 3000;

// The defused screen blinks a full row of blocks three times; each phase
// lines up with one step of SEQ_DEFUSED.
const unsigned long DEFUSE_BLINK_MS = 120;

// After a round, # must be held this long to reset, so a stray press by a
// player can't wipe the result. A shorter tap shows a hint instead.
const unsigned long RESET_HOLD_MS = 2000;
const unsigned long HOLD_HINT_MS = 2000;

struct Note { uint16_t freq; uint16_t ms; };
const uint16_t NOISE = 1; // random low rumble: the closest a piezo gets to a boom
const Note SEQ_PLANTED[]   = {{2900, 90}, {0, 70}, {2900, 90}};
const Note SEQ_DEFUSED[]   = {{1800, 120}, {0, 120}, {2400, 120}, {0, 120}, {3200, 300}};
const Note SEQ_EXPLOSION[] = {{NOISE, 1600}};

// Buzzer/LED switching noise can knock the HD44780 out of 4-bit sync and
// garble the screen, so the static end screens periodically resync it.
const unsigned long LCD_RESYNC_MS = 3000;

const char BLOCK = (char)0xFF; // solid block in the HD44780 character ROM

// ---------------------------------------------------------------------------
// EEPROM layout
// ---------------------------------------------------------------------------

const int EEPROM_MAGIC_ADDR = 0;
const uint8_t EEPROM_MAGIC_VAL = 0xB6; // change whenever the layout changes; resets to defaults
const int EEPROM_COUNTDOWN_ADDR = 1;   // 2 bytes: uint16_t seconds
const int EEPROM_ARM_CODE_ADDR = 3;    // 6 bytes
const int EEPROM_DEFUSE_CODE_ADDR = 9; // 6 bytes
const int EEPROM_ADMIN_PIN_ADDR = 15;  // 4 bytes
const int EEPROM_LANG_ADDR = 19;       // 1 byte: Lang enum value

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
  STR_TITLE_DISARMED,
  STR_ENTER_ARM_CODE,
  STR_HINT_SETTINGS,
  STR_TITLE_ARMED,
  STR_ENTER_DEFUSE_CODE,
  STR_TITLE_DEFUSED,
  STR_BOOM_TITLE,
  STR_BOOM_SUBTITLE,
  STR_HINT_SETTINGS_RESET,
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
  STR_ENTER_6_DIGITS,
  STR_WRONG_CODE,
  STR_SET_PIN_TITLE,
  STR_CONFIRM_PIN_TITLE,
  STR_ENTER_4_DIGITS,
  STR_PIN_MISMATCH,
  STR_HOLD_TO_RESET,
  STR_COUNT
};

const char *const STRINGS_EN[STR_COUNT] = {
  "** BOMB DISARMED **",
  "Enter arming code",
  "Press * for Settings",
  "*** BOMB ARMED ***",
  "Enter disarming code",
  "*** BOMB DEFUSED ***",
  "!!!!!! BOOM !!!!!!",
  "The bomb exploded!",
  "*=Settings  #=Reset",
  "Enter the admin PIN:",
  "# = OK   * = Back",
  "1) Timer  2) PIN",
  "3) Arming code",
  "4) Disarming code",
  "5) Language #/*=Exit",
  "Set timer (seconds)",
  "Valid range: %d-%d",
  "# = Save  * = Cancel",
  "New arming code:",
  "New disarming code:",
  "Enter 6-digit code:",
  "Wrong code!",
  "New admin PIN:",
  "Repeat new PIN:",
  "Enter 4-digit PIN:",
  "PINs don't match",
  "Hold # to reset"
};

const char *const STRINGS_FI[STR_COUNT] = {
  "POMMI EI VIRITETTY",
  "Syota virityskoodi",
  "Paina * asetuksiin",
  "* POMMI VIRITETTY *",
  "Syota purkukoodi",
  "** POMMI PURETTU **",
  "!!!!!! PAM !!!!!!",
  "Pommi on rajahtanyt!",
  "*=Asetukset #=Nollaa",
  "Anna PIN-koodi:",
  "# = OK  * = Takaisin",
  "1) Ajastin  2) PIN",
  "3) Virityskoodi",
  "4) Purkukoodi",
  "5) Kieli #/*=Poistu",
  "Aseta ajastin (s)",
  "Sallittu alue %d-%d",
  "#=Tallenna *=Peru",
  "Uusi virityskoodi:",
  "Uusi purkukoodi:",
  "Anna 6 numeroa:",
  "Vaara koodi",
  "Uusi PIN-koodi:",
  "Toista PIN-koodi:",
  "Anna 4 numeroa:",
  "Koodit eivat tasmaa",
  "Pida # pohjassa"
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
char armCode[CODE_LEN + 1];
char defuseCode[CODE_LEN + 1];
char adminPin[PIN_LEN + 1];
char pendingPin[PIN_LEN + 1]; // new PIN awaiting its confirmation entry

enum AppState {
  STATE_IDLE,
  STATE_ARMED,
  STATE_DEFUSED,
  STATE_EXPLODED,
  STATE_ENTER_ADMIN_PIN,
  STATE_MENU,
  STATE_SET_TIME,
  STATE_SET_ADMIN_PIN,
  STATE_CONFIRM_ADMIN_PIN,
  STATE_SET_ARM_CODE,
  STATE_SET_DEFUSE_CODE
};

AppState appState = STATE_IDLE;
AppState returnState = STATE_IDLE; // where to go back to if admin PIN entry is cancelled/wrong

char entryBuffer[CODE_LEN + 1] = ""; // CODE_LEN is the longest entry
uint8_t entryLen = 0;

bool resetHolding = false;
unsigned long resetHoldStartMs = 0;

unsigned long armedStartMs = 0;
unsigned long armedRemainingMs = 0;
unsigned long defusedRemainingMs = 0;
unsigned long defusedAtMs = 0;

char transientMsg[LCD_COLS + 1] = "";
unsigned long transientUntilMs = 0;

unsigned long lastBeepMs = 0;
unsigned long nextTickBeepMs = 0;
unsigned long redFlashUntilMs = 0;
bool pendulumRight = true;
bool finalToneOn = false;

const Note *seq = nullptr;
uint8_t seqLen = 0;
uint8_t seqIdx = 0;
unsigned long seqStepEndMs = 0;
unsigned long nextNoiseMs = 0;

unsigned long nextLcdResyncMs = 0;

// What the screen should show vs. what it currently shows. Only the
// characters that differ are sent, since a full 4-line rewrite over I2C
// blocks for ~0.1 s and would make the countdown animation stutter.
char frame[LCD_ROWS][LCD_COLS];
char shown[LCD_ROWS][LCD_COLS];

// ---------------------------------------------------------------------------
// EEPROM helpers
// ---------------------------------------------------------------------------

void saveCode(int addr, const char *code, uint8_t len) {
  for (uint8_t i = 0; i < len; i++) EEPROM.update(addr + i, code[i]);
}

void saveSettings() {
  EEPROM.update(EEPROM_MAGIC_ADDR, EEPROM_MAGIC_VAL);
  uint16_t seconds = (uint16_t)(countdownDurationMs / 1000UL);
  EEPROM.put(EEPROM_COUNTDOWN_ADDR, seconds);
  saveCode(EEPROM_ARM_CODE_ADDR, armCode, CODE_LEN);
  saveCode(EEPROM_DEFUSE_CODE_ADDR, defuseCode, CODE_LEN);
  saveCode(EEPROM_ADMIN_PIN_ADDR, adminPin, PIN_LEN);
  EEPROM.update(EEPROM_LANG_ADDR, (uint8_t)currentLang);
}

void loadCode(int addr, char *code, uint8_t len, const char *fallback) {
  for (uint8_t i = 0; i < len; i++) {
    char c = EEPROM.read(addr + i);
    code[i] = (c >= '0' && c <= '9') ? c : fallback[i];
  }
  code[len] = '\0';
}

void loadSettings() {
  if (EEPROM.read(EEPROM_MAGIC_ADDR) != EEPROM_MAGIC_VAL) {
    countdownDurationMs = DEFAULT_COUNTDOWN_SECONDS * 1000UL;
    memcpy(armCode, DEFAULT_ARM_CODE, CODE_LEN + 1);
    memcpy(defuseCode, DEFAULT_DEFUSE_CODE, CODE_LEN + 1);
    memcpy(adminPin, DEFAULT_ADMIN_PIN, PIN_LEN + 1);
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

  loadCode(EEPROM_ARM_CODE_ADDR, armCode, CODE_LEN, DEFAULT_ARM_CODE);
  loadCode(EEPROM_DEFUSE_CODE_ADDR, defuseCode, CODE_LEN, DEFAULT_DEFUSE_CODE);
  loadCode(EEPROM_ADMIN_PIN_ADDR, adminPin, PIN_LEN, DEFAULT_ADMIN_PIN);

  uint8_t lang = EEPROM.read(EEPROM_LANG_ADDR);
  currentLang = (lang == LANG_FI) ? LANG_FI : LANG_EN;
}

// ---------------------------------------------------------------------------
// LEDs
// ---------------------------------------------------------------------------

void updateLeds() {
  bool green = appState != STATE_ARMED && appState != STATE_EXPLODED;
  bool red = false;
  if (appState == STATE_ARMED) {
    red = finalToneOn || millis() < redFlashUntilMs;
  } else if (appState == STATE_EXPLODED) {
    red = seq ? (millis() / 50) % 2 : true; // flicker during the blast, then stay lit
  }
  digitalWrite(LED_GREEN_PIN, green ? HIGH : LOW);
  digitalWrite(LED_RED_PIN, red ? HIGH : LOW);
}

// ---------------------------------------------------------------------------
// Sound sequences (non-blocking)
// ---------------------------------------------------------------------------

void startNote() {
  const Note &n = seq[seqIdx];
  seqStepEndMs = millis() + n.ms;
  nextNoiseMs = 0;
  if (n.freq == 0) noTone(BUZZER_PIN);
  else if (n.freq != NOISE) tone(BUZZER_PIN, n.freq);
}

template <size_t N>
void playSeq(const Note (&s)[N]) {
  seq = s;
  seqLen = N;
  seqIdx = 0;
  startNote();
}

void stopSound() {
  seq = nullptr;
  finalToneOn = false;
  noTone(BUZZER_PIN);
}

void updateSeq() {
  if (!seq) return;
  if (millis() >= seqStepEndMs) {
    if (++seqIdx >= seqLen) {
      seq = nullptr;
      noTone(BUZZER_PIN);
      return;
    }
    startNote();
  }
  if (seq[seqIdx].freq == NOISE && millis() >= nextNoiseMs) {
    tone(BUZZER_PIN, random(60, 260));
    nextNoiseMs = millis() + 12;
  }
}

// Short one-off beeps; skipped during the final-second tone so it isn't cut off.
void blip(unsigned int freq, unsigned int ms) {
  if (!finalToneOn) tone(BUZZER_PIN, freq, ms);
}

// ---------------------------------------------------------------------------
// Code entry and wrong-code cooldown
// ---------------------------------------------------------------------------

void resetEntry() {
  entryLen = 0;
  entryBuffer[0] = '\0';
}

bool appendDigit(char key, uint8_t maxLen) {
  if (entryLen >= maxLen) return false;
  entryBuffer[entryLen++] = key;
  entryBuffer[entryLen] = '\0';
  return true;
}

void showTransient(const char *msg, unsigned long ms = WRONG_CODE_COOLDOWN_MS) {
  strncpy(transientMsg, msg, sizeof(transientMsg) - 1);
  transientMsg[sizeof(transientMsg) - 1] = '\0';
  transientUntilMs = millis() + ms;
}

bool transientActive() {
  return transientMsg[0] != '\0';
}

void updateTransient() {
  if (transientActive() && millis() >= transientUntilMs) transientMsg[0] = '\0';
}

// ---------------------------------------------------------------------------
// State transitions
// ---------------------------------------------------------------------------

void enterIdle() {
  appState = STATE_IDLE;
  resetEntry();
  stopSound();
  transientMsg[0] = '\0';
  resetHolding = false;
}

void enterArmed() {
  appState = STATE_ARMED;
  resetEntry();
  armedStartMs = millis();
  armedRemainingMs = countdownDurationMs;
  lastBeepMs = millis();
  nextTickBeepMs = millis() + TICK_INTERVAL_START_MS; // let the "planted" chirp finish first
  pendulumRight = true;
  redFlashUntilMs = 0;
  finalToneOn = false;
  playSeq(SEQ_PLANTED);
}

void enterDefused() {
  appState = STATE_DEFUSED;
  resetEntry();
  defusedRemainingMs = armedRemainingMs;
  defusedAtMs = millis();
  finalToneOn = false;
  playSeq(SEQ_DEFUSED);
  nextLcdResyncMs = millis() + LCD_RESYNC_MS;
}

void enterExploded() {
  appState = STATE_EXPLODED;
  resetEntry();
  transientMsg[0] = '\0';
  finalToneOn = false;
  playSeq(SEQ_EXPLOSION);
  nextLcdResyncMs = millis() + LCD_RESYNC_MS;
}

void enterAdminGate(AppState from) {
  stopSound(); // untimed tones would otherwise play on through the menu
  returnState = from;
  appState = STATE_ENTER_ADMIN_PIN;
  resetEntry();
  transientMsg[0] = '\0';
  resetHolding = false;
}

// ---------------------------------------------------------------------------
// Hold # to reset (defused/exploded screens)
// ---------------------------------------------------------------------------

// getKey() only reports presses, so a hold is read from the library's key list.
bool keyDown(char c) {
  int i = keypad.findInList(c);
  return i >= 0 && (keypad.key[i].kstate == PRESSED || keypad.key[i].kstate == HOLD);
}

void startResetHold() {
  blip(KEY_CLICK_FREQ, KEY_CLICK_MS);
  transientMsg[0] = '\0';
  resetHolding = true;
  resetHoldStartMs = millis();
}

void updateResetHold() {
  if (!resetHolding) return;
  if (!keyDown('#')) {
    resetHolding = false;
    showTransient(tr(STR_HOLD_TO_RESET), HOLD_HINT_MS);
  } else if (millis() - resetHoldStartMs >= RESET_HOLD_MS) {
    enterIdle();
  }
}

// ---------------------------------------------------------------------------
// Countdown
// ---------------------------------------------------------------------------

void updateArmed() {
  unsigned long elapsed = millis() - armedStartMs;
  armedRemainingMs = (elapsed >= countdownDurationMs) ? 0 : countdownDurationMs - elapsed;

  if (armedRemainingMs == 0) {
    enterExploded();
    return;
  }

  if (armedRemainingMs <= FINAL_TONE_MS) {
    if (!finalToneOn) {
      seq = nullptr;
      finalToneOn = true;
      tone(BUZZER_PIN, FINAL_TONE_FREQ);
    }
    return;
  }

  if (millis() >= nextTickBeepMs) {
    tone(BUZZER_PIN, BEEP_FREQ_TICK, BEEP_DURATION_TICK_MS);
    redFlashUntilMs = millis() + RED_FLASH_MS;
    unsigned long interval = TICK_INTERVAL_END_MS +
        (TICK_INTERVAL_START_MS - TICK_INTERVAL_END_MS) * armedRemainingMs / countdownDurationMs;
    lastBeepMs = millis();
    nextTickBeepMs = lastBeepMs + interval;
    pendulumRight = !pendulumRight;
  }
}

// The block crosses the whole row once per beep interval, reaching the far
// end exactly as the next beep plays, then swings back.
uint8_t pendulumPos() {
  unsigned long span = nextTickBeepMs - lastBeepMs;
  unsigned long t = millis() - lastBeepMs;
  if (t > span) t = span;
  uint8_t p = t * (LCD_COLS - 1) / span;
  return pendulumRight ? p : (LCD_COLS - 1) - p;
}

// ---------------------------------------------------------------------------
// LCD rendering
// ---------------------------------------------------------------------------

void invalidateLcd() {
  memset(shown, 0, sizeof(shown)); // 0 is never drawn, so every cell gets resent
}

void putLine(uint8_t row, const char *text, bool center = false) {
  uint8_t len = 0;
  while (len < LCD_COLS && text[len]) len++;
  uint8_t start = center ? (LCD_COLS - len) / 2 : 0;
  memset(frame[row], ' ', LCD_COLS);
  memcpy(frame[row] + start, text, len);
}

void fillLine(uint8_t row, char c) {
  memset(frame[row], c, LCD_COLS);
}

void flushLcd() {
  for (uint8_t r = 0; r < LCD_ROWS; r++) {
    uint8_t c = 0;
    while (c < LCD_COLS) {
      if (frame[r][c] == shown[r][c]) { c++; continue; }
      lcd.setCursor(c, r);
      while (c < LCD_COLS && frame[r][c] != shown[r][c]) {
        lcd.write((uint8_t)frame[r][c]);
        shown[r][c] = frame[r][c];
        c++;
      }
    }
  }
}

// MM:SS.t, rounded up to the tenth, so a fresh 90 s bomb shows 01:30.0 and
// the last shown value is 00:00.1.
void putTime(uint8_t row, unsigned long ms) {
  unsigned long tenths = (ms + 99) / 100;
  unsigned long s = tenths / 10;
  char timeStr[12];
  snprintf(timeStr, sizeof(timeStr), "%02lu:%02lu.%lu", s / 60, s % 60, tenths % 10);
  putLine(row, timeStr, true);
}

void maskedEntry(char *out, size_t outSize) {
  uint8_t i = 0;
  for (; i < entryLen && i + 1 < outSize; i++) out[i] = '*';
  out[i] = '\0';
}

// Typed digits, with * for the positions still to fill: "1*****", "12****", ...
void codeEntryLine(uint8_t row, StrId prompt) {
  if (transientActive()) {
    putLine(row, transientMsg, true);
  } else if (entryLen == 0) {
    putLine(row, tr(prompt), true);
  } else {
    char code[CODE_LEN + 1];
    for (uint8_t i = 0; i < CODE_LEN; i++) code[i] = i < entryLen ? entryBuffer[i] : '*';
    code[CODE_LEN] = '\0';
    putLine(row, code, true);
  }
}

void renderIdle() {
  putLine(0, tr(STR_TITLE_DISARMED), true);
  codeEntryLine(1, STR_ENTER_ARM_CODE);
  putLine(2, "");
  putLine(3, tr(STR_HINT_SETTINGS));
}

void renderArmed() {
  putLine(0, tr(STR_TITLE_ARMED), true);
  putTime(1, armedRemainingMs);
  codeEntryLine(2, STR_ENTER_DEFUSE_CODE);
  if (finalToneOn) {
    fillLine(3, BLOCK);
  } else {
    fillLine(3, ' ');
    frame[3][pendulumPos()] = BLOCK;
  }
}

// Bottom row of the end screens: a bar filling up while # is held, the
// "hold #" hint after a short tap, otherwise the key hint.
void endScreenHintLine() {
  if (resetHolding) {
    unsigned long held = millis() - resetHoldStartMs;
    if (held > RESET_HOLD_MS) held = RESET_HOLD_MS;
    fillLine(3, ' ');
    memset(frame[3], BLOCK, held * LCD_COLS / RESET_HOLD_MS);
  } else if (transientActive()) {
    putLine(3, transientMsg);
  } else {
    putLine(3, tr(STR_HINT_SETTINGS_RESET));
  }
}

void renderDefused() {
  putLine(0, tr(STR_TITLE_DEFUSED), true);
  putTime(1, defusedRemainingMs);
  putLine(2, "");
  unsigned long e = millis() - defusedAtMs;
  if (e < 6 * DEFUSE_BLINK_MS && !resetHolding) {
    fillLine(3, (e / DEFUSE_BLINK_MS) % 2 == 0 ? BLOCK : ' ');
  } else {
    endScreenHintLine();
  }
}

void renderExploded() {
  putLine(0, tr(STR_BOOM_TITLE), true);
  putLine(1, tr(STR_BOOM_SUBTITLE), true);
  putLine(2, "");
  endScreenHintLine();
}

void renderEnterAdminPin() {
  putLine(0, tr(STR_ENTER_PIN));
  char masked[PIN_LEN + 1];
  maskedEntry(masked, sizeof(masked));
  putLine(1, masked);
  putLine(2, "");
  putLine(3, tr(STR_HINT_PIN_CONFIRM_BACK));
}

void renderMenu() {
  putLine(0, tr(STR_MENU_LINE0));
  putLine(1, tr(STR_MENU_LINE1));
  putLine(2, tr(STR_MENU_LINE2));
  putLine(3, tr(STR_MENU_LINE3));
}

void renderSetTime() {
  putLine(0, tr(STR_SET_TIME_TITLE));
  char range[LCD_COLS + 1];
  snprintf(range, sizeof(range), tr(STR_RANGE_FMT), MIN_COUNTDOWN_SECONDS, MAX_COUNTDOWN_SECONDS);
  putLine(1, range);
  putLine(2, entryLen > 0 ? entryBuffer : tr(STR_HINT_SAVE_CANCEL));
  putLine(3, "");
}

void renderSetCode(StrId title, StrId prompt, StrId footer = STR_HINT_SAVE_CANCEL) {
  putLine(0, tr(title));
  putLine(1, transientActive() ? transientMsg : tr(prompt));
  putLine(2, entryBuffer);
  putLine(3, tr(footer));
}

void render() {
  switch (appState) {
    case STATE_IDLE:              renderIdle();                                        break;
    case STATE_ARMED:             renderArmed();                                       break;
    case STATE_DEFUSED:           renderDefused();                                     break;
    case STATE_EXPLODED:          renderExploded();                                    break;
    case STATE_ENTER_ADMIN_PIN:   renderEnterAdminPin();                               break;
    case STATE_MENU:              renderMenu();                                        break;
    case STATE_SET_TIME:          renderSetTime();                                     break;
    case STATE_SET_ADMIN_PIN:
      renderSetCode(STR_SET_PIN_TITLE, STR_ENTER_4_DIGITS, STR_HINT_PIN_CONFIRM_BACK);
      break;
    case STATE_CONFIRM_ADMIN_PIN: renderSetCode(STR_CONFIRM_PIN_TITLE, STR_ENTER_4_DIGITS); break;
    case STATE_SET_ARM_CODE:      renderSetCode(STR_SET_ARM_TITLE, STR_ENTER_6_DIGITS);     break;
    case STATE_SET_DEFUSE_CODE:   renderSetCode(STR_SET_DEFUSE_TITLE, STR_ENTER_6_DIGITS);  break;
  }
  flushLcd();
}

// ---------------------------------------------------------------------------
// Keypad handling
// ---------------------------------------------------------------------------

void openSetting(AppState setting) {
  appState = setting;
  resetEntry();
  transientMsg[0] = '\0';
}

void handleKey(char key) {
  // After a wrong code the keypad stays locked until the message clears.
  if ((appState == STATE_IDLE || appState == STATE_ARMED) && transientActive()) return;

  switch (appState) {
    case STATE_IDLE:
    case STATE_ARMED: {
      bool armed = appState == STATE_ARMED;
      if (isdigit(key)) {
        if (appendDigit(key, CODE_LEN)) blip(KEY_CLICK_FREQ, KEY_CLICK_MS);
      } else if (key == '#' && entryLen > 0) { // a stray # with nothing typed is ignored
        bool ok = entryLen == CODE_LEN && strcmp(entryBuffer, armed ? defuseCode : armCode) == 0;
        resetEntry();
        if (ok && armed) {
          enterDefused();
        } else if (ok) {
          enterArmed();
        } else {
          blip(BEEP_FREQ_WRONG, BEEP_DURATION_WRONG_MS);
          showTransient(tr(STR_WRONG_CODE));
        }
      } else if (key == '*') {
        if (entryLen > 0 || armed) resetEntry(); // settings are locked while armed
        else enterAdminGate(STATE_IDLE);
      }
      break;
    }

    case STATE_DEFUSED:
    case STATE_EXPLODED:
      if (key == '*') enterAdminGate(appState);
      else if (key == '#') startResetHold();
      break;

    case STATE_ENTER_ADMIN_PIN:
      if (isdigit(key)) {
        appendDigit(key, PIN_LEN);
      } else if (key == '#') {
        bool ok = entryLen == PIN_LEN && strcmp(entryBuffer, adminPin) == 0;
        resetEntry();
        if (!ok) blip(BEEP_FREQ_WRONG, BEEP_DURATION_WRONG_MS);
        appState = ok ? STATE_MENU : returnState;
      } else if (key == '*') {
        resetEntry();
        appState = returnState;
      }
      break;

    case STATE_MENU:
      if (key == '1') {
        openSetting(STATE_SET_TIME);
      } else if (key == '2') {
        openSetting(STATE_SET_ADMIN_PIN);
      } else if (key == '3') {
        openSetting(STATE_SET_ARM_CODE);
      } else if (key == '4') {
        openSetting(STATE_SET_DEFUSE_CODE);
      } else if (key == '5') {
        currentLang = (currentLang == LANG_EN) ? LANG_FI : LANG_EN;
        saveSettings();
      } else if (key == '#' || key == '*') {
        saveSettings();
        enterIdle();
      }
      break;

    case STATE_SET_TIME:
      if (isdigit(key)) {
        appendDigit(key, 3);
      } else if (key == '#' || key == '*') {
        if (key == '#' && entryLen > 0) {
          int seconds = atoi(entryBuffer);
          if (seconds >= MIN_COUNTDOWN_SECONDS && seconds <= MAX_COUNTDOWN_SECONDS) {
            countdownDurationMs = (unsigned long)seconds * 1000UL;
          }
        }
        openSetting(STATE_MENU);
      }
      break;

    // A new PIN has to be typed twice, since a typo would lock the admin out.
    case STATE_SET_ADMIN_PIN:
    case STATE_CONFIRM_ADMIN_PIN:
      if (isdigit(key)) {
        appendDigit(key, PIN_LEN);
      } else if (key == '#' && entryLen == PIN_LEN && appState == STATE_SET_ADMIN_PIN) {
        memcpy(pendingPin, entryBuffer, PIN_LEN + 1);
        openSetting(STATE_CONFIRM_ADMIN_PIN);
      } else if (key == '#' && entryLen == PIN_LEN) {
        if (strcmp(entryBuffer, pendingPin) == 0) {
          memcpy(adminPin, pendingPin, PIN_LEN + 1);
          openSetting(STATE_MENU);
        } else {
          openSetting(STATE_SET_ADMIN_PIN);
          blip(BEEP_FREQ_WRONG, BEEP_DURATION_WRONG_MS);
          showTransient(tr(STR_PIN_MISMATCH));
        }
      } else if (key == '#' || key == '*') {
        openSetting(STATE_MENU);
      }
      break;

    case STATE_SET_ARM_CODE:
    case STATE_SET_DEFUSE_CODE:
      if (isdigit(key)) {
        appendDigit(key, CODE_LEN);
      } else if (key == '#' || key == '*') {
        if (key == '#' && entryLen == CODE_LEN) {
          memcpy(appState == STATE_SET_ARM_CODE ? armCode : defuseCode, entryBuffer, CODE_LEN + 1);
        }
        openSetting(STATE_MENU);
      }
      break;
  }
}

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------

void setup() {
  pinMode(LED_RED_PIN, OUTPUT);
  pinMode(LED_GREEN_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  loadSettings();

  Wire.begin();
  lcd.init();
  lcd.backlight();
  invalidateLcd();

  enterIdle();
}

void loop() {
  char key = keypad.getKey();
  if (key) {
    handleKey(key);
  }
  updateResetHold();

  if (appState == STATE_ARMED) updateArmed();
  updateSeq();
  updateLeds();
  updateTransient();

  if ((appState == STATE_EXPLODED || appState == STATE_DEFUSED) && millis() >= nextLcdResyncMs) {
    lcd.begin(LCD_COLS, LCD_ROWS); // re-runs the HD44780 init sequence to recover sync
    lcd.backlight();
    invalidateLcd();
    nextLcdResyncMs = millis() + LCD_RESYNC_MS;
  }

  render();
}
