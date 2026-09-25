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

**Extra parts:** 2 momentary pushbuttons, 2 small 5 mm LEDs (red for team A, green for team B) with 470 Ω resistors, 16x2 character LCD (parallel-wired, e.g. HD44780).

Two teams each hold their button to capture the point. The LED brightness ramps up as you hold (releasing early resets progress), and the buzzer beeps faster the closer you get. Holding for the full capture time scores a point for that team and flashes the LED. The LCD shows both teams' live scores and capture progress. The first team to reach the configurable goal score (default 5) wins — their LED stays lit, the buzzer plays a victory tone, and further captures are locked out until the scores are reset.

Press `*` to open the PIN-gated settings menu (default PIN `1234`) to adjust the capture time or the goal score, or to reset the scores.

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
