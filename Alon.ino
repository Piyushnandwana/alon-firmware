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
#define IMU_ADDR    0x6B

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
bool imuProbe(){
  Wire.beginTransmission(IMU_ADDR);
  return Wire.endTransmission()==0;
}
bool imuRead(){
  if(!imuOK) return false;
  if(!imu.getDataReady()) return false;
  imu.getAccelerometer(accX,accY,accZ);   // accelerometer only (gyro unused)
  return true;
}

// --- PCF85063 real-time clock via direct registers (non-blocking) ---
uint8_t bcd2dec(uint8_t b){ return (uint8_t)((b>>4)*10 + (b&0x0F)); }
uint8_t dec2bcd(uint8_t d){ return (uint8_t)(((d/10)<<4) | (d%10)); }
bool rtcProbe(){ Wire.beginTransmission(RTC_ADDR); return Wire.endTransmission()==0; }
bool rtcGet(int &hh,int &mm,int &ss){
  Wire.beginTransmission(RTC_ADDR); Wire.write(0x04);
  if(Wire.endTransmission(true)!=0) return false;
  if(Wire.requestFrom(RTC_ADDR,3)!=3) return false;
  ss=bcd2dec(Wire.read()&0x7F); mm=bcd2dec(Wire.read()&0x7F); hh=bcd2dec(Wire.read()&0x3F);
  return true;
}
void rtcSet(int hh,int mm,int ss){
  Wire.beginTransmission(RTC_ADDR); Wire.write(0x04);
  Wire.write(dec2bcd((uint8_t)ss)); Wire.write(dec2bcd((uint8_t)mm)); Wire.write(dec2bcd((uint8_t)hh));
  Wire.endTransmission(true);
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
  SCR_SNAKE, SCR_MOLE, SCR_2048, SCR_SETTINGS,
  SCR_APPS, SCR_LEVEL, SCR_HORIZON, SCR_STEPS, SCR_CLOCK,
  SCR_STOPWATCH, SCR_TIMER, SCR_BATTERY, SCR_FLASH, SCR_SYSINFO
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
void handle2048 (const UiEvent &ev);
bool hitBack    (const UiEvent &ev);
bool onAvatar   (const UiEvent &ev);
void handleSettings(const UiEvent &ev);
void handleApps (const UiEvent &ev);
void handleLevel(const UiEvent &ev);
void handleMarble(const UiEvent &ev);
void handleSteps(const UiEvent &ev);
void handleClock(const UiEvent &ev);
void handleStopwatch(const UiEvent &ev);
void handleTimer(const UiEvent &ev);
void handleBattery(const UiEvent &ev);
void handleFlash(const UiEvent &ev);
void handleSysinfo(const UiEvent &ev);
bool appBack(const UiEvent &ev);

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

/* ----------------------- AVATAR (procedural) ------------------------
   A LEGO-ish minifig head drawn in code. No image arrays needed.
   face: 0 idle  1 happy  2 sleep  3 agent  4 sad/sick
   To use your real artwork later, replace drawAvatar() with a call that
   blits a 466x466 RGB565 array from PROGMEM.                           */
void drawAvatar(int face, int cx, int cy, int scale /*percent 100=normal*/) {
  Theme &th = THEMES[pet.equipped];
  int r = 70 * scale / 100;
  // head (rounded square)
  gfx->fillRoundRect(cx - r, cy - r, 2 * r, 2 * r, r / 2, th.head);
  // stud on top
  gfx->fillRoundRect(cx - r / 4, cy - r - r / 6, r / 2, r / 4, r / 8, th.head);
  gfx->drawRoundRect(cx - r / 4, cy - r - r / 6, r / 2, r / 4, r / 8, C_DIMGREY);

  int eyeY = cy - r / 5;
  int eyeDX = r / 2;
  int eyeR = r / 7;
  if (face == 2) {               // sleeping: closed eyes
    gfx->drawFastHLine(cx - eyeDX - eyeR, eyeY, 2 * eyeR, C_BLACK);
    gfx->drawFastHLine(cx + eyeDX - eyeR, eyeY, 2 * eyeR, C_BLACK);
    textC("z", cx + r, cy - r, 2, C_CYAN);
  } else {
    gfx->fillCircle(cx - eyeDX, eyeY, eyeR, C_BLACK);
    gfx->fillCircle(cx + eyeDX, eyeY, eyeR, C_BLACK);
    // little catchlight
    gfx->fillCircle(cx - eyeDX + eyeR / 3, eyeY - eyeR / 3, eyeR / 3, C_WHITE);
    gfx->fillCircle(cx + eyeDX + eyeR / 3, eyeY - eyeR / 3, eyeR / 3, C_WHITE);
  }

  int mY = cy + r / 3;
  if (face == 1) {               // big smile
    gfx->fillArc(cx, mY - r / 4, r / 2, r / 2 - 6, 20, 160, C_BLACK);
  } else if (face == 4) {        // frown
    gfx->fillArc(cx, mY + r / 2, r / 2, r / 2 - 5, 200, 340, C_BLACK);
  } else if (face == 3) {        // agent: neutral + visor glow
    gfx->drawFastHLine(cx - r / 3, mY, 2 * r / 3, C_BLACK);
  } else {                       // idle: gentle smile
    gfx->fillArc(cx, mY - r / 6, r / 2 - 4, r / 2 - 9, 30, 150, C_BLACK);
  }

  if (face == 3) {               // agent aura ring
    for (int i = 0; i < 3; i++)
      gfx->drawCircle(cx, cy, r + 10 + i * 7, th.accent);
  }
}

/* celebration splash on level up */
void celebrate() {
  gfx->draw16bitRGBBitmap(0, 0, img_celebration, 466, 466);   // full-screen render
  char buf[24]; snprintf(buf, sizeof(buf), "LEVEL %d!", pet.level);
  gfx->fillRect(60, 24, 346, 30, C_BLACK);
  textC(buf, LCD_W / 2, 38, 3, THEMES[pet.equipped].accent);
  gfx->fillRect(130, 420, 206, 24, C_BLACK);
  textC("+20 studs", LCD_W / 2, 432, 2, C_WHITE);
  delay(1400);
  screenDirty = true;
}

/* ----------------------- STAT BAR WIDGET ---------------------------- */
// Draws one labelled bar; caches last value so it only repaints on change.
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
   ANIMATED AVATAR  (home screen)
   Draws Alon's face with squash/stretch, blinking, and a pet reaction
   (bounce + happy eyes + rising hearts). Only the avatar box is redrawn.
   =================================================================== */
// The avatar now fills the whole 466x466 screen (full artwork, uncropped).
bool     petAnimOn = false;
uint32_t petAnimStart = 0;

const uint16_t* homeImg(){ return pet.sleeping ? img_sleep : img_idle; }
void blitFull(const uint16_t* img){ gfx->draw16bitRGBBitmap(0, 0, img, 466, 466); }

// Pet reaction: a lively particle burst (sparkles + rising hearts) animated
// over Alon's real idle image - no frame-swap, it actually moves.
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
  for(int i=0;i<18;i++){
    float a=(i/18.0f)*6.2832f, sp=2.2f+(esp_random()%120)/40.0f;
    sparks[i].x=233; sparks[i].y=175;
    sparks[i].vx=cosf(a)*sp; sparks[i].vy=sinf(a)*sp-2.0f;
    sparks[i].col=(i%3==0)?C_YELLOW:(i%3==1)?C_CYAN:0xF81F;
  }
}
void animateAvatar(){
  if(!petAnimOn) return;
  uint32_t now=millis(); int age=now-petAnimStart;
  if(age>=PET_MS){ petAnimOn=false; screenDirty=true; return; }   // settle back to home
  if(now-lastPetFrame<40) return; lastPetFrame=now;
  blitFull(img_idle);                         // re-blit clears last frame's particles
  for(int i=0;i<18;i++){
    sparks[i].x+=sparks[i].vx; sparks[i].y+=sparks[i].vy; sparks[i].vy+=0.18f;
    int sx=(int)sparks[i].x, sy=(int)sparks[i].y;
    if(sx>3&&sx<463&&sy>3&&sy<463) gfx->fillCircle(sx,sy,3,sparks[i].col);
  }
  int hy = 205 - age*70/PET_MS;               // hearts float up
  drawHeartAt(205,hy,0xF81F); drawHeartAt(262,hy-14,C_RED);
}

/* ===================================================================
   HOME SCREEN
   =================================================================== */
// Bottom menu buttons (kept inside the visible circle y<=376)
struct Btn { int x, y, w, h; const char *label; };
Btn homeBtns[4] = {
  { 96, 322, 128, 44, "CARE"  },
  {242, 322, 128, 44, "PLAY"  },
  { 96, 372 - 0,  0, 0, "" },   // placeholder (unused)
  {242, 372 - 0,  0, 0, "" },
};
// Second row inside circle
Btn homeBtns2[2] = {
  { 96, 268, 128, 44, "SHOP" },
  {242, 268, 128, 44, "Zzz"  },
};

uint32_t lastStudsShown = 0xFFFFFFFF;
int lastLevelShown = -1;

void miniBar(int val,int x,int y,int w){
  val=clampi(val,0,100);
  gfx->fillRoundRect(x,y,w,10,4,C_DIMGREY);
  uint16_t c=val>50?C_GREEN:(val>25?C_YELLOW:C_RED);
  int fw=(w-2)*val/100; gfx->fillRoundRect(x+1,y+1,fw<3?3:fw,8,3,c);
}

void drawHomeStatic() {
  blitFull(homeImg());                       // full-screen LEGO avatar art
  lastStudsShown = 0xFFFFFFFF; lastLevelShown = -1;
}

// A clean little battery glyph with fill.
void drawBatteryIcon(int x,int y,int pct){
  gfx->drawRoundRect(x,y,34,16,3,C_WHITE);
  gfx->fillRect(x+34,y+5,3,6,C_WHITE);
  if(pct>=0){ uint16_t c=pct<20?C_RED:(pct<50?0xFD20:C_GREEN);
    int w=28*pct/100; gfx->fillRoundRect(x+3,y+3,w<3?3:w,10,2,c); }
}

void drawHomeDynamic() {
  if (petAnimOn) return;                      // don't draw HUD over the reaction
  static uint32_t lastClk=0, lastBatMs=0;
  static int hh=-1, mm=-1, ss=0, bat=-1, lastSec=-2, lastBat=-2;
  uint32_t now=millis();
  if (rtcOK && now-lastClk>500){ lastClk=now; rtcGet(hh,mm,ss); }
  if (now-lastBatMs>5000 || lastBat==-2){ lastBatMs=now; bat=batteryPercent(); }

  // Top HUD: live clock + battery glyph (centre-top, inside the circle). No level/studs.
  if (ss!=lastSec || bat!=lastBat){
    lastSec=ss; lastBat=bat;
    gfx->fillRoundRect(120, 24, 226, 40, 12, C_BLACK);
    char c[8]; if(hh>=0) snprintf(c,sizeof(c),"%02d:%02d",hh,mm); else snprintf(c,sizeof(c),"--:--");
    textC(c, 192, 44, 2, C_CYAN);
    drawBatteryIcon(272, 36, bat);
  }
}

void handleHome(const UiEvent &ev) {
  bool central = (ev.y>=44 && ev.y<=424);     // avatar area (not the HUD strips)
  if (ev.type == EV_TAP && central) {         // tap = pet
    pet.fun = clampi(pet.fun + 6, 0, 100);
    reward(1, 3);
    triggerPet(); saveState();
  }
  else if (ev.type == EV_HOLD && central) { screen = SCR_AGENT; screenDirty = true; }
  else if (ev.type == EV_SWIPE_U || ev.type == EV_SWIPE_L || ev.type == EV_SWIPE_R) { screen = SCR_APPS; screenDirty = true; }
  else if (ev.type == EV_SWIPE_D) { screen = SCR_SETTINGS; screenDirty = true; }
}

/* ------- shared BACK/HOME button for every sub-screen ------- */
#define BK_X 158
#define BK_Y 346
#define BK_W 150
#define BK_H 38
void drawBackBtn(){
  gfx->fillRoundRect(BK_X,BK_Y,BK_W,BK_H,12,THEMES[pet.equipped].accent);
  textC("HOME", BK_X+BK_W/2, BK_Y+BK_H/2, 2, C_BLACK);
}
bool hitBack(const UiEvent &ev){
  return ev.type==EV_DOWN && ev.x>=BK_X && ev.x<=BK_X+BK_W &&
         ev.y>=BK_Y && ev.y<=BK_Y+BK_H;
}

/* ===================================================================
   CARE SCREEN  (feed / clean / medicine)
   =================================================================== */
Btn careBtns[3] = {
  {133, 150, 200, 54, "FEED  (-5)"},
  {133, 218, 200, 54, "CLEAN (-3)"},
  {133, 286, 200, 54, "TREAT (-8)"},
};
void drawCare() {
  gfx->fillScreen(C_BLACK);
  textC("CARE", LCD_W/2, 70, 3, THEMES[pet.equipped].accent);
  for (int i=0;i<3;i++){ Btn&b=careBtns[i];
    gfx->fillRoundRect(b.x,b.y,b.w,b.h,14,C_DIMGREY);
    gfx->drawRoundRect(b.x,b.y,b.w,b.h,14,THEMES[pet.equipped].accent);
    textC(b.label,b.x+b.w/2,b.y+b.h/2,2,C_WHITE);
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
    gfx->fillRoundRect(100,y,266,46,12,C_DIMGREY);
    gfx->drawRoundRect(100,y,266,46,12,THEMES[i].accent);
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
  blitFull(img_agent);                        // full-screen agent render (has its own aura)
  gfx->fillRect(0, 0, 466, 28, C_BLACK);
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
  gfx->fillRoundRect(120,106,44,40,10,C_DIMGREY); textC("-",142,126,3,C_WHITE);
  char b[8]; snprintf(b,sizeof(b),"%d",brightness); textC(b,233,126,2,C_CYAN);
  gfx->fillRoundRect(302,106,44,40,10,C_DIMGREY); textC("+",324,126,3,C_WHITE);
  // Clock
  int hh=0,mm=0,ss=0; if(rtcOK){ rtcGet(hh,mm,ss); }
  textC(rtcOK?"Clock":"Clock (no RTC)", 233, 168, 2, C_WHITE);
  gfx->fillRoundRect(96,186,34,36,8,C_DIMGREY);  textC("-",113,204,2,C_WHITE);
  char hb[6]; snprintf(hb,sizeof(hb),"%02d",hh);  textC(hb,160,204,2,C_CYAN);
  gfx->fillRoundRect(178,186,34,36,8,C_DIMGREY); textC("+",195,204,2,C_WHITE);
  textC(":",233,204,2,C_WHITE);
  gfx->fillRoundRect(254,186,34,36,8,C_DIMGREY); textC("-",271,204,2,C_WHITE);
  char mb[6]; snprintf(mb,sizeof(mb),"%02d",mm);  textC(mb,318,204,2,C_CYAN);
  gfx->fillRoundRect(336,186,34,36,8,C_DIMGREY); textC("+",353,204,2,C_WHITE);
  // Sleep + Sound toggles, then Reset
  gfx->fillRoundRect(96,240,120,40,12,0x4A69);  textC(pet.sleeping?"WAKE":"SLEEP",156,260,2,C_WHITE);
  gfx->fillRoundRect(250,240,120,40,12,silent?C_RED:0x2FEC); textC(silent?"SILENT":"SOUND",310,260,2,C_BLACK);
  gfx->fillRoundRect(143,292,180,38,12,C_RED); textC("RESET",233,311,2,C_WHITE);
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
Btn gameBtns[3]={
  {133,120,200,54,"SNAKE"},
  {133,190,200,54,"WHACK"},
  {133,260,200,54,"2048"},
};
void drawGames(){
  gfx->fillScreen(C_BLACK);
  textC("GAMES", LCD_W/2, 68, 3, THEMES[pet.equipped].accent);
  for (int i=0;i<3;i++){ Btn&b=gameBtns[i];
    gfx->fillRoundRect(b.x,b.y,b.w,b.h,14,C_DIMGREY);
    gfx->drawRoundRect(b.x,b.y,b.w,b.h,14,THEMES[pet.equipped].accent);
    textC(b.label,b.x+b.w/2,b.y+b.h/2,2,C_WHITE);
  }
  textC("win studs!", LCD_W/2, 326, 1, C_GREY);
  drawBackBtn();
}
void handleGames(const UiEvent &ev){
  if (isSwipe(ev) || hitBack(ev)){ screen=SCR_HOME; screenDirty=true; return; }
  if (ev.type!=EV_DOWN) return;
  for (int i=0;i<3;i++){ Btn&b=gameBtns[i];
    if (ev.x>=b.x&&ev.x<=b.x+b.w&&ev.y>=b.y&&ev.y<=b.y+b.h){
      screen = (i==0)?SCR_SNAKE:(i==1)?SCR_MOLE:SCR_2048;
      screenDirty=true; return;
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

/* ------------------------- WHACK-A-MOLE ----------------------------- */
int moleIdx; uint32_t moleTick; int moleScore; int moleMiss; uint32_t moleStart;
const int MOLE_X[9]={133,233,333,133,233,333,133,233,333};
const int MOLE_Y[9]={150,150,150,240,240,240,330,330,330};
void drawMoleHole(int i,bool up){
  gfx->fillCircle(MOLE_X[i],MOLE_Y[i],36,up?THEMES[pet.equipped].head:C_DIMGREY);
  if (up){ gfx->fillCircle(MOLE_X[i]-12,MOLE_Y[i]-6,5,C_BLACK); gfx->fillCircle(MOLE_X[i]+12,MOLE_Y[i]-6,5,C_BLACK); }
}
void drawMoleStatic(){
  gfx->fillScreen(C_BLACK);
  textC("WHACK", LCD_W/2, 40, 2, THEMES[pet.equipped].accent);
  moleScore=0; moleMiss=0; moleIdx=esp_random()%9; moleTick=millis(); moleStart=millis();
  for (int i=0;i<9;i++) drawMoleHole(i,false);
  drawMoleHole(moleIdx,true);
}
void stepMole(){
  uint32_t now=millis();
  if (now-moleStart>30000){               // 30s round
    reward(moleScore,moleScore*2); saveState();
    gfx->fillRect(0,60,LCD_W,40,C_BLACK);
    char b[24]; snprintf(b,sizeof(b),"SCORE %d",moleScore); textC(b,LCD_W/2,80,2,C_GREEN);
    textC("swipe down: exit",LCD_W/2,LCD_H-28,1,C_GREY);
    return;
  }
  if (now-moleTick>850){
    drawMoleHole(moleIdx,false);
    moleIdx=esp_random()%9; moleTick=now;
    drawMoleHole(moleIdx,true);
  }
  gfx->fillRect(0,64,LCD_W,16,C_BLACK);
  char b[20]; snprintf(b,sizeof(b),"hits %d",moleScore); textC(b,LCD_W/2,72,1,C_CYAN);
}
void handleMole(const UiEvent &ev){
  if (ev.type==EV_HOLD){ screen=SCR_GAMES; screenDirty=true; return; }   // hold = quit
  if (ev.type==EV_SWIPE_D && millis()-moleStart>30000){ screen=SCR_GAMES; screenDirty=true; return; }
  if (ev.type!=EV_DOWN) return;
  int dx=ev.x-MOLE_X[moleIdx], dy=ev.y-MOLE_Y[moleIdx];
  if (dx*dx+dy*dy<40*40){ moleScore++; drawMoleHole(moleIdx,false); moleIdx=esp_random()%9; drawMoleHole(moleIdx,true); moleTick=millis(); }
}

/* ------------------------- 2048 ------------------------------------- */
int bd[4][4]; bool g2048over;
const int G2_OX=101, G2_OY=120, G2_CELL=66;
uint16_t tileColor(int v){
  switch(v){case 2:return 0x9492;case 4:return 0xBDD7;case 8:return 0xFD60;case 16:return 0xFCA0;
    case 32:return 0xFB40;case 64:return 0xF9A6;case 128:return 0xEF20;case 256:return 0xEEC0;
    case 512:return 0xEE60;case 1024:return 0xE640;default:return 0xE600;}
}
void g2Spawn(){ int e[16],n=0; for(int r=0;r<4;r++)for(int c=0;c<4;c++)if(!bd[r][c])e[n++]=r*4+c;
  if(!n)return; int k=e[esp_random()%n]; bd[k/4][k%4]=(esp_random()%10)?2:4; }
void g2DrawTile(int r,int c){
  int x=G2_OX+c*G2_CELL, y=G2_OY+r*G2_CELL; int v=bd[r][c];
  gfx->fillRoundRect(x+2,y+2,G2_CELL-4,G2_CELL-4,8,v?tileColor(v):C_DIMGREY);
  if(v){ char b[6]; snprintf(b,sizeof(b),"%d",v); textC(b,x+G2_CELL/2,y+G2_CELL/2,v<128?3:2,C_BLACK); }
}
void drawG2Static(){
  gfx->fillScreen(C_BLACK);
  textC("2048",LCD_W/2,54,3,THEMES[pet.equipped].accent);
  for(int r=0;r<4;r++)for(int c=0;c<4;c++)bd[r][c]=0;
  g2048over=false; g2Spawn(); g2Spawn();
  for(int r=0;r<4;r++)for(int c=0;c<4;c++)g2DrawTile(r,c);
  textC("swipe to move   down-hold: exit",LCD_W/2,404,1,C_GREY);
}
bool g2Slide(int dir){ // 0 up 1 right 2 down 3 left ; returns moved
  bool moved=false; int merged[4][4]={{0}};
  for(int it=0;it<4;it++){
    for(int r=0;r<4;r++)for(int c=0;c<4;c++){
      int rr=r,cc=c;
      if(bd[rr][cc]==0)continue;
      int nr=rr,nc=cc;
      if(dir==0)nr=rr-1; else if(dir==2)nr=rr+1; else if(dir==3)nc=cc-1; else nc=cc+1;
      if(nr<0||nr>3||nc<0||nc>3)continue;
      if(bd[nr][nc]==0){ bd[nr][nc]=bd[rr][cc]; bd[rr][cc]=0; moved=true; }
      else if(bd[nr][nc]==bd[rr][cc]&&!merged[nr][nc]&&!merged[rr][cc]){
        bd[nr][nc]*=2; bd[rr][cc]=0; merged[nr][nc]=1; moved=true; reward(1,1);
      }
    }
  }
  return moved;
}
void g2Move(int dir){
  if(g2048over)return;
  if(g2Slide(dir)){ g2Spawn();
    for(int r=0;r<4;r++)for(int c=0;c<4;c++)g2DrawTile(r,c);
    saveState();
  }
}
void handle2048(const UiEvent &ev){
  if(ev.type==EV_HOLD){ screen=SCR_GAMES; screenDirty=true; return; }
  if(ev.type==EV_SWIPE_U)g2Move(0);
  else if(ev.type==EV_SWIPE_R)g2Move(1);
  else if(ev.type==EV_SWIPE_D)g2Move(2);
  else if(ev.type==EV_SWIPE_L)g2Move(3);
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
  if(!imu.getDataReady())return;
  float ax,ay,az;
  if(!imu.getAccelerometer(ax,ay,az))return;
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
  {"System", SCR_SYSINFO},
};
const int APP_COUNT = sizeof(APPS)/sizeof(APPS[0]);
#define APPS_PER_PAGE 6
int appPage = 0;

void appTileXY(int i,int &x,int &y){ int c=i%3, r=i/3; x=24+c*144; y=100+r*100; }

void drawApps(){
  gfx->fillScreen(C_BLACK);
  textC("APPS", 233, 40, 3, THEMES[pet.equipped].accent);
  int start=appPage*APPS_PER_PAGE;
  for(int i=0;i<APPS_PER_PAGE;i++){
    int idx=start+i; if(idx>=APP_COUNT) break;
    int x,y; appTileXY(i,x,y);
    gfx->fillRoundRect(x,y,130,88,16,0x4A69);   // clean flat tile, no outline
    textC(APPS[idx].name, x+65, y+44, 2, C_WHITE);
  }
  int pages=(APP_COUNT+APPS_PER_PAGE-1)/APPS_PER_PAGE;
  for(int p=0;p<pages;p++)
    gfx->fillCircle(233-(pages-1)*9+p*18, 312, 4, p==appPage?THEMES[pet.equipped].accent:C_DIMGREY);
  textC("swipe L/R: pages", 233, 334, 1, C_GREY);
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

/* ---------------- LEVELER (bubble level, uses accelerometer) ------ */
void drawLevelStatic(){ appHeader("LEVELER"); drawBackBtn(); }
void tickLevel(){
  static int px=233,py=210; static uint32_t last=0;
  if(millis()-last<45) return; last=millis();
  if(!imuOK){ textC("IMU not available",233,210,2,C_RED); return; }
  if(!imuRead()) return;
  gfx->fillCircle(px,py,18,C_BLACK);
  int bx=233-(int)(accX*150), by=210+(int)(accY*150);
  float dx=bx-233, dy=by-210, d=sqrtf(dx*dx+dy*dy);
  if(d>104){ bx=233+(int)(dx*104/d); by=210+(int)(dy*104/d); }
  gfx->drawCircle(233,210,120,C_DIMGREY);
  gfx->drawCircle(233,210,30,C_GREEN);
  bool level=(abs(bx-233)<16 && abs(by-210)<16);
  gfx->fillCircle(bx,by,16, level?C_GREEN:C_CYAN);
  px=bx; py=by;
  gfx->fillRect(120,348,226,22,C_BLACK);
  char b[28]; snprintf(b,sizeof(b),"%d, %d deg",(int)(accX*90),(int)(accY*90));
  textC(level?"LEVEL!":b,233,356,2, level?C_GREEN:C_WHITE);
}
void handleLevel(const UiEvent &ev){ appBack(ev); }

/* ---------------- MARBLE (tilt a ball around a bowl) -------------- */
float mbx=233,mby=233,mvx=0,mvy=0;
void drawMarbleStatic(){ appHeader("MARBLE"); gfx->drawCircle(233,236,150,C_DIMGREY); mbx=mby=233; mvx=mvy=0; drawBackBtn(); }
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
void drawStepsStatic(){ appHeader("STEPS"); gfx->fillRoundRect(133,300,200,40,10,C_DIMGREY); textC("RESET",233,320,2,C_WHITE); drawBackBtn(); }
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
  gfx->fillRoundRect(96,300,120,42,10,C_GREEN); textC(swRun?"STOP":"START",156,321,2,C_BLACK);
  gfx->fillRoundRect(250,300,120,42,10,C_DIMGREY); textC("RESET",310,321,2,C_WHITE);
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
  gfx->fillRoundRect(96,150,44,44,10,C_DIMGREY); textC("-",118,172,3,C_WHITE);
  gfx->fillRoundRect(326,150,44,44,10,C_DIMGREY); textC("+",348,172,3,C_WHITE);
  gfx->fillRoundRect(96,300,274,42,10,timerRun?C_RED:C_GREEN); textC(timerRun?"STOP":"START",233,321,2,C_BLACK);
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
  imuOK = false;
  (void)imuProbe; (void)imu;

  // ---- Real-time clock (PCF85063) for the home clock + settings ----
  rtcOK = rtcProbe();
  if(!rtcOK) Serial.println("rtc begin failed");

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

// SHORT side button (BOOT): wake + jump home from anywhere.
void checkBoot(){
  bool lvl = digitalRead(BOOT_BTN);         // LOW = pressed
  if(bootPrev && !lvl){
    if(pet.sleeping){ pet.sleeping=false; gfx->setBrightness(brightness); }
    if(screen!=SCR_HOME){ screen=SCR_HOME; screenDirty=true; }
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
    case SCR_SNAKE: drawSnakeStatic(); break;
    case SCR_MOLE:  drawMoleStatic();  break;
    case SCR_2048:  drawG2Static();    break;
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
  }
}

void loop(){
#if TOUCH_TEST
  touchTestLoop();
  return;
#endif
  if(screenDirty){ screenDirty=false; enterScreen(); return; }

  UiEvent ev = pollGesture();

  switch(screen){
    case SCR_HOME:  handleHome(ev);  break;
    case SCR_CARE:  handleCare(ev);  break;
    case SCR_SHOP:  handleShop(ev);  break;
    case SCR_GAMES: handleGames(ev); break;
    case SCR_STATS: handleStats(ev); break;
    case SCR_AGENT: handleAgent(ev); break;
    case SCR_SNAKE: handleSnake(ev); break;
    case SCR_MOLE:  handleMole(ev);  break;
    case SCR_2048:  handle2048(ev);  break;
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
  }

  // if a handler changed screens, let the next loop draw it fresh
  if(screenDirty){ inputCooldownUntil = millis() + 300; resetGesture(); return; }

  // per-screen animation / ticking
  switch(screen){
    case SCR_HOME:  drawHomeDynamic(); animateAvatar(); break;
    case SCR_LEVEL:   tickLevel();     break;
    case SCR_HORIZON: tickMarble();    break;
    case SCR_STEPS:   tickSteps();     break;
    case SCR_CLOCK:   tickClock();     break;
    case SCR_STOPWATCH: tickStopwatch(); break;
    case SCR_TIMER:   tickTimer();     break;
    case SCR_BATTERY: tickBattery();   break;
    case SCR_SYSINFO: tickSysinfo();   break;
    case SCR_AGENT: drawAgentDynamic();break;
    case SCR_SNAKE: stepSnake();       break;
    case SCR_MOLE:  stepMole();        break;
    default: break;
  }

#if TOUCH_DEBUG
  drawTouchDebug();
#endif
  checkShake();
  checkBoot();
  delay(2);          // keeps the touch loop tight & responsive
}
