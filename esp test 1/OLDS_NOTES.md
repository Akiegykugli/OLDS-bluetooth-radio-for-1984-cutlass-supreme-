# OLDS firmware - notes for future sessions

Project: custom ESP32 head unit for a 1984 Oldsmobile Cutlass Supreme (Hungary), reusing the Delco faceplate.
Current firmware: **v2.9.0** (alpha of v3) = v2.1.0 + working controls (see "v2.9.0 behaviour"). v2.1.0 = v2.0.1 + Wi-Fi network table (`src/wifi_networks.h`, eduroam support). v2.0.1 = v1.5.1 (Wi-Fi NTP sync + clean restart + Bluetooth A2DP + DS3231 clock + TM1637)
+ control bench (pots, switches, encoder). v2.0.0 (same but on the v1.3.5 base, no Wi-Fi) was bench-verified and is the
fallback baseline. v2.0.1 was bench-verified by the user; v2.1.0 was run by the user; v2.9.0 is NOT yet run on hardware (host-compiled against the real A2DPVolumeControl.h only).

## Working rules (from the user)
- Explain the reasoning behind every decision, at electronics-student level.
- Double-check the reliability of anything recommended: it is an automotive project.
- Deliver code as PlatformIO C++ (`src/main.cpp`, `#include <Arduino.h>`), never `.ino`.
- Metric units, European/Hungarian standards.
- The user pastes working code in; keep what works byte-for-byte unless there is a reason to change it.

## Files
| File | Purpose |
|---|---|
| `platformio.ini` | Board, pinned platform, libraries, partition table |
| `add_build_epoch.py` | Defines `BUILD_EPOCH` (UTC seconds) for `src/main.cpp` only |
| `src/main.cpp` | The firmware |
| `src/wifi_networks.h` | DATA ONLY: Wi-Fi table (contains passwords: do not share/upload publicly) |

## Toolchain status (what is verified and what is not)
- VERIFIED (web check): `espressif32@6.9.0` exists and ships Arduino core 2.0.17 / ESP-IDF 4.4.7.
- Libraries are installed from GitHub URLs in `lib_deps`. The PlatformIO registry entry
  `pschatzmann/ESP32-A2DP @ ~1.8.0` failed with "UnknownPackageError" on the user's machine.
- FIRST BUILD (user's log): platform + libraries installed fine; ESP32-A2DP **v1.8.11 failed to compile** on core 2.0.17
  (`ESP_A2D_AUDIO_STATE_SUSPEND` not declared; IDF 4.4.7 only has `..._REMOTE_SUSPEND`). v1.8.11 is the first tag that uses it.
- Fix: pinned ESP32-A2DP to **v1.8.7**. Checked by script against ESP-IDF 4.4.7 headers: the BT/A2DP/AVRC symbols
  v1.8.7 uses all exist there (the only misses are inside the code path for newer IDF). NOT yet confirmed by a real build.
  Alternative if v1.8.7 fails: v1.8.10 (also avoids the SUSPEND name, but uses more newer-IDF code paths behind version checks).
- Harmless build warning: "AudioTools library is not included first or installed" (legacy I2S driver is used on purpose).
- Other libraries pinned to the commits from the first build log: RTClib 2.1.4 (5298434), Adafruit BusIO 1.17.4 (3b83642),
  TM1637 (3cca196, no version tag).
- The assistant cannot run PlatformIO (no network for it in the sandbox): build results come from the user.
- Core 2.0.17 is used on purpose: `main.cpp` uses the legacy I2S structs. A newer core (3.x / pioarduino)
  needs a re-test of the audio before use.

## Pin map (every GPIO used exactly once; checked by script)
| GPIO | Function | Part |
|---|---|---|
| 2 | status LED (out) | on-board |
| 4 | FREE (push button dropped) | - |
| 5 | TM1637 CLK | display |
| 16 | toggle switch -> GND | controls |
| 17 | switch on volume pot -> GND | controls |
| 18 | TM1637 DIO | display |
| 21 / 22 | I2C SDA / SCL | DS3231 (later RDA5807 on the same bus) |
| 23 | encoder state 4 (green) | controls (**moved from 25**) |
| 25 | I2S word select (LCK) | CJMCU-1334 DAC |
| 26 | I2S bit clock (BCK) | DAC |
| 27 | I2S data (DIN) | DAC |
| 32 | encoder state 1 (red) | controls |
| 33 | encoder state 2 (black) | controls |
| 34 | volume pot wiper (ADC1) | controls |
| 35 | balance pot wiper (ADC1) | controls |

Encoder: common (grey) -> GND, 3 switched contacts, 12 clicks per turn, turning right gives 4 -> 2 -> 1.
Pins free for later: 13, 14, 19 (all have internal pull-up, no boot role). Avoid 0, 12, 15 (strapping) and 36/39 (no pull-up).

## Why GPIO25 was changed (important)
The bench sketch had the encoder green wire on GPIO25, but GPIO25 is the I2S LCK line, which toggles at 44.1 kHz
while audio plays. An encoder interrupt on it would flood the CPU. Decision: move the encoder wire to GPIO23
(one wire), leave the proven audio wiring untouched. **The physical wire must be moved on the bench.**

## Architecture rules (do not break these)
1. Only `setup()` and `loop()` talk to the DS3231 over I2C. The display task never touches I2C.
2. The display task reads time only through `getLowNoiseTime()` (mutex-protected base epoch + `millis()`).
3. The DS3231 stores LOCAL Budapest time. Serial sync takes a UTC epoch in seconds (`date +%s`).
4. Controls are polled from `loop()` (every 5 ms) and the encoder uses one ISR. No extra tasks, so Serial
   output from controls cannot interleave. Every control line is a single `Serial.printf`.
5. Boot order of v1.3.5 is unchanged; `controlsBegin()` runs last in `setup()`.
6. Pots are on ADC1 only (works with Bluetooth). Never use ADC2 pins for analog input.

## v2.9.0 behaviour (all tunable constants are in the "OUTPUT / MODE SETTINGS" block of main.cpp)
- Modes from the two switches: power switch OPEN = OFF (clock mode), else toggle CLOSED = RADIO (not implemented), OPEN = BLUETOOTH
  (`TOGGLE_CLOSED_IS_RADIO` flips the toggle meaning). Power switch has priority.
- Output stage = custom `OldsVolumeControl` (`a2dp_sink.set_volume_control`): software volume (-50..0 dB, step 0 = mute), software
  balance (only the opposite channel is attenuated, fully off at the ends), muting, 10 ms gain ramps. Starts muted at boot.
- Phone volume: the library's `set_volume()` is ignored by our stage (knob = master). Knob changes are sent to the phone with
  `a2dp_sink.set_volume()` (rate limit 120 ms, last value always sent); if the phone changes its volume, the knob value is sent back.
  Sent again 2.5 s after every Bluetooth connect (phone registers for volume messages late).
- Muted (OFF or RADIO): output muted at once; after 1 s "pause" goes to the phone, up to 3 tries 3 s apart while it keeps streaming;
  one extra cycle if the phone starts again later; back to BLUETOOTH = unmute + "play" only if it was playing before.
- Encoder in BLUETOOTH mode: right = `next()`, left = `previous()`, one skip per click, 400 ms lockout. RADIO: prints an error. OFF: ignored.
- Display: BLUETOOTH = clock 10 s, then "bLUE"/"----" 2.5 s (as before); RADIO = "----"; OFF = clock only.
- NOT done: radio tuner (RDA5807 over I2C), audio source switch (CD4052 mux vs DPDT relay still open), amp mute pin.
- Known behaviour of the iPhone: "previous" restarts the current track if it has played for more than a few seconds.
- Still open from before: Wi-Fi sync runs at EVERY boot (scan + one restart before Bluetooth). In the car the ESP32 resets at each engine
  start, so that costs about 10-15 s of silence. Proposal (not done): sync only when the RTC is untrusted or every few days.

## Hardware plan (decided/under consideration, none of it in the firmware)
- ESP32 on switched 12 V (accessory + ignition merged with two diodes, fuse + TVS before the buck converter). Expect a reset at engine start.
- Volume range 16 steps is fine for radio mode (RDA5807 volume register).
- Audio source switch: CD4052B is a dual 4:1 mux: X section = left, Y section = right (one signal wire per channel, ground is NOT switched).
  Needs: VEE = -5 V from ICL7660 (fed from the 5 V buck rail), VSS = GND, VDD = 3.3 V (so 3.3 V select pins are valid), signals attenuated and with
  ~1 kOhm series resistors on the inputs, amp muted while switching. A small DPDT signal relay is the simpler alternative for two sources.

## v2.1.0 Wi-Fi list (how to add a network)
- Add ONE line to the table in `src/wifi_networks.h`; never edit main.cpp for that. Order = priority.
  `{ "SSID", NET_PSK, "password", "", "" }` or `{ "eduroam", NET_EAP, "account password", "outer identity", "inner username" }`.
- `NET_EAP` = WPA2-Enterprise PEAP/MSCHAPv2 via `WiFi.begin(ssid, WPA2_AUTH_PEAP, identity, username, password)`.
  Verified in the core 2.0.17 headers/IDF 4.4.7 that these functions exist; not yet tested on real eduroam.
  NO server-certificate check (no CA cert given) -> credentials could be sent to a fake "eduroam" access point.
  Use the institution's anonymous outer identity; adding the CA cert is a possible later hardening.
- Boot flow: scan first, then try only networks in range, in table order (PSK timeout 8 s, EAP 20 s). If the scan
  fails, all configured entries are tried. Text starting with `YOUR_` = placeholder, entry skipped.
- Hidden (non-broadcast) SSIDs are not supported (they never show up in the scan).
- Passwords are no longer in main.cpp; they are only in wifi_networks.h. Do not copy them into notes.

## v2.0.1 specifics (Wi-Fi base)
- Boot 1: RTC init -> Wi-Fi + NTP -> ONE `ESP.restart()` (flag `wifiDoneMagic` in RTC memory) -> Boot 2: no Wi-Fi, Bluetooth,
  display task, then `controlsBegin()`. So on a Wi-Fi boot the controls only start after the restart (expected).
- Wi-Fi touches no GPIOs and ends before the controls start; pots are on ADC1, so no ADC2/Wi-Fi conflict.
- `platformio.ini` is unchanged (WiFi.h is part of the Arduino core; huge_app.csv leaves room).

## Controls: current behaviour (print only)
- `[VOLUME] mV | %`, `[BALANCE] mV | -50..+50`, `[POWER]`, `[TOGGLE]`, `[BUTTON]`, `[ENCODER] position=`
- Switches: `[POWER]`/`[TOGGLE]` print ON/OFF on change; `[BUTTON]` prints on press.
- Calibration constants in code: volume 150..3100 mV (inverted: low mV = loud), balance 150 / 1650 / 3100 mV.
  The last bench POT_CAL run read about 142..3118 mV, balance centre about 1600 mV. Recalibrate with
  `POT_CAL 1` while Bluetooth is running (radio noise shifts ADC readings slightly).
- `ENC_DEBUG 1` prints every settled encoder state for troubleshooting.

## First-run checklist for v2.9.0
- [ ] Boots muted, then volume follows the pot; phone slider follows the pot; changing volume on the phone snaps back to the knob
- [ ] Balance: centre = both channels equal; ends mute one channel (check which side is which, flip `BAL_INVERT` if needed)
- [ ] Encoder: right = next track, left = previous, no skip-spam when turned fast
- [ ] Power switch OFF: sound stops at once, display shows the clock, phone pauses after about 1 s; ON: sound returns and playback resumes
- [ ] Toggle: radio position prints the radio error, shows "----", mutes; back = Bluetooth again (check the toggle polarity)
- [ ] No pops/clicks when muting/unmuting; no audio glitches while turning the knobs

## First-run checklist for v2.1.0 (v2.0.0: all items except Wi-Fi were verified on the bench)
- [ ] Builds, boots, prints version 2.1.0 and `=== OLDS control test (print only) ===`
- [ ] Phone pairs as "OLDS Car Stereo", music plays, clock shows on TM1637
- [ ] Encoder: 12 clicks either way = position +/-12, with music playing
- [ ] Pots print sensible mV/percent with Bluetooth active; no spam when untouched
- [ ] Switches and button print once per action
- [ ] Serial time sync (`date +%s`) and `?` status still work
- [ ] Boot 1 prints scan result and "Connected successfully to <SSID>" (home/hotspot), then NTP, then restart
- [ ] Boot 1 prints Wi-Fi/NTP lines then "Restarting once"; boot 2 prints "Clean second boot", then controls banner
- [ ] Encoder, pots and switches work after the restart (second boot)

## Open items
- DAC makes a click at ESP32 boot (cause not found). GPIO-driven DAC MUTE worked but left noise after unmute.
- Implement real actions for the controls (volume, balance, source switching, tuning knob) - not started.
- Display: only the last digit has segments wired (can show "1" or blank); 12-hour time only.
- FM (RDA5807 over I2C), RDS-CT time sync, PCM2706 USB audio path: later.
- Wi-Fi NTP sync is merged in v2.0.1 (pending hardware test).