#include <Arduino.h>
#include <LiquidCrystal.h>
#include <Keypad.h>
#include <EEPROM.h>
#include <DataFlashBlockDevice.h>

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
const uint8_t LCD_COLS = 16;
const uint8_t LCD_ROWS = 2;

// Red team / yellow team buttons + LEDs (LEDs must be PWM-capable pins)
const uint8_t BTN_RED_PIN = 2;
const uint8_t LED_RED_PIN = 3;
const uint8_t BTN_YELLOW_PIN = 7;
const uint8_t LED_YELLOW_PIN = 6;

// Passive buzzer (driven with tone()). Uses the Serial1 TX pin since every
// other pin is committed to the LCD/keypad/buttons/LEDs. Serial (USB) debug
// output doesn't use this pin, so it still works.
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
const unsigned long CAPTURE_MS = 5000; // hold a button this long to take the point
const char *DEFAULT_PIN = "1234";

// # must be held this long to clear a finished game, so a stray press
// can't wipe the result.
const unsigned long HASH_HOLD_MS = 2000;

// Screens with several hints on the bottom line cycle through them this often.
const unsigned long HINT_SWAP_MS = 3000;

// The settings screens (PIN entry included) go back to the scoreboard after
// this long without a key press, since the buttons don't work while they're
// open.
const unsigned long MENU_TIMEOUT_MS = 15000;

// A running game is saved this often, so a power cut loses at most this much.
// Every capture, the end of a game and a reset are also saved straight away.
const unsigned long SAVE_INTERVAL_MS = 10000;

// Clock warnings: a double beep at 5 minutes left, a triple beep at 1 minute,
// then one beep per second for the last 10 seconds.
const unsigned long WARN_FIRST_SEC = 300;
const unsigned long WARN_SECOND_SEC = 60;
const unsigned long COUNTDOWN_SEC = 10;

// Buzzer tones
const unsigned int KEY_CLICK_FREQ = 1800;
const unsigned int KEY_CLICK_MS = 35;
const unsigned int BEEP_FREQ_PROGRESS = 1000;
const unsigned int BEEP_DURATION_PROGRESS_MS = 60;
const unsigned int POINT_TICK_FREQ = 1200;   // soft tick each time a point is scored
const unsigned int POINT_TICK_MS = 15;
const unsigned int COUNTDOWN_FREQ = 2500;
const unsigned int COUNTDOWN_MS = 100;
const unsigned int BEEP_FREQ_WRONG = 300;
const unsigned int BEEP_DURATION_WRONG_MS = 200;

struct Note { uint16_t freq; uint16_t ms; };
const Note SEQ_CAPTURED[]   = {{2000, 1000}}; // the point changes hands
const Note SEQ_WARN_FIRST[] = {{1500, 200}, {0, 150}, {1500, 200}};
const Note SEQ_WARN_SECOND[] = {{1500, 200}, {0, 150}, {1500, 200}, {0, 150}, {1500, 200}};
const Note SEQ_WIN[]        = {{2000, 200}, {0, 80}, {2600, 200}, {0, 80}, {3200, 900}};
const Note SEQ_DRAW[]       = {{1200, 1500}};

const char BLOCK = (char)0xFF; // solid block in the HD44780 character ROM

// ---------------------------------------------------------------------------
// EEPROM layout (settings)
// ---------------------------------------------------------------------------

const int EEPROM_MAGIC_ADDR = 0;
const uint8_t EEPROM_MAGIC_VAL = 0xA7; // change whenever the layout changes; resets to defaults
const int EEPROM_PIN_ADDR = 1;         // 4 bytes: ASCII digits
const int EEPROM_SETTINGS_ADDR = 5;    // 2 bytes (uint16_t) per entry of `settings`
                                       // byte 11 onwards is free (e.g. for a saved language)

// ---------------------------------------------------------------------------
// On-screen text
//
// All text lives in the STRINGS table, one row per language, so the prop can
// be translated without touching the rest of the code. To add a language:
//   1. Add it to the Lang enum, before LANG_COUNT (e.g. LANG_DE).
//   2. Add a matching row to STRINGS with one string per StrId, in the same
//      order as the English row. Each must fit the 16-column display, and
//      the %s / %d / %% placeholders must stay as they are. The scoreboard
//      shows only the first letter of each team name, so give the teams
//      names that start with different letters.
//   3. Set currentLang to the new language.
// The LCD's built-in character set is ASCII plus Japanese katakana, so
// letters such as ä, ö, é or ß won't display; they'd need custom characters
// made with lcd.createChar() (8 slots at most).
// To switch language from the settings menu instead, add a menu entry that
// changes currentLang and save it to EEPROM byte 11.
// ---------------------------------------------------------------------------

enum Lang { LANG_EN, LANG_COUNT };
Lang currentLang = LANG_EN;

enum StrId {
  STR_TEAM_RED,
  STR_TEAM_YELLOW,
  STR_CAPTURE_TO_START,
  STR_HINT_SETTINGS,
  STR_NEUTRAL,
  STR_HELD_BY_FMT,
  STR_CAPTURING_FMT,
  STR_WIN_FMT,
  STR_DRAW,
  STR_HOLD_TO_RESET,
  STR_HINT_STATS,
  STR_STATS_HELD,
  STR_STATS_CAPTURES,
  STR_STATS_LONGEST,
  STR_ENTER_PIN,
  STR_MENU_LINE0,
  STR_MENU_LINE1,
  STR_SET_GAME_TIME,
  STR_SET_POINT_TIME,
  STR_SET_GOAL,
  STR_RANGE_FMT,
  STR_CURRENT_FMT,
  STR_NEW_GAME,
  STR_YES_NO,
  STR_COUNT
};

const char *const STRINGS[LANG_COUNT][STR_COUNT] = {
  { // LANG_EN
    "Red",
    "Yellow",
    "Capture to start",
    "* for settings",
    "Point is neutral",
    "Held by %s",
    "%s: %d%%",
    "%s wins!",
    "Draw!",
    "Hold # to reset",
    "B/C/D for stats",
    "B: Time held",
    "C: Captures",
    "D: Longest hold",
    "Enter admin PIN:",
    "1Time  2Rate",
    "3Goal 4New #Exit",
    "Game time (min)",
    "Point every (s)",
    "Goal pts (0=off)",
    "%d-%d, # to save",
    "Current: %d",
    "Start new game?",
    "# = yes  * = no"
  }
};

const char *tr(StrId id) {
  return STRINGS[currentLang][id];
}

// ---------------------------------------------------------------------------
// Settings (numbered as in the menu, 1-3)
// ---------------------------------------------------------------------------

struct NumSetting {
  StrId title;
  int minValue;
  int maxValue;
  int defaultValue;
  uint8_t digits;
  int value;
};

enum SettingId { SET_GAME_MINUTES, SET_POINT_SECONDS, SET_GOAL_POINTS, SETTING_COUNT };

// A team holding the point for a whole 15 min game at one point per 5 s would
// score 180, so a goal of 100 means dominating for well over half the game.
NumSetting settings[SETTING_COUNT] = {
  {STR_SET_GAME_TIME,  1, 99,  15,  2},
  {STR_SET_POINT_TIME, 1, 60,  5,   2},
  {STR_SET_GOAL,       0, 999, 100, 3}, // 0 = no goal, the clock decides
};

int setting(SettingId id) {
  return settings[id].value;
}

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

LiquidCrystal lcd(LCD_RS, LCD_E, LCD_D4, LCD_D5, LCD_D6, LCD_D7);
Keypad keypad = Keypad(makeKeymap(keypadKeys), keypadRowPins, keypadColPins, KEYPAD_ROWS, KEYPAD_COLS);

char adminPin[5] = "1234"; // 4 digits + null terminator

struct Team {
  uint8_t buttonPin;
  uint8_t ledPin;
  StrId name;
  int score = 0;

  // Statistics (keys B/C/D after the game). heldMs and longestMs are booked
  // each time a spell of holding the point ends, the game's end included.
  int captures = 0;
  unsigned long heldMs = 0;
  unsigned long longestMs = 0;

  bool rawState = HIGH;      // last raw button reading (active LOW)
  bool debouncedState = HIGH;
  unsigned long lastEdgeMs = 0;

  bool holding = false;
  unsigned long holdStartMs = 0;

  int ledLevel = -1;         // last PWM value written, to skip redundant writes
};

Team teamRed = {BTN_RED_PIN, LED_RED_PIN, STR_TEAM_RED};
Team teamYellow = {BTN_YELLOW_PIN, LED_YELLOW_PIN, STR_TEAM_YELLOW};

// READY: waiting for the first capture, which starts the clock.
enum GamePhase { PHASE_READY, PHASE_RUNNING, PHASE_OVER };
GamePhase phase = PHASE_READY;
unsigned long gameEndMs = 0;
unsigned long finalRemainingMs = 0; // clock frozen at the end of the game
Team *owner = nullptr;              // team holding the point, nullptr = neutral
Team *winner = nullptr;             // at game over; nullptr = draw
unsigned long ownedSinceMs = 0;
unsigned long nextPointMs = 0;
unsigned long lastWarnSec = 0;
unsigned long nextSaveMs = 0;

enum AppState {
  STATE_SCOREBOARD,
  STATE_ENTER_PIN,
  STATE_MENU,
  STATE_EDIT_SETTING,
  STATE_CONFIRM_NEW_GAME
};

AppState appState = STATE_SCOREBOARD;
SettingId editing = SET_GAME_MINUTES;
char entryBuffer[5];   // scratch buffer for PIN / setting entry
uint8_t entryLen = 0;

enum StatsPage { PAGE_SCORES, PAGE_HELD, PAGE_CAPTURES, PAGE_LONGEST };
StatsPage page = PAGE_SCORES;
unsigned long lastKeyMs = 0;

bool hashHolding = false;
unsigned long hashHoldStartMs = 0;

unsigned long nextProgressBeepMs = 0;

const Note *seq = nullptr;
uint8_t seqLen = 0;
uint8_t seqIdx = 0;
unsigned long seqStepEndMs = 0;

// What the screen should show vs. what it currently shows; only the
// characters that differ are sent, so the ticking clock doesn't flicker.
char frame[LCD_ROWS][LCD_COLS];
char shown[LCD_ROWS][LCD_COLS];

// ---------------------------------------------------------------------------
// EEPROM helpers (settings)
// ---------------------------------------------------------------------------

void saveSettings() {
  EEPROM.update(EEPROM_MAGIC_ADDR, EEPROM_MAGIC_VAL);
  for (uint8_t i = 0; i < 4; i++) {
    EEPROM.update(EEPROM_PIN_ADDR + i, adminPin[i]);
  }
  for (uint8_t i = 0; i < SETTING_COUNT; i++) {
    EEPROM.put(EEPROM_SETTINGS_ADDR + 2 * i, (uint16_t)settings[i].value);
  }
}

void loadSettings() {
  bool valid = EEPROM.read(EEPROM_MAGIC_ADDR) == EEPROM_MAGIC_VAL;

  for (uint8_t i = 0; i < 4; i++) {
    char c = EEPROM.read(EEPROM_PIN_ADDR + i);
    adminPin[i] = (valid && c >= '0' && c <= '9') ? c : DEFAULT_PIN[i];
  }
  adminPin[4] = '\0';

  for (uint8_t i = 0; i < SETTING_COUNT; i++) {
    NumSetting &s = settings[i];
    uint16_t v;
    EEPROM.get(EEPROM_SETTINGS_ADDR + 2 * i, v);
    s.value = (valid && v >= s.minValue && v <= s.maxValue) ? v : s.defaultValue;
  }

  if (!valid) saveSettings();
}

// ---------------------------------------------------------------------------
// Saved game (survives a power cut)
//
// The R4 has no real EEPROM: EEPROM.put() erases and rewrites a whole 1 KB
// flash block for every byte that changes, which would stall the game for
// a moment on every save and wear the flash out. So the game is instead
// appended as a new record into blank flash after the settings block, and
// a block is only erased when the log wraps round to it (once per 16 saves,
// so each block is erased roughly every 20 minutes of play; the flash is
// rated for 100,000 erases).
// At power-up the newest intact record is restored. Time spent without
// power doesn't count: the clock carries on from where it was saved.
// ---------------------------------------------------------------------------

const uint32_t FLASH_BLOCK_BYTES = 1024;
const uint32_t SAVE_LOG_START = 1024; // block 0 holds the settings
const uint32_t SAVE_LOG_END = 8192;   // end of the R4's 8 KB data flash
const uint32_t SAVE_SLOT_BYTES = 64;
const int SAVE_SLOTS = (SAVE_LOG_END - SAVE_LOG_START) / SAVE_SLOT_BYTES;
const uint8_t SAVE_MAGIC = 0x5A;

struct SaveRecord {
  uint32_t seq;          // newest record wins
  uint8_t magic;
  uint8_t phase;
  uint8_t owner;         // 0 = none, 1 = red, 2 = yellow
  uint8_t winner;        // 0 = none/draw, 1 = red, 2 = yellow
  uint32_t remainingMs;
  uint32_t untilPointMs;
  uint32_t streakMs;     // how long the current owner has held the point
  uint16_t score[2];
  uint16_t captures[2];
  uint32_t heldMs[2];
  uint32_t longestMs[2];
  uint32_t checksum;     // catches a record cut short by a power cut
};
static_assert(sizeof(SaveRecord) <= SAVE_SLOT_BYTES, "save record must fit its slot");

uint32_t saveSeq = 0;
int nextSaveSlot = 0;

uint32_t slotAddr(int slot) {
  return SAVE_LOG_START + (uint32_t)slot * SAVE_SLOT_BYTES;
}

uint32_t checksumOf(const SaveRecord &r) {
  const uint8_t *p = (const uint8_t *)&r;
  uint32_t h = 2166136261UL; // FNV-1a
  for (size_t i = 0; i < offsetof(SaveRecord, checksum); i++) {
    h = (h ^ p[i]) * 16777619UL;
  }
  return h;
}

bool slotBlank(int slot) {
  uint8_t buf[SAVE_SLOT_BYTES];
  DataFlashBlockDevice::getInstance().read(buf, slotAddr(slot), SAVE_SLOT_BYTES);
  for (uint32_t i = 0; i < SAVE_SLOT_BYTES; i++) {
    if (buf[i] != 0xFF) return false;
  }
  return true;
}

void writeRecord(SaveRecord &r) {
  DataFlashBlockDevice &flash = DataFlashBlockDevice::getInstance();
  r.seq = ++saveSeq;
  r.magic = SAVE_MAGIC;
  r.checksum = checksumOf(r);

  // Flash can only be written where it's blank. A slot left dirty by a power
  // cut mid-write means skipping ahead to the next block.
  if (slotAddr(nextSaveSlot) % FLASH_BLOCK_BYTES != 0 && !slotBlank(nextSaveSlot)) {
    const int slotsPerBlock = FLASH_BLOCK_BYTES / SAVE_SLOT_BYTES;
    nextSaveSlot = ((nextSaveSlot / slotsPerBlock + 1) * slotsPerBlock) % SAVE_SLOTS;
  }
  uint32_t addr = slotAddr(nextSaveSlot);
  if (addr % FLASH_BLOCK_BYTES == 0) flash.erase(addr, FLASH_BLOCK_BYTES);

  uint8_t buf[SAVE_SLOT_BYTES];
  memset(buf, 0xFF, sizeof(buf));
  memcpy(buf, &r, sizeof(r));
  flash.program(buf, addr, SAVE_SLOT_BYTES);
  nextSaveSlot = (nextSaveSlot + 1) % SAVE_SLOTS;
}

bool readNewestRecord(SaveRecord &out) {
  bool found = false;
  int newestSlot = -1;
  for (int s = 0; s < SAVE_SLOTS; s++) {
    SaveRecord r;
    DataFlashBlockDevice::getInstance().read(&r, slotAddr(s), sizeof(r));
    if (r.magic != SAVE_MAGIC || r.checksum != checksumOf(r)) continue;
    if (!found || r.seq > out.seq) {
      out = r;
      found = true;
      newestSlot = s;
    }
  }
  saveSeq = found ? out.seq : 0;
  nextSaveSlot = found ? (newestSlot + 1) % SAVE_SLOTS : 0;
  return found;
}

uint8_t teamId(const Team *t) {
  return t == &teamRed ? 1 : t == &teamYellow ? 2 : 0;
}

Team *teamFromId(uint8_t id) {
  return id == 1 ? &teamRed : id == 2 ? &teamYellow : nullptr;
}

unsigned long remainingMs();

void saveGame() {
  unsigned long now = millis();
  SaveRecord r;
  r.phase = phase;
  r.owner = teamId(owner);
  r.winner = teamId(winner);
  r.remainingMs = remainingMs();
  r.untilPointMs = (owner && nextPointMs > now) ? nextPointMs - now : 0;
  r.streakMs = owner ? now - ownedSinceMs : 0;
  Team *teams[] = {&teamRed, &teamYellow};
  for (uint8_t i = 0; i < 2; i++) {
    r.score[i] = teams[i]->score;
    r.captures[i] = teams[i]->captures;
    r.heldMs[i] = teams[i]->heldMs;
    r.longestMs[i] = teams[i]->longestMs;
  }
  writeRecord(r);
  nextSaveMs = now + SAVE_INTERVAL_MS;
}

void restoreGame() {
  SaveRecord r;
  if (!readNewestRecord(r) || r.phase > PHASE_OVER) return;

  unsigned long now = millis();
  phase = (GamePhase)r.phase;
  owner = teamFromId(r.owner);
  winner = teamFromId(r.winner);
  Team *teams[] = {&teamRed, &teamYellow};
  for (uint8_t i = 0; i < 2; i++) {
    teams[i]->score = r.score[i];
    teams[i]->captures = r.captures[i];
    teams[i]->heldMs = r.heldMs[i];
    teams[i]->longestMs = r.longestMs[i];
  }
  if (phase == PHASE_RUNNING) {
    gameEndMs = now + r.remainingMs;
    nextPointMs = now + r.untilPointMs;
    ownedSinceMs = now - r.streakMs;
    lastWarnSec = (r.remainingMs + 999) / 1000;
    nextSaveMs = now + SAVE_INTERVAL_MS;
  } else {
    finalRemainingMs = r.remainingMs;
  }
}

// ---------------------------------------------------------------------------
// Sound sequences (non-blocking)
// ---------------------------------------------------------------------------

void startNote() {
  const Note &n = seq[seqIdx];
  seqStepEndMs = millis() + n.ms;
  if (n.freq == 0) noTone(BUZZER_PIN);
  else tone(BUZZER_PIN, n.freq);
}

template <size_t N>
void playSeq(const Note (&s)[N]) {
  seq = s;
  seqLen = N;
  seqIdx = 0;
  startNote();
}

void updateSeq() {
  if (!seq || millis() < seqStepEndMs) return;
  if (++seqIdx >= seqLen) {
    seq = nullptr;
    noTone(BUZZER_PIN);
    return;
  }
  startNote();
}

// Short one-off beeps; skipped while a sequence plays so it isn't cut off.
void blip(unsigned int freq, unsigned int ms) {
  if (!seq) tone(BUZZER_PIN, freq, ms);
}

// ---------------------------------------------------------------------------
// Game flow
// ---------------------------------------------------------------------------

// A hold is only valid while the scoreboard is live; a stale one would count
// the whole time away as hold time and capture instantly on return.
void cancelHolds() {
  teamRed.holding = false;
  teamYellow.holding = false;
}

unsigned long remainingMs() {
  switch (phase) {
    case PHASE_READY:
      return setting(SET_GAME_MINUTES) * 60000UL;
    case PHASE_RUNNING:
      return millis() >= gameEndMs ? 0 : gameEndMs - millis();
    default:
      return finalRemainingMs;
  }
}

// Books the current owner's spell of holding the point into its statistics.
void closeStreak() {
  if (!owner) return;
  unsigned long streak = millis() - ownedSinceMs;
  owner->heldMs += streak;
  if (streak > owner->longestMs) owner->longestMs = streak;
}

// Back to the ready screen with the scores and statistics cleared.
void newGame() {
  phase = PHASE_READY;
  Team *teams[] = {&teamRed, &teamYellow};
  for (Team *t : teams) {
    t->score = 0;
    t->captures = 0;
    t->heldMs = 0;
    t->longestMs = 0;
  }
  owner = nullptr;
  winner = nullptr;
  page = PAGE_SCORES;
  cancelHolds();
  saveGame();
}

void endGame(Team *w) {
  finalRemainingMs = remainingMs();
  closeStreak();
  owner = nullptr;
  phase = PHASE_OVER;
  winner = w;
  cancelHolds();
  if (w) playSeq(SEQ_WIN);
  else playSeq(SEQ_DRAW);
  saveGame();
}

void awardPoint() {
  owner->score++;
  int goal = setting(SET_GOAL_POINTS);
  if (goal > 0 && owner->score >= goal) endGame(owner);
}

void capturePoint(Team &team) {
  unsigned long now = millis();
  if (phase == PHASE_READY) {
    // The first capture starts the clock.
    phase = PHASE_RUNNING;
    gameEndMs = now + setting(SET_GAME_MINUTES) * 60000UL;
    lastWarnSec = (remainingMs() + 999) / 1000;
    nextSaveMs = now + SAVE_INTERVAL_MS;
  }
  closeStreak();
  owner = &team;
  ownedSinceMs = now;
  team.captures++;
  cancelHolds(); // a rival capturing at the same time has to start over
  playSeq(SEQ_CAPTURED);

  // The first point comes straight away, then one per interval.
  nextPointMs = now + setting(SET_POINT_SECONDS) * 1000UL;
  awardPoint();
  if (phase == PHASE_RUNNING) saveGame(); // endGame() already saved otherwise
}

void updateWarnings() {
  unsigned long sec = (remainingMs() + 999) / 1000;
  if (sec == lastWarnSec) return;
  lastWarnSec = sec;
  if (sec == WARN_FIRST_SEC) playSeq(SEQ_WARN_FIRST);
  else if (sec == WARN_SECOND_SEC) playSeq(SEQ_WARN_SECOND);
  else if (sec > 0 && sec <= COUNTDOWN_SEC) {
    seq = nullptr; // the countdown matters more than a capture tone still playing
    tone(BUZZER_PIN, COUNTDOWN_FREQ, COUNTDOWN_MS);
  }
}

// Runs on every screen, so the clock and the points keep going while an
// admin is in the settings menu.
void updateGame() {
  if (phase != PHASE_RUNNING) return;

  if (owner && millis() >= nextPointMs) {
    nextPointMs += setting(SET_POINT_SECONDS) * 1000UL;
    blip(POINT_TICK_FREQ, POINT_TICK_MS);
    awardPoint();
    if (phase != PHASE_RUNNING) return;
  }

  if (millis() >= gameEndMs) {
    if (teamRed.score > teamYellow.score) endGame(&teamRed);
    else if (teamYellow.score > teamRed.score) endGame(&teamYellow);
    else endGame(nullptr);
    return;
  }

  updateWarnings();
  if (millis() >= nextSaveMs) saveGame();
}

// ---------------------------------------------------------------------------
// Buttons and capturing
// ---------------------------------------------------------------------------

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
  if (elapsed >= CAPTURE_MS) return 100;
  return (int)((elapsed * 100UL) / CAPTURE_MS);
}

void updateCapture(Team &team) {
  if (&team == owner) return; // nothing to capture: the point is already theirs

  bool pressed = (team.debouncedState == LOW);
  if (pressed && !team.holding) {
    team.holding = true;
    team.holdStartMs = millis();
  } else if (!pressed && team.holding) {
    team.holding = false; // letting go early loses the progress
  }

  if (team.holding && millis() - team.holdStartMs >= CAPTURE_MS) {
    capturePoint(team);
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
    blip(BEEP_FREQ_PROGRESS, BEEP_DURATION_PROGRESS_MS);
    int percent = captureProgressPercent(*active);
    unsigned long interval = map(percent, 0, 100, 320, 90);
    nextProgressBeepMs = millis() + interval;
  }
}

// ---------------------------------------------------------------------------
// LEDs
// ---------------------------------------------------------------------------

// A team's LED fades up while it captures and stays lit while it holds the
// point. At the end the winner's LED stays lit, or both on a draw.
void updateLed(Team &team) {
  int level = 0;
  if (phase == PHASE_OVER) {
    level = (winner == &team || winner == nullptr) ? 255 : 0;
  } else if (team.holding) {
    level = captureProgressPercent(team) * 255 / 100;
  } else if (owner == &team) {
    level = 255;
  }
  if (level != team.ledLevel) {
    analogWrite(team.ledPin, level);
    team.ledLevel = level;
  }
}

// ---------------------------------------------------------------------------
// Hold # to reset (scoreboard, after a game)
// ---------------------------------------------------------------------------

// getKey() only reports presses, so a hold is read from the library's key list.
bool keyDown(char c) {
  int i = keypad.findInList(c);
  return i >= 0 && (keypad.key[i].kstate == PRESSED || keypad.key[i].kstate == HOLD);
}

void updateHashHold() {
  if (!hashHolding) return;
  if (appState != STATE_SCOREBOARD || !keyDown('#')) {
    hashHolding = false;
  } else if (millis() - hashHoldStartMs >= HASH_HOLD_MS) {
    hashHolding = false;
    newGame();
  }
}

// ---------------------------------------------------------------------------
// LCD rendering
// ---------------------------------------------------------------------------

void invalidateLcd() {
  memset(shown, 0, sizeof(shown)); // 0 is never drawn, so every cell gets resent
}

void putText(uint8_t row, uint8_t col, const char *text) {
  for (uint8_t i = 0; text[i] && col + i < LCD_COLS; i++) frame[row][col + i] = text[i];
}

void putLine(uint8_t row, const char *text) {
  memset(frame[row], ' ', LCD_COLS);
  putText(row, 0, text);
}

// One text at the left edge and one at the right edge of a row.
void putEnds(uint8_t row, const char *left, const char *right) {
  putLine(row, left);
  putText(row, LCD_COLS - strlen(right), right);
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

void formatMMSS(unsigned long sec, char *out, size_t outSize) {
  snprintf(out, outSize, "%02lu:%02lu", sec / 60, sec % 60);
}

// Picks one of `count` hints for the bottom line, changing every HINT_SWAP_MS.
uint8_t hintIndex(uint8_t count) {
  return (millis() / HINT_SWAP_MS) % count;
}

// "R42  14:59  Y17": scores at the edges, the game clock in the middle.
void renderScoreLine() {
  char left[8], right[8], clock[8];
  snprintf(left, sizeof(left), "%c%d", tr(teamRed.name)[0], teamRed.score);
  snprintf(right, sizeof(right), "%c%d", tr(teamYellow.name)[0], teamYellow.score);
  putEnds(0, left, right);
  formatMMSS((remainingMs() + 999) / 1000, clock, sizeof(clock)); // a fresh game shows 15:00
  putText(0, (LCD_COLS - 5) / 2, clock);
}

void renderStatusLine() {
  char buf[LCD_COLS + 1];

  if (hashHolding) {
    unsigned long held = millis() - hashHoldStartMs;
    if (held > HASH_HOLD_MS) held = HASH_HOLD_MS;
    memset(frame[1], ' ', LCD_COLS);
    memset(frame[1], BLOCK, held * LCD_COLS / HASH_HOLD_MS);
    return;
  }

  if (phase == PHASE_READY) {
    putLine(1, tr(hintIndex(2) == 0 ? STR_CAPTURE_TO_START : STR_HINT_SETTINGS));
    return;
  }

  if (phase == PHASE_OVER) {
    uint8_t hint = hintIndex(3);
    if (hint == 1) {
      putLine(1, tr(STR_HOLD_TO_RESET));
    } else if (hint == 2) {
      putLine(1, tr(STR_HINT_STATS));
    } else if (winner) {
      snprintf(buf, sizeof(buf), tr(STR_WIN_FMT), tr(winner->name));
      putLine(1, buf);
    } else {
      putLine(1, tr(STR_DRAW));
    }
    return;
  }

  if (teamRed.holding && teamYellow.holding) {
    // Both capturing a neutral point: only the first letters fit.
    snprintf(buf, sizeof(buf), "%c:%3d%%  %c:%3d%%",
             tr(teamRed.name)[0], captureProgressPercent(teamRed),
             tr(teamYellow.name)[0], captureProgressPercent(teamYellow));
  } else if (teamRed.holding || teamYellow.holding) {
    Team &t = teamRed.holding ? teamRed : teamYellow;
    snprintf(buf, sizeof(buf), tr(STR_CAPTURING_FMT), tr(t.name), captureProgressPercent(t));
  } else if (owner) {
    snprintf(buf, sizeof(buf), tr(STR_HELD_BY_FMT), tr(owner->name));
  } else {
    snprintf(buf, sizeof(buf), "%s", tr(STR_NEUTRAL));
  }
  putLine(1, buf);
}

// A statistics page: the title, then red's value on the left and yellow's
// on the right, e.g. "R 08:32   Y 05:10".
void renderStatsPage() {
  char left[10], right[10], value[8];
  StrId title;
  const Team *teams[] = {&teamRed, &teamYellow};
  char *out[] = {left, right};
  for (uint8_t i = 0; i < 2; i++) {
    const Team &t = *teams[i];
    if (page == PAGE_CAPTURES) {
      snprintf(value, sizeof(value), "%d", t.captures);
    } else {
      unsigned long ms = page == PAGE_HELD ? t.heldMs : t.longestMs;
      formatMMSS(ms / 1000, value, sizeof(value));
    }
    snprintf(out[i], sizeof(left), "%c %s", tr(t.name)[0], value);
  }
  title = page == PAGE_HELD ? STR_STATS_HELD : page == PAGE_CAPTURES ? STR_STATS_CAPTURES : STR_STATS_LONGEST;
  putLine(0, tr(title));
  putEnds(1, left, right);
}

void renderEnterPin() {
  putLine(0, tr(STR_ENTER_PIN));
  char masked[5];
  for (uint8_t i = 0; i < entryLen; i++) masked[i] = '*';
  masked[entryLen] = '\0';
  putLine(1, masked);
}

void renderEditSetting() {
  const NumSetting &s = settings[editing];
  putLine(0, tr(s.title));
  char line[LCD_COLS + 1];
  if (entryLen > 0) {
    putLine(1, entryBuffer);
  } else if (hintIndex(2) == 0) {
    snprintf(line, sizeof(line), tr(STR_CURRENT_FMT), s.value);
    putLine(1, line);
  } else {
    snprintf(line, sizeof(line), tr(STR_RANGE_FMT), s.minValue, s.maxValue);
    putLine(1, line);
  }
}

void render() {
  switch (appState) {
    case STATE_SCOREBOARD:
      if (page == PAGE_SCORES) {
        renderScoreLine();
        renderStatusLine();
      } else {
        renderStatsPage();
      }
      break;
    case STATE_ENTER_PIN:
      renderEnterPin();
      break;
    case STATE_MENU:
      putLine(0, tr(STR_MENU_LINE0));
      putLine(1, tr(STR_MENU_LINE1));
      break;
    case STATE_EDIT_SETTING:
      renderEditSetting();
      break;
    case STATE_CONFIRM_NEW_GAME:
      putLine(0, tr(STR_NEW_GAME));
      putLine(1, tr(STR_YES_NO));
      break;
  }
  flushLcd();
}

// ---------------------------------------------------------------------------
// Keypad / settings menu handling
// ---------------------------------------------------------------------------

void enterState(AppState next) {
  appState = next;
  entryLen = 0;
  entryBuffer[0] = '\0';
}

bool appendDigit(char key, uint8_t maxLen) {
  if (!isdigit(key) || entryLen >= maxLen) return false;
  entryBuffer[entryLen++] = key;
  entryBuffer[entryLen] = '\0';
  return true;
}

void handleKey(char key) {
  switch (appState) {
    case STATE_SCOREBOARD:
      if (key >= 'A' && key <= 'D' && phase == PHASE_OVER) {
        page = (StatsPage)(key - 'A');
      } else if (key == '*') {
        cancelHolds();
        hashHolding = false;
        page = PAGE_SCORES;
        enterState(STATE_ENTER_PIN);
      } else if (key == '#' && phase == PHASE_OVER) {
        blip(KEY_CLICK_FREQ, KEY_CLICK_MS);
        page = PAGE_SCORES;
        hashHolding = true;
        hashHoldStartMs = millis();
      }
      break;

    case STATE_ENTER_PIN:
      if (key == '*') {
        enterState(STATE_SCOREBOARD);
      } else if (key == '#') {
        if (entryLen == 4 && strcmp(entryBuffer, adminPin) == 0) {
          enterState(STATE_MENU);
        } else {
          blip(BEEP_FREQ_WRONG, BEEP_DURATION_WRONG_MS);
          enterState(STATE_SCOREBOARD);
        }
      } else {
        appendDigit(key, 4);
      }
      break;

    case STATE_MENU:
      if (key >= '1' && key < '1' + SETTING_COUNT) {
        editing = (SettingId)(key - '1');
        enterState(STATE_EDIT_SETTING);
      } else if (key == '1' + SETTING_COUNT) {
        enterState(STATE_CONFIRM_NEW_GAME);
      } else if (key == '#' || key == '*') {
        enterState(STATE_SCOREBOARD);
      }
      break;

    case STATE_EDIT_SETTING: {
      NumSetting &s = settings[editing];
      if (key == '*') {
        enterState(STATE_MENU);
      } else if (key == '#') {
        int v = atoi(entryBuffer);
        if (entryLen > 0 && v >= s.minValue && v <= s.maxValue) {
          s.value = v;
          saveSettings();
          enterState(STATE_MENU);
        } else {
          blip(BEEP_FREQ_WRONG, BEEP_DURATION_WRONG_MS);
          enterState(STATE_EDIT_SETTING); // clear the entry and try again
        }
      } else {
        appendDigit(key, s.digits);
      }
      break;
    }

    case STATE_CONFIRM_NEW_GAME:
      if (key == '#') {
        newGame();
        enterState(STATE_SCOREBOARD);
      } else if (key == '*') {
        enterState(STATE_MENU);
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
  restoreGame();

  lcd.begin(LCD_COLS, LCD_ROWS);
  invalidateLcd();

  teamRed.rawState = teamRed.debouncedState = digitalRead(BTN_RED_PIN);
  teamYellow.rawState = teamYellow.debouncedState = digitalRead(BTN_YELLOW_PIN);
}

void loop() {
  updateButton(teamRed);
  updateButton(teamYellow);

  // Buttons only count on the scoreboard before and during a game; the
  // settings menu takes priority over gameplay input.
  if (appState == STATE_SCOREBOARD && phase != PHASE_OVER) {
    updateCapture(teamRed);
    updateCapture(teamYellow);
    updateProgressBuzzer();
  }

  char key = keypad.getKey();
  if (key) {
    lastKeyMs = millis();
    handleKey(key);
  }
  updateHashHold();

  // Settings saved so far are kept; anything half-typed is dropped.
  if (appState != STATE_SCOREBOARD && millis() - lastKeyMs >= MENU_TIMEOUT_MS) {
    enterState(STATE_SCOREBOARD);
  }

  updateGame();
  updateSeq();
  updateLed(teamRed);
  updateLed(teamYellow);
  render();
}
