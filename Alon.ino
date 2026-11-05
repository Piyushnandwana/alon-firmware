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
#define TOUCH_FLIP_X 0
#define TOUCH_FLIP_Y 0
#define TOUCH_SWAP_XY 0

/* ----------------------- DISPLAY OBJECTS ---------------------------- */
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS /*CS*/, LCD_SCLK /*SCK*/, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus, LCD_RST, 0 /*rotation*/, LCD_W, LCD_H, 6, 0, 0, 0);

TouchDrvCST92xx touch;
SensorQMI8658   imu;
bool imuOK = false;

Preferences prefs;

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
  SCR_SNAKE, SCR_MOLE, SCR_2048
};
Screen screen = SCR_HOME;
bool    screenDirty = true;   // force a full redraw of the current screen

/* ----------------------- TOUCH ENGINE ------------------------------- */
// The CST9217 interrupts only on state edges, so we also poll while a
// finger is held and on a short heartbeat.
volatile bool tpPending = false;
void IRAM_ATTR onTouchISR() { tpPending = true; }

struct Gesture {
  bool  down       = false;   // finger currently on glass
  int   downX, downY;         // where it first touched
  int   curX,  curY;          // latest position
  uint32_t downMs;            // when it went down
  bool  swiped     = false;   // a swipe already fired this gesture
  bool  held       = false;   // a long-press already fired this gesture
  uint32_t lastSeen;          // last ms we saw a point
} g;

enum EvType { EV_NONE, EV_DOWN, EV_TAP, EV_SWIPE_L, EV_SWIPE_R, EV_SWIPE_U, EV_SWIPE_D, EV_HOLD };
struct Event { EvType type = EV_NONE; int x = 0, y = 0; };

#define SWIPE_THRESH 60     // px of drag before a swipe fires (mid-drag)
#define TAP_SLOP     18     // movement under this on release = a tap
#define HOLD_MS      650    // long-press duration
#define TP_HEARTBEAT 120    // ms safety poll

uint32_t lastPollMs = 0;

// Read one raw point; returns true if a finger is present.
bool readTouch(int &x, int &y) {
  int16_t px[2], py[2];
  uint8_t n = touch.getPoint(px, py, 2);
  if (n == 0) return false;
  int rx = px[0], ry = py[0];
#if TOUCH_SWAP_XY
  int t = rx; rx = ry; ry = t;
#endif
#if TOUCH_FLIP_X
  rx = (LCD_W - 1) - rx;
#endif
#if TOUCH_FLIP_Y
  ry = (LCD_H - 1) - ry;
#endif
  if (rx < 0) rx = 0; if (rx > LCD_W - 1) rx = LCD_W - 1;
  if (ry < 0) ry = 0; if (ry > LCD_H - 1) ry = LCD_H - 1;
  x = rx; y = ry;
  return true;
}

// Produce at most one high-level event per call. Button actions use EV_DOWN;
// swipes fire mid-drag; long-press fires EV_HOLD; a short lift is EV_TAP.
Event pollGesture() {
  Event ev;
  uint32_t now = millis();
  bool due = tpPending || g.down || (now - lastPollMs >= TP_HEARTBEAT);
  if (!due) return ev;
  tpPending = false;
  lastPollMs = now;

  int x, y;
  bool present = readTouch(x, y);

  if (present) {
    if (!g.down) {                       // ---- finger just landed ----
      g.down = true; g.swiped = false; g.held = false;
      g.downX = g.curX = x; g.downY = g.curY = y;
      g.downMs = now; g.lastSeen = now;
      ev.type = EV_DOWN; ev.x = x; ev.y = y;   // buttons act immediately
      return ev;
    }
    g.curX = x; g.curY = y; g.lastSeen = now;
    int dx = x - g.downX, dy = y - g.downY;
    if (!g.swiped && !g.held) {
      if (abs(dx) >= SWIPE_THRESH || abs(dy) >= SWIPE_THRESH) {
        g.swiped = true;                 // ---- swipe mid-drag ----
        if (abs(dx) > abs(dy)) ev.type = (dx > 0) ? EV_SWIPE_R : EV_SWIPE_L;
        else                   ev.type = (dy > 0) ? EV_SWIPE_D : EV_SWIPE_U;
        return ev;
      }
      if (now - g.downMs >= HOLD_MS) {   // ---- long press in place ----
        g.held = true;
        ev.type = EV_HOLD; ev.x = x; ev.y = y;
        return ev;
      }
    }
  } else {
    // No point. Only treat as "up" once the heartbeat confirms it's gone.
    if (g.down && (now - g.lastSeen >= TP_HEARTBEAT)) {
      bool wasTap = (!g.swiped && !g.held &&
                     abs(g.curX - g.downX) < TAP_SLOP &&
                     abs(g.curY - g.downY) < TAP_SLOP);
      g.down = false;
      if (wasTap) { ev.type = EV_TAP; ev.x = g.downX; ev.y = g.downY; }
    }
  }
  return ev;
}

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
void beep(int /*freq*/, int /*ms*/) { /* Phase 2 */ }

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
  gfx->fillScreen(C_BLACK);
  drawAvatar(1, LCD_W / 2, LCD_H / 2 - 10, 120);
  char buf[24]; snprintf(buf, sizeof(buf), "LEVEL %d!", pet.level);
  textC(buf, LCD_W / 2, 120, 3, THEMES[pet.equipped].accent);
  textC("+20 studs", LCD_W / 2, LCD_H - 120, 2, C_WHITE);
  for (int r = 20; r < 230; r += 22) { gfx->drawCircle(LCD_W/2, LCD_H/2, r, C_VIOLET); }
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

void drawHomeStatic() {
  gfx->fillScreen(C_BLACK);
  // top HUD
  textC("ALON", LCD_W / 2, 40, 3, THEMES[pet.equipped].accent);
  lastStudsShown = 0xFFFFFFFF; lastLevelShown = -1;
  for (int i = 0; i < 4; i++) lastBar[i] = -1;
  // avatar
  int face = pet.sleeping ? 2 : (pet.fun > 55 && pet.hunger > 40 ? 1 : (pet.hunger < 25 || pet.clean < 25 ? 4 : 0));
  drawAvatar(face, LCD_W / 2, 150, 95);
  // buttons
  Btn *all[] = {&homeBtns[0], &homeBtns[1], &homeBtns2[0], &homeBtns2[1]};
  for (int i = 0; i < 4; i++) {
    Btn *b = all[i];
    gfx->fillRoundRect(b->x, b->y, b->w, b->h, 12, C_DIMGREY);
    gfx->drawRoundRect(b->x, b->y, b->w, b->h, 12, THEMES[pet.equipped].accent);
    textC(b->label, b->x + b->w / 2, b->y + b->h / 2, 2, C_WHITE);
  }
  textC("swipe up: games   hold face: agent", LCD_W/2, 404, 1, C_GREY);
}

void drawHomeDynamic() {
  // studs + level line
  if (pet.studs != lastStudsShown || pet.level != lastLevelShown) {
    lastStudsShown = pet.studs; lastLevelShown = pet.level;
    gfx->fillRect(60, 62, LCD_W - 120, 20, C_BLACK);
    char buf[40];
    snprintf(buf, sizeof(buf), "Lv %d   %lu studs", pet.level, (unsigned long)pet.studs);
    textC(buf, LCD_W / 2, 72, 2, C_CYAN);
  }
  // 4 stat bars in two columns under avatar
  drawStatBar(0, "HUNGER", pet.hunger, 70, 224, 150);
  drawStatBar(1, "ENERGY", pet.energy, 246, 224, 150);
  // (clean/fun live on the STATS screen to keep home uncluttered)
}

void handleHome(const Event &ev) {
  Btn *all[] = {&homeBtns[0], &homeBtns[1], &homeBtns2[0], &homeBtns2[1]};
  if (ev.type == EV_DOWN) {
    // avatar tap zone -> pet him (fun up)
    int dxp = ev.x - LCD_W/2, dyp = ev.y - 150;
    if (dxp*dxp + dyp*dyp < 95*95) {
      pet.fun = clampi(pet.fun + 6, 0, 100);
      reward(1, 3);
      screenDirty = true; saveState();
      return;
    }
    for (int i = 0; i < 4; i++) {
      Btn *b = all[i];
      if (ev.x >= b->x && ev.x <= b->x + b->w && ev.y >= b->y && ev.y <= b->y + b->h) {
        if (!strcmp(b->label, "CARE")) screen = SCR_CARE;
        else if (!strcmp(b->label, "PLAY")) { pet.fun = clampi(pet.fun+8,0,100); pet.energy = clampi(pet.energy-4,0,100); reward(2,5); saveState(); screenDirty=true; }
        else if (!strcmp(b->label, "SHOP")) screen = SCR_SHOP;
        else if (!strcmp(b->label, "Zzz"))  { pet.sleeping = !pet.sleeping; gfx->setBrightness(pet.sleeping?30:140); }
        screenDirty = true;
        return;
      }
    }
  } else if (ev.type == EV_HOLD) {
    int dxp = ev.x - LCD_W/2, dyp = ev.y - 150;
    if (dxp*dxp + dyp*dyp < 110*110) { screen = SCR_AGENT; screenDirty = true; }
  } else if (ev.type == EV_SWIPE_U) { screen = SCR_GAMES; screenDirty = true; }
  else if (ev.type == EV_SWIPE_L) { screen = SCR_STATS; screenDirty = true; }
  else if (ev.type == EV_SWIPE_R) { screen = SCR_STATS; screenDirty = true; }
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
  textC(buf, LCD_W/2, 360, 2, C_CYAN);
  textC("swipe down: back", LCD_W/2, 400, 1, C_GREY);
}
void handleCare(const Event &ev){
  if (ev.type==EV_SWIPE_D){ screen=SCR_HOME; screenDirty=true; return; }
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
  textC("swipe down: back", LCD_W/2, 404, 1, C_GREY);
}
void handleShop(const Event &ev){
  if (ev.type==EV_SWIPE_D){ screen=SCR_HOME; screenDirty=true; return; }
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
  textC(buf,LCD_W/2,340,2,C_CYAN);
  textC("swipe right: back", LCD_W/2, 404, 1, C_GREY);
}
void handleStats(const Event &ev){
  if (ev.type==EV_SWIPE_R||ev.type==EV_SWIPE_L||ev.type==EV_SWIPE_D){ screen=SCR_HOME; screenDirty=true; }
}

/* ===================================================================
   AGENT SCREEN  (voice-assistant state - animated aura + level bar)
   Mic capture (ES7210) is Phase 2; the bar animates a placeholder.
   =================================================================== */
uint32_t agentAnim=0;
void drawAgentStatic(){
  gfx->fillScreen(C_BLACK);
  textC("AGENT", LCD_W/2, 46, 3, THEMES[pet.equipped].accent);
  drawAvatar(3, LCD_W/2, 180, 95);
  textC("listening...", LCD_W/2, 300, 2, C_CYAN);
  textC("swipe down: back", LCD_W/2, 404, 1, C_GREY);
}
void drawAgentDynamic(){
  uint32_t now=millis();
  if (now-agentAnim<70) return; agentAnim=now;
  // pulsing aura
  static int ph=0; ph=(ph+1)%3;
  Theme &th=THEMES[pet.equipped];
  for (int i=0;i<3;i++){
    uint16_t c = (i==ph)? th.accent : C_BLACK;
    gfx->drawCircle(LCD_W/2,180, 112+i*8, c);
  }
  // simulated mic level bar
  int lvl = 20 + (esp_random()%60);
  gfx->fillRect(103,330,260,16,C_BLACK);
  gfx->fillRoundRect(103,330,260*lvl/100,16,5,th.accent);
  gfx->drawRoundRect(103,330,260,16,5,C_DIMGREY);
}
void handleAgent(const Event &ev){
  if (ev.type==EV_SWIPE_D){ screen=SCR_HOME; screenDirty=true; }
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
  textC("win studs!   swipe down: back", LCD_W/2, 330, 1, C_GREY);
}
void handleGames(const Event &ev){
  if (ev.type==EV_SWIPE_D){ screen=SCR_HOME; screenDirty=true; return; }
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
struct P{int x,y;};
P snake[SN_COLS*SN_ROWS]; int snLen; int snDir; P food; bool snDead; uint32_t snTick;
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
  P nh=snake[0];
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
void handleSnake(const Event &ev){
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
void handleMole(const Event &ev){
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
void handle2048(const Event &ev){
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
   SETUP / LOOP
   =================================================================== */
void setup(){
  Serial.begin(115200);
  delay(200);

  // ---- I2C (shared bus, 400kHz as the board BSP requires) ----
  Wire.begin(IIC_SDA, IIC_SCL);
  Wire.setClock(400000);

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

  // ---- IMU (optional: shake). Never hang if it is missing. ----
  imuOK = imu.begin(Wire, IMU_ADDR, IIC_SDA, IIC_SCL);
  if(imuOK){
    imu.configAccelerometer(SensorQMI8658::ACC_RANGE_4G,
                            SensorQMI8658::ACC_ODR_125Hz,
                            SensorQMI8658::LPF_MODE_0);
    imu.enableAccelerometer();
  } else { Serial.println("imu begin failed (shake disabled)"); }

  loadState();
  lastDecay = millis();
  delay(400);
  screen = SCR_HOME;
  screenDirty = true;
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
  }
}

void loop(){
  if(screenDirty){ screenDirty=false; enterScreen(); return; }

  Event ev = pollGesture();

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
  }

  // if a handler changed screens, let the next loop draw it fresh
  if(screenDirty) return;

  // per-screen animation / ticking
  switch(screen){
    case SCR_HOME:  drawHomeDynamic(); break;
    case SCR_AGENT: drawAgentDynamic();break;
    case SCR_SNAKE: stepSnake();       break;
    case SCR_MOLE:  stepMole();        break;
    default: break;
  }

  decayNeeds();
  checkShake();
  delay(2);          // keeps the touch loop tight & responsive
}
