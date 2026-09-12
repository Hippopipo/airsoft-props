# Airsoft Props

Two Arduino Uno R4 firmware projects for electronic airsoft game props, built with [PlatformIO](https://platformio.org/).

- **`Airsoft capture point/`** — hold-to-capture scoring point for two teams.
- **`Airsoft Bomb/`** — arm/defuse bomb prop with a countdown timer.

Both use a 4x4 matrix keypad for input and support switching the on-screen language between **English and Finnish** from their settings menus.

## Required parts (both props)

- Arduino Uno R4 (WiFi variant, per `platformio.ini` — Minima also works)
- 4x4 matrix membrane keypad
- Passive buzzer
- Jumper wires / breadboard or perfboard

Exact pin assignments are defined as named constants at the top of each project's `src/main.cpp` — check/adjust them to match your wiring before flashing.

## Capture point

**Extra parts:** 2 momentary pushbuttons with built-in LEDs, 16x2 character LCD (parallel-wired, e.g. HD44780).

Two teams each hold their button to capture the point. The LED brightness ramps up as you hold (releasing early resets progress), and the buzzer beeps faster the closer you get. Holding for the full capture time scores a point for that team and flashes the LED. The LCD shows both teams' live scores and capture progress.

Press `*` to open the PIN-gated settings menu (default PIN `1234`) to adjust the capture time, reset scores, or switch language.

## Bomb

**Extra parts:** 2 status LEDs (no buttons — used for feedback only), 20x4 I2C character LCD (e.g. PCF8574 backpack, address `0x27` or `0x3F`).

An attacker enters the arm code on the keypad and confirms with `#` to start the countdown. While armed, the LEDs flash in sync and the buzzer ticks faster as time runs out. A defender enters the defuse code before time runs out to stop the clock (solid LED + confirmation tone). If the timer reaches zero, the bomb "explodes" (rapid LED flash + alarm siren) until reset.

Press `*` from the safe/defused/exploded screens to open the PIN-gated settings menu (default PIN `1234`) to change the countdown length, arm code, defuse code, or language. Exiting the menu also resets the prop for the next round.

## Language

Both settings menus have a numbered option to toggle between English and Finnish; the choice is saved to EEPROM and survives power cycles. Finnish strings intentionally spell words without ä/ö dots (e.g. "virittää" → "virita") since the common HD44780 character ROM doesn't reliably display them — this avoids garbled characters on typical hardware.

## Building / flashing

From either project's folder:

```
pio run              # compile
pio run -t upload    # flash to a connected Uno R4
```
