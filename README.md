# Airsoft Props

Two Arduino Uno R4 firmware projects for electronic airsoft game props, built with [PlatformIO](https://platformio.org/).

- **`Airsoft capture point/`** — hold-to-capture scoring point for two teams.
- **`Airsoft Bomb/`** — arm/defuse bomb prop with a countdown timer.

Both use a 4x4 matrix keypad for input.

## Required parts (both props)

- Arduino Uno R4 (WiFi variant, per `platformio.ini` — Minima also works)
- 4x4 matrix membrane keypad
- Passive buzzer
- Jumper wires / breadboard or perfboard

Exact pin assignments are defined as named constants at the top of each project's `src/main.cpp` — check/adjust them to match your wiring before flashing.

## Capture point

**Extra parts:** 2 momentary pushbuttons (one red, one yellow), 2 small 5 mm LEDs (red and yellow) with 470 Ω resistors, 16x2 character LCD (parallel-wired, e.g. HD44780).

A domination-style point for two teams, Red and Yellow. A team captures the point by holding its own coloured button for 5 seconds: its LED fades up and the buzzer beeps faster as it gets closer, and letting go early loses the progress. The first capture starts the game clock (default 15 minutes). Every time the point changes hands a long beep sounds, the capturing team scores a point straight away, and then it earns another point every few seconds (default 5, with a soft tick each time) until the other team captures it back. Its LED stays lit while it holds the point. The top line of the LCD shows both scores and the time left (`R42  14:59  Y17`); the bottom line shows who holds the point or who is capturing it. The buzzer gives a double beep at 5 minutes left, a triple beep at 1 minute, and counts down the last 10 seconds.

The game ends when a team reaches the goal score (default 100, which takes well over half a 15-minute game of holding the point) or when the clock runs out, in which case the team with more points wins and equal scores are a draw. The winner's LED stays lit (both LEDs on a draw). Hold `#` for two seconds to clear the result, ready for the next game.

The lettered keys show statistics: `B` time each team held the point, `C` how many times each team captured it, `D` each team's longest unbroken hold, and `A` goes back to the scoreboard. During a game a statistics page returns to the scoreboard by itself after 10 seconds.

The game is saved every 10 seconds and at every capture (and whenever it ends or is reset), so after a power cut it picks up where it left off; time without power doesn't count. Saves go to a separate area of the R4's flash as a rolling log, so they don't wear out the memory holding the settings.

Press `*` to open the PIN-gated settings menu (default PIN `1234`) to change the game length, how often points are awarded, or the goal score (`0` turns the goal off so only the clock decides), or to start a new game. The game clock keeps running while the menu is open, and a new game length applies from the next game.

## Bomb

**Extra parts:** a red and a green 5 mm LED with 470 Ω resistors (no buttons), 20x4 I2C character LCD (e.g. PCF8574 backpack, address `0x27` or `0x3F`).

An attacker types the 6-digit arming code on the keypad (each key clicks, and the digits replace the `******` placeholders) and confirms with `#` to plant the bomb. A wrong code shows "Wrong code!" and locks the keypad for 3 seconds. The green LED stays lit whenever the bomb isn't counting down. While armed, it behaves like the Counter-Strike C4: a short high beep with a red blink, the gap shrinking from one second to a tenth of a second as time runs out, then a continuous tone with the red LED solid for the final second. A block on the bottom row swings from side to side in time with the beeps, and the timer counts down in tenths of a second (`01:29.9`). A defender enters the disarming code before time runs out to stop the clock (three quick blinks with a rising chirp, green LED back on). If the timer reaches zero, the bomb "explodes": a low rumble with the red LED flickering, after which both LEDs stay lit until reset. Set the countdown to 40 seconds to match Counter-Strike's C4 timer.

After a round, hold `#` for two seconds to reset the bomb for the next one; a bar fills along the bottom row while you hold, and a short tap does nothing, so a player can't wipe the result by accident. Press `*` from the disarmed, defused or exploded screen to open the settings menu, protected by a 4-digit admin PIN (default `1234`). From there you can change the countdown length, the admin PIN (typed twice to confirm), the arming code (default `111111`), or the disarming code (default `222222`). Exiting the menu also resets the prop.

## Language

All on-screen text is English and lives in one `STRINGS` table near the top of each `src/main.cpp`. To translate a prop, add a language there; the comment above the table explains the steps. Keep in mind that the LCD's built-in character set has no accented letters such as ä or ö.

## Building / flashing

From either project's folder:

```
pio run              # compile
pio run -t upload    # flash to a connected Uno R4
```
