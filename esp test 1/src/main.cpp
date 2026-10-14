/* ============================================================
 * OLDS ESP32 Radio Clock
 * Firmware Version: 2.9.0 (alpha of v3: controls now DO something)
 *
 * WHAT IS NEW IN 2.9.0  (everything not listed here is unchanged from 2.1.0)
 * - ROTARY ENCODER = track skip over Bluetooth (right = next, left = previous).
 *   One skip per click, minimum 400 ms between two skips.
 * - VOLUME POT = master volume. The ESP32 scales the audio itself (software
 *   volume, 50 dB range, smooth ramps) and tells the phone the new value, so
 *   the phone slider follows. If the phone changes its volume, the ESP32
 *   puts the knob value back (the knob always wins).
 * - BALANCE POT = software balance before the DAC: only the opposite
 *   channel is attenuated, at the end positions it is fully off.
 * - POWER SWITCH (on the volume pot): OFF = "clock mode". Output muted at once,
 *   the display shows the clock, Bluetooth stays connected. After 1 s the
 *   phone gets a "pause" (re-sent up to 3 times if it keeps streaming).
 *   ON again = unmute, and "play" if it was playing before.
 * - TOGGLE SWITCH = Bluetooth / radio. Radio is NOT implemented yet: radio mode
 *   mutes, shows "----" and prints an error. Closed to GND = radio
 *   (flip TOGGLE_CLOSED_IS_RADIO to swap).
 * - PUSH BUTTON removed (GPIO4 is free now).
 * - NOT in this version: radio tuner (I2C), audio source multiplexer.
 *
 * WHAT WAS NEW IN 2.1.0
 * - Wi-Fi networks now live in ONE TABLE in src/wifi_networks.h. To add a
 *   network you add one line there; main.cpp is never touched.
 *   Order in the table = priority (first line is tried first).
 * - Two network types: NET_PSK (normal password) and NET_EAP (WPA2-Enterprise,
 *   needed for eduroam: PEAP/MSCHAPv2 with identity, username, password).
 * - The ESP32 scans first and only tries networks that are really in range,
 *   so a long list does not make the boot slower. If the scan itself fails,
 *   all configured networks are tried in order.
 * - Entries whose text starts with "YOUR_" are treated as "not filled in yet"
 *   and skipped. Everything after the Wi-Fi phase is unchanged from 2.0.1.
 *
 * WHAT WAS NEW IN 2.0.1
 * - Base is v1.5.1 (Wi-Fi NTP auto-sync, clean restart before Bluetooth),
 *   taken unchanged. The control code of v2.0.0 (volume pot, balance pot,
 *   pot on/off switch, toggle, push button, 3-contact rotary encoder) is
 *   added on top, exactly as it ran in v2.0.0.
 * - The controls only PRINT to the serial terminal. They do not change the
 *   volume, the display or Bluetooth. That comes later.
 * - Controls start LAST in setup(), after the Wi-Fi phase and after
 *   Bluetooth. On the Wi-Fi boot the ESP32 restarts before that point,
 *   so the controls simply start on the second (clean) boot.
 * - Encoder "state 4" contact (green wire) is on GPIO23 (GPIO25 is the
 *   I2S word-select line of the DAC).
 * - loop() runs every 5 ms (v1.5.1: 100 ms) so switches/pots are polled
 *   fast enough. RTC refresh and serial time sync are rate-limited by
 *   millis() inside their own functions, so they behave as before.
 *
 * HOW THE PARTS STAY APART (no shared resources)
 *   Wi-Fi/NTP ........ only in setup() before Bluetooth, no GPIOs used
 *   Bluetooth/I2S .... GPIO 26, 25, 27 (I2S), BT stack tasks
 *   Display .......... GPIO 5, 18 (TM1637), DisplayTask on core 1
 *   RTC .............. GPIO 21, 22 (I2C), only touched from setup()/loop()
 *   Controls ......... GPIO 34, 35 (ADC1), 17, 16, 32, 33, 23, ISR for the
 *                      encoder, polled from loop(). No I2C, no I2S, no display.
 *   Serial output .... every control line is ONE Serial.printf() call.
 *
 * Changes in 1.5.1 (merge of 1.3.4 baseline + 1.5.0):
 * - Bluetooth now always starts from a "clean chip", exactly like 1.3.4.
 *   After the Wi-Fi/NTP phase the ESP32 does ONE software restart and the
 *   second boot skips Wi-Fi completely. The Wi-Fi driver leaves RAM
 *   allocations, tasks and RF state behind even after WIFI_OFF, and
 *   Bluetooth Classic + A2DP needs a lot of free RAM.
 *   (Set CLEAN_REBOOT_AFTER_WIFI to 0 to disable this and test.)
 * - Wi-Fi is only switched on if at least one network is configured.
 * - WiFi.persistent(false): credentials are not written to flash.
 * - Heap diagnostics printed on the serial monitor before/after BT start.
 * - Reset reason printed at boot.
 *
 * Changes in 1.5.0:
 * - Dual Wi-Fi network support on boot (Home Network -> Phone Hotspot).
 * - NTP time synchronization via pool.ntp.org.
 * - Wi-Fi radio shut down (WIFI_OFF) before Bluetooth starts.
 *
 * Changes in 1.3.4 / 1.3.5:
 * - Only loop() talks to the RTC over I2C (no mutex contention on I2C bus).
 * - Display task reads time through a mutex-protected base epoch + millis() pair.
 * - Validated RTC reads (two consecutive reads matching).
 * - Timezone offset is DST-aware (EU rule): CEST +2h / CET +1h.
 *
 * TIME MODEL: the DS3231 stores LOCAL Budapest wall-clock time.
 *
 * ENCODER, as measured on the bench:
 *   common -> GND, contacts -> GPIO32 (state 1), GPIO33 (state 2), GPIO23 (state 4).
 *   Each click closes exactly one contact. 12 clicks = one full turn.
 *   Turning RIGHT gives the order 4 -> 2 -> 1 -> 4 ...
 *   Turning LEFT  gives the order 1 -> 2 -> 4 -> 1 ...
 *
 * Not compiled or tested on hardware by the author: check the first run.
 * ============================================================ */

#include <Arduino.h>
#include <Wire.h>
#include <RTClib.h>
#include <TM1637Display.h>
#include <stdlib.h>
#include <string.h>
#include <WiFi.h>
#include "esp_wpa2.h"   // WPA2-Enterprise (eduroam) helper functions
#include "time.h"
#include "BluetoothA2DPSink.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// System & Hardware Control
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_system.h"
#include "esp_heap_caps.h"

// Low-level ESP32 Bluetooth Stack Headers
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_avrc_api.h"

#define FIRMWARE_VERSION "2.9.0"
#define BUILD_TIMESTAMP  (__DATE__ " " __TIME__)

#ifndef BUILD_EPOCH
#define BUILD_EPOCH 0UL
#endif

// 1 = restart once after the Wi-Fi phase so Bluetooth starts on a clean chip
// 0 = no restart (v1.5.0 behaviour) - only for testing
#define CLEAN_REBOOT_AFTER_WIFI 1

// Broadcast Name containing "Car Stereo" for automatic iOS/Spotify detection
#define DEVICE_NAME "OLDS Car Stereo"

// ============================================================
// WIFI NETWORK LIST
// ============================================================
// The list itself is in src/wifi_networks.h (passwords stay out of this file).
// To add a network, add one line to that table. Nothing here changes.

enum WifiNetType : uint8_t {
    NET_PSK,   // normal Wi-Fi with a password (WPA2-Personal)
    NET_EAP    // WPA2-Enterprise, PEAP/MSCHAPv2 (eduroam)
};

struct WifiNetwork {
    const char *ssid;      // network name
    WifiNetType type;      // NET_PSK or NET_EAP
    const char *password;  // PSK: the Wi-Fi password. EAP: the account password
    const char *identity;  // EAP only: outer identity (anonymous@institution.hu), "" for PSK
    const char *username;  // EAP only: inner identity (user@institution.hu), "" for PSK
};

#include "wifi_networks.h"   // defines WIFI_NETWORKS[]

const size_t WIFI_NET_COUNT = sizeof(WIFI_NETWORKS) / sizeof(WIFI_NETWORKS[0]);

const unsigned long WIFI_TIMEOUT_PSK_MS = 8000UL;   // normal networks
const unsigned long WIFI_TIMEOUT_EAP_MS = 20000UL;  // 802.1X handshake is slower

// ============================================================
// HARDWARE PIN CONFIGURATION  (every GPIO is used exactly once)
// ============================================================
//
//  GPIO  | function                      | part
//  ------+-------------------------------+----------------------
//    2   | status LED (output)           | on-board LED
//    4   | (free, push button dropped)   | -
//    5   | TM1637 CLK                    | display
//   16   | toggle switch -> GND          | controls
//   17   | volume-pot on/off switch->GND | controls
//   18   | TM1637 DIO                    | display
//   21   | I2C SDA                       | DS3231 RTC
//   22   | I2C SCL                       | DS3231 RTC
//   23   | encoder state 4 (green)       | controls
//   25   | I2S word select (LCK)         | DAC
//   26   | I2S bit clock (BCK)           | DAC
//   27   | I2S data out (DIN)            | DAC
//   32   | encoder state 1 (red)         | controls
//   33   | encoder state 2 (black)       | controls
//   34   | volume pot wiper (ADC1)       | controls
//   35   | balance pot wiper (ADC1)      | controls

// --- 1. TM1637 DISPLAY ---
const int PIN_TM1637_CLK = 5;  // Board label: D5
const int PIN_TM1637_DIO = 18; // Board label: D18

// --- 2. I2C BUS (DS3231 RTC) ---
const int PIN_I2C_SDA = 21; // Board label: SDA / D21
const int PIN_I2C_SCL = 22; // Board label: SCL / D22

// --- 3. I2S AUDIO BUS (CJMCU-1334 DAC) ---
const int PIN_I2S_BCLK = 26; // Board label: D26 -> BCK
const int PIN_I2S_WSEL = 25; // Board label: D25 -> LCK / WSEL
const int PIN_I2S_DOUT = 27; // Board label: D27 -> DIN

// --- 4. STATUS LED ---
const int PIN_STATUS_LED = 2; // Board label: D2 / BUILTIN_LED

// --- 5. CONTROLS (pots, switches, encoder) ---
const uint8_t PIN_VOLUME   = 34;  // volume pot wiper (ADC1, input-only)
const uint8_t PIN_BALANCE  = 35;  // balance pot wiper (ADC1, input-only)
const uint8_t PIN_POWER_SW = 17;  // switch on the volume pot -> GND when closed
const uint8_t PIN_TOGGLE   = 16;  // toggle switch -> GND when closed
// state bit0 = GPIO32 (red), bit1 = GPIO33 (black), bit2 = GPIO23 (green)
const uint8_t ENC_PINS[3]  = {32, 33, 23};

// Bluetooth Class of Device (CoD): 0x200420 (Car Audio / Rendering)
#define CAR_AUDIO_COD 0x200420

// ============================================================
// DISPLAY TIMING
// ============================================================

const unsigned long CLOCK_DISPLAY_MS     = 10000UL; // Show clock for 10s
const unsigned long SECONDARY_DISPLAY_MS = 2500UL;  // Show "bLUE" or "----" for 2.5s
const unsigned long LED_BLINK_MS         = 1000UL;  // Blinking colon/LED interval

// ============================================================
// TIME CONSTANTS
// ============================================================

const uint32_t MIN_VALID_EPOCH = 1767225600UL; // 2026-01-01 00:00:00
const uint32_t MAX_VALID_EPOCH = 2082758400UL; // 2036-01-01 00:00:00
const uint16_t MIN_VALID_YEAR  = 2026;
const uint16_t MAX_VALID_YEAR  = 2035;

const unsigned long RTC_REFRESH_MS = 600000UL; // Re-read RTC every 10 min
const unsigned long RTC_RETRY_MS   = 30000UL;  // Retry after a failed read

// ============================================================
// CONTROLS SETTINGS
// ============================================================

#define POT_CAL   0   // 1 = print raw pot millivolts only (for calibration)
#define ENC_DEBUG 0   // 1 = print every settled encoder state (for troubleshooting)

const uint32_t DEBOUNCE_MS = 30;

// ---- CALIBRATION (mV at the pin, measure with POT_CAL 1) ----
// NOTE: the ADC reads slightly differently while Bluetooth is running
// (radio noise on the supply). Re-run POT_CAL 1 with Bluetooth active.
const int VOL_MIN_MV    = 150;    // pot turned to one end
const int VOL_MAX_MV    = 3100;   // pot turned to the other end
const bool VOL_INVERT   = true;   // true: low mV = loud (as in the old sketch)
const int BAL_MIN_MV    = 150;    // balance pot end 1
const int BAL_CENTER_MV = 1650;   // balance pot centre
const int BAL_MAX_MV    = 3100;   // balance pot end 2
const int BAL_DEAD_MV   = 60;     // +/- mV around centre that counts as 0
const int EDGE_MV       = 40;     // dead zone at the ends so 0/100 % are reachable
const int POT_HYST_MV   = 10;     // ignore changes smaller than this

// ---- OUTPUT / MODE SETTINGS (v2.9.0) ----
const float VOL_RANGE_DB   = 50.0f;  // pot range: bottom = -50 dB, top = 0 dB (step 0 = mute)
const int   VOL_MAX_STEP   = 127;    // volume cap 0..127; lower it to limit the maximum loudness
const bool  BAL_INVERT     = false;  // true = swap the balance direction
const bool  TOGGLE_CLOSED_IS_RADIO = true;   // toggle closed to GND = radio, open = Bluetooth
const uint32_t ENC_SKIP_LOCKOUT_MS  = 400;   // minimum time between two track skips
const uint32_t PAUSE_FIRST_DELAY_MS = 1000;  // wait before the first "pause" goes to the phone
const uint32_t PAUSE_RETRY_MS       = 3000;  // wait before "pause" is sent again
const uint8_t  PAUSE_MAX_TRIES      = 3;     // give up after this many pause commands
const uint32_t PHONE_SYNC_MIN_MS    = 120;   // minimum time between volume messages to the phone
const float    GAIN_STEP = 1.0f / 441.0f;    // gain change per sample (full range in ~10 ms at 44.1 kHz)

// ---- ENCODER SETTINGS ----
// ENC_SEQ = the contact states in the order they appear when turning RIGHT.
const int8_t ENC_SEQ[] = {4, 2, 1};
const int ENC_STEPS_PER_DETENT = 1;   // measured: 1 state per click
#define ENC_REVERSE 0                 // 1 = swap the counting direction

// ============================================================
// GLOBAL OBJECTS & HANDLES
// ============================================================

RTC_DS3231 rtc;
TM1637Display display(PIN_TM1637_CLK, PIN_TM1637_DIO);
BluetoothA2DPSink a2dp_sink;

TaskHandle_t DisplayTaskHandle = NULL;
bool rtcFound = false;

// ============================================================
// STATE VARIABLES
// ============================================================

bool secondaryScreen = false;
unsigned long displayTimer = 0;

bool ledState = false;
unsigned long ledTimer = 0;

volatile bool displayNeedsRedraw = true;

SemaphoreHandle_t timeMutex = NULL;
uint32_t baseEpoch  = 0;
uint32_t baseMillis = 0;
bool     timeValid  = false;
const char *timeSource = "none";
unsigned long lastRtcRefreshMs = 0;

// Flag that survives a SOFTWARE restart (kept in RTC memory, not cleared by
// esp_restart) but contains random data after a power cycle. It only counts
// as "set" if it holds this exact magic number.
#define WIFI_DONE_MAGIC 0xC0DE1501UL
RTC_NOINIT_ATTR uint32_t wifiDoneMagic;

// Custom 7-Segment Patterns
const uint8_t MY_SEG_B    = 0b01111100; // 'b'
const uint8_t MY_SEG_L    = 0b00111000; // 'L'
const uint8_t MY_SEG_U    = 0b00111110; // 'u'
const uint8_t MY_SEG_E    = 0b01111001; // 'E'
const uint8_t MY_SEG_DASH = 0b01000000; // '-'

// ============================================================
// OUTPUT STAGE: software volume + balance + mute (v2.9.0)
// ============================================================
// The A2DP library calls update_audio_data() for every block of audio, right
// before it goes to the I2S/DAC. Here the left and right gain are applied.
// set_volume() is called by the library when the PHONE changes its volume:
// it is ignored on purpose, the knob is the master (see phone sync below).
// The gains are written by loop() and read by the audio task; a float is a
// single 32-bit value on the ESP32, so no lock is needed. The gain moves
// toward the target by GAIN_STEP per sample (no clicks, no zipper noise).

class OldsVolumeControl : public A2DPVolumeControl {
 public:
  using A2DPVolumeControl::update_audio_data;

  void set_volume(uint8_t volume) override { (void)volume; }   // phone volume ignored

  void setTargets(float left, float right) { targetL = left; targetR = right; }

  void update_audio_data(Frame *data, uint16_t frameCount) override {
    if (data == nullptr || frameCount == 0) return;
    const float tl = targetL;
    const float tr = targetR;
    float cl = curL;
    float cr = curR;
    for (uint16_t i = 0; i < frameCount; i++) {
      cl = approach(cl, tl);
      cr = approach(cr, tr);
      data[i].channel1 = (int16_t)clip((int32_t)((float)data[i].channel1 * cl));
      data[i].channel2 = (int16_t)clip((int32_t)((float)data[i].channel2 * cr));
    }
    curL = cl;
    curR = cr;
  }

 private:
  volatile float targetL = 0.0f;   // start muted: no blast while the controls start
  volatile float targetR = 0.0f;
  float curL = 0.0f;
  float curR = 0.0f;

  static float approach(float cur, float target) {
    if (cur < target) {
      cur += GAIN_STEP;
      if (cur > target) cur = target;
    } else if (cur > target) {
      cur -= GAIN_STEP;
      if (cur < target) cur = target;
    }
    return cur;
  }
};

OldsVolumeControl oldsVol;

// What the unit is doing. Written by loop(), read by the display task.
enum UiMode : uint8_t { UI_BT = 0, UI_RADIO = 1, UI_OFF = 2 };
volatile uint8_t uiMode = UI_OFF;     // set for real by controlsBegin()

int  potVolume127 = -1;               // knob volume 0..127 (-1 = knob not read yet)
int  balanceVal   = 0;                // -50..+50 (0 = centre)

// Phone volume sync (written by the Bluetooth task too)
volatile bool     pendingPhoneSync  = false;
volatile uint32_t phoneSyncNotBefore = 0;

// Pause / resume bookkeeping (loop() only)
bool     resumeAfterUnmute = false;   // phone was playing when we muted
bool     pcActive          = false;   // "pause cycle" running
uint8_t  pcTries           = 0;
uint32_t pcNextAt          = 0;
uint32_t pcCooldownUntil   = 0;
uint8_t  extraCyclesLeft   = 0;       // one more cycle if the phone starts again while muted
uint32_t lastSkipMs        = 0;

// ============================================================
// HELPER: BROWNOUT DISABLE
// ============================================================

void disableBrownoutDetector()
{
#ifdef RTC_CNTL_BROWN_OUT_REG
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
#elif defined(RTC_CNTL_BROWNOUT_REG)
    WRITE_PERI_REG(RTC_CNTL_BROWNOUT_REG, 0);
#endif
}

// ============================================================
// HELPER: HEAP DIAGNOSTICS
// ============================================================

void printHeap(const char *label)
{
    Serial.printf("[HEAP] %-16s free %6u B | largest block %6u B | lowest ever %6u B\n",
                  label,
                  (unsigned)esp_get_free_heap_size(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                  (unsigned)esp_get_minimum_free_heap_size());
}

// ============================================================
// TIME BASE & TIMEZONE
// ============================================================

DateTime getLowNoiseTime()
{
    uint32_t e = 0, m = 0;
    bool v = false;

    if (timeMutex != NULL) {
        xSemaphoreTake(timeMutex, portMAX_DELAY);
        e = baseEpoch;
        m = baseMillis;
        v = timeValid;
        xSemaphoreGive(timeMutex);
    }

    if (!v) {
        return DateTime(1970, 1, 1, 0, 0, 0);
    }

    uint32_t elapsedSecs = (millis() - m) / 1000UL;
    return DateTime(e + elapsedSecs);
}

void setTimeBase(uint32_t localEpoch, const char *source)
{
    xSemaphoreTake(timeMutex, portMAX_DELAY);
    baseEpoch  = localEpoch;
    baseMillis = millis();
    timeValid  = true;
    xSemaphoreGive(timeMutex);

    timeSource = source;
    lastRtcRefreshMs = millis();
    displayNeedsRedraw = true;
}

void printStamp(const char *label, const DateTime &t)
{
    Serial.printf("%s %04u-%02u-%02u %02u:%02u:%02u\n", label,
                  (unsigned)t.year(), (unsigned)t.month(), (unsigned)t.day(),
                  (unsigned)t.hour(), (unsigned)t.minute(), (unsigned)t.second());
}

int32_t budapestOffsetSec(uint32_t utcEpoch)
{
    uint16_t y = DateTime(utcEpoch).year();
    uint8_t lastSunMar = 31 - (5 * y / 4 + 4) % 7;
    uint8_t lastSunOct = 31 - (5 * y / 4 + 1) % 7;

    uint32_t dstStart = DateTime(y, 3,  lastSunMar, 1, 0, 0).unixtime();
    uint32_t dstEnd   = DateTime(y, 10, lastSunOct, 1, 0, 0).unixtime();

    return (utcEpoch >= dstStart && utcEpoch < dstEnd) ? 7200 : 3600;
}

// ============================================================
// RTC ACCESS
// ============================================================

bool isPlausibleRtc(const DateTime &t)
{
    return t.year()   >= MIN_VALID_YEAR && t.year() <= MAX_VALID_YEAR &&
           t.month()  >= 1 && t.month() <= 12 &&
           t.day()    >= 1 && t.day()   <= 31 &&
           t.hour()   <= 23 && t.minute() <= 59 && t.second() <= 59;
}

bool readRtcValidated(uint32_t &epochOut)
{
    for (uint8_t attempt = 0; attempt < 3; attempt++) {
        DateTime a = rtc.now();
        delay(3);
        DateTime b = rtc.now();

        if (isPlausibleRtc(a) && isPlausibleRtc(b)) {
            int32_t d = (int32_t)b.unixtime() - (int32_t)a.unixtime();
            if (d >= 0 && d <= 2) {
                epochOut = b.unixtime();
                return true;
            }
        }
        delay(20);
    }
    return false;
}

void refreshFromRtc()
{
    if (!rtcFound) return;
    if (millis() - lastRtcRefreshMs < RTC_REFRESH_MS) return;

    uint32_t e = 0;
    if (readRtcValidated(e)) {
        int32_t drift = (int32_t)e - (int32_t)getLowNoiseTime().unixtime();
        Serial.printf("[RTC] Periodic refresh OK (internal clock was %+ld s off)\n", (long)drift);
        setTimeBase(e, timeSource);
    } else {
        Serial.println("[RTC] Periodic refresh FAILED (bad I2C data) - keeping internal clock, retry soon");
        lastRtcRefreshMs = millis() - (RTC_REFRESH_MS - RTC_RETRY_MS);
    }
}

// ============================================================
// WIFI & NTP AUTO-SYNC
// ============================================================

void applyUtcEpoch(uint32_t utcEpoch)
{
    const int32_t  offset     = budapestOffsetSec(utcEpoch);
    const uint32_t localEpoch = utcEpoch + offset;

    if (rtcFound) {
        rtc.adjust(DateTime(localEpoch));

        uint32_t back = 0;
        if (readRtcValidated(back) && abs((int32_t)back - (int32_t)localEpoch) <= 3) {
            setTimeBase(back, "NTP / Serial sync");
            Serial.printf("[SYNC] OK: UTC epoch %lu, offset +%ld h (%s). RTC write verified.\n",
                          (unsigned long)utcEpoch, (long)(offset / 3600),
                          offset == 7200 ? "CEST" : "CET");
        } else {
            setTimeBase(localEpoch, "NTP / Serial sync (RTC write NOT verified)");
            Serial.println("[SYNC] WARNING: RTC did not confirm the new time.");
        }
    } else {
        setTimeBase(localEpoch, "NTP sync (no RTC)");
    }

    printStamp("[SYNC] Local time now:", getLowNoiseTime());
}

// "YOUR_..." text = placeholder that was not filled in yet
static bool isPlaceholder(const char *t)
{
    return t != NULL && strncmp(t, "YOUR_", 5) == 0;
}

static bool isNetConfigured(const WifiNetwork &n)
{
    if (n.ssid == NULL || strlen(n.ssid) == 0) return false;
    if (isPlaceholder(n.ssid) || isPlaceholder(n.password)) return false;
    if (n.type == NET_EAP &&
        (n.username == NULL || strlen(n.username) == 0 || isPlaceholder(n.username) ||
         isPlaceholder(n.identity))) return false;
    return true;
}

// Switch the WPA2-Enterprise settings off again and wipe them from RAM.
static void eapCleanup()
{
    esp_wifi_sta_wpa2_ent_disable();
    esp_wifi_sta_wpa2_ent_clear_identity();
    esp_wifi_sta_wpa2_ent_clear_username();
    esp_wifi_sta_wpa2_ent_clear_password();
}

bool tryConnectWiFi(const WifiNetwork &n)
{
    Serial.printf("[WIFI] Attempting connection to: %s%s\n", n.ssid,
                  n.type == NET_EAP ? " (WPA2-Enterprise)" : "");

    unsigned long timeoutMs = WIFI_TIMEOUT_PSK_MS;
    if (n.type == NET_EAP) {
        timeoutMs = WIFI_TIMEOUT_EAP_MS;
        WiFi.begin(n.ssid, WPA2_AUTH_PEAP, n.identity, n.username, n.password);
    } else {
        WiFi.begin(n.ssid, n.password);
    }

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();

    const bool ok = (WiFi.status() == WL_CONNECTED);
    if (!ok) {
        WiFi.disconnect(false);   // clean slate for the next entry
        delay(100);
        if (n.type == NET_EAP) eapCleanup();
    }
    return ok;
}

// Returns true if the Wi-Fi radio was switched on (so the caller knows the
// Wi-Fi driver has touched the chip), false if it was never started.
bool syncTimeViaWiFi()
{
    size_t configured = 0;
    for (size_t i = 0; i < WIFI_NET_COUNT; i++) {
        if (isNetConfigured(WIFI_NETWORKS[i])) configured++;
    }

    if (configured == 0) {
        Serial.println("[WIFI] No network configured - skipping NTP sync (Wi-Fi never started).");
        return false;
    }

    WiFi.persistent(false);   // do not write credentials to flash
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    // 1. Scan, so only networks that are really in range are tried.
    bool seen[WIFI_NET_COUNT];
    for (size_t i = 0; i < WIFI_NET_COUNT; i++) seen[i] = false;
    bool scanOk   = false;
    bool anySeen  = false;

    for (int attempt = 0; attempt < 2 && !anySeen; attempt++) {
        Serial.println("[WIFI] Scanning for known networks...");
        const int found = WiFi.scanNetworks();
        if (found < 0) {
            Serial.println("[WIFI] Scan failed - will try all configured networks in order.");
            scanOk = false;
            break;
        }
        scanOk = true;
        for (size_t i = 0; i < WIFI_NET_COUNT; i++) {
            if (!isNetConfigured(WIFI_NETWORKS[i])) continue;
            for (int j = 0; j < found; j++) {
                if (WiFi.SSID(j) == WIFI_NETWORKS[i].ssid) {
                    seen[i] = true;
                    anySeen = true;
                }
            }
        }
        WiFi.scanDelete();
        Serial.printf("[WIFI] Scan: %d network(s) in range, %s.\n", found,
                      anySeen ? "at least one is known" : "none is known");
    }

    // 2. Try the table from top to bottom (order = priority).
    bool connected = false;
    for (size_t i = 0; i < WIFI_NET_COUNT && !connected; i++) {
        if (!isNetConfigured(WIFI_NETWORKS[i])) continue;
        if (scanOk && !seen[i]) continue;      // not in range, do not waste time
        connected = tryConnectWiFi(WIFI_NETWORKS[i]);
    }

    if (connected) {
        Serial.printf("[WIFI] Connected successfully to %s! IP: %s\n",
                      WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());

        // Configure NTP (pool.ntp.org yields UTC epoch)
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");

        Serial.print("[NTP] Fetching atomic time");
        time_t now = 0;
        unsigned long ntpStart = millis();

        while (millis() - ntpStart < 6000UL) {
            time(&now);
            if (now > (time_t)MIN_VALID_EPOCH) {
                break;
            }
            delay(500);
            Serial.print(".");
        }
        Serial.println();

        if (now > (time_t)MIN_VALID_EPOCH) {
            Serial.printf("[NTP] Sync successful! Epoch: %lu\n", (unsigned long)now);
            applyUtcEpoch((uint32_t)now);
        } else {
            Serial.println("[NTP] Failed to get time from NTP server.");
        }
    } else {
        Serial.println("[WIFI] Could not connect to any known Wi-Fi network.");
    }

    // Shut down Wi-Fi entirely to clear the radio for Bluetooth
    eapCleanup();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("[WIFI] Radio shut down.");
    printHeap("after Wi-Fi off");
    return true;
}

// ============================================================
// SERIAL TIME SYNC
// ============================================================

void printTimeStatus()
{
    Serial.println("---------- TIME STATUS ----------");
    printStamp("[TIME] Display clock (local):", getLowNoiseTime());
    Serial.printf("[TIME] Source: %s\n", timeSource);

    if (rtcFound) {
        uint32_t e = 0;
        if (readRtcValidated(e)) {
            printStamp("[TIME] RTC raw        (local):", DateTime(e));
            Serial.printf("[TIME] Display vs RTC: %+ld s, RTC temp %.2f C, lostPower=%d\n",
                          (long)((int32_t)getLowNoiseTime().unixtime() - (int32_t)e),
                          rtc.getTemperature(), rtc.lostPower() ? 1 : 0);
        } else {
            Serial.println("[TIME] RTC read FAILED / implausible data.");
        }
    } else {
        Serial.println("[TIME] RTC not found on I2C bus.");
    }
    Serial.println("---------------------------------");
}

void processSerialLine(const char *line)
{
    if (line[0] == '?') {
        printTimeStatus();
        return;
    }

    const size_t n = strlen(line);
    for (size_t i = 0; i < n; i++) {
        if (line[i] < '0' || line[i] > '9') return;
    }
    if (n > 10) return;

    unsigned long v = strtoul(line, NULL, 10);
    if (v < MIN_VALID_EPOCH || v > MAX_VALID_EPOCH) return;

    applyUtcEpoch((uint32_t)v);
}

static void flushSerialLine(char *buf, uint8_t &len, bool &overflow)
{
    if (!overflow && len > 0) {
        buf[len] = 0;
        processSerialLine(buf);
    }
    len = 0;
    overflow = false;
}

void checkSerialTimeSync()
{
    static char          lineBuf[24];
    static uint8_t       lineLen  = 0;
    static bool          overflow = false;
    static unsigned long lastRxMs = 0;

    while (Serial.available() > 0) {
        char c = (char)Serial.read();
        lastRxMs = millis();

        if (c == '\n' || c == '\r') {
            if (lineLen > 0 || overflow) flushSerialLine(lineBuf, lineLen, overflow);
        } else if (c == ' ' || c == '\t') {
            continue;
        } else if (lineLen < sizeof(lineBuf) - 1) {
            lineBuf[lineLen++] = c;
        } else {
            overflow = true;
        }
    }

    if ((lineLen > 0 || overflow) && (millis() - lastRxMs > 200UL)) {
        flushSerialLine(lineBuf, lineLen, overflow);
    }
}

// ============================================================
// BLUETOOTH CALLBACKS & CONFIG
// ============================================================

void avrc_metadata_callback(uint8_t id, const uint8_t *text) {
    if (id == ESP_AVRC_MD_ATTR_TITLE) {
        Serial.printf("[AVRCP] Title: %s\n", text);
    } else if (id == ESP_AVRC_MD_ATTR_ARTIST) {
        Serial.printf("[AVRCP] Artist: %s\n", text);
    }
}

// The PHONE changed its volume. The knob wins: schedule the knob value to be sent back.
void onPhoneVolumeChanged(int phoneVolume)
{
    (void)phoneVolume;
    pendingPhoneSync = true;
}

void bluetoothConnectionStateChanged(esp_a2d_connection_state_t state, void *ptr)
{
    if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
        Serial.println("\n[BT] CONNECTED");
        // The phone registers for volume messages a moment after connecting:
        // send the knob volume once it is ready.
        phoneSyncNotBefore = millis() + 2500UL;
        pendingPhoneSync = true;
    } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
        Serial.println("\n[BT] DISCONNECTED - Seeking last connected device...");
    }
    displayNeedsRedraw = true;
}

void configure_car_stereo_identity()
{
    uint32_t cod_raw = CAR_AUDIO_COD;
    esp_bt_cod_t cod = *(esp_bt_cod_t *)&cod_raw;

    esp_err_t cod_err = esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_ALL);
    if (cod_err == ESP_OK) {
        Serial.printf("[BT] Class of Device set to 0x%06X (Car Audio)\n", CAR_AUDIO_COD);
    }
}

// ============================================================
// DISPLAY & FREERTOS TASK
// ============================================================

void showClock()
{
    DateTime now = getLowNoiseTime();
    int value = now.hour() * 100 + now.minute();
    uint8_t dots = ledState ? 0x40 : 0x00;

    display.showNumberDecEx(value, dots, true);
}

void showBlue()
{
    uint8_t data[] = { MY_SEG_B, MY_SEG_L, MY_SEG_U, MY_SEG_E };
    display.setSegments(data);
}

void showPlaceholder()
{
    uint8_t data[] = { MY_SEG_DASH, MY_SEG_DASH, MY_SEG_DASH, MY_SEG_DASH };
    display.setSegments(data);
}

void updateDisplay()
{
    unsigned long now = millis();
    static uint8_t lastMode = 255;
    const uint8_t mode = uiMode;

    if (mode != lastMode) {            // mode changed: restart the screen sequence
        lastMode = mode;
        secondaryScreen = (mode == UI_RADIO);
        displayTimer = now;
        displayNeedsRedraw = true;
    }

    if (mode == UI_BT) {               // clock 10 s, then "bLUE"/"----" 2.5 s
        if (!secondaryScreen) {
            if (now - displayTimer >= CLOCK_DISPLAY_MS) {
                secondaryScreen = true;
                displayTimer = now;
                displayNeedsRedraw = true;
            }
        } else {
            if (now - displayTimer >= SECONDARY_DISPLAY_MS) {
                secondaryScreen = false;
                displayTimer = now;
                displayNeedsRedraw = true;
            }
        }
    }

    if (displayNeedsRedraw) {
        displayNeedsRedraw = false;

        if (mode == UI_RADIO) {
            showPlaceholder();         // "----" until the radio exists
        } else if (mode == UI_OFF || !secondaryScreen) {
            showClock();               // OFF = clock mode
        } else {
            if (a2dp_sink.is_connected()) {
                showBlue();
            } else {
                showPlaceholder();
            }
        }
    }
}

void updateLED()
{
    unsigned long now = millis();

    if (now - ledTimer >= LED_BLINK_MS) {
        ledTimer = now;
        ledState = !ledState;
        digitalWrite(PIN_STATUS_LED, ledState ? HIGH : LOW);

        displayNeedsRedraw = true;
    }
}

void displayTaskCode(void *pvParameters)
{
    for (;;) {
        updateDisplay();
        updateLED();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ============================================================
// CONTROLS: HELPERS (debounced input, pot)
// Called ONLY from setup() and loop().
// ============================================================

struct DebouncedInput {
  uint8_t pin = 0;
  bool stable = HIGH, lastRead = HIGH;
  uint32_t changedAt = 0;

  void begin(uint8_t p) {
    pin = p;
    pinMode(pin, INPUT_PULLUP);
    stable = lastRead = digitalRead(pin);
  }
  bool update(uint32_t now) {            // true when the debounced state changed
    bool r = digitalRead(pin);
    if (r != lastRead) { lastRead = r; changedAt = now; }
    if (r != stable && (now - changedAt) >= DEBOUNCE_MS) { stable = r; return true; }
    return false;
  }
  bool active() const { return stable == LOW; }
};

struct Pot {
  uint8_t pin = 0;
  float filt = -1;
  int accepted = -10000;

  void begin(uint8_t p) { pin = p; pinMode(pin, INPUT); }
  int readMv() {                         // 16-sample average, factory-calibrated
    long sum = 0;
    for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(pin);
    return (int)(sum / 16);
  }
  bool update() {                        // true when the filtered value moved enough
    int mv = readMv();
    filt = (filt < 0) ? mv : filt + 0.2f * (mv - filt);
    if (abs((int)filt - accepted) >= POT_HYST_MV) { accepted = (int)filt; return true; }
    return false;
  }
};

DebouncedInput powerSw, toggleSw;
Pot volPot, balPot;

// ============================================================
// CONTROLS: ENCODER (state-table decoder, interrupt driven)
// ============================================================

const int ENC_N = sizeof(ENC_SEQ) / sizeof(ENC_SEQ[0]);
int8_t encIndex[8];                      // state -> position in ENC_SEQ, or -1
volatile long encSteps = 0;              // +1 per step to the right, -1 to the left
volatile uint8_t encPrev = 0;            // last raw state seen (any state)
volatile int8_t encLast = -1;            // position in ENC_SEQ of the last VALID state
portMUX_TYPE encMux = portMUX_INITIALIZER_UNLOCKED;

uint8_t IRAM_ATTR readEncState() {
  uint8_t s = 0;
  for (int i = 0; i < 3; i++) {
    if (digitalRead(ENC_PINS[i]) == LOW) s |= (1 << i);
  }
  return s;
}

void IRAM_ATTR encoderISR() {
  uint8_t s = readEncState();
  portENTER_CRITICAL_ISR(&encMux);
  if (s != encPrev) {
    encPrev = s;
    int8_t b = encIndex[s];
    if (b >= 0) {                        // only states that belong to the cycle count;
      if (encLast >= 0) {                // state 0 (between contacts) and bounce are ignored
        int d = (b - encLast + ENC_N) % ENC_N;
        if (d == 1) encSteps++;
        else if (d == ENC_N - 1) encSteps--;
      }
      encLast = b;
    }
  }
  portEXIT_CRITICAL_ISR(&encMux);
}

// ============================================================
// OUTPUT POLICY: mode, mute, pause/resume, phone volume sync, track skip
// ============================================================

static bool timeReached(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

const char *modeName(uint8_t m)
{
  if (m == UI_BT)    return "BLUETOOTH";
  if (m == UI_RADIO) return "RADIO (not implemented)";
  return "OFF (clock mode)";
}

// Bluetooth commands only make sense when the phone is connected.
bool btReady()          { return a2dp_sink.is_connected() && a2dp_sink.is_avrc_connected(); }
bool phoneIsStreaming() { return a2dp_sink.get_audio_state() == ESP_A2D_AUDIO_STATE_STARTED; }

// Turn knob volume + balance + mode into the two channel gains.
void applyOutputTargets()
{
  const bool active = (uiMode == UI_BT);

  float g = 0.0f;                                   // 0 = mute
  if (potVolume127 > 0) {
    const float dB = -VOL_RANGE_DB * (1.0f - (float)potVolume127 / 127.0f);
    g = powf(10.0f, dB / 20.0f);
  }

  float bl = 1.0f, br = 1.0f;                       // only the opposite channel is attenuated
  const int b = BAL_INVERT ? -balanceVal : balanceVal;
  if (b > 0)      bl = 1.0f - (float)b / 50.0f;     // positive = right: left gets quieter
  else if (b < 0) br = 1.0f + (float)b / 50.0f;     // negative = left: right gets quieter

  oldsVol.setTargets(active ? g * bl : 0.0f, active ? g * br : 0.0f);
}

void enterMuted(uint32_t now)
{
  resumeAfterUnmute = phoneIsStreaming();           // remember: was it playing?
  extraCyclesLeft   = 1;
  pcActive          = true;                         // pause cycle starts after a short delay
  pcTries           = 0;
  pcNextAt          = now + PAUSE_FIRST_DELAY_MS;
}

void leaveMuted()
{
  pcActive = false;                                 // cancel a pause that was not sent yet
  if (resumeAfterUnmute && btReady()) {
    a2dp_sink.play();                               // harmless if it is already playing
    Serial.printf("[BT] resume playback\n");
  }
  resumeAfterUnmute = false;
}

// Reads the two switches and moves between BLUETOOTH / RADIO / OFF.
void updateMode(uint32_t now, bool first)
{
  const bool power    = powerSw.active();
  const bool radioSel = TOGGLE_CLOSED_IS_RADIO ? toggleSw.active() : !toggleSw.active();
  const uint8_t newMode = !power ? UI_OFF : (radioSel ? UI_RADIO : UI_BT);

  if (!first && newMode == uiMode) return;

  const bool wasActive = (!first && uiMode == UI_BT);
  const bool nowActive = (newMode == UI_BT);

  uiMode = newMode;
  displayNeedsRedraw = true;
  Serial.printf("[MODE] %s\n", modeName(newMode));
  if (newMode == UI_RADIO) {
    Serial.printf("[RADIO] ERROR: radio is not implemented yet (tuner and audio switch come later). Output muted.\n");
  }

  if (nowActive && !wasActive)               leaveMuted();
  else if (!nowActive && (wasActive || first)) enterMuted(now);

  applyOutputTargets();
}

// While muted: ask the phone to pause (1 s delay, then up to 3 tries, 3 s apart).
void pauseCycleTick(uint32_t now)
{
  if (uiMode == UI_BT) { pcActive = false; return; }

  if (!pcActive) {
    // The phone started playing again while we are muted: run the cycle once more.
    if (extraCyclesLeft > 0 && timeReached(now, pcCooldownUntil) &&
        btReady() && phoneIsStreaming()) {
      extraCyclesLeft--;
      pcActive = true;
      pcTries  = 0;
      pcNextAt = now + PAUSE_FIRST_DELAY_MS;
    }
    return;
  }

  if (!timeReached(now, pcNextAt)) return;

  if (!btReady() || !phoneIsStreaming()) {          // nothing (left) to pause
    pcActive = false;
    pcCooldownUntil = now + 5000UL;
    return;
  }
  if (pcTries >= PAUSE_MAX_TRIES) {
    Serial.printf("[BT] phone keeps streaming after %u pause commands - staying muted\n", (unsigned)pcTries);
    pcActive = false;
    pcCooldownUntil = now + 5000UL;
    return;
  }

  a2dp_sink.pause();
  pcTries++;
  pcNextAt = now + PAUSE_RETRY_MS;
  Serial.printf("[BT] pause sent (%u/%u)\n", (unsigned)pcTries, (unsigned)PAUSE_MAX_TRIES);
}

// Send the knob volume to the phone (rate limited, last value always goes out).
void phoneVolumeSyncTick(uint32_t now)
{
  if (!pendingPhoneSync || potVolume127 < 0) return;
  if (!timeReached(now, phoneSyncNotBefore)) return;
  if (!a2dp_sink.is_connected()) return;            // stays pending until connected

  a2dp_sink.set_volume((uint8_t)potVolume127);      // our stage ignores it; the phone gets the message
  pendingPhoneSync   = false;
  phoneSyncNotBefore = now + PHONE_SYNC_MIN_MS;
}

void audioPolicyTick(uint32_t now)
{
  pauseCycleTick(now);
  phoneVolumeSyncTick(now);
}

// Encoder: track skip in Bluetooth mode.
void handleEncoderDelta(long delta, uint32_t now)
{
  if (uiMode == UI_BT) {
    if (!btReady()) {
      Serial.printf("[BT] not connected - encoder ignored\n");
    } else if (now - lastSkipMs >= ENC_SKIP_LOCKOUT_MS) {
      lastSkipMs = now;
      if (delta > 0) { a2dp_sink.next();     Serial.printf("[BT] next track\n"); }
      else           { a2dp_sink.previous(); Serial.printf("[BT] previous track\n"); }
    }
  } else if (uiMode == UI_RADIO) {
    Serial.printf("[RADIO] ERROR: tuning not implemented yet\n");
  }
  // OFF (clock mode): encoder does nothing
}

// ============================================================
// CONTROLS: START-UP (called LAST in setup())
// ============================================================

void controlsBegin()
{
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  volPot.begin(PIN_VOLUME);
  balPot.begin(PIN_BALANCE);
  powerSw.begin(PIN_POWER_SW);
  toggleSw.begin(PIN_TOGGLE);

  for (int i = 0; i < 8; i++) encIndex[i] = -1;
  for (int i = 0; i < ENC_N; i++) encIndex[ENC_SEQ[i]] = i;
  for (int i = 0; i < 3; i++) pinMode(ENC_PINS[i], INPUT_PULLUP);
  encPrev = readEncState();
  encLast = encIndex[encPrev];
  for (int i = 0; i < 3; i++)
    attachInterrupt(digitalPinToInterrupt(ENC_PINS[i]), encoderISR, CHANGE);

  Serial.printf("\n=== OLDS controls ===\n");
  Serial.printf("[POWER]  %s\n",  powerSw.active()  ? "ON (closed)" : "OFF (open)");
  Serial.printf("[TOGGLE] %s\n",  toggleSw.active() ? "ON (closed)" : "OFF (open)");
#if POT_CAL
  Serial.printf("POT_CAL mode: turn each pot fully both ways and note the mV values.\n");
#endif

  updateMode(millis(), true);   // set the starting mode from the switch positions
}

// ============================================================
// CONTROLS: POLLING (called every loop() pass, ~5 ms)
// ============================================================

void controlsUpdate(uint32_t now)
{
#if POT_CAL
  static uint32_t lastCal = 0;
  if (now - lastCal >= 250) {
    lastCal = now;
    Serial.printf("[CAL] volume %4d mV | balance %4d mV\n", volPot.readMv(), balPot.readMv());
  }
#else
  // ---- volume ----
  if (volPot.update()) {
    static int lastPct = -1;
    int lo = VOL_MIN_MV + EDGE_MV, hi = VOL_MAX_MV - EDGE_MV;
    int pct = VOL_INVERT ? map(volPot.accepted, lo, hi, 100, 0)
                         : map(volPot.accepted, lo, hi, 0, 100);
    pct = constrain(pct, 0, 100);
    if (pct != lastPct) {
      lastPct = pct;
      Serial.printf("[VOLUME]  %4d mV | %3d%%\n", volPot.accepted, pct);

      int v = (pct * 127 + 50) / 100;
      if (v > VOL_MAX_STEP) v = VOL_MAX_STEP;
      potVolume127 = v;
      applyOutputTargets();
      pendingPhoneSync = true;                      // phone slider follows the knob
    }
  }

  // ---- balance (-50 = one end, 0 = centre, +50 = other end) ----
  if (balPot.update()) {
    static int lastBal = -1000;
    int mv = balPot.accepted;
    int d = mv - BAL_CENTER_MV;
    int val;
    if (abs(d) <= BAL_DEAD_MV)  val = 0;
    else if (d < 0)             val = map(mv, BAL_MIN_MV + EDGE_MV, BAL_CENTER_MV - BAL_DEAD_MV, -50, 0);
    else                        val = map(mv, BAL_CENTER_MV + BAL_DEAD_MV, BAL_MAX_MV - EDGE_MV, 0, 50);
    val = constrain(val, -50, 50);
    if (val != lastBal) {
      lastBal = val;
      Serial.printf("[BALANCE] %4d mV | %+3d\n", mv, val);
      balanceVal = val;
      applyOutputTargets();
    }
  }
#endif

  // ---- switches (one printf per line = one atomic write) ----
  if (powerSw.update(now))  Serial.printf("%s\n", powerSw.active()  ? "[POWER]   ON (closed to GND)" : "[POWER]   OFF (open)");
  if (toggleSw.update(now)) Serial.printf("%s\n", toggleSw.active() ? "[TOGGLE]  ON (closed to GND)" : "[TOGGLE]  OFF (open)");
  updateMode(now, false);

  // ---- encoder ----
#if ENC_DEBUG
  static uint8_t seen = 255, shown = 255;
  static uint32_t seenAt = 0;
  uint8_t cur = encPrev;
  if (cur != seen) { seen = cur; seenAt = now; }
  else if (cur != shown && (now - seenAt) >= 5) {
    shown = cur;
    Serial.printf("[ENC SETTLED] state=%d  (P%d:%d P%d:%d P%d:%d )\n", cur,
                  ENC_PINS[0], (cur >> 0) & 1,
                  ENC_PINS[1], (cur >> 1) & 1,
                  ENC_PINS[2], (cur >> 2) & 1);
  }
#endif
  static long lastPos = 0;
  long steps;
  portENTER_CRITICAL(&encMux);
  steps = encSteps;
  portEXIT_CRITICAL(&encMux);
  if (ENC_REVERSE) steps = -steps;
  long pos = steps / ENC_STEPS_PER_DETENT;
  if (pos != lastPos) {
    const long delta = pos - lastPos;
    lastPos = pos;
    Serial.printf("[ENCODER] position=%ld\n", pos);
    handleEncoderDelta(delta, now);
  }
}

// ============================================================
// RTC INITIALISATION
// ============================================================

void initRtcAndTime()
{
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(100000);

    if (!rtc.begin()) {
        Serial.println("[RTC] ERROR: RTC not found on I2C bus!");
        rtcFound = false;
        return;
    }
    rtcFound = true;
    Serial.println("[RTC] Hardware RTC initialized.");

    const bool rtcLost = rtc.lostPower();
    uint32_t rtcEpoch = 0;
    const bool rtcOk = readRtcValidated(rtcEpoch);
    const bool buildValid = ((uint32_t)BUILD_EPOCH >= MIN_VALID_EPOCH);
    const uint32_t buildLocal = buildValid
        ? (uint32_t)BUILD_EPOCH + budapestOffsetSec((uint32_t)BUILD_EPOCH) : 0;

    if (rtcLost || !rtcOk || (buildValid && buildLocal > rtcEpoch)) {
        if (buildValid) {
            rtc.adjust(DateTime(buildLocal));
            setTimeBase(buildLocal, "build-time fallback");
        }
    } else if (rtcOk) {
        setTimeBase(rtcEpoch, "RTC");
    }
}

// ============================================================
// SETUP
// ============================================================

void setup()
{
    disableBrownoutDetector();

    Serial.begin(115200);
    delay(1000);

    Serial.println("\n========================================");
    Serial.print("OLDS ESP32 RADIO CLOCK v");
    Serial.println(FIRMWARE_VERSION);
    Serial.print("Build Date: ");
    Serial.println(BUILD_TIMESTAMP);
    Serial.printf("Reset reason code: %d\n", (int)esp_reset_reason());
    Serial.println("========================================");

    pinMode(PIN_STATUS_LED, OUTPUT);
    digitalWrite(PIN_STATUS_LED, LOW);

    timeMutex = xSemaphoreCreateMutex();

    // 1. Safely initialize NVS Flash
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    // 2. Initialize RTC
    initRtcAndTime();

    // 3. Wi-Fi NTP sync - happens before Bluetooth spins up.
    //    Is this the second boot, right after our own restart?
    //    The flag is cleared immediately, so at most ONE restart can happen
    //    per power-up (no restart loop is possible).
    bool secondBootAfterWifi = false;
#if CLEAN_REBOOT_AFTER_WIFI
    secondBootAfterWifi = (wifiDoneMagic == WIFI_DONE_MAGIC);
    wifiDoneMagic = 0;
#endif

    if (secondBootAfterWifi) {
        Serial.println("[BOOT] Clean second boot after Wi-Fi sync: skipping Wi-Fi, starting Bluetooth.");
    } else {
        const bool wifiWasUsed = syncTimeViaWiFi();

#if CLEAN_REBOOT_AFTER_WIFI
        if (wifiWasUsed) {
            if (rtcFound) {
                // The RTC keeps the (possibly NTP-corrected) time through the restart.
                wifiDoneMagic = WIFI_DONE_MAGIC;
                Serial.println("[BOOT] Restarting once so Bluetooth starts on a clean chip...");
                Serial.flush();
                delay(200);
                ESP.restart();
            } else {
                Serial.println("[BOOT] WARNING: no RTC, so the NTP time would be lost by a restart. "
                               "Continuing without restart - Bluetooth may be unreliable.");
            }
        }
#else
        (void)wifiWasUsed;
#endif
    }

    // 4. Initialize TM1637 Display
    display.setBrightness(2);
    display.clear();

    secondaryScreen = false;
    displayTimer = millis();
    ledTimer = millis();
    displayNeedsRedraw = true;

    // 5. Configure Hardware I2S & Bluetooth A2DP
    i2s_config_t i2s_config = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate          = 44100,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = 32,
        .dma_buf_len          = 512,
        .use_apll             = false,
        .tx_desc_auto_clear   = true
    };
    a2dp_sink.set_i2s_config(i2s_config);

    i2s_pin_config_t my_pin_config = {
        .bck_io_num   = PIN_I2S_BCLK,
        .ws_io_num    = PIN_I2S_WSEL,
        .data_out_num = PIN_I2S_DOUT,
        .data_in_num  = I2S_PIN_NO_CHANGE
    };
    a2dp_sink.set_pin_config(my_pin_config);

    a2dp_sink.set_auto_reconnect(true, 1000);
    a2dp_sink.set_on_connection_state_changed(bluetoothConnectionStateChanged);
    a2dp_sink.set_avrc_metadata_callback(avrc_metadata_callback);
    a2dp_sink.set_avrc_metadata_attribute_mask(
        ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST | ESP_AVRC_MD_ATTR_ALBUM
    );

    // Our own volume/balance/mute stage. It starts muted and ramps up only
    // when the controls (started last) say so.
    a2dp_sink.set_volume_control(&oldsVol);
    a2dp_sink.set_on_volumechange(onPhoneVolumeChanged);

    printHeap("before BT start");
    a2dp_sink.start(DEVICE_NAME);

    vTaskDelay(pdMS_TO_TICKS(100));
    configure_car_stereo_identity();
    printHeap("after BT start");

    xTaskCreatePinnedToCore(
        displayTaskCode,
        "DisplayTask",
        8192,
        NULL,
        1,
        &DisplayTaskHandle,
        1
    );

    // 6. Controls (pots, switches, encoder) - started LAST, after the Wi-Fi
    //    phase and Bluetooth, so the v1.5.1 boot sequence is exactly as it was.
    controlsBegin();

    Serial.println("========================================");
    Serial.println("[BT] Ready. Device Name: '" DEVICE_NAME "'");
    Serial.println("========================================");
}

void loop()
{
    // loop() is the ONLY place that talks to the RTC after boot.
    // It also polls the controls (no I2C, no I2S, no display involved).
    checkSerialTimeSync();
    refreshFromRtc();
    const uint32_t nowMs = millis();
    controlsUpdate(nowMs);
    audioPolicyTick(nowMs);
    vTaskDelay(pdMS_TO_TICKS(5));
}