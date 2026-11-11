/* =====================================================================
   ALON  -  LEGO-style desk pet for the Waveshare
            ESP32-S3-Touch-AMOLED-1.75C  (466x466 round AMOLED, CO5300)
   ---------------------------------------------------------------------
   Rebuilt from scratch to fix the two reported problems:
     * swipe / scroll not working
     * touch feels slow / unresponsive

   WHY THE OLD BUILD WAS BROKEN (and how this fixes it)
   -----------------------------------------------------
   1. TOUCH RESET PIN.  The old handover said to pulse the touch reset on
      GPIO2.  On THIS board GPIO2 is the SD-card clock, NOT the touch
      reset.  The real touch reset is GPIO40.  We hand the correct reset
      pin to the SensorLib driver and let IT drive the reset exactly as
      Waveshare's own CST9217 example does.  This is the single biggest
      fix.
   2. SWIPE LATENCY.  Swipes now fire the moment the finger has dragged
      ~60 px, instead of waiting for the finger to lift.  Button taps fire
      on finger-DOWN.  That removes the "laggy" feel.
   3. REDRAW COST.  A full 466x466 QSPI frame is slow.  Each screen draws
      its static parts once, then only repaints the regions that change
      (stat bars, counters, avatar).  No full-screen clears except on a
      screen change.
   4. IRQ-GATED POLLING.  The CST9217 only interrupts on state edges, so
      we poll getPoint() when the IRQ fires, while a finger is tracked
      down, and on a 120 ms safety heartbeat.

   HARDWARE PINS are taken from Waveshare's official HARDWARE_REFERENCE.md
   and pin_config.h for this exact board - not guessed.

   LIBRARIES (all vendored in /libraries so the cloud build is
   self-contained):
     * GFX Library for Arduino  (Arduino_CO5300 / Arduino_ESP32QSPI)
     * SensorLib                (TouchDrvCST92xx, SensorQMI8658)
   Built-in: Wire, Preferences (NVS).

   Everything is offline.  State persists in NVS flash.
   The avatar face is drawn procedurally (no image files needed), so this
   compiles and boots stand-alone.  Drop-in artwork hooks are noted below.
   ===================================================================== */

#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <WiFi.h>
#include <WiFiManager.h>          // tzapu/WiFiManager - phone captive-portal setup
#include <WiFiClientSecure.h>
#include <HTTPUpdate.h>           // OTA: pull app image over WiFi
#include "Arduino_GFX_Library.h"
#include "TouchDrvCSTXXX.hpp"
#include "SensorQMI8658.hpp"
#include "SensorPCF85063.hpp"   // onboard real-time clock
#include "img_alon.h"           // real LEGO avatar images (idle/sleep/agent/celebration)

/* ----------------------- BOARD PIN MAP ------------------------------ */
// Display (CO5300 QSPI)
#define LCD_SDIO0   4
#define LCD_SDIO1   5
#define LCD_SDIO2   6
#define LCD_SDIO3   7
#define LCD_SCLK    38
#define LCD_CS      12
#define LCD_RST     39
#define LCD_W       466
#define LCD_H       466
// Shared I2C
#define IIC_SDA     15
#define IIC_SCL     14
// Touch (CST9217)
#define TP_INT      11
#define TP_RST      40
#define TP_ADDR     0x5A
// IMU (QMI8658)
#define IMU_ADDR    0x6B   // QMI8658_L_SLAVE_ADDRESS (Waveshare's own value); 0x6A is the H addr

/* If taps land in the wrong place, flip these (then rebuild):          */
#define TOUCH_FLIP_X 1
#define TOUCH_FLIP_Y 1
#define TOUCH_SWAP_XY 0

/* ----------------------- DISPLAY OBJECTS ---------------------------- */
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS /*CS*/, LCD_SCLK /*SCK*/, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus, LCD_RST, 0 /*rotation*/, LCD_W, LCD_H, 6, 0, 0, 0);

TouchDrvCST92xx touch;
SensorQMI8658   imu;
bool imuOK = false;
float accX=0, accY=0, accZ=1, gyrX=0, gyrY=0, gyrZ=0;

bool rtcOK = false;

#define BOOT_BTN 0             // SHORT side button = BOOT (GPIO0, active low)
#define RTC_ADDR 0x51
#define PMU_ADDR 0x34          // AXP2101 power chip

int  brightness = 140;         // 20..250, saved in NVS
bool screenOff = false;        // true = display blanked (power save)
bool silent = false;          // mute tap/UI sounds (audio comes in Phase 2)
bool bootPrev = true;          // BOOT button previous level (pulled up = high)

Preferences prefs;

// Battery % read straight from the AXP2101 gauge register (no extra lib).
int batteryPercent(){
  Wire.beginTransmission(PMU_ADDR);
  Wire.write(0xA4);
  if(Wire.endTransmission(true)!=0) return -1;
  if(Wire.requestFrom(PMU_ADDR,1)!=1) return -1;
  int p = Wire.read();
  return (p>=0 && p<=100) ? p : -1;
}
// AXP2101 charging status (reg 0x00 bit5 = battery present; reg 0x01 charge state).
bool batteryCharging(){
  Wire.beginTransmission(PMU_ADDR); Wire.write(0x01);
  if(Wire.endTransmission(true)!=0) return false;
  if(Wire.requestFrom(PMU_ADDR,1)!=1) return false;
  int s=Wire.read();
  return ((s>>5)&0x03)==0x01;   // 01b = charging
}

// Bounded I2C ping: does the IMU ACK its address? Prevents the library init
// from hanging when the chip isn't answering, without touching its registers.
// --- Raw I2C QMI8658 driver (bypasses SensorLib which crashes on begin()) ---
static uint8_t imuAddr = 0;   // detected address

static void qmiWrite(uint8_t reg, uint8_t val){
  Wire.beginTransmission(imuAddr);
  Wire.write(reg); Wire.write(val);
  Wire.endTransmission();
}
static uint8_t qmiRead1(uint8_t reg){
  Wire.beginTransmission(imuAddr);
  Wire.write(reg); Wire.endTransmission(false);
  Wire.requestFrom(imuAddr,(uint8_t)1);
  return Wire.available()?Wire.read():0;
}
static void qmiReadN(uint8_t reg, uint8_t *buf, uint8_t n){
  Wire.beginTransmission(imuAddr);
  Wire.write(reg); Wire.endTransmission(false);
  Wire.requestFrom(imuAddr, n);
  for(int i=0;i<n&&Wire.available();i++) buf[i]=Wire.read();
}

bool imuRead(){
  if(!imuOK) return false;
  uint8_t st = qmiRead1(0x2E);             // STATUSINT register
  if(!(st & 0x01)) return false;            // accel data not ready
  uint8_t raw[6];
  qmiReadN(0x35, raw, 6);                   // AX_L, AX_H, AY_L, AY_H, AZ_L, AZ_H
  int16_t ax = (int16_t)((raw[1]<<8)|raw[0]);
  int16_t ay = (int16_t)((raw[3]<<8)|raw[2]);
  int16_t az = (int16_t)((raw[5]<<8)|raw[4]);
  // 4G range: 1 LSB = 4/32768 g
  accX = ax * (4.0f/32768.0f);
  accY = ay * (4.0f/32768.0f);
  accZ = az * (4.0f/32768.0f);
  return true;
}

// Lazy, one-shot IMU bring-up via raw registers. Called when a motion app opens -
// never at boot, so a missing/slow chip can't hang startup.
bool imuEnsure(){
  static bool tried=false;
  if(tried) return imuOK;
  tried=true;
  // Auto-detect: try 0x6B then 0x6A
  const uint8_t cands[2]={0x6B,0x6A};
  for(int i=0;i<2;i++){
    Wire.beginTransmission(cands[i]);
    if(Wire.endTransmission()==0){ imuAddr=cands[i]; break; }
  }
  if(!imuAddr) return false;
  // Verify WHO_AM_I (register 0x00, expect 0x05 for QMI8658)
  uint8_t wai = qmiRead1(0x00);
  if(wai != 0x05){ imuAddr=0; return false; }
  // Soft reset
  qmiWrite(0x60, 0xB0); delay(20);          // RESET register
  // CTRL1: SPI auto-inc, little-endian
  qmiWrite(0x02, 0x60);
  // CTRL2: Accel enable, 4G range, 1000Hz ODR
  //   range[6:4]=0b010 (4G), ODR[3:0]=0b0011 (1000Hz)
  qmiWrite(0x03, 0x23);
  // CTRL5: no low-pass filter
  qmiWrite(0x06, 0x00);
  // CTRL7: enable accelerometer only (bit 0)
  qmiWrite(0x08, 0x01);
  delay(10);
  imuOK = true;
  return true;
}

// --- Clock: ESP32-S3 INTERNAL RTC. This board has NO external RTC chip (the
//     schematic shows a 32.768kHz crystal wired to the ESP32's XTAL_32K pins and
//     a VBACKUP rail - not a PCF85063). So we keep time with the chip's own clock
//     via time()/settimeofday(), which ticks every second while powered. Later,
//     WiFi/NTP can set it automatically. ---
bool rtcGet(int &hh,int &mm,int &ss){
  time_t now = time(nullptr);
  struct tm t; localtime_r(&now, &t);
  hh = t.tm_hour; mm = t.tm_min; ss = t.tm_sec;
  return true;
}
void rtcSet(int hh,int mm,int ss){
  time_t now = time(nullptr);
  struct tm t; localtime_r(&now, &t);
  t.tm_hour = hh; t.tm_min = mm; t.tm_sec = ss;
  time_t nt = mktime(&t);
  struct timeval tv; tv.tv_sec = nt; tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
}
bool rtcInit(){
  time_t now = time(nullptr);
  struct tm t; localtime_r(&now, &t);
  if(t.tm_year < 120){                 // unset (before year 2020) -> seed a sane time so it ticks now
    struct tm s = {}; s.tm_year = 126; s.tm_mon = 0; s.tm_mday = 1; s.tm_hour = 12; s.tm_min = 0; s.tm_sec = 0;
    time_t nt = mktime(&s);
    struct timeval tv; tv.tv_sec = nt; tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
  }
  return true;                          // internal clock is always available
}

/* ----------------------- COLOR / THEME ------------------------------ */
// The AMOLED is true black: black pixels are off.
#define C_BLACK     0x0000
#define C_WHITE     0xFFFF
#define C_CYAN      0x07FF
#define C_VIOLET    0x8A1F   // HUD violet
#define C_DIMGREY   0x4208
#define C_GREY      0x8410
#define C_RED       0xF9A6
#define C_GREEN     0x37E0
#define C_YELLOW    0xFEE0   // LEGO head yellow
#define C_ORANGE    0xFD20
#define C_TILE      0x0000   // pure black - icons/text glow on true black AMOLED

// Accent themes (owned/equipped in the shop). Index 0 = default cyan/violet.
struct Theme { const char *name; uint16_t accent; uint16_t head; uint32_t cost; };
Theme THEMES[] = {
  {"Classic", C_CYAN,   C_YELLOW, 0   },
  {"Gold",    0xFEA0,   0xFEE0,   40  },
  {"Emerald", 0x2FEC,   0x9FD3,   60  },
  {"Magenta", 0xF81F,   0xFCBF,   80  },
};
const int THEME_COUNT = sizeof(THEMES) / sizeof(THEMES[0]);

/* ----------------------- PET MODEL ---------------------------------- */
struct Pet {
  int   hunger   = 80;   // 0..100 (100 = full)
  int   energy   = 80;
  int   fun      = 80;
  int   clean    = 80;
  uint32_t studs = 25;   // currency
  uint32_t xp    = 0;
  int   level    = 1;
  uint8_t ownedMask = 0x01;  // bit per theme owned; bit0 = Classic
  uint8_t equipped  = 0;     // theme index
  bool  sleeping    = false;
} pet;

int xpForLevel(int lv) { return lv * 100; }   // xp needed to reach next level

// forward declarations (defined further down)
void celebrate();
void addXP(uint32_t amt);
void reward(uint32_t studs, uint32_t xp);

/* ----------------------- SCREENS ------------------------------------ */
enum Screen {
  SCR_HOME, SCR_CARE, SCR_SHOP, SCR_GAMES, SCR_STATS, SCR_AGENT,
  SCR_SNAKE, SCR_PONG, SCR_TETRIS, SCR_BREAKOUT, SCR_FLAPPY, SCR_SETTINGS,
  SCR_APPS, SCR_LEVEL, SCR_HORIZON, SCR_STEPS, SCR_CLOCK,
  SCR_STOPWATCH, SCR_TIMER, SCR_BATTERY, SCR_FLASH, SCR_SYSINFO,
  SCR_WIFI
};
Screen screen = SCR_HOME;
bool    screenDirty = true;   // force a full redraw of the current screen

/* ----------------------- TOUCH ENGINE ------------------------------- */
// The CST9217 interrupts only on state edges, so we also poll while a
// finger is held and on a short heartbeat.
volatile bool tpPending = false;
void IRAM_ATTR onTouchISR() { tpPending = true; }

struct TouchTrack {
  bool  down       = false;   // finger currently on glass
  int   downX, downY;         // where it first touched
  int   curX,  curY;          // latest position
  uint32_t downMs;            // when it went down
  bool  swiped     = false;   // a swipe already fired this gesture
  bool  held       = false;   // a long-press already fired this gesture
  uint32_t lastSeen;          // last ms we saw a point
} g;

enum EvType { EV_NONE, EV_DOWN, EV_TAP, EV_SWIPE_L, EV_SWIPE_R, EV_SWIPE_U, EV_SWIPE_D, EV_HOLD };
struct UiEvent { EvType type = EV_NONE; int x = 0, y = 0; };

// Explicit prototypes for every function that uses UiEvent in its
// signature. This stops the Arduino IDE from auto-inserting its own
// prototypes ABOVE this struct (which caused "'UiEvent' does not name a
// type"). Keep these if you add more handlers.
UiEvent pollGesture();
bool isSwipe(const UiEvent &e);
void textC(const char *s, int cx, int cy, uint8_t size, uint16_t color);
void textL(const char *s, int x, int y, uint8_t size, uint16_t color);
void handleHome (const UiEvent &ev);
void handleCare (const UiEvent &ev);
void handleShop (const UiEvent &ev);
void handleStats(const UiEvent &ev);
void handleAgent(const UiEvent &ev);
void handleGames(const UiEvent &ev);
void handleSnake(const UiEvent &ev);
void handleMole (const UiEvent &ev);
void handlePong    (const UiEvent &ev);
void handleTetris  (const UiEvent &ev);
void handleBreakout(const UiEvent &ev);
void handleFlappy  (const UiEvent &ev);
bool hitBack    (const UiEvent &ev);
bool onAvatar   (const UiEvent &ev);
void handleSettings(const UiEvent &ev);
void handleApps (const UiEvent &ev);
void handleLevel(const UiEvent &ev);
void handleMarble(const UiEvent &ev);
void handleSteps(const UiEvent &ev);
void handleClock(const UiEvent &ev);
void handleWifi (const UiEvent &ev);
void handleStopwatch(const UiEvent &ev);
void handleTimer(const UiEvent &ev);
void handleBattery(const UiEvent &ev);
void handleFlash(const UiEvent &ev);
void handleSysinfo(const UiEvent &ev);
bool appBack(const UiEvent &ev);
void screenWake();

#ifndef TOUCH_DEBUG
#define TOUCH_DEBUG 0       // 1 = show a live touch readout (diagnostic)
#endif

// TOUCH_TEST: boot into a calibration screen instead of the pet. Tap the
// rings and report the x,y it prints. Set back to 0 to run the pet.
#ifndef TOUCH_TEST
#define TOUCH_TEST 0
#endif

#define SWIPE_THRESH 72     // px of drag before a swipe fires
#define TAP_SLOP     24     // movement under this = a tap (not a swipe)
#define HOLD_MS      750    // long-press duration
#define TP_HEARTBEAT 110    // ms safety poll when idle
#define JUMP_MAX     170    // ignore single-sample jumps bigger than this (noise)
#define DOWN_CONFIRM 2      // consecutive present reads to accept a finger
#define UP_CONFIRM   2      // consecutive absent reads to release

uint32_t lastPollMs = 0;
uint32_t inputCooldownUntil = 0;   // ignore input briefly after a screen change
int contactN = 0;                  // consecutive present reads
int absentN  = 0;                  // consecutive absent reads

// Debug: the raw controller report, surfaced on screen so we can tune mapping.
int dbgN = 0, dbgX = 0, dbgY = 0;
const char *dbgEv = "idle";

// Read one point. Returns point count; fills mapped x,y of point 0.
int readTouch(int &x, int &y) {
  int16_t px[2], py[2];
  uint8_t n = touch.getPoint(px, py, 2);
  dbgN = n;
  if (n == 0) return 0;
  int rx = px[0], ry = py[0];
#if TOUCH_SWAP_XY
  { int t = rx; rx = ry; ry = t; }
#endif
#if TOUCH_FLIP_X
  rx = (LCD_W - 1) - rx;
#endif
#if TOUCH_FLIP_Y
  ry = (LCD_H - 1) - ry;
#endif
  if (rx < 0) rx = 0; if (rx > LCD_W - 1) rx = LCD_W - 1;
  if (ry < 0) ry = 0; if (ry > LCD_H - 1) ry = LCD_H - 1;
  x = rx; y = ry; dbgX = rx; dbgY = ry;
  return n;
}

void resetGesture() { g.down = false; g.swiped = g.held = false; contactN = absentN = 0; }
inline bool isSwipe(const UiEvent &e) {
  return e.type == EV_SWIPE_L || e.type == EV_SWIPE_R ||
         e.type == EV_SWIPE_U || e.type == EV_SWIPE_D;
}

// Debounced gesture engine. A touch must be confirmed over 2 reads before it
// counts (kills phantom points); single-sample coordinate spikes are ignored;
// buttons fire on confirmed DOWN, swipes mid-drag, a clean lift is a TAP.
UiEvent pollGesture() {
  UiEvent ev;
  uint32_t now = millis();
  bool due = tpPending || g.down || (now - lastPollMs >= 40);
  if (!due) return ev;
  tpPending = false;
  lastPollMs = now;

  int x, y;
  bool present = (readTouch(x, y) > 0);
  bool cooling = (now < inputCooldownUntil);

  if (present) {
    if (!g.down) {                        // first contact = down immediately
      g.down = true; g.swiped = g.held = false;
      g.downX = g.curX = x; g.downY = g.curY = y;
      g.downMs = g.lastSeen = now;
      if (!cooling) { ev.type = EV_DOWN; ev.x = x; ev.y = y; dbgEv = "down"; }
      return ev;
    }
    // reject single noisy jumps; otherwise track the finger
    if (abs(x - g.curX) + abs(y - g.curY) <= JUMP_MAX) { g.curX = x; g.curY = y; }
    g.lastSeen = now;
    int dx = g.curX - g.downX, dy = g.curY - g.downY;
    if (!g.swiped && !g.held && !cooling) {
      if (abs(dx) >= SWIPE_THRESH || abs(dy) >= SWIPE_THRESH) {
        g.swiped = true;
        if (abs(dx) > abs(dy)) { ev.type = (dx > 0) ? EV_SWIPE_R : EV_SWIPE_L; dbgEv = (dx > 0) ? "swR" : "swL"; }
        else                   { ev.type = (dy > 0) ? EV_SWIPE_D : EV_SWIPE_U; dbgEv = (dy > 0) ? "swD" : "swU"; }
        return ev;
      }
      if (now - g.downMs >= HOLD_MS) { g.held = true; ev.type = EV_HOLD; ev.x = g.curX; ev.y = g.curY; dbgEv = "hold"; return ev; }
    }
  } else {
    // end the gesture only after the finger has really been gone (tolerates flicker)
    if (g.down && (now - g.lastSeen >= 120)) {
      bool wasTap = (!g.swiped && !g.held &&
                     abs(g.curX - g.downX) < TAP_SLOP &&
                     abs(g.curY - g.downY) < TAP_SLOP &&
                     (now - g.downMs) >= 25);
      g.down = false;
      if (wasTap && !cooling) { ev.type = EV_TAP; ev.x = g.downX; ev.y = g.downY; dbgEv = "tap"; }
    }
  }
  return ev;
}

#if TOUCH_DEBUG
// Tiny live readout at the bottom: raw point count + mapped x,y + last event.
void drawTouchDebug() {
  static uint32_t last = 0; static int lx = -2, ly = -2, ln = -2;
  if (millis() - last < 120) return; last = millis();
  if (dbgX == lx && dbgY == ly && dbgN == ln) return;
  lx = dbgX; ly = dbgY; ln = dbgN;
  gfx->fillRect(30, 294, LCD_W - 60, 24, C_BLACK);
  char b[48]; snprintf(b, sizeof(b), "n%d x%d y%d %s", dbgN, dbgX, dbgY, dbgEv);
  textC(b, LCD_W / 2, 304, 2, C_YELLOW);
}
#endif

void textC(const char *s, int cx, int cy, uint8_t size, uint16_t color);
void textL(const char *s, int x, int y, uint8_t size, uint16_t color);

#if TOUCH_TEST
/* ----------------------- TOUCH CALIBRATION -------------------------- */
// Draws 5 target rings at KNOWN screen positions. The user taps each and
// reports the x,y we print, which reveals the controller's coordinate map.
struct TPt { int x, y; const char *n; };
TPt TTARGETS[5] = { {233,130,"TOP"}, {346,233,"RIGHT"},
                    {233,336,"BOTTOM"}, {120,233,"LEFT"}, {233,233,"CENTER"} };
void drawTouchTestStatic() {
  gfx->fillScreen(C_BLACK);
  textC("TOUCH TEST", LCD_W/2, 54, 2, C_VIOLET);
  textC("tap each ring, read me x,y", LCD_W/2, 80, 1, C_GREY);
  for (int i=0;i<5;i++){ TPt &t=TTARGETS[i];
    gfx->drawCircle(t.x,t.y,16,C_WHITE);
    gfx->fillCircle(t.x,t.y,2,C_RED);
    textC(t.n,t.x,t.y-26,1,C_GREY);
  }
}
void touchTestLoop() {
  int16_t px[2], py[2];
  uint8_t n = touch.getPoint(px, py, 2);
  static uint32_t lastTxt = 0;
  if (n > 0) {
    int rx = px[0], ry = py[0];
    int sx = rx<0?0:(rx>LCD_W-1?LCD_W-1:rx);
    int sy = ry<0?0:(ry>LCD_H-1?LCD_H-1:ry);
    gfx->fillCircle(sx, sy, 4, C_CYAN);          // dot where raw maps on panel
    if (millis()-lastTxt > 70) { lastTxt = millis();
      gfx->fillRect(30, 288, LCD_W-60, 22, C_BLACK);
      char b[48]; snprintf(b,sizeof(b),"n%d  x=%d  y=%d", n, rx, ry);
      textC(b, LCD_W/2, 300, 2, C_YELLOW);
    }
  }
  delay(3);
}
#endif

/* ----------------------- TEXT HELPERS ------------------------------- */
void textC(const char *s, int cx, int cy, uint8_t size, uint16_t color) {
  int w = strlen(s) * 6 * size;
  int h = 8 * size;
  gfx->setTextColor(color);
  gfx->setTextSize(size);
  gfx->setCursor(cx - w / 2, cy - h / 2);
  gfx->print(s);
}
void textL(const char *s, int x, int y, uint8_t size, uint16_t color) {
  gfx->setTextColor(color);
  gfx->setTextSize(size);
  gfx->setCursor(x, y);
  gfx->print(s);
}

/* ----------------------- SOUND (synth beeps) ------------------------
   Audio uses the ES8311 codec behind the TCA9554 expander + I2S. That
   setup is involved; to guarantee a rock-solid first build we ship v1
   silent. Hook your beeps in here once the codec path is wired.        */
void beep(int /*freq*/, int /*ms*/) { if(silent) return; /* speaker output: Phase 2 */ }

/* ----------------------- NVS PERSISTENCE ---------------------------- */
void saveState() {
  prefs.begin("alon", false);
  prefs.putInt("hunger", pet.hunger);
  prefs.putInt("energy", pet.energy);
  prefs.putInt("fun",    pet.fun);
  prefs.putInt("clean",  pet.clean);
  prefs.putULong("studs", pet.studs);
  prefs.putULong("xp",   pet.xp);
  prefs.putInt("level",  pet.level);
  prefs.putUChar("owned", pet.ownedMask);
  prefs.putUChar("equip", pet.equipped);
  prefs.end();
}
void loadState() {
  prefs.begin("alon", true);
  pet.hunger = prefs.getInt("hunger", 80);
  pet.energy = prefs.getInt("energy", 80);
  pet.fun    = prefs.getInt("fun",    80);
  pet.clean  = prefs.getInt("clean",  80);
  pet.studs  = prefs.getULong("studs", 25);
  pet.xp     = prefs.getULong("xp",   0);
  pet.level  = prefs.getInt("level",  1);
  pet.ownedMask = prefs.getUChar("owned", 0x01);
  pet.equipped  = prefs.getUChar("equip", 0);
  brightness    = prefs.getInt("bri", 140);
  silent        = prefs.getBool("silent", false);
  prefs.end();
}

/* ----------------------- GAME HELPERS ------------------------------- */
int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void addXP(uint32_t amt) {
  pet.xp += amt;
  while ((int)pet.xp >= xpForLevel(pet.level)) {
    pet.xp -= xpForLevel(pet.level);
    pet.level++;
    pet.studs += 20;              // level-up bonus
    celebrate();
  }
}
void reward(uint32_t studs, uint32_t xp) { pet.studs += studs; addXP(xp); }

/* ----------------------- AVATAR EXPRESSIONS ------------------------- */
// Expression indices: 0=neutral, 1=happy, 2=sleepy, 3=sad
enum Expression : uint8_t { EXPR_NEUTRAL=0, EXPR_HAPPY=1, EXPR_SLEEPY=2, EXPR_SAD=3 };
const uint16_t* EXPR_IMGS[4] = { img_neutral, img_happy, img_sleepy, img_sad };

Expression  curExpr     = EXPR_NEUTRAL;    // current displayed expression
Expression  baseExpr    = EXPR_NEUTRAL;    // expression when not doing a blink/reaction
uint32_t    exprOverrideEnd = 0;           // millis when a temporary expression reverts
bool        blinkActive = false;
uint32_t    nextBlink   = 0;               // when next blink starts

// Draw just the avatar into its zone (no full-screen blit)
void blitAvatar(Expression e){
  gfx->draw16bitRGBBitmap(ALON_X, ALON_Y, EXPR_IMGS[e], ALON_IMG_W, ALON_IMG_H);
  curExpr = e;
}

// Pick the base expression from context (time, battery, charging)
Expression autoExpression(){
  int bat = batteryPercent();
  if(bat >= 0 && bat < 10) return EXPR_SLEEPY;     // dying
  if(bat >= 0 && bat < 20) return EXPR_SAD;         // low battery
  if(batteryCharging())    return EXPR_HAPPY;        // charging = happy

  // Time-of-day mood
  int hh = -1, mm = 0, ss = 0;
  rtcGet(hh, mm, ss);
  if(hh >= 0){
    if(hh >= 22 || hh < 7) return EXPR_SLEEPY;      // nighttime
    if(hh >= 7 && hh < 10) return EXPR_HAPPY;        // morning energy
  }
  return EXPR_NEUTRAL;
}

// Trigger a temporary expression (e.g. happy on tap, reverts after duration)
void triggerExpression(Expression e, uint32_t durationMs){
  exprOverrideEnd = millis() + durationMs;
  blitAvatar(e);
}

/* celebration splash on level up */
void celebrate() {
  gfx->fillScreen(C_BLACK);
  blitAvatar(EXPR_HAPPY);
  char buf[24]; snprintf(buf, sizeof(buf), "LEVEL %d!", pet.level);
  textC(buf, LCD_W / 2, 38, 3, THEMES[pet.equipped].accent);
  textC("+20 studs", LCD_W / 2, 432, 2, C_WHITE);
  delay(1400);
  screenDirty = true;
}

/* ----------------------- STAT BAR WIDGET ---------------------------- */
int lastBar[4] = {-1,-1,-1,-1};
void drawStatBar(int idx, const char *label, int val, int x, int y, int w) {
  val = clampi(val, 0, 100);
  if (lastBar[idx] == val && !screenDirty) return;
  lastBar[idx] = val;
  int h = 16;
  gfx->fillRoundRect(x, y, w, h, 6, C_DIMGREY);
  uint16_t col = val > 50 ? C_GREEN : (val > 25 ? C_YELLOW : C_RED);
  int fw = (w - 4) * val / 100;
  gfx->fillRoundRect(x + 2, y + 2, fw < 4 ? 4 : fw, h - 4, 5, col);
  gfx->setTextColor(C_WHITE); gfx->setTextSize(1);
  gfx->setCursor(x, y - 11); gfx->print(label);
}

/* ===================================================================
   PET REACTION ANIMATION  (sparkles + hearts over avatar zone)
   =================================================================== */
bool     petAnimOn = false;
uint32_t petAnimStart = 0;
#define PET_MS 780
struct Spark { float x,y,vx,vy; uint16_t col; };
Spark sparks[18];
uint32_t lastPetFrame=0;
void drawHeartAt(int cx,int cy,uint16_t col){
  gfx->fillCircle(cx-5,cy,5,col); gfx->fillCircle(cx+5,cy,5,col);
  gfx->fillTriangle(cx-10,cy+1,cx+10,cy+1,cx,cy+13,col);
}
void triggerPet(){
  petAnimOn = true; petAnimStart = millis(); lastPetFrame = 0;
  int acx = ALON_X + ALON_IMG_W/2, acy = ALON_Y + ALON_IMG_H/2;
  for(int i=0;i<18;i++){
    float a=(i/18.0f)*6.2832f, sp=2.2f+(esp_random()%120)/40.0f;
    sparks[i].x=acx; sparks[i].y=acy;
    sparks[i].vx=cosf(a)*sp; sparks[i].vy=sinf(a)*sp-2.0f;
    sparks[i].col=(i%3==0)?C_YELLOW:(i%3==1)?C_CYAN:0xF81F;
  }
}
void animatePetReaction(){
  if(!petAnimOn) return;
  uint32_t now=millis(); int age=now-petAnimStart;
  if(age>=PET_MS){ petAnimOn=false; blitAvatar(curExpr); return; }
  if(now-lastPetFrame<40) return; lastPetFrame=now;
  blitAvatar(EXPR_HAPPY);     // re-blit clears last frame's particles
  for(int i=0;i<18;i++){
    sparks[i].x+=sparks[i].vx; sparks[i].y+=sparks[i].vy; sparks[i].vy+=0.18f;
    int sx=(int)sparks[i].x, sy=(int)sparks[i].y;
    if(sx>3&&sx<463&&sy>3&&sy<463) gfx->fillCircle(sx,sy,3,sparks[i].col);
  }
  int acx = ALON_X + ALON_IMG_W/2;
  int hy = (ALON_Y + ALON_IMG_H/2) - age*70/PET_MS;
  drawHeartAt(acx-28,hy,0xF81F); drawHeartAt(acx+29,hy-14,C_RED);
}

/* ===================================================================
   ZONE-BASED HOME SCREEN
   Layout from Muse's zone map (466x466 round AMOLED):
     CLOCK   — top centre:  digits (133-333, 26-78), date (143-323, 84-112)
     WEATHER — right:       icon (352-438, 150-236), temp centre (395,262)
     AVATAR  — centre:      figure (175-291, 134-340)
     BATTERY — bottom arc:  radius 208, thickness 10, 140°-220° (0°=12 o'clock CW)
               percent text centre (233, 402)
   =================================================================== */

// ---- Weather placeholder (updated via WiFi later) ----
int   weatherTemp    = -99;           // Celsius; -99 = unknown
int   weatherCode    = -1;            // 0=sun, 1=cloud, 2=rain, 3=snow, etc.
char  weatherCond[16] = "";           // e.g. "Cloudy"

// ---- Draw weather icon (simple glyphs) ----
void drawWeatherIcon(int cx, int cy){
  if(weatherCode <= 0){
    // Sun
    gfx->fillCircle(cx, cy, 18, C_YELLOW);
    for(int i=0;i<8;i++){
      float a = i * 0.7854f;
      int x1 = cx + (int)(cosf(a)*24), y1 = cy + (int)(sinf(a)*24);
      int x2 = cx + (int)(cosf(a)*32), y2 = cy + (int)(sinf(a)*32);
      gfx->drawLine(x1,y1,x2,y2,C_YELLOW);
    }
  } else if(weatherCode == 1){
    // Cloud
    gfx->fillCircle(cx-10, cy, 14, C_GREY);
    gfx->fillCircle(cx+10, cy-4, 16, C_GREY);
    gfx->fillCircle(cx+6,  cy+8, 12, C_GREY);
  } else if(weatherCode == 2){
    // Rain cloud + drops
    gfx->fillCircle(cx-8, cy-8, 12, C_GREY);
    gfx->fillCircle(cx+8, cy-10, 14, C_GREY);
    gfx->fillRect(cx-14, cy-2, 30, 10, C_GREY);
    for(int i=0;i<3;i++){
      int dx = cx - 10 + i*10, dy = cy + 12 + (i%2)*6;
      gfx->fillRect(dx, dy, 2, 8, C_CYAN);
    }
  } else {
    // Snow
    gfx->fillCircle(cx-8, cy-8, 12, C_GREY);
    gfx->fillCircle(cx+8, cy-10, 14, C_GREY);
    for(int i=0;i<4;i++){
      int dx = cx - 12 + i*8, dy = cy + 10 + (i%2)*8;
      gfx->fillCircle(dx, dy, 2, C_WHITE);
    }
  }
}

// ---- Draw battery arc ----
// Arc parameters: centre (233,233), radius 208, thickness 10
// Span: 140° to 220° (0°=12 o'clock, clockwise)
// Fill direction: 220° → 140° (left to right)
void drawBatteryArc(int pct){
  if(pct < 0) pct = 0;
  if(pct > 100) pct = 100;
  uint16_t fg = pct < 20 ? C_RED : (pct < 50 ? C_ORANGE : C_GREEN);
  // Background arc (grey)
  gfx->fillArc(233, 233, 213, 203, 140, 220, C_DIMGREY);
  // Filled arc: from 220° backwards (left to right visually)
  if(pct > 0){
    float fillDeg = 80.0f * pct / 100.0f;   // 80° total span
    float endAngle = 220.0f;
    float startAngle = endAngle - fillDeg;
    gfx->fillArc(233, 233, 213, 203, (int)startAngle, (int)endAngle, fg);
  }
}

// ---- Home screen static draw (called once on screen entry) ----
void drawHomeStatic() {
  gfx->fillScreen(C_BLACK);

  // Set base expression from context
  baseExpr = autoExpression();
  if(pet.sleeping) baseExpr = EXPR_SLEEPY;
  blitAvatar(baseExpr);

  // Init blink timer
  nextBlink = millis() + 3000 + (esp_random() % 5000);  // 3-8 sec

  // Force all zones to draw on first frame
  // (drawHomeDynamic will handle clock, weather, battery)
}

// Cached values to avoid redrawing unchanged zones
static int  hm_lastHH = -2, hm_lastMM = -2, hm_lastBat = -2;
static int  hm_lastTemp = -999;
static bool hm_lastCharging = false;

void drawHomeDynamic() {
  if(petAnimOn) return;    // sparkle animation is running
  uint32_t now = millis();

  // ---- CLOCK ZONE (top centre) ----
  static uint32_t lastClkMs = 0;
  int hh = -1, mm = 0, ss = 0;
  if(now - lastClkMs > 500){ lastClkMs = now; rtcGet(hh, mm, ss); }
  else { rtcGet(hh, mm, ss); }

  if(hh != hm_lastHH || mm != hm_lastMM){
    hm_lastHH = hh; hm_lastMM = mm;
    // Clear clock area
    gfx->fillRect(133, 26, 200, 90, C_BLACK);
    // Time digits (large, ~44px = size 5-ish, but we use size 4 for clean fit)
    char tbuf[8];
    if(hh >= 0) snprintf(tbuf, sizeof(tbuf), "%02d:%02d", hh, mm);
    else        snprintf(tbuf, sizeof(tbuf), "--:--");
    textC(tbuf, 233, 56, 4, C_WHITE);

    // Date line
    time_t tn = time(nullptr);
    struct tm tmm; localtime_r(&tn, &tmm);
    const char *days[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    const char *months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                            "Jul","Aug","Sep","Oct","Nov","Dec"};
    char dbuf[20];
    if(tmm.tm_year > 120)
      snprintf(dbuf, sizeof(dbuf), "%s %d %s", days[tmm.tm_wday], tmm.tm_mday, months[tmm.tm_mon]);
    else
      snprintf(dbuf, sizeof(dbuf), "---");
    textC(dbuf, 233, 102, 2, C_GREY);
  }

  // ---- BATTERY ZONE (bottom arc + percentage) ----
  static uint32_t lastBatMs = 0;
  int bat = -1;
  bool charging = false;
  if(now - lastBatMs > 5000 || hm_lastBat == -2){
    lastBatMs = now;
    bat = batteryPercent();
    charging = batteryCharging();
  } else {
    bat = hm_lastBat; charging = hm_lastCharging;
  }

  if(bat != hm_lastBat || charging != hm_lastCharging){
    hm_lastBat = bat; hm_lastCharging = charging;
    drawBatteryArc(bat);
    // Percentage text
    gfx->fillRect(183, 386, 100, 36, C_BLACK);
    char bbuf[8];
    if(bat >= 0) snprintf(bbuf, sizeof(bbuf), "%d%%", bat);
    else         snprintf(bbuf, sizeof(bbuf), "--%%");
    uint16_t bcol = charging ? C_GREEN : (bat < 20 ? C_RED : C_CYAN);
    textC(bbuf, 233, 406, 3, bcol);
    // Charging indicator
    if(charging){
      textC("charging", 233, 378, 1, C_GREEN);
    }
  }

  // ---- WEATHER ZONE (right side) ----
  if(weatherTemp != hm_lastTemp){
    hm_lastTemp = weatherTemp;
    // Clear weather area
    gfx->fillRect(345, 150, 100, 160, C_BLACK);
    drawWeatherIcon(395, 193);
    // Temperature
    if(weatherTemp > -99){
      char wbuf[8]; snprintf(wbuf, sizeof(wbuf), "%d°", weatherTemp);
      textC(wbuf, 395, 266, 3, C_WHITE);
      if(weatherCond[0]) textC(weatherCond, 395, 298, 1, C_GREY);
    }
  }

  // ---- BLINK ANIMATION ----
  if(now >= nextBlink && !blinkActive && !petAnimOn){
    // Start blink: show sleepy face briefly
    blinkActive = true;
    blitAvatar(EXPR_SLEEPY);
    nextBlink = now + 150;   // blink lasts 150ms
  }
  if(blinkActive && now >= nextBlink){
    // Blink done: restore expression
    blinkActive = false;
    Expression show = (exprOverrideEnd > now) ? curExpr : baseExpr;
    if(show == EXPR_SLEEPY){
      // Already sleepy, just keep it
    } else {
      blitAvatar(show);
    }
    nextBlink = now + 3000 + (esp_random() % 5000);  // next blink in 3-8s
  }

  // ---- EXPRESSION OVERRIDE TIMEOUT ----
  if(exprOverrideEnd > 0 && now >= exprOverrideEnd){
    exprOverrideEnd = 0;
    baseExpr = autoExpression();
    if(!blinkActive) blitAvatar(baseExpr);
  }

  // ---- UPDATE BASE EXPRESSION (check periodically) ----
  static uint32_t lastExprCheck = 0;
  if(now - lastExprCheck > 30000){   // every 30s
    lastExprCheck = now;
    Expression newBase = autoExpression();
    if(pet.sleeping) newBase = EXPR_SLEEPY;
    if(newBase != baseExpr && exprOverrideEnd == 0 && !blinkActive){
      baseExpr = newBase;
      blitAvatar(baseExpr);
    }
  }
}

void handleHome(const UiEvent &ev) {
  if(ev.type == EV_NONE) return;

  // ---- TAP ZONES ----
  if(ev.type == EV_TAP){
    int tx = ev.x, ty = ev.y;

    // Clock zone: circle centre (233, 64), radius 66
    int cdx = tx - 233, cdy = ty - 64;
    if(cdx*cdx + cdy*cdy < 66*66){
      screen = SCR_CLOCK; screenDirty = true; return;
    }

    // Weather zone: rect x 326-466, y 126-306
    if(tx >= 326 && tx <= 466 && ty >= 126 && ty <= 306){
      // TODO: open weather detail screen
      return;
    }

    // Battery zone: rect x 143-323, y 336-466
    if(tx >= 143 && tx <= 323 && ty >= 336 && ty <= 466){
      screen = SCR_BATTERY; screenDirty = true; return;
    }

    // Avatar zone: tap = pet reaction
    if(tx >= ALON_X && tx <= ALON_X + ALON_IMG_W &&
       ty >= ALON_Y && ty <= ALON_Y + ALON_IMG_H){
      pet.fun = clampi(pet.fun + 6, 0, 100);
      reward(1, 3);
      triggerExpression(EXPR_HAPPY, 2000);
      triggerPet(); saveState();
      return;
    }
  }

  // Hold on avatar = agent mode
  if(ev.type == EV_HOLD){
    if(ev.x >= ALON_X && ev.x <= ALON_X + ALON_IMG_W &&
       ev.y >= ALON_Y && ev.y <= ALON_Y + ALON_IMG_H){
      screen = SCR_AGENT; screenDirty = true; return;
    }
  }

  // Swipes
  if(ev.type == EV_SWIPE_U || ev.type == EV_SWIPE_L || ev.type == EV_SWIPE_R){
    screen = SCR_APPS; screenDirty = true;
  }
  else if(ev.type == EV_SWIPE_D){
    screen = SCR_SETTINGS; screenDirty = true;
  }
}

/* ------- shared BACK/HOME button for every sub-screen ------- */
#define BK_X 158
#define BK_Y 346
#define BK_W 150
#define BK_H 38
void drawBackBtn(){
  uint16_t ac=THEMES[pet.equipped].accent;
  gfx->fillRoundRect(BK_X,BK_Y,BK_W,BK_H,12,C_TILE);          // dark pill, no border
  textC("HOME", BK_X+BK_W/2, BK_Y+BK_H/2, 2, ac);
}
bool hitBack(const UiEvent &ev){
  return ev.type==EV_DOWN && ev.x>=BK_X && ev.x<=BK_X+BK_W &&
         ev.y>=BK_Y && ev.y<=BK_Y+BK_H;
}

/* ===================================================================
   CARE SCREEN  (feed / clean / medicine)
   =================================================================== */
struct Btn { int x, y, w, h; const char *label; };
Btn careBtns[3] = {
  {133, 150, 200, 54, "FEED  (-5)"},
  {133, 218, 200, 54, "CLEAN (-3)"},
  {133, 286, 200, 54, "TREAT (-8)"},
};
void drawCare() {
  gfx->fillScreen(C_BLACK);
  textC("CARE", LCD_W/2, 70, 3, THEMES[pet.equipped].accent);
  for (int i=0;i<3;i++){ Btn&b=careBtns[i];
    uint16_t ac = (i%2==0) ? C_CYAN : C_VIOLET;
    gfx->fillRoundRect(b.x,b.y,b.w,b.h,14,C_TILE);
    textC(b.label,b.x+b.w/2,b.y+b.h/2,2,ac);
  }
  char buf[24]; snprintf(buf,sizeof(buf),"%lu studs",(unsigned long)pet.studs);
  textC(buf, LCD_W/2, 120, 2, C_CYAN);
  drawBackBtn();
}
void handleCare(const UiEvent &ev){
  if (isSwipe(ev) || hitBack(ev)){ screen=SCR_HOME; screenDirty=true; return; }
  if (ev.type!=EV_DOWN) return;
  for (int i=0;i<3;i++){ Btn&b=careBtns[i];
    if (ev.x>=b.x&&ev.x<=b.x+b.w&&ev.y>=b.y&&ev.y<=b.y+b.h){
      if (i==0 && pet.studs>=5){ pet.studs-=5; pet.hunger=clampi(pet.hunger+30,0,100); reward(0,6); }
      else if (i==1 && pet.studs>=3){ pet.studs-=3; pet.clean=clampi(pet.clean+35,0,100); reward(0,5); }
      else if (i==2 && pet.studs>=8){ pet.studs-=8; pet.fun=clampi(pet.fun+25,0,100); pet.energy=clampi(pet.energy+10,0,100); reward(0,8); }
      saveState(); screenDirty=true; return;
    }
  }
}

/* ===================================================================
   SHOP SCREEN  (buy / equip themes)
   =================================================================== */
void drawShop(){
  gfx->fillScreen(C_BLACK);
  textC("SHOP", LCD_W/2, 54, 3, THEMES[pet.equipped].accent);
  char buf[24]; snprintf(buf,sizeof(buf),"%lu studs",(unsigned long)pet.studs);
  textC(buf, LCD_W/2, 86, 2, C_CYAN);
  int y=120;
  for (int i=0;i<THEME_COUNT;i++){
    bool owned = pet.ownedMask & (1<<i);
    bool equip = pet.equipped==i;
    gfx->fillRoundRect(100,y,266,46,12,C_TILE);
    gfx->fillCircle(128,y+23,14,THEMES[i].accent);
    textL(THEMES[i].name,152,y+10,2,C_WHITE);
    char tag[16];
    if (equip) snprintf(tag,sizeof(tag),"EQUIPPED");
    else if (owned) snprintf(tag,sizeof(tag),"tap:use");
    else snprintf(tag,sizeof(tag),"%lu",(unsigned long)THEMES[i].cost);
    textL(tag,250,y+26,1,equip?C_GREEN:C_CYAN);
    y+=58;
  }
  drawBackBtn();
}
void handleShop(const UiEvent &ev){
  if (isSwipe(ev) || hitBack(ev)){ screen=SCR_HOME; screenDirty=true; return; }
  if (ev.type!=EV_DOWN) return;
  int y=120;
  for (int i=0;i<THEME_COUNT;i++){
    if (ev.x>=100&&ev.x<=366&&ev.y>=y&&ev.y<=y+46){
      bool owned = pet.ownedMask & (1<<i);
      if (owned){ pet.equipped=i; }
      else if (pet.studs>=THEMES[i].cost){ pet.studs-=THEMES[i].cost; pet.ownedMask|=(1<<i); pet.equipped=i; }
      saveState(); screenDirty=true; return;
    }
    y+=58;
  }
}

/* ===================================================================
   STATS SCREEN  (all 4 needs)
   =================================================================== */
void drawStats(){
  gfx->fillScreen(C_BLACK);
  textC("STATS", LCD_W/2, 60, 3, THEMES[pet.equipped].accent);
  for (int i=0;i<4;i++) lastBar[i]=-1;
  drawStatBar(0,"HUNGER",pet.hunger,110,140,246);
  drawStatBar(1,"ENERGY",pet.energy,110,190,246);
  drawStatBar(2,"FUN",   pet.fun,   110,240,246);
  drawStatBar(3,"CLEAN", pet.clean, 110,290,246);
  char buf[40]; snprintf(buf,sizeof(buf),"Lv %d   xp %lu/%d",pet.level,(unsigned long)pet.xp,xpForLevel(pet.level));
  textC(buf,LCD_W/2,322,2,C_CYAN);
  drawBackBtn();
}
void handleStats(const UiEvent &ev){
  if (isSwipe(ev) || hitBack(ev)){ screen=SCR_HOME; screenDirty=true; }
}

/* ===================================================================
   AGENT SCREEN  (voice-assistant state - animated aura + level bar)
   Mic capture (ES7210) is Phase 2; the bar animates a placeholder.
   =================================================================== */
uint32_t agentAnim=0;
void drawAgentStatic(){
  gfx->fillScreen(C_BLACK);
  blitAvatar(EXPR_NEUTRAL);
  // Aura rings around the avatar
  Theme &th=THEMES[pet.equipped];
  int acx = ALON_X + ALON_IMG_W/2, acy = ALON_Y + ALON_IMG_H/2;
  for(int i=0;i<3;i++) gfx->drawCircle(acx, acy, 80+i*12, th.accent);
  textC("AGENT  -  listening...", LCD_W/2, 13, 2, C_CYAN);
  drawBackBtn();
}
void drawAgentDynamic(){
  uint32_t now=millis();
  if (now-agentAnim<80) return; agentAnim=now;
  Theme &th=THEMES[pet.equipped];
  int lvl = 20 + (esp_random()%60);           // simulated mic level (real mic = WiFi phase)
  gfx->fillRect(103,408,260,16,C_BLACK);
  gfx->fillRoundRect(103,408,260*lvl/100,16,5,th.accent);
  gfx->drawRoundRect(103,408,260,16,5,C_DIMGREY);
}
void handleAgent(const UiEvent &ev){
  if (isSwipe(ev) || hitBack(ev)){ screen=SCR_HOME; screenDirty=true; }
}

/* ===================================================================
   SETTINGS SCREEN  (brightness / clock / reset) - all offline
   =================================================================== */
void saveBright(){ prefs.begin("alon",false); prefs.putInt("bri",brightness); prefs.end(); }
void saveSilent(){ prefs.begin("alon",false); prefs.putBool("silent",silent); prefs.end(); }

void bumpTime(int dh,int dm){
  if(!rtcOK) return;
  int hh=0,mm=0,ss=0; rtcGet(hh,mm,ss);
  hh=(hh+dh+24)%24; mm=(mm+dm+60)%60;
  rtcSet(hh,mm,0);
}

void resetPet(){
  uint8_t owned=pet.ownedMask, eq=pet.equipped;   // keep purchased themes
  pet = Pet();
  pet.ownedMask=owned; pet.equipped=eq;
  saveState();
}

void drawSettings(){
  gfx->fillScreen(C_BLACK);
  textC("SETTINGS", 233, 44, 3, THEMES[pet.equipped].accent);
  // Brightness
  textC("Brightness", 233, 88, 2, C_WHITE);
  gfx->fillRoundRect(120,106,44,40,10,C_TILE); textC("-",142,126,3,C_CYAN);
  char b[8]; snprintf(b,sizeof(b),"%d",brightness); textC(b,233,126,2,C_CYAN);
  gfx->fillRoundRect(302,106,44,40,10,C_TILE); textC("+",324,126,3,C_CYAN);
  // Clock
  int hh=0,mm=0,ss=0; if(rtcOK){ rtcGet(hh,mm,ss); }
  textC(rtcOK?"Clock":"Clock (no RTC)", 233, 168, 2, C_WHITE);
  gfx->fillRoundRect(96,186,34,36,8,C_TILE);  textC("-",113,204,2,C_VIOLET);
  char hb[6]; snprintf(hb,sizeof(hb),"%02d",hh);  textC(hb,160,204,2,C_CYAN);
  gfx->fillRoundRect(178,186,34,36,8,C_TILE); textC("+",195,204,2,C_VIOLET);
  textC(":",233,204,2,C_WHITE);
  gfx->fillRoundRect(254,186,34,36,8,C_TILE); textC("-",271,204,2,C_VIOLET);
  char mb[6]; snprintf(mb,sizeof(mb),"%02d",mm);  textC(mb,318,204,2,C_CYAN);
  gfx->fillRoundRect(336,186,34,36,8,C_TILE); textC("+",353,204,2,C_VIOLET);
  // Sleep + Sound + Screen Off toggles, then Reset
  gfx->fillRoundRect(96,240,120,40,12,C_TILE);  textC(pet.sleeping?"WAKE":"SLEEP",156,260,2,C_CYAN);
  gfx->fillRoundRect(250,240,120,40,12,C_TILE); textC(silent?"SILENT":"SOUND",310,260,2,silent?C_RED:C_CYAN);
  gfx->fillRoundRect(143,292,180,38,12,C_TILE); textC("RESET",233,311,2,C_RED);
  drawBackBtn();
}
void handleSettings(const UiEvent &ev){
  if(isSwipe(ev)||hitBack(ev)){ screen=SCR_HOME; screenDirty=true; return; }
  if(ev.type!=EV_DOWN) return;
  int x=ev.x,y=ev.y;
  if(y>=106&&y<=146){
    if(x>=120&&x<=164){ brightness=clampi(brightness-20,20,250); gfx->setBrightness(brightness); saveBright(); screenDirty=true; return; }
    if(x>=302&&x<=346){ brightness=clampi(brightness+20,20,250); gfx->setBrightness(brightness); saveBright(); screenDirty=true; return; }
  }
  if(y>=186&&y<=222){
    if(x>=96 &&x<=130){ bumpTime(-1,0); screenDirty=true; return; }
    if(x>=178&&x<=212){ bumpTime(+1,0); screenDirty=true; return; }
    if(x>=254&&x<=288){ bumpTime(0,-1); screenDirty=true; return; }
    if(x>=336&&x<=370){ bumpTime(0,+1); screenDirty=true; return; }
  }
  if(y>=240&&y<=280){
    if(x>=96 &&x<=216){ pet.sleeping=!pet.sleeping; gfx->setBrightness(pet.sleeping?30:brightness); saveState(); screenDirty=true; return; }
    if(x>=250&&x<=370){ silent=!silent; saveSilent(); screenDirty=true; return; }
  }
  if(x>=143&&x<=323&&y>=292&&y<=330){ resetPet(); screenDirty=true; return; }
}

/* ===================================================================
   GAMES MENU
   =================================================================== */
const int GAME_COUNT = 5;
struct GameDef { const char *name; Screen scr; };
GameDef GAME_LIST[GAME_COUNT]={
  {"SNAKE",SCR_SNAKE}, {"PONG",SCR_PONG}, {"TETRIS",SCR_TETRIS},
  {"BREAKOUT",SCR_BREAKOUT}, {"FLAPPY",SCR_FLAPPY},
};
void drawGames(){
  gfx->fillScreen(C_BLACK);
  textC("GAMES", LCD_W/2, 50, 3, THEMES[pet.equipped].accent);
  for (int i=0;i<GAME_COUNT;i++){
    int y = 90 + i*52;
    uint16_t ac = (i%2==0) ? C_CYAN : C_VIOLET;
    gfx->fillRoundRect(133,y,200,44,14,C_TILE);
    textC(GAME_LIST[i].name,233,y+22,2,ac);
  }
  textC("win studs!", LCD_W/2, 358, 1, C_GREY);
  drawBackBtn();
}
void handleGames(const UiEvent &ev){
  if (isSwipe(ev) || hitBack(ev)){ screen=SCR_HOME; screenDirty=true; return; }
  if (ev.type!=EV_DOWN) return;
  for (int i=0;i<GAME_COUNT;i++){
    int y = 90 + i*52;
    if(ev.x>=133&&ev.x<=333&&ev.y>=y&&ev.y<=y+44){
      screen=GAME_LIST[i].scr; screenDirty=true; return;
    }
  }
}

/* ------------------------- SNAKE ------------------------------------ */
#define SN_CELL 22
#define SN_COLS 12
#define SN_ROWS 12
#define SN_OX   101          // origin so the 264x264 board sits centered
#define SN_OY   101
struct GPt{int x,y;};
GPt snake[SN_COLS*SN_ROWS]; int snLen; int snDir; GPt food; bool snDead; uint32_t snTick;
void snPlaceFood(){ food.x=esp_random()%SN_COLS; food.y=esp_random()%SN_ROWS; }
void snReset(){ snLen=3; snake[0]={6,6};snake[1]={5,6};snake[2]={4,6}; snDir=1; snDead=false; snPlaceFood(); snTick=millis(); }
void snDrawCell(int cx,int cy,uint16_t col){ gfx->fillRoundRect(SN_OX+cx*SN_CELL+1,SN_OY+cy*SN_CELL+1,SN_CELL-2,SN_CELL-2,4,col); }
void drawSnakeStatic(){
  gfx->fillScreen(C_BLACK);
  gfx->drawRect(SN_OX-2,SN_OY-2,SN_COLS*SN_CELL+4,SN_ROWS*SN_CELL+4,C_DIMGREY);
  snReset();
}
void stepSnake(){
  if (snDead) return;
  uint32_t now=millis(); if (now-snTick<220) return; snTick=now;
  GPt nh=snake[0];
  if (snDir==0)nh.y--; else if(snDir==1)nh.x++; else if(snDir==2)nh.y++; else nh.x--;
  if (nh.x<0||nh.x>=SN_COLS||nh.y<0||nh.y>=SN_ROWS){ snDead=true; }
  for (int i=0;i<snLen&&!snDead;i++) if(snake[i].x==nh.x&&snake[i].y==nh.y) snDead=true;
  if (snDead){
    reward(5,10); saveState();
    textC("GAME OVER +5", LCD_W/2, 40, 2, C_RED);
    textC("swipe down: exit", LCD_W/2, LCD_H-28, 1, C_GREY);
    return;
  }
  bool grew=(nh.x==food.x&&nh.y==food.y);
  // erase tail
  if (!grew){ snDrawCell(snake[snLen-1].x,snake[snLen-1].y,C_BLACK); }
  else { snLen++; reward(1,2); snPlaceFood(); }
  for (int i=snLen-1;i>0;i--) snake[i]=snake[i-1];
  snake[0]=nh;
  snDrawCell(nh.x,nh.y,THEMES[pet.equipped].accent);
  snDrawCell(food.x,food.y,C_RED);
  // score
  gfx->fillRect(0,8,LCD_W,18,C_BLACK);
  char b[20]; snprintf(b,sizeof(b),"len %d",snLen); textC(b,LCD_W/2,16,2,C_CYAN);
}
void handleSnake(const UiEvent &ev){
  if (ev.type==EV_HOLD){ screen=SCR_GAMES; screenDirty=true; return; }   // hold = quit
  if (ev.type==EV_SWIPE_D && snDead){ screen=SCR_GAMES; screenDirty=true; return; }
  if (ev.type==EV_SWIPE_U && snDir!=2) snDir=0;
  else if (ev.type==EV_SWIPE_R && snDir!=3) snDir=1;
  else if (ev.type==EV_SWIPE_D && snDir!=0) snDir=2;
  else if (ev.type==EV_SWIPE_L && snDir!=1) snDir=3;
}

/* ========================= PONG (tilt-controlled) ==================== */
// Ball and two paddles; player tilts Alon to move bottom paddle, AI moves top.
#define PN_PW 60       // paddle width
#define PN_PH  8       // paddle height
#define PN_BR  6       // ball radius
float pnBx,pnBy,pnBvx,pnBvy;   // ball pos/vel
int   pnPly, pnAI;              // paddle x centres
int   pnScoreP, pnScoreA;
bool  pnOver;
uint32_t pnTick;
void pnResetBall(){ pnBx=233; pnBy=233; pnBvx=(esp_random()%2)?2.5f:-2.5f; pnBvy=-3.0f; }
void drawPongStatic(){
  gfx->fillScreen(C_BLACK);
  pnPly=pnAI=233; pnScoreP=pnScoreA=0; pnOver=false; pnResetBall(); pnTick=millis();
  textC("tilt to move",233,230,1,C_GREY);
}
void stepPong(){
  if(pnOver) return;
  uint32_t now=millis(); if(now-pnTick<25) return; pnTick=now;
  // player paddle from IMU tilt
  if(imuOK && imuRead()){ pnPly = clampi(233 + (int)(accX * 160), PN_PW/2+20, 466-PN_PW/2-20); }
  // AI paddle tracks ball loosely
  if(pnBx > pnAI+8) pnAI = clampi(pnAI+3, PN_PW/2+20, 466-PN_PW/2-20);
  else if(pnBx < pnAI-8) pnAI = clampi(pnAI-3, PN_PW/2+20, 466-PN_PW/2-20);
  // erase old ball
  gfx->fillCircle((int)pnBx,(int)pnBy,PN_BR+1,C_BLACK);
  // move ball
  pnBx+=pnBvx; pnBy+=pnBvy;
  // wall bounce
  if(pnBx<30){ pnBx=30; pnBvx=-pnBvx; }
  if(pnBx>436){ pnBx=436; pnBvx=-pnBvx; }
  // player paddle (bottom, y=420)
  if(pnBy>=414 && pnBvy>0 && fabsf(pnBx-pnPly)<PN_PW/2+PN_BR){
    pnBvy=-fabsf(pnBvy)-0.1f; pnBvx+=(pnBx-pnPly)*0.08f;
  }
  // AI paddle (top, y=50)
  if(pnBy<=56 && pnBvy<0 && fabsf(pnBx-pnAI)<PN_PW/2+PN_BR){
    pnBvy=fabsf(pnBvy)+0.1f; pnBvx+=(pnBx-pnAI)*0.08f;
  }
  // score
  if(pnBy<20){ pnScoreP++; pnResetBall(); pnBvy=3.0f; }
  if(pnBy>446){ pnScoreA++; pnResetBall(); }
  if(pnScoreP>=5||pnScoreA>=5){
    pnOver=true; reward(pnScoreP*2, pnScoreP*3); saveState();
    textC(pnScoreP>=5?"YOU WIN!":"YOU LOSE",233,230,3,pnScoreP>=5?C_GREEN:C_RED);
    textC("hold: exit",233,440,1,C_GREY); return;
  }
  // draw paddles
  gfx->fillRect(20,418,426,PN_PH,C_BLACK); // clear bottom row
  gfx->fillRoundRect(pnPly-PN_PW/2,418,PN_PW,PN_PH,3,C_CYAN);
  gfx->fillRect(20,46,426,PN_PH,C_BLACK);  // clear top row
  gfx->fillRoundRect(pnAI-PN_PW/2,46,PN_PW,PN_PH,3,C_RED);
  // draw ball
  gfx->fillCircle((int)pnBx,(int)pnBy,PN_BR,C_WHITE);
  // score display
  gfx->fillRect(180,0,106,20,C_BLACK);
  char sb[12]; snprintf(sb,sizeof(sb),"%d - %d",pnScoreP,pnScoreA); textC(sb,233,10,1,C_CYAN);
  // centre line
  for(int x=40;x<440;x+=20) gfx->fillRect(x,233,2,2,C_DIMGREY);
}
void handlePong(const UiEvent &ev){
  if(ev.type==EV_HOLD){ screen=SCR_GAMES; screenDirty=true; return; }
  // also allow swipe-based paddle control as fallback
  if(!imuOK){
    if(ev.type==EV_DOWN){ pnPly=clampi(ev.x,PN_PW/2+20,466-PN_PW/2-20); }
  }
}

/* ========================= TETRIS ==================================== */
#define TT_COLS  10
#define TT_ROWS  18
#define TT_CELL  18
#define TT_OX    ((466-TT_COLS*TT_CELL)/2)   // centre the board
#define TT_OY    40
uint8_t ttBoard[TT_ROWS][TT_COLS];   // 0=empty, 1-7=colour index
// 7 standard tetrominoes, each as 4 (row,col) offsets from pivot
struct TTPiece { int cells[4][2]; };
const TTPiece TT_SHAPES[7]={
  {{{0,0},{0,1},{1,0},{1,1}}},       // O
  {{{0,0},{0,1},{0,2},{0,3}}},       // I
  {{{0,0},{1,0},{1,1},{1,2}}},       // J
  {{{0,2},{1,0},{1,1},{1,2}}},       // L
  {{{0,0},{0,1},{1,1},{1,2}}},       // S
  {{{0,1},{0,2},{1,0},{1,1}}},       // Z
  {{{0,1},{1,0},{1,1},{1,2}}},       // T
};
const uint16_t TT_COLORS[7]={C_YELLOW,C_CYAN,0x001F,C_ORANGE,C_GREEN,C_RED,C_VIOLET};
int ttCur,ttRot,ttCx,ttCy; // current piece index, rotation, position
int ttScore; bool ttOver; uint32_t ttTick;

void ttGetCells(int shape,int rot,int cx,int cy,int out[4][2]){
  for(int i=0;i<4;i++){
    int r=TT_SHAPES[shape].cells[i][0], c=TT_SHAPES[shape].cells[i][1];
    for(int j=0;j<(rot%4);j++){ int t=r; r=c; c=2-t; } // 90-degree CW rotations
    out[i][0]=cy+r; out[i][1]=cx+c;
  }
}
bool ttFits(int shape,int rot,int cx,int cy){
  int cells[4][2]; ttGetCells(shape,rot,cx,cy,cells);
  for(int i=0;i<4;i++){
    int r=cells[i][0],c=cells[i][1];
    if(c<0||c>=TT_COLS||r<0||r>=TT_ROWS) return false;
    if(ttBoard[r][c]) return false;
  }
  return true;
}
void ttDrawCell(int r,int c,uint16_t col){
  if(r<0||r>=TT_ROWS||c<0||c>=TT_COLS) return;
  gfx->fillRect(TT_OX+c*TT_CELL+1,TT_OY+r*TT_CELL+1,TT_CELL-2,TT_CELL-2,col);
}
void ttSpawn(){
  ttCur=esp_random()%7; ttRot=0; ttCx=TT_COLS/2-1; ttCy=0;
  if(!ttFits(ttCur,ttRot,ttCx,ttCy)) ttOver=true;
}
void ttLock(){
  int cells[4][2]; ttGetCells(ttCur,ttRot,ttCx,ttCy,cells);
  for(int i=0;i<4;i++) ttBoard[cells[i][0]][cells[i][1]]=ttCur+1;
  // clear full rows
  int cleared=0;
  for(int r=TT_ROWS-1;r>=0;r--){
    bool full=true; for(int c=0;c<TT_COLS;c++) if(!ttBoard[r][c]){full=false;break;}
    if(full){ cleared++;
      for(int rr=r;rr>0;rr--) for(int c=0;c<TT_COLS;c++) ttBoard[rr][c]=ttBoard[rr-1][c];
      for(int c=0;c<TT_COLS;c++) ttBoard[0][c]=0;
      r++; // re-check this row
    }
  }
  if(cleared){ ttScore+=cleared*100; reward(cleared,cleared*2); }
  ttSpawn();
}
void ttDrawBoard(){
  for(int r=0;r<TT_ROWS;r++) for(int c=0;c<TT_COLS;c++)
    ttDrawCell(r,c,ttBoard[r][c]?TT_COLORS[ttBoard[r][c]-1]:C_BLACK);
  // draw current piece
  int cells[4][2]; ttGetCells(ttCur,ttRot,ttCx,ttCy,cells);
  for(int i=0;i<4;i++) ttDrawCell(cells[i][0],cells[i][1],TT_COLORS[ttCur]);
}
void drawTetrisStatic(){
  gfx->fillScreen(C_BLACK);
  memset(ttBoard,0,sizeof(ttBoard)); ttScore=0; ttOver=false; ttTick=millis();
  ttSpawn();
  gfx->drawRect(TT_OX-1,TT_OY-1,TT_COLS*TT_CELL+2,TT_ROWS*TT_CELL+2,C_DIMGREY);
  textC("hold: exit",233,440,1,C_GREY);
}
void stepTetris(){
  if(ttOver){
    textC("GAME OVER",233,220,3,C_RED);
    char sb[20]; snprintf(sb,sizeof(sb),"score %d",ttScore); textC(sb,233,260,2,C_CYAN);
    textC("hold: exit",233,440,1,C_GREY); return;
  }
  uint32_t now=millis();
  int spd = 500 - ttScore/2; if(spd<100) spd=100;
  if(now-ttTick<(uint32_t)spd) return;
  ttTick=now;
  if(ttFits(ttCur,ttRot,ttCx,ttCy+1)) ttCy++;
  else ttLock();
  ttDrawBoard();
  gfx->fillRect(0,0,466,20,C_BLACK);
  char sb[16]; snprintf(sb,sizeof(sb),"score %d",ttScore); textC(sb,233,10,1,C_CYAN);
}
void handleTetris(const UiEvent &ev){
  if(ev.type==EV_HOLD){ screen=SCR_GAMES; screenDirty=true; return; }
  if(ttOver) return;
  if(ev.type==EV_SWIPE_L && ttFits(ttCur,ttRot,ttCx-1,ttCy)) ttCx--;
  else if(ev.type==EV_SWIPE_R && ttFits(ttCur,ttRot,ttCx+1,ttCy)) ttCx++;
  else if(ev.type==EV_SWIPE_D){ while(ttFits(ttCur,ttRot,ttCx,ttCy+1)) ttCy++; ttLock(); ttTick=millis(); }
  else if(ev.type==EV_TAP || ev.type==EV_SWIPE_U){
    int nr=(ttRot+1)%4;
    if(ttFits(ttCur,nr,ttCx,ttCy)) ttRot=nr;
    else if(ttFits(ttCur,nr,ttCx-1,ttCy)){ ttCx--; ttRot=nr; }   // wall kick
    else if(ttFits(ttCur,nr,ttCx+1,ttCy)){ ttCx++; ttRot=nr; }
  }
}

/* ========================= BREAKOUT ================================== */
#define BK_COLS  8
#define BK_ROWS  5
#define BK_BW    (400/BK_COLS)  // brick width = 50
#define BK_BH    14
#define BK_TOP   70
#define BK_BOX   33  // left offset to centre bricks
bool bkBricks[BK_ROWS][BK_COLS];
float bkBx,bkBy,bkBvx,bkBvy;
int bkPadX;  // paddle centre x
int bkScore; bool bkOver; bool bkWin;
const uint16_t BK_ROW_COLS[BK_ROWS]={C_RED,C_ORANGE,C_YELLOW,C_GREEN,C_CYAN};

void bkResetBall(){ bkBx=233; bkBy=360; bkBvx=(esp_random()%2)?2.0f:-2.0f; bkBvy=-3.0f; }
void drawBreakoutStatic(){
  gfx->fillScreen(C_BLACK);
  for(int r=0;r<BK_ROWS;r++) for(int c=0;c<BK_COLS;c++){
    bkBricks[r][c]=true;
    gfx->fillRoundRect(BK_BOX+c*BK_BW+1,BK_TOP+r*(BK_BH+2)+1,BK_BW-2,BK_BH,4,BK_ROW_COLS[r]);
  }
  bkPadX=233; bkScore=0; bkOver=false; bkWin=false; bkResetBall();
  textC("hold: exit",233,440,1,C_GREY);
}
void stepBreakout(){
  if(bkOver){ textC(bkWin?"YOU WIN!":"GAME OVER",233,230,3,bkWin?C_GREEN:C_RED);
    char sb[16]; snprintf(sb,sizeof(sb),"score %d",bkScore); textC(sb,233,270,2,C_CYAN);
    textC("hold: exit",233,440,1,C_GREY); return; }
  uint32_t now=millis(); static uint32_t bkT=0; if(now-bkT<20) return; bkT=now;
  // erase ball
  gfx->fillCircle((int)bkBx,(int)bkBy,5,C_BLACK);
  bkBx+=bkBvx; bkBy+=bkBvy;
  // wall bounce
  if(bkBx<30){bkBx=30;bkBvx=-bkBvx;} if(bkBx>436){bkBx=436;bkBvx=-bkBvx;}
  if(bkBy<30){bkBy=30;bkBvy=-bkBvy;}
  // paddle (y=400, w=70)
  if(bkBy>=395 && bkBvy>0 && fabsf(bkBx-bkPadX)<40){
    bkBvy=-fabsf(bkBvy); bkBvx+=(bkBx-bkPadX)*0.06f;
  }
  if(bkBy>450){ bkOver=true; reward(bkScore/2,bkScore); saveState(); return; }
  // brick collision
  for(int r=0;r<BK_ROWS;r++) for(int c=0;c<BK_COLS;c++){
    if(!bkBricks[r][c]) continue;
    int bx=BK_BOX+c*BK_BW, by=BK_TOP+r*(BK_BH+2);
    if(bkBx>=bx && bkBx<=bx+BK_BW && bkBy>=by && bkBy<=by+BK_BH){
      bkBricks[r][c]=false; bkBvy=-bkBvy; bkScore+=10;
      gfx->fillRoundRect(bx+1,by+1,BK_BW-2,BK_BH,4,C_BLACK);
      // check win
      bool any=false; for(int rr=0;rr<BK_ROWS&&!any;rr++) for(int cc=0;cc<BK_COLS;cc++) if(bkBricks[rr][cc]){any=true;break;}
      if(!any){ bkWin=true; bkOver=true; reward(bkScore,bkScore*2); saveState(); }
      goto bk_done;
    }
  }
  bk_done:
  // draw paddle
  gfx->fillRect(20,398,426,10,C_BLACK);
  gfx->fillRoundRect(bkPadX-35,398,70,8,3,C_CYAN);
  // draw ball
  gfx->fillCircle((int)bkBx,(int)bkBy,5,C_WHITE);
  // score
  gfx->fillRect(0,0,466,18,C_BLACK);
  char sb[16]; snprintf(sb,sizeof(sb),"score %d",bkScore); textC(sb,233,10,1,C_CYAN);
}
void handleBreakout(const UiEvent &ev){
  if(ev.type==EV_HOLD){ screen=SCR_GAMES; screenDirty=true; return; }
  if(ev.type==EV_DOWN||ev.type==EV_TAP){
    bkPadX=clampi(ev.x,55,411);
  }
}

/* ========================= FLAPPY BIRD =============================== */
#define FL_GRAV   0.28f
#define FL_FLAP  -4.5f
#define FL_GAP    110     // gap between top and bottom pipe
#define FL_PW     36      // pipe width
float flY, flVy;          // bird y position and velocity
int   flPipes[4][2];      // x, gapY for up to 4 pipes
int   flPipeN;
int   flScore; bool flOver; bool flStarted;
uint32_t flTick;

void flResetPipes(){
  flPipeN=4;
  for(int i=0;i<4;i++){
    flPipes[i][0] = 500 + i*140;  // x
    flPipes[i][1] = 120 + (int)(esp_random()%180);  // gapY centre
  }
}
void drawFlappyStatic(){
  gfx->fillScreen(C_BLACK);
  flY=233; flVy=0; flScore=0; flOver=false; flStarted=false; flTick=millis();
  flResetPipes();
  textC("TAP TO FLAP",233,200,2,C_CYAN);
  // draw bird
  gfx->fillCircle(100,(int)flY,10,C_YELLOW);
}
void stepFlappy(){
  if(!flStarted || flOver) return;
  uint32_t now=millis(); if(now-flTick<30) return; flTick=now;
  // physics
  flVy+=FL_GRAV; flY+=flVy;
  if(flY<15) { flY=15; flVy=0; }
  if(flY>450){ flOver=true; reward(flScore,flScore*2); saveState();
    textC("GAME OVER",233,220,3,C_RED);
    char sb[16]; snprintf(sb,sizeof(sb),"score %d",flScore); textC(sb,233,260,2,C_CYAN);
    textC("hold: exit",233,440,1,C_GREY); return; }
  // clear playfield (only the moving strip)
  gfx->fillRect(20,20,426,426,C_BLACK);
  // pipes
  bool scored=false;
  for(int i=0;i<flPipeN;i++){
    flPipes[i][0]-=3;
    int px=flPipes[i][0], gy=flPipes[i][1];
    if(px>-FL_PW && px<480){
      // top pipe
      gfx->fillRect(px,20,FL_PW,gy-FL_GAP/2-20,C_GREEN);
      // bottom pipe
      gfx->fillRect(px,gy+FL_GAP/2,FL_PW,450-gy-FL_GAP/2,C_GREEN);
      // collision
      if(100>=px && 100<=px+FL_PW){
        if(flY<gy-FL_GAP/2+10 || flY>gy+FL_GAP/2-10){
          flOver=true; reward(flScore,flScore*2); saveState();
          textC("GAME OVER",233,220,3,C_RED);
          char sb[16]; snprintf(sb,sizeof(sb),"score %d",flScore); textC(sb,233,260,2,C_CYAN);
          textC("hold: exit",233,440,1,C_GREY); return;
        }
      }
    }
    // score when pipe passes bird
    if(flPipes[i][0]+FL_PW < 100 && flPipes[i][0]+FL_PW >= 97) { flScore++; scored=true; }
    // recycle pipe
    if(flPipes[i][0]<-FL_PW){
      flPipes[i][0]=flPipes[(i+flPipeN-1)%flPipeN][0]+140;
      flPipes[i][1]=120+(int)(esp_random()%180);
    }
  }
  // draw bird
  gfx->fillCircle(100,(int)flY,10,C_YELLOW);
  gfx->fillCircle(107,(int)flY-3,3,C_BLACK);  // eye
  // score display
  char sb[8]; snprintf(sb,sizeof(sb),"%d",flScore); textC(sb,233,10,2,C_WHITE);
}
void handleFlappy(const UiEvent &ev){
  if(ev.type==EV_HOLD){ screen=SCR_GAMES; screenDirty=true; return; }
  if(flOver && ev.type==EV_TAP){ screenDirty=true; return; } // restart
  if(ev.type==EV_TAP || ev.type==EV_DOWN){
    if(!flStarted) flStarted=true;
    flVy=FL_FLAP;
  }
}

/* ===================================================================
   NEED DECAY  (real-time while running)
   =================================================================== */
uint32_t lastDecay=0;
void decayNeeds(){
  uint32_t now=millis();
  if(now-lastDecay<60000UL)return;          // once a minute
  lastDecay=now;
  pet.hunger=clampi(pet.hunger-2,0,100);
  pet.clean =clampi(pet.clean -1,0,100);
  if(pet.sleeping){ pet.energy=clampi(pet.energy+6,0,100); }
  else           { pet.energy=clampi(pet.energy-2,0,100); pet.fun=clampi(pet.fun-2,0,100); }
  saveState();
  if(screen==SCR_HOME||screen==SCR_STATS) /* dynamic repaint handles it */;
}

/* ===================================================================
   SHAKE detection (IMU) -> small surprise
   =================================================================== */
uint32_t lastShake=0;
void checkShake(){
  if(!imuOK)return;
  float ax=accX,ay=accY,az=accZ;
  if(!imuRead())return;
  ax=accX; ay=accY; az=accZ;
  float mag=sqrtf(ax*ax+ay*ay+az*az);
  if(mag>2.2f && millis()-lastShake>1500){     // ~2.2g jolt
    lastShake=millis();
    if(pet.sleeping){ pet.sleeping=false; gfx->setBrightness(140); }
    reward(2,3); pet.fun=clampi(pet.fun+4,0,100);
    screenDirty=true; saveState();
  }
}

/* ===================================================================
   APPS LAUNCHER  +  MINI-APPS   (swipe up from home)
   =================================================================== */
struct AppDef { const char *name; Screen scr; };
AppDef APPS[] = {
  {"Games", SCR_GAMES}, {"Leveler", SCR_LEVEL}, {"Marble", SCR_HORIZON},
  {"Steps", SCR_STEPS}, {"Clock", SCR_CLOCK},  {"Stopwatch", SCR_STOPWATCH},
  {"Timer", SCR_TIMER}, {"Battery", SCR_BATTERY}, {"Light", SCR_FLASH},
  {"WiFi", SCR_WIFI}, {"System", SCR_SYSINFO},
};
const int APP_COUNT = sizeof(APPS)/sizeof(APPS[0]);
#define APPS_PER_PAGE 6
int appPage = 0;

void appTileXY(int i,int &x,int &y){ int c=i%3, r=i/3; x=24+c*144; y=100+r*100; }

// Simple line-art app icons drawn from GFX primitives (accent-coloured).
void drawAppIcon(const char* n, int cx, int cy, uint16_t c){
  if(!strcmp(n,"Games")){
    gfx->drawRoundRect(cx-16,cy-8,32,16,7,c);
    gfx->drawLine(cx-9,cy-2,cx-9,cy+2,c); gfx->drawLine(cx-11,cy,cx-7,cy,c);
    gfx->fillCircle(cx+8,cy-2,2,c); gfx->fillCircle(cx+12,cy+2,2,c);
  } else if(!strcmp(n,"Leveler")){
    gfx->drawCircle(cx,cy,14,c); gfx->drawLine(cx-14,cy,cx+14,cy,c); gfx->fillCircle(cx,cy,4,c);
  } else if(!strcmp(n,"Marble")){
    gfx->drawCircle(cx,cy,14,c); gfx->fillCircle(cx+4,cy-3,5,c);
  } else if(!strcmp(n,"Steps")){
    gfx->fillRoundRect(cx-11,cy-2,8,14,4,c); gfx->fillRoundRect(cx+3,cy-8,8,14,4,c);
  } else if(!strcmp(n,"Clock")){
    gfx->drawCircle(cx,cy,14,c); gfx->drawLine(cx,cy,cx,cy-9,c); gfx->drawLine(cx,cy,cx+7,cy+2,c);
  } else if(!strcmp(n,"Stopwatch")){
    gfx->drawCircle(cx,cy,13,c); gfx->drawLine(cx,cy-13,cx,cy-19,c); gfx->drawLine(cx,cy,cx,cy-8,c);
  } else if(!strcmp(n,"Timer")){
    gfx->drawLine(cx-10,cy-12,cx+10,cy-12,c); gfx->drawLine(cx+10,cy-12,cx,cy,c); gfx->drawLine(cx,cy,cx-10,cy-12,c);
    gfx->drawLine(cx-10,cy+12,cx+10,cy+12,c); gfx->drawLine(cx+10,cy+12,cx,cy,c); gfx->drawLine(cx,cy,cx-10,cy+12,c);
  } else if(!strcmp(n,"Battery")){
    gfx->drawRoundRect(cx-16,cy-8,30,16,3,c); gfx->fillRect(cx+14,cy-3,3,6,c);
    gfx->fillRoundRect(cx-13,cy-5,10,10,2,c);
  } else if(!strcmp(n,"Light")){
    gfx->fillCircle(cx,cy,6,c);
    for(int a=0;a<360;a+=45){ float r=a*0.01745f;
      gfx->drawLine(cx+(int)(11*cosf(r)),cy+(int)(11*sinf(r)),cx+(int)(16*cosf(r)),cy+(int)(16*sinf(r)),c); }
  } else if(!strcmp(n,"WiFi")){
    for(int rr=6;rr<=16;rr+=5) for(int a=210;a<=330;a+=7){ float r=a*0.01745f;
      gfx->fillCircle(cx+(int)(rr*cosf(r)),cy+8+(int)(rr*sinf(r)),1,c); }
    gfx->fillCircle(cx,cy+8,2,c);
  } else if(!strcmp(n,"System")){
    gfx->drawCircle(cx,cy,12,c); gfx->fillCircle(cx,cy,3,c);
    for(int a=0;a<360;a+=45){ float r=a*0.01745f;
      gfx->drawLine(cx+(int)(12*cosf(r)),cy+(int)(12*sinf(r)),cx+(int)(16*cosf(r)),cy+(int)(16*sinf(r)),c); }
  } else { gfx->drawCircle(cx,cy,12,c); }
}

void drawApps(){
  gfx->fillScreen(C_BLACK);
  textC("APPS", 233, 40, 3, THEMES[pet.equipped].accent);
  int start=appPage*APPS_PER_PAGE;
  for(int i=0;i<APPS_PER_PAGE;i++){
    int idx=start+i; if(idx>=APP_COUNT) break;
    int x,y; appTileXY(i,x,y);
    uint16_t ac = (idx%2==0) ? C_CYAN : C_VIOLET;
    // no tile background - icons + text glow directly on black AMOLED
    drawAppIcon(APPS[idx].name, x+65, y+30, ac);
    textC(APPS[idx].name, x+65, y+68, 2, ac);
  }
  int pages=(APP_COUNT+APPS_PER_PAGE-1)/APPS_PER_PAGE;
  for(int p=0;p<pages;p++)
    gfx->fillCircle(233-(pages-1)*9+p*18, 322, 4, p==appPage?THEMES[pet.equipped].accent:C_DIMGREY);
  drawBackBtn();
}
void handleApps(const UiEvent &ev){
  int pages=(APP_COUNT+APPS_PER_PAGE-1)/APPS_PER_PAGE;
  if(hitBack(ev) || ev.type==EV_SWIPE_D){ screen=SCR_HOME; screenDirty=true; return; }
  if(ev.type==EV_SWIPE_L){ appPage=(appPage+1)%pages; screenDirty=true; return; }
  if(ev.type==EV_SWIPE_R){ appPage=(appPage+pages-1)%pages; screenDirty=true; return; }
  if(ev.type!=EV_DOWN) return;
  int start=appPage*APPS_PER_PAGE;
  for(int i=0;i<APPS_PER_PAGE;i++){
    int idx=start+i; if(idx>=APP_COUNT) break;
    int x,y; appTileXY(i,x,y);
    if(ev.x>=x&&ev.x<=x+130&&ev.y>=y&&ev.y<=y+88){ screen=APPS[idx].scr; screenDirty=true; return; }
  }
}

// ---- small helper: standard app header + back ----
void appHeader(const char *title){
  gfx->fillScreen(C_BLACK);
  textC(title, 233, 36, 2, THEMES[pet.equipped].accent);
}
bool appBack(const UiEvent &ev){
  if(hitBack(ev) || ev.type==EV_SWIPE_D){ screen=SCR_APPS; screenDirty=true; return true; }
  return false;
}

/* ===================================================================
   WiFi + OTA (wireless updates)
   - Setup is keyboard-free: Alon makes a hotspot "Alon-Setup"; you join it
     on your phone, pick your home network, done (WiFiManager handles it).
   - The ESP32 remembers the network and reconnects on its own after that.
   - CHECK UPDATE pulls the latest app image from GitHub and self-updates.
   =================================================================== */
#define OTA_URL "https://raw.githubusercontent.com/Piyushnandwana/alon-firmware/firmware-bin/Alon-ota-app.bin"
#define TZ_RULE "EST5EDT,M3.2.0,M11.1.0"     // US Eastern (auto DST)
String   otaMsg = "";
uint32_t wifiStatMs = 0;

bool wifiConnected(){ return WiFi.status()==WL_CONNECTED; }

// Non-blocking at boot: reconnect to the last saved network (ESP32 stores it)
// and arm NTP so the clock sets itself once online.
void wifiBoot(){
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin();
  configTzTime(TZ_RULE, "pool.ntp.org", "time.nist.gov");
}

void drawWifi(){
  gfx->fillScreen(C_BLACK);
  textC("WIFI", 233, 40, 3, THEMES[pet.equipped].accent);
  if(wifiConnected()){
    textC("Connected", 233, 92, 2, C_GREEN);
    textC(WiFi.SSID().c_str(), 233, 122, 2, C_WHITE);
    textC(WiFi.localIP().toString().c_str(), 233, 150, 2, C_CYAN);
  } else {
    textC("Not connected", 233, 108, 2, C_RED);
    textC("tap SETUP, then join", 233, 138, 1, C_GREY);
    textC("'Alon-Setup' on phone", 233, 156, 1, C_GREY);
  }
  gfx->fillRoundRect(96,186,120,44,12,C_TILE);      textC("SETUP",156,208,2,C_CYAN);
  gfx->fillRoundRect(250,186,120,44,12,C_TILE);    textC("FORGET",310,208,2,C_VIOLET);
  gfx->fillRoundRect(133,246,200,46,12,C_TILE);    textC("CHECK UPDATE",233,269,2,C_CYAN);
  if(otaMsg.length()) textC(otaMsg.c_str(), 233, 316, 1, C_YELLOW);
  drawBackBtn();
}

// User-initiated, so a blocking portal is fine; show instructions first.
void wifiStartPortal(){
  gfx->fillScreen(C_BLACK);
  textC("WIFI SETUP", 233, 70, 2, C_CYAN);
  textC("On your phone, join", 233, 140, 2, C_WHITE);
  textC("Wi-Fi: Alon-Setup", 233, 172, 2, C_YELLOW);
  textC("then pick your network", 233, 212, 2, C_WHITE);
  textC("(waits up to 3 min)", 233, 252, 1, C_GREY);
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  wm.startConfigPortal("Alon-Setup");
  configTzTime(TZ_RULE, "pool.ntp.org", "time.nist.gov");
  otaMsg = wifiConnected() ? "connected" : "setup timed out";
  screenDirty = true;
}

void wifiForget(){
  WiFiManager wm; wm.resetSettings();
  WiFi.disconnect(true, true);
  otaMsg = "network forgotten";
}

// Pull latest app image and flash it. Reboots automatically on success; a
// failed download leaves the running firmware untouched.
void otaCheck(){
  if(!wifiConnected()){ otaMsg="connect wifi first"; screenDirty=true; return; }
  otaMsg="updating... keep still"; drawWifi();
  WiFiClientSecure client; client.setInsecure();
  httpUpdate.rebootOnUpdate(true);
  t_httpUpdate_return r = httpUpdate.update(client, OTA_URL);
  if(r==HTTP_UPDATE_FAILED)          otaMsg = String("fail: ")+httpUpdate.getLastError();
  else if(r==HTTP_UPDATE_NO_UPDATES) otaMsg = "already latest";
  screenDirty=true;
}

void handleWifi(const UiEvent &ev){
  if(appBack(ev)) return;
  if(ev.type!=EV_DOWN) return;
  int x=ev.x, y=ev.y;
  if(y>=186&&y<=230){
    if(x>=96 &&x<=216){ wifiStartPortal(); return; }
    if(x>=250&&x<=370){ wifiForget(); screenDirty=true; return; }
  }
  if(x>=133&&x<=333&&y>=246&&y<=292){ otaCheck(); return; }
}

// Flip the screen to "Connected" once it associates in the background.
void tickWifi(){
  if(millis()-wifiStatMs<1500) return; wifiStatMs=millis();
  static int last=-1;
  if((int)wifiConnected()!=last){ last=(int)wifiConnected(); screenDirty=true; }
}

/* ---------------- LEVELER (bubble level, uses accelerometer) ------ */
void drawLevelStatic(){ imuEnsure(); appHeader("LEVELER"); drawBackBtn(); }
void tickLevel(){
  static uint32_t last=0;
  if(millis()-last<50) return; last=millis();
  if(!imuOK){ textC("IMU not available",233,210,2,C_RED); return; }
  if(!imuRead()) return;
  // Clear the entire play area each frame to prevent ghost trails
  gfx->fillCircle(233,210,136,C_BLACK);
  // Draw reference circles
  gfx->drawCircle(233,210,120,C_DIMGREY);
  gfx->drawCircle(233,210,60,C_DIMGREY);
  gfx->drawCircle(233,210,30,C_GREEN);
  // Draw crosshair
  gfx->drawFastHLine(113,210,240,C_DIMGREY);
  gfx->fillRect(233,90,1,240,C_DIMGREY);   // vertical crosshair
  // Calculate bubble position
  int bx=233-(int)(accX*120), by=210+(int)(accY*120);
  float dx=bx-233, dy=by-210, d=sqrtf(dx*dx+dy*dy);
  if(d>104){ bx=233+(int)(dx*104/d); by=210+(int)(dy*104/d); }
  bool level=(abs(bx-233)<16 && abs(by-210)<16);
  gfx->fillCircle(bx,by,14, level?C_GREEN:C_CYAN);
  // Degree readout
  gfx->fillRect(120,340,226,26,C_BLACK);
  char b[28]; snprintf(b,sizeof(b),"%d, %d deg",(int)(accX*90),(int)(accY*90));
  textC(level?"LEVEL!":b,233,352,2, level?C_GREEN:C_WHITE);
}
void handleLevel(const UiEvent &ev){ appBack(ev); }

/* ---------------- MARBLE (tilt a ball around a bowl) -------------- */
float mbx=233,mby=233,mvx=0,mvy=0;
void drawMarbleStatic(){ imuEnsure(); appHeader("MARBLE"); gfx->drawCircle(233,236,150,C_DIMGREY); mbx=mby=233; mvx=mvy=0; drawBackBtn(); }
void tickMarble(){
  static uint32_t last=0; if(millis()-last<33) return; last=millis();
  if(!imuOK){ textC("IMU not available",233,236,2,C_RED); return; }
  if(!imuRead()) return;
  gfx->fillCircle((int)mbx,(int)mby,12,C_BLACK);
  mvx += accX*1.6f; mvy -= accY*1.6f; mvx*=0.93f; mvy*=0.93f;
  mbx += mvx*0.12f; mby += mvy*0.12f;
  float dx=mbx-233, dy=mby-236, d=sqrtf(dx*dx+dy*dy);
  if(d>138){ mbx=233+dx*138/d; mby=236+dy*138/d; mvx*=-0.5f; mvy*=-0.5f; }
  gfx->drawCircle(233,236,150,C_DIMGREY);
  gfx->fillCircle((int)mbx,(int)mby,12,THEMES[pet.equipped].accent);
}
void handleMarble(const UiEvent &ev){ if(appBack(ev)) return; if(ev.type==EV_HOLD){ mbx=mby=233; mvx=mvy=0; } }

/* ---------------- STEP COUNTER (accel peak detect) --------------- */
long stepCount=0;
void drawStepsStatic(){ imuEnsure(); appHeader("STEPS"); gfx->fillRoundRect(133,300,200,40,10,C_TILE); textC("RESET",233,320,2,C_CYAN); drawBackBtn(); }
void tickSteps(){
  if(imuOK && imuRead()){
    float mag=sqrtf(accX*accX+accY*accY+accZ*accZ);
    static float lp=1; lp=lp*0.85f+mag*0.15f;
    static uint32_t lastStep=0;
    if(mag-lp>0.22f && millis()-lastStep>300){ stepCount++; lastStep=millis(); }
  }
  static long shown=-1;
  if(stepCount!=shown){ shown=stepCount; gfx->fillRect(80,150,306,90,C_BLACK);
    char b[16]; snprintf(b,sizeof(b),"%ld",stepCount); textC(b,233,195,6,C_CYAN); }
}
void handleSteps(const UiEvent &ev){
  if(appBack(ev)) return;
  if(ev.type==EV_DOWN && ev.x>=133&&ev.x<=333&&ev.y>=300&&ev.y<=340){ stepCount=0; screenDirty=true; }
}

/* ---------------- ANALOG CLOCK (from RTC) ------------------------ */
void clockHand(float deg,int len,int w,uint16_t col){
  float a=deg*0.0174533f; int x2=233+(int)(sinf(a)*len), y2=236-(int)(cosf(a)*len);
  gfx->drawLine(233,236,x2,y2,col);
  if(w>1){ gfx->drawLine(232,236,x2,y2,col); gfx->drawLine(234,236,x2,y2,col); }
}
void drawClockStatic(){ appHeader("CLOCK"); drawBackBtn(); }
void tickClock(){
  static int ls=-1;
  if(!rtcOK){ textC("set time in settings",233,236,2,C_GREY); return; }
  int h=0,m=0,s=0; rtcGet(h,m,s);
  if(s==ls) return; ls=s;
  gfx->fillCircle(233,236,145,C_BLACK);
  gfx->drawCircle(233,236,145,C_DIMGREY);
  for(int i=0;i<12;i++){ float a=i*30*0.0174533f;
    gfx->fillCircle(233+(int)(sinf(a)*128),236-(int)(cosf(a)*128),2,C_GREY); }
  clockHand((h%12)*30+m*0.5f, 70, 2, C_CYAN);
  clockHand(m*6, 100, 2, C_WHITE);
  clockHand(s*6, 115, 1, C_RED);
  gfx->fillCircle(233,236,4,THEMES[pet.equipped].accent);
  gfx->fillRect(150,392,166,20,C_BLACK);
  char b[16]; snprintf(b,sizeof(b),"%02d:%02d:%02d",h,m,s); textC(b,233,400,2,C_CYAN);
}
void handleClock(const UiEvent &ev){ appBack(ev); }

/* ---------------- STOPWATCH -------------------------------------- */
uint32_t swStart=0, swAccum=0; bool swRun=false;
void drawStopwatchStatic(){
  appHeader("STOPWATCH");
  gfx->fillRoundRect(96,300,120,42,10,C_TILE); textC(swRun?"STOP":"START",156,321,2,swRun?C_RED:C_CYAN);
  gfx->fillRoundRect(250,300,120,42,10,C_TILE); textC("RESET",310,321,2,C_VIOLET);
  drawBackBtn();
}
void tickStopwatch(){
  static uint32_t last=0; if(millis()-last<60) return; last=millis();
  uint32_t ms = swAccum + (swRun? millis()-swStart : 0);
  gfx->fillRect(80,170,306,60,C_BLACK);
  char b[16]; snprintf(b,sizeof(b),"%02lu:%02lu.%02lu",(unsigned long)(ms/60000),(unsigned long)((ms/1000)%60),(unsigned long)((ms/10)%100));
  textC(b,233,200,4,C_WHITE);
}
void handleStopwatch(const UiEvent &ev){
  if(appBack(ev)) return;
  if(ev.type!=EV_DOWN) return;
  if(ev.x>=96&&ev.x<=216&&ev.y>=300&&ev.y<=342){
    if(swRun){ swAccum+=millis()-swStart; swRun=false; } else { swStart=millis(); swRun=true; }
    screenDirty=true;
  } else if(ev.x>=250&&ev.x<=370&&ev.y>=300&&ev.y<=342){ swRun=false; swAccum=0; swStart=0; screenDirty=true; }
}

/* ---------------- TIMER ------------------------------------------ */
int timerMin=5; uint32_t tmrEnd=0; bool timerRun=false; bool timerDone=false;
void drawTimerStatic(){
  appHeader("TIMER");
  gfx->fillRoundRect(96,150,44,44,10,C_TILE); textC("-",118,172,3,C_VIOLET);
  gfx->fillRoundRect(326,150,44,44,10,C_TILE); textC("+",348,172,3,C_VIOLET);
  gfx->fillRoundRect(96,300,274,42,10,C_TILE); textC(timerRun?"STOP":"START",233,321,2,timerRun?C_RED:C_CYAN);
  drawBackBtn();
}
void tickTimer(){
  static uint32_t last=0; if(millis()-last<100) return; last=millis();
  long rem;
  if(timerRun){ rem=(long)tmrEnd-(long)millis(); if(rem<=0){ rem=0; timerRun=false; timerDone=true; } }
  else rem = timerDone?0:(long)timerMin*60000;
  gfx->fillRect(150,158,176,36,C_BLACK);
  char b[12]; snprintf(b,sizeof(b),"%02ld:%02ld",(rem/60000),(rem/1000)%60); textC(b,233,176,3,timerDone?C_RED:C_WHITE);
  if(timerDone){ static bool f=false; f=!f; gfx->fillRect(60,210,346,60,f?C_RED:C_BLACK); if(f) textC("TIME!",233,240,4,C_WHITE); }
}
void handleTimer(const UiEvent &ev){
  if(appBack(ev)) return;
  if(ev.type!=EV_DOWN) return;
  if(ev.y>=150&&ev.y<=194){
    if(ev.x>=96&&ev.x<=140){ timerMin=clampi(timerMin-1,1,99); timerDone=false; screenDirty=true; }
    if(ev.x>=326&&ev.x<=370){ timerMin=clampi(timerMin+1,1,99); timerDone=false; screenDirty=true; }
  }
  if(ev.x>=96&&ev.x<=370&&ev.y>=300&&ev.y<=342){
    if(timerRun){ timerRun=false; } else { tmrEnd=millis()+(uint32_t)timerMin*60000; timerRun=true; timerDone=false; }
    screenDirty=true;
  }
}

/* ---------------- BATTERY DASHBOARD ------------------------------ */
void drawBatteryStatic(){ appHeader("BATTERY"); drawBackBtn(); }
void tickBattery(){
  static uint32_t last=0; if(millis()-last<2000) return; last=millis();
  int b=batteryPercent(); bool chg=batteryCharging();
  gfx->fillRect(60,120,346,180,C_BLACK);
  // battery outline
  gfx->drawRoundRect(143,150,180,90,10,C_WHITE); gfx->fillRect(323,175,10,40,C_WHITE);
  if(b>=0){ uint16_t c=b<20?C_RED:(b<50?C_YELLOW:C_GREEN); gfx->fillRoundRect(149,156,(168)*b/100,78,6,c); }
  char t[16]; if(b>=0) snprintf(t,sizeof(t),"%d%%",b); else snprintf(t,sizeof(t),"--");
  textC(t,233,280,3,C_WHITE);
  textC(chg?"charging":"on battery",233,312,2,chg?C_GREEN:C_GREY);
}
void handleBattery(const UiEvent &ev){ appBack(ev); }

/* ---------------- FLASHLIGHT ------------------------------------- */
void drawFlashStatic(){ gfx->fillScreen(C_WHITE); gfx->setBrightness(255); textC("tap to exit",233,440,2,C_DIMGREY); }
void handleFlash(const UiEvent &ev){
  if(ev.type==EV_TAP || ev.type==EV_HOLD || hitBack(ev) || isSwipe(ev)){
    gfx->setBrightness(brightness); screen=SCR_APPS; screenDirty=true;
  }
}

/* ---------------- SYSTEM INFO ------------------------------------ */
void drawSysinfoStatic(){
  appHeader("SYSTEM");
  char b[40];
  textL("Chip:",70,110,2,C_GREY);  textL((char*)ESP.getChipModel(),200,110,2,C_WHITE);
  snprintf(b,sizeof(b),"%d MHz",ESP.getCpuFreqMHz()); textL("CPU:",70,146,2,C_GREY); textL(b,200,146,2,C_WHITE);
  snprintf(b,sizeof(b),"%u MB",(unsigned)(ESP.getFlashChipSize()/1048576)); textL("Flash:",70,182,2,C_GREY); textL(b,200,182,2,C_WHITE);
  snprintf(b,sizeof(b),"%u KB",(unsigned)(ESP.getPsramSize()/1024)); textL("PSRAM:",70,218,2,C_GREY); textL(b,200,218,2,C_WHITE);
  drawBackBtn();
}
void tickSysinfo(){
  static uint32_t last=0; if(millis()-last<1000) return; last=millis();
  gfx->fillRect(60,254,346,60,C_BLACK);
  char b[40]; snprintf(b,sizeof(b),"%u KB",(unsigned)(ESP.getFreeHeap()/1024)); textL("Free RAM:",70,254,2,C_GREY); textL(b,220,254,2,C_WHITE);
  uint32_t up=millis()/1000; snprintf(b,sizeof(b),"%lum %lus",(unsigned long)(up/60),(unsigned long)(up%60));
  textL("Uptime:",70,290,2,C_GREY); textL(b,220,290,2,C_WHITE);
}
void handleSysinfo(const UiEvent &ev){ appBack(ev); }

/* ===================================================================
   SETUP / LOOP
   =================================================================== */
void setup(){
  Serial.begin(115200);
  delay(200);

  // ---- I2C (shared bus, 400kHz as the board BSP requires) ----
  Wire.begin(IIC_SDA, IIC_SCL);
  Wire.setClock(400000);
  Wire.setTimeOut(50);        // never let a stuck I2C read freeze the loop

  // ---- Display ----
  if(!gfx->begin()){ Serial.println("gfx begin failed"); }
  gfx->fillScreen(C_BLACK);
  gfx->setBrightness(140);
  textC("ALON", LCD_W/2, LCD_H/2-10, 4, C_CYAN);
  textC("booting...", LCD_W/2, LCD_H/2+30, 2, C_VIOLET);

  // ---- Touch: EXACTLY like Waveshare's own CST9217 example ----
  //  give the driver the REAL reset pin (GPIO40) + INT (GPIO11);
  //  do NOT pulse GPIO2 (that is the SD clock on this board).
  touch.setPins(TP_RST, TP_INT);
  bool tpOK = touch.begin(Wire, TP_ADDR, IIC_SDA, IIC_SCL);
  if(!tpOK){ Serial.println("touch begin failed"); }
  pinMode(TP_INT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(TP_INT), onTouchISR, FALLING);

#if TOUCH_TEST
  drawTouchTestStatic();
  return;                 // calibration mode: skip the pet
#endif

  // ---- IMU (QMI8658): probe first so a missing/stuck chip can't hang boot,
  //      then run Waveshare's proven init sequence. Enables shake + sensor apps.
  // NOTE: the SensorLib QMI8658 init blocks on this unit (hangs boot), so it
  // is disabled here to guarantee a clean boot. Sensor apps show "IMU not
  // available" until we bring it up through a non-blocking path.
  // IMU (QMI8658) starts lazily when a motion app opens (correct addr 0x6A),
  // so a missing/slow chip can never stall boot.
  // IMU initialized lazily when a motion app opens (imuEnsure)

  // ---- Real-time clock (PCF85063): start the oscillator so it actually ticks ----
  rtcOK = rtcInit();
  if(!rtcOK) Serial.println("rtc begin failed");

  // ---- WiFi: reconnect to saved network (non-blocking) + NTP auto-time ----
  wifiBoot();

  // ---- BOOT side button (short button) = go home / wake ----
  pinMode(BOOT_BTN, INPUT_PULLUP);
  bootPrev = digitalRead(BOOT_BTN);

  loadState();
  gfx->setBrightness(brightness);     // apply saved brightness
  lastDecay = millis();
  delay(400);
  screen = SCR_HOME;
  screenDirty = true;
}

// SHORT side button (BOOT): toggle screen on/off. When screen is off, press
// turns it back on (to wherever you were). When screen is on, press turns it off.
void screenWake(){
  if(!screenOff) return;
  screenOff = false;
  gfx->setBrightness(brightness);
  screenDirty = true;                         // redraw current screen
}
void checkBoot(){
  bool lvl = digitalRead(BOOT_BTN);         // LOW = pressed
  if(bootPrev && !lvl){
    if(screenOff){
      screenWake();
    } else {
      // Turn screen off
      screenOff = true;
      gfx->fillScreen(C_BLACK);
      gfx->setBrightness(0);
    }
  }
  bootPrev = lvl;
}

void enterScreen(){
  switch(screen){
    case SCR_HOME:  drawHomeStatic();  break;
    case SCR_CARE:  drawCare();        break;
    case SCR_SHOP:  drawShop();        break;
    case SCR_GAMES: drawGames();       break;
    case SCR_STATS: drawStats();       break;
    case SCR_AGENT: drawAgentStatic(); break;
    case SCR_SNAKE:   drawSnakeStatic();    break;
    case SCR_PONG:    imuEnsure(); drawPongStatic(); break;
    case SCR_TETRIS:  drawTetrisStatic();  break;
    case SCR_BREAKOUT:drawBreakoutStatic();break;
    case SCR_FLAPPY:  drawFlappyStatic();  break;
    case SCR_SETTINGS: drawSettings(); break;
    case SCR_APPS:    drawApps();        break;
    case SCR_LEVEL:   drawLevelStatic(); break;
    case SCR_HORIZON: drawMarbleStatic();break;
    case SCR_STEPS:   drawStepsStatic(); break;
    case SCR_CLOCK:   drawClockStatic(); break;
    case SCR_STOPWATCH: drawStopwatchStatic(); break;
    case SCR_TIMER:   drawTimerStatic(); break;
    case SCR_BATTERY: drawBatteryStatic();break;
    case SCR_FLASH:   drawFlashStatic(); break;
    case SCR_SYSINFO: drawSysinfoStatic();break;
    case SCR_WIFI:    drawWifi();          break;
  }
}

void loop(){
#if TOUCH_TEST
  touchTestLoop();
  return;
#endif
  checkBoot();                                // side button toggles screen on/off (must run even when off)
  if(screenOff){
    // Screen is off - touch also wakes it
    UiEvent ev = pollGesture();
    if(ev.type == EV_DOWN || ev.type == EV_TAP) screenWake();
    return;
  }
  if(screenDirty){ screenDirty=false; enterScreen(); return; }

  UiEvent ev = pollGesture();

  switch(screen){
    case SCR_HOME:  handleHome(ev);  break;
    case SCR_CARE:  handleCare(ev);  break;
    case SCR_SHOP:  handleShop(ev);  break;
    case SCR_GAMES: handleGames(ev); break;
    case SCR_STATS: handleStats(ev); break;
    case SCR_AGENT: handleAgent(ev); break;
    case SCR_SNAKE:   handleSnake(ev);   break;
    case SCR_PONG:    handlePong(ev);    break;
    case SCR_TETRIS:  handleTetris(ev);  break;
    case SCR_BREAKOUT:handleBreakout(ev);break;
    case SCR_FLAPPY:  handleFlappy(ev);  break;
    case SCR_SETTINGS: handleSettings(ev); break;
    case SCR_APPS:    handleApps(ev);     break;
    case SCR_LEVEL:   handleLevel(ev);    break;
    case SCR_HORIZON: handleMarble(ev);   break;
    case SCR_STEPS:   handleSteps(ev);    break;
    case SCR_CLOCK:   handleClock(ev);    break;
    case SCR_STOPWATCH: handleStopwatch(ev); break;
    case SCR_TIMER:   handleTimer(ev);    break;
    case SCR_BATTERY: handleBattery(ev);  break;
    case SCR_FLASH:   handleFlash(ev);    break;
    case SCR_SYSINFO: handleSysinfo(ev);  break;
    case SCR_WIFI:    handleWifi(ev);     break;
  }

  // if a handler changed screens, let the next loop draw it fresh
  if(screenDirty){ inputCooldownUntil = millis() + 300; resetGesture(); return; }

  // per-screen animation / ticking
  switch(screen){
    case SCR_HOME:  drawHomeDynamic(); animatePetReaction(); break;
    case SCR_LEVEL:   tickLevel();     break;
    case SCR_HORIZON: tickMarble();    break;
    case SCR_STEPS:   tickSteps();     break;
    case SCR_CLOCK:   tickClock();     break;
    case SCR_STOPWATCH: tickStopwatch(); break;
    case SCR_TIMER:   tickTimer();     break;
    case SCR_BATTERY: tickBattery();   break;
    case SCR_SYSINFO: tickSysinfo();   break;
    case SCR_WIFI:    tickWifi();      break;
    case SCR_AGENT: drawAgentDynamic();break;
    case SCR_SNAKE:   stepSnake();      break;
    case SCR_PONG:    stepPong();      break;
    case SCR_TETRIS:  stepTetris();    break;
    case SCR_BREAKOUT:stepBreakout();  break;
    case SCR_FLAPPY:  stepFlappy();    break;
    default: break;
  }

#if TOUCH_DEBUG
  drawTouchDebug();
#endif
  checkShake();
  delay(2);          // keeps the touch loop tight & responsive
}
