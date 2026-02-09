#include <SPI.h>
#include <Wire.h>
#include <SD.h>

#include "Adafruit_ThinkInk.h"
#include <U8g2_for_Adafruit_GFX.h>

#include <Adafruit_seesaw.h>
#include <BleGamepad.h>
#include <BleMouse.h>
#include <NimBLEDevice.h>
#include "esp_sleep.h"

// -------------------- Types / helpers --------------------

enum JoystickDir : uint8_t { DIR_CENTER = 0, DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT };
enum BtnEvent    : uint8_t { BTN_NONE   = 0, BTN_SHORT = 1, BTN_LONG = 2 };

struct BtnState {
  uint8_t pin;
  bool stableLevel;
  bool lastRaw;
  uint32_t lastChangeMs;
  uint32_t tDown;
  bool longSent;
};

static inline int imax(int a, int b) { return (a > b) ? a : b; }
static inline int imin(int a, int b) { return (a < b) ? a : b; }

static bool sdRemoveIfExists(const String &path) {
  if (!path.length()) return true;
  const char *p = path.c_str();
  if (SD.exists(p)) return SD.remove(p);
  return true;
}

static bool sdRemoveIfExistsC(const char *path) {
  if (!path || !path[0]) return true;
  if (SD.exists(path)) return SD.remove(path);
  return true;
}

static File sdOpenRead(const String &path) {
  return SD.open(path.c_str(), FILE_READ);
}

static File sdOpenWrite(const String &path) {
  return SD.open(path.c_str(), FILE_WRITE);
}

// -------------------- Config --------------------

// Books
static const char *BOOKS_DIR      = "/books";
static const int   MAX_BOOKS      = 64;
static const char *LAST_BOOK_FILE = "/lastbook.txt";

// Images
static const char *BOOT_BMP = "/pictures/pingo_boot.bmp";
static const char *IDLE_BMP = "/pictures/pingo_idle.bmp";

// ThinkInk
#define EPD_DC    33
#define EPD_CS    15
#define EPD_RESET -1
#define EPD_BUSY  -1

ThinkInk_290_Grayscale4_EAAMFGN display(EPD_DC, EPD_RESET, EPD_CS, -1, EPD_BUSY, &SPI);
U8G2_FOR_ADAFRUIT_GFX u8g2;

// SD
#define SD_CS 14

// Screen buttons
#define BTN_A 27  // Prev
#define BTN_B 13  // Next
#define BTN_C 12  // Action

#define NEXT_BTN BTN_B
#define PREV_BTN BTN_A
#define SAVE_BTN BTN_C

// UI layout
#define ROTATION 0
int SW = 0, SH = 0;
static const int HEADER_H = 20;
static const int FOOTER_H = 20;
int MARGIN_X = 14;
static const int FIRSTLINE_XPAD = 3;

static const uint16_t UI_FG     = EPD_BLACK;
static const uint16_t UI_BG     = EPD_WHITE;
static const uint16_t UI_HDR_BG = EPD_DARK;

static const int TRANSITION_DELAY_MS  = 60;
static const int PROGRESS_BAR_HEIGHT  = 3;

// Book state
String BOOK_TITLE = "";
int lineH = 0, maxLines = 0, textTop = 0, textHeight = 0, textWidth = 0;

File   book;
size_t bookSize = 0;

static const int  READ_CHUNK = 4096;
static const int  MAX_PAGES  = 6000;
long pageIndex[MAX_PAGES];
int  pageCount = 0;
int  curPage   = 0;

// Index cache
uint32_t quickChecksum = 0;
long     idxLastPos    = 0;
bool     idxComplete   = false;

// Anti-ghosting
int pageFlipsSinceFull = 0;
static const int FULL_REFRESH_EVERY = 15;

// Library
String gBooks[MAX_BOOKS];
int    gBookCount = 0;

// Active paths
String gBookPath;
String gBookmarkPath;
String gIndexPath;

// Idle timers
static const uint32_t IDLE_TIMEOUT_MS       = 60000;
static const uint32_t SUPER_IDLE_TIMEOUT_MS = 5UL * 60000;
uint32_t lastActivityMs = 0;
bool inIdle = false;

// Joy FeatherWing
#define JOY_FEATHER_ADDR 0x49
Adafruit_seesaw ss;
bool joyPresent = false;

// Analog channels
#define JOY_X_CH 2
#define JOY_Y_CH 3

// Seesaw buttons
#define BUTTON_RIGHT 6   // A
#define BUTTON_DOWN  7   // B
#define BUTTON_LEFT  9   // X
#define BUTTON_UP    10  // Y
#define BUTTON_SEL   14  // Select

static const uint32_t JOY_BUTTON_MASK =
  (1UL << BUTTON_RIGHT) |
  (1UL << BUTTON_DOWN)  |
  (1UL << BUTTON_LEFT)  |
  (1UL << BUTTON_UP)    |
  (1UL << BUTTON_SEL);

static const int JOY_DEADBAND = 200;
int centerX = 0;
int centerY = 0;
static const bool JOY_INVERT_X = false;
static const bool JOY_INVERT_Y = false;
static const bool JOY_SWAP_XY  = false;

// BLE
BleGamepad bleGamepad("Pingo Remote", "Pingo Labs", 100);
bool bleGamepadStarted = false;

bool lastButtonPressedRemote[5] = { false, false, false, false, false };
static const uint16_t buttonIdRemote[5] = {
  BUTTON_1, BUTTON_2, BUTTON_3, BUTTON_4, BUTTON_5
};

bool lastScreenBtnState[3] = { false, false, false };
static const uint16_t screenButtonId[3] = { BUTTON_6, BUTTON_7, BUTTON_8 };

BleMouse bleMouse("Pingo Mouse", "Pingo Labs", 100);
bool bleMouseStarted = false;

static const int   JOY_MOUSE_DEADBAND  = 200;
static const int   JOY_SCROLL_DEADBAND = 250;

float       mouseSpeed       = 0.015f;
static const float MOUSE_SPEED_MIN  = 0.006f;
static const float MOUSE_SPEED_MAX  = 0.06f;
static const float MOUSE_SPEED_STEP = 0.003f;

static const int MOUSE_MAX_STEP = 8;

bool lastRightClick = false; // A
bool lastLeftClick  = false; // B

// Modes
enum PingoMode { MODE_READER = 0, MODE_MOUSE = 1, MODE_REMOTE = 2 };
PingoMode gMode = MODE_READER;

// Long press SELECT on JoyWing to change modes
static const uint16_t JOY_SELECT_LONG_MS = 800;
bool joySelLast = false;
uint32_t joySelDownMs = 0;

// Screen button debounce
static const uint16_t DEBOUNCE_MS = 25;
static const uint16_t LONG_MS     = 650;

BtnState bNext{ NEXT_BTN, HIGH, HIGH, 0, 0, false };
BtnState bPrev{ PREV_BTN, HIGH, HIGH, 0, 0, false };
BtnState bSave{ SAVE_BTN, HIGH, HIGH, 0, 0, false };

// Runtime state
JoystickDir readerLastDir = DIR_CENTER;
JoystickDir lastDirRemote = DIR_CENTER;

// -------------------- Forward declarations --------------------

// Buttons / joystick
static void exitIdle();
static BtnEvent pollButton(BtnState &b);
static JoystickDir mapDirForRotation(JoystickDir d);
static bool menuNavFromDir(JoystickDir dir, JoystickDir &lastDir, int &sel, int count);

static uint32_t readJoyButtonsMask();
static JoystickDir getJoystickDir();

// UI / rendering
static void computeTextArea();
static void header();
static void footer(long pageStart);
static void drawProgressBarBottom(int leftX, int rightX, long pageStart);
static void renderCurrentPage(bool forceFull = false);

// Layout / paging
static String readChunk(File &f, long pos, int maxBytes);
static int layoutPage(const String &src, int startX, int startY, int maxW, int maxH, int maxLineCount, bool draw);
static int paginateFrom(long pos, bool draw);

// Index cache
static uint32_t computeQuickChecksum(File &f);
static bool loadPageIndexCache();
static bool savePageIndexCache(bool completeFlag);
static bool rebuildIndexWithProgress(long startPos, long stopAfterPages, bool showProgressScreen);
static bool extendIndex(int pagesToAdd, bool silentQuick);

static void ensureIndexCoversPage(int pg);
static void ensureIndexCoversOffset(long off);

// Bookmarks
static void saveBookmark();
static bool loadBookmark();
static void clearBookmark();

// Library / book
static void deriveTitleFromPath(const String &path);
static void ensurePageVisible();
static void jumpToPercent(int percent, bool renderNow);
static bool saveLastBookPath(const String &path);
static bool loadLastBookPath(String &outPath);

static void scanBooks();
static bool openBookWithPath(const String &path, bool ignoreBookmark = false);
static void openDefaultOrLastBook();
static void libraryMenu();

// Menus
static void menuPrincipal();
static void readerMenu();
static void percentMenu();
static void drawMenuLines(const char *title, const char *const *opts, int n, int sel);
static void startupModeMenu();

// BMP
static bool drawBMP1bitFromSD_auto(const char *path, int x, int y, int *outW = nullptr, int *outH = nullptr);
static void showBootSplash();

// Idle / sleep
static void maybeShowIdle();
static void enterSuperIdle();

// Reader / Mouse / Remote
static void readerHandleJoystick();
static void readerLoopStep();

static void showRemoteScreen();
static void enterRemoteMode();
static void remoteLoopStep();

static void showMouseScreen();
static void enterMouseMode();
static void mouseLoopStep();

// BLE / mode helpers
static void stopAllBle();
static void enterReaderMode(bool fullRefresh);
static void nextMode();
static void checkJoySelectLongPress();

// -------------------- Input handling --------------------

static BtnEvent pollButton(BtnState &b) {
  uint32_t now = millis();
  bool raw = digitalRead(b.pin);

  if (raw != b.lastRaw) {
    b.lastRaw = raw;
    b.lastChangeMs = now;
  }

  // Debounce settle
  if ((now - b.lastChangeMs) >= DEBOUNCE_MS && raw != b.stableLevel) {
    b.stableLevel = raw;

    if (b.stableLevel == LOW) {
      b.tDown = now;
      b.longSent = false;
      if (inIdle) exitIdle();
      return BTN_NONE;
    }

    // Released
    if (!b.longSent) {
      if (inIdle) exitIdle();
      return BTN_SHORT;
    }
    return BTN_NONE;
  }

  // Long press
  if (b.stableLevel == LOW && !b.longSent && (now - b.tDown >= LONG_MS)) {
    b.longSent = true;
    if (inIdle) exitIdle();
    return BTN_LONG;
  }

  return BTN_NONE;
}

static bool menuNavFromDir(JoystickDir dir, JoystickDir &lastDir, int &sel, int count) {
  if (dir == lastDir) return false;

  bool changed = false;
  if (lastDir == DIR_CENTER) {
    if (dir == DIR_DOWN || dir == DIR_RIGHT) {
      sel = (sel + 1) % count;
      changed = true;
    } else if (dir == DIR_UP || dir == DIR_LEFT) {
      sel = (sel + count - 1) % count;
      changed = true;
    }
  }

  lastDir = dir;
  return changed;
}

// -------------------- Bookmark --------------------

static void saveBookmark() {
  sdRemoveIfExists(gBookmarkPath);

  File bm = sdOpenWrite(gBookmarkPath);
  if (!bm) return;

  long offset = (curPage >= 0 && curPage < pageCount) ? pageIndex[curPage] : 0;
  bm.print("v1;");
  bm.print(curPage);
  bm.print(";");
  bm.print(offset);
  bm.print("\n");
  bm.close();
}

static void ensureIndexCoversPage(int pg) {
  if (pg < 0) return;
  if (idxComplete) return;

  while (!idxComplete && pageCount <= pg && pageCount < MAX_PAGES) {
    extendIndex(120, true);
  }
}

static void ensureIndexCoversOffset(long off) {
  if (off < 0) return;
  if (idxComplete) return;

  while (!idxComplete && idxLastPos <= off && pageCount < MAX_PAGES) {
    extendIndex(120, true);
  }
}

static bool loadBookmark() {
  if (!gBookmarkPath.length()) return false;
  if (!SD.exists(gBookmarkPath.c_str())) return false;

  File bm = sdOpenRead(gBookmarkPath);
  if (!bm) return false;

  String s = bm.readStringUntil('\n');
  bm.close();

  s.trim();
  if (!s.length()) return false;

  int p1 = s.indexOf(';');
  int p2 = (p1 >= 0) ? s.indexOf(';', p1 + 1) : -1;
  if (p1 < 0 || p2 < 0) return false;

  int  pg  = s.substring(p1 + 1, p2).toInt();
  long off = s.substring(p2 + 1).toInt();

  // Prefer page if present
  if (pg >= 0) {
    ensureIndexCoversPage(pg);
    if (pg >= 0 && pg < pageCount) { curPage = pg; return true; }
  }

  // Fallback: find by offset
  if (off >= 0 && off < (long)bookSize) {
    ensureIndexCoversOffset(off);

    int best = 0;
    for (int i = 0; i < pageCount; i++) {
      if (pageIndex[i] <= off) best = i;
      else break;
    }
    curPage = best;
    return true;
  }

  return false;
}

static void clearBookmark() {
  sdRemoveIfExists(gBookmarkPath);
}

// -------------------- UI --------------------

static void computeTextArea() {
  SW = display.width();
  SH = display.height();

  u8g2.setFont(u8g2_font_helvR10_tf);
  int ascent  = (int)u8g2.getFontAscent();
  int descent = (int)u8g2.getFontDescent();

  lineH      = (ascent - descent) + 4;
  textTop    = HEADER_H + 4;
  textHeight = SH - textTop - FOOTER_H;
  textWidth  = SW - 2 * MARGIN_X;
  maxLines   = textHeight / lineH;

  if (maxLines < 4) {
    lineH    = imax(lineH - 2, (ascent - descent) + 2);
    maxLines = imax(4, textHeight / lineH);
  }
}

static void header() {
  display.fillRect(0, 0, SW, HEADER_H, UI_HDR_BG);

  u8g2.setFont(u8g2_font_helvR10_tf);
  u8g2.setFontMode(1);
  u8g2.setForegroundColor(EPD_LIGHT);
  u8g2.setBackgroundColor(UI_HDR_BG);

  const char *title = BOOK_TITLE.length() ? BOOK_TITLE.c_str() : "Pingo Reader";
  int aw = u8g2.getUTF8Width(title);
  int ax = imax((SW - aw) / 2, 2);
  int ay = 2 + u8g2.getFontAscent();
  u8g2.drawUTF8(ax, ay, title);

  u8g2.setForegroundColor(UI_FG);
  u8g2.setBackgroundColor(UI_BG);
}

static void drawProgressBarBottom(int leftX, int rightX, long pageStart) {
  int percent = (bookSize > 0) ? (int)((100.0 * pageStart) / bookSize) : 0;
  int margin  = 6;
  int barX    = leftX + margin;
  int barW    = imax(0, rightX - margin - barX);
  int barY    = SH - (FOOTER_H / 2) - (PROGRESS_BAR_HEIGHT / 2);
  int filled  = (barW * percent) / 100;

  if (barW <= 0) return;

  display.drawRect(barX, barY, barW, PROGRESS_BAR_HEIGHT, EPD_DARK);
  if (filled > 0) display.fillRect(barX, barY, filled, PROGRESS_BAR_HEIGHT, EPD_DARK);
}

static void footer(long pageStart) {
  int percent = (bookSize > 0) ? (int)((100.0 * pageStart) / bookSize) : 0;
  String left  = String("Pg ") + String(curPage + 1) + "/" + String(imax(1, pageCount));
  String right = String(percent) + "%";

  int leftX = MARGIN_X;
  int baseY = SH - 3;

  u8g2.drawUTF8(leftX, baseY, left.c_str());

  int rightW = u8g2.getUTF8Width(right.c_str());
  int rightX = SW - rightW - MARGIN_X;
  u8g2.drawUTF8(rightX, baseY, right.c_str());

  drawProgressBarBottom(leftX + u8g2.getUTF8Width(left.c_str()), rightX, pageStart);
}

// -------------------- Layout / pagination --------------------

static int layoutPage(const String &src, int startX, int startY, int maxW, int maxH, int maxLineCount, bool draw) {
  const int ascent  = (int)u8g2.getFontAscent();
  const int descent = (int)u8g2.getFontDescent();
  const int _lineH  = (ascent - descent) + 4;

  int baseline = startY + ascent;
  int usedH = 0;
  int lines = 0;

  int i = 0;
  int N = src.length();
  int iSafe = 0;

  String line, word;

  auto placeWord = [&](const String &w) -> bool {
    const int wWord = u8g2.getUTF8Width(w.c_str());
    const int wLine = u8g2.getUTF8Width(line.c_str());
    const int sp    = line.length() ? u8g2.getUTF8Width(" ") : 0;
    if (wLine + sp + wWord <= maxW) {
      if (line.length()) line += " ";
      line += w;
      return true;
    }
    return false;
  };

  auto flushLine = [&]() {
    if (draw) {
      const int x0 = startX + FIRSTLINE_XPAD;
      u8g2.drawUTF8(x0, baseline, line.c_str());
    }
    baseline += _lineH;
    usedH += _lineH;
    lines++;
    line = "";
  };

  while (i < N && lines < maxLineCount && (usedH + _lineH) <= maxH) {
    char c = src[i++];
    if (c == '\r') continue;

    if (c == '\n') {
      if (word.length()) {
        if (!placeWord(word)) {
          flushLine();
          if (lines >= maxLineCount || (usedH + _lineH) > maxH) {
            i -= word.length();
            return imax(iSafe, 0);
          }
          line = word;
        }
        word = "";
        iSafe = i;
      }
      flushLine();
      iSafe = i;
      if (lines >= maxLineCount || (usedH + _lineH) > maxH) return imax(iSafe, 0);
      continue;
    }

    if (c == ' ' || c == '\t') {
      if (word.length()) {
        if (!placeWord(word)) {
          flushLine();
          if (lines >= maxLineCount || (usedH + _lineH) > maxH) {
            i -= word.length();
            return imax(iSafe, 0);
          }
          line = word;
        }
        word = "";
        iSafe = i;
      }
    } else {
      word += c;
    }

    if (i == N) {
      if (word.length()) {
        if (!placeWord(word)) {
          flushLine();
          if (lines < maxLineCount && (usedH + _lineH) <= maxH) line = word;
          else { i -= word.length(); return imax(iSafe, 0); }
        } else {
          iSafe = i;
        }
        word = "";
      }
      if (line.length() && lines < maxLineCount && (usedH + _lineH) <= maxH) {
        flushLine();
        iSafe = i;
      }
    }
  }

  if (lines >= maxLineCount || (usedH + _lineH) > maxH) return imax(iSafe, 0);

  if (word.length()) {
    if (placeWord(word)) iSafe = i;
    else { i -= word.length(); return imax(iSafe, 0); }
  }

  if (line.length()) {
    if ((usedH + _lineH) <= maxH && lines < maxLineCount) {
      if (draw) {
        const int x0 = startX + FIRSTLINE_XPAD;
        u8g2.drawUTF8(x0, baseline, line.c_str());
      }
      iSafe = i;
    } else {
      return imax(iSafe, 0);
    }
  }

  return imax(iSafe, 0);
}

static String readChunk(File &f, long pos, int maxBytes) {
  if (!f) return "";

  f.seek(pos);
  String out;
  out.reserve(maxBytes + 64);

  int count = 0;
  while (f.available() && count < maxBytes) {
    out += (char)f.read();
    count++;
  }
  return out;
}

static int paginateFrom(long pos, bool draw) {
  String chunk = readChunk(book, pos, READ_CHUNK);

  // Skip UTF-8 BOM
  if (pos == 0 && chunk.length() >= 3 &&
      (uint8_t)chunk[0] == 0xEF && (uint8_t)chunk[1] == 0xBB && (uint8_t)chunk[2] == 0xBF) {
    pos += 3;
    chunk = readChunk(book, pos, READ_CHUNK);
  }

  return layoutPage(chunk, MARGIN_X, textTop, textWidth, textHeight, maxLines, draw);
}

// -------------------- PGX cache --------------------

static uint32_t computeQuickChecksum(File &f) {
  if (!f) return 0;

  const size_t SLICE = 4096;
  uint32_t sum = 0;

  f.seek(0);
  for (size_t i = 0; i < SLICE && f.available(); i++) sum = (sum * 131) + (uint8_t)f.read();

  if (f.size() > SLICE) {
    long start = (long)f.size() - (long)SLICE;
    if (start < 0) start = 0;

    f.seek(start);
    for (size_t i = 0; i < SLICE && f.available(); i++) sum = (sum * 131) + (uint8_t)f.read();
  }

  return sum;
}

static bool loadPageIndexCache() {
  if (!gIndexPath.length()) return false;

  File f = sdOpenRead(gIndexPath);
  if (!f) return false;

  String hdr = f.readStringUntil('\n');
  hdr.trim();

  int sep[9];
  int n = 0;
  int start = 0;

  while (n < 9) {
    int p = hdr.indexOf(';', start);
    sep[n++] = p;
    if (p < 0) break;
    start = p + 1;
  }

  if (n < 9) { f.close(); return false; }

  String tag = hdr.substring(0, sep[0]);
  tag.trim();
  if (tag != "PGX2") { f.close(); return false; }

  size_t   cacheSize     = (size_t)hdr.substring(sep[0] + 1, sep[1]).toInt();
  uint32_t cacheCks      = (uint32_t)hdr.substring(sep[1] + 1, sep[2]).toInt();
  int      cacheTW       = hdr.substring(sep[2] + 1, sep[3]).toInt();
  int      cacheTH       = hdr.substring(sep[3] + 1, sep[4]).toInt();
  int      cacheLH       = hdr.substring(sep[4] + 1, sep[5]).toInt();
  int      cacheML       = hdr.substring(sep[5] + 1, sep[6]).toInt();
  int      cachePages    = hdr.substring(sep[6] + 1, sep[7]).toInt();
  long     cacheLastPos  = hdr.substring(sep[7] + 1, sep[8]).toInt();
  int      cacheComplete = hdr.substring(sep[8] + 1).toInt();

  if (cacheSize != bookSize) { f.close(); return false; }

  computeTextArea();
  if (cacheTW != textWidth || cacheTH != textHeight || cacheLH != lineH || cacheML != maxLines) {
    f.close();
    return false;
  }

  uint32_t nowCks = computeQuickChecksum(book);
  if (nowCks != cacheCks) { f.close(); return false; }

  int count = 0;
  while (f.available() && count < MAX_PAGES) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (!line.length()) break;
    pageIndex[count++] = line.toInt();
  }

  f.close();

  if (count <= 0 || count != cachePages) return false;

  pageCount     = count;
  idxLastPos    = cacheLastPos;
  idxComplete   = (cacheComplete != 0);
  quickChecksum = nowCks;

  return true;
}

static bool savePageIndexCache(bool completeFlag) {
  if (!gIndexPath.length()) return false;

  sdRemoveIfExists(gIndexPath);
  File f = sdOpenWrite(gIndexPath);
  if (!f) return false;

  f.print("PGX2;");
  f.print((unsigned long)bookSize);      f.print(";");
  f.print((unsigned long)quickChecksum); f.print(";");
  f.print(textWidth);                   f.print(";");
  f.print(textHeight);                  f.print(";");
  f.print(lineH);                       f.print(";");
  f.print(maxLines);                    f.print(";");
  f.print(pageCount);                   f.print(";");
  f.print(idxLastPos);                  f.print(";");
  f.print(completeFlag ? 1 : 0);        f.print("\n");

  for (int i = 0; i < pageCount; i++) {
    f.print(pageIndex[i]);
    f.print("\n");
  }
  f.close();

  idxComplete = completeFlag;
  return true;
}

// -------------------- Rendering --------------------

static void renderCurrentPage(bool forceFull) {
  if (curPage < 0) curPage = 0;
  if (pageCount <= 0) return;
  if (curPage >= pageCount) curPage = pageCount - 1;

  long pos = pageIndex[curPage];

  if (forceFull || pageFlipsSinceFull >= FULL_REFRESH_EVERY) {
    pageFlipsSinceFull = 0;
  }

  display.clearBuffer();
  display.setRotation(ROTATION);

  u8g2.setFont(u8g2_font_helvR10_tf);
  u8g2.setFontMode(1);
  u8g2.setFontDirection(0);
  u8g2.setForegroundColor(UI_FG);
  u8g2.setBackgroundColor(UI_BG);

  computeTextArea();
  header();
  (void)paginateFrom(pos, true);
  footer(pos);

  display.display();
  delay(TRANSITION_DELAY_MS);
}

// -------------------- Book title + last book persistence --------------------

static void deriveTitleFromPath(const String &path) {
  int slash = path.lastIndexOf('/');
  String base = (slash >= 0) ? path.substring(slash + 1) : path;

  int dot = base.lastIndexOf('.');
  if (dot > 0) base = base.substring(0, dot);

  base.replace('_', ' ');
  base.replace('-', ' ');

  String out;
  for (int i = 0; i < (int)base.length(); ++i) {
    char c = base[i];
    if (i > 0 && isupper((unsigned char)c) && islower((unsigned char)base[i - 1])) out += ' ';
    out += c;
  }

  bool newWord = true;
  for (int i = 0; i < (int)out.length(); ++i) {
    char c = out[i];
    if (newWord && isalpha((unsigned char)c)) {
      out.setCharAt(i, toupper((unsigned char)c));
      newWord = false;
    }
    if (c == ' ') newWord = true;
  }

  BOOK_TITLE = out;
}

static bool saveLastBookPath(const String &path) {
  sdRemoveIfExistsC(LAST_BOOK_FILE);

  File f = SD.open(LAST_BOOK_FILE, FILE_WRITE);
  if (!f) return false;

  f.println(path);
  f.close();
  return true;
}

static bool loadLastBookPath(String &outPath) {
  File f = SD.open(LAST_BOOK_FILE, FILE_READ);
  if (!f) return false;

  String line = f.readStringUntil('\n');
  f.close();

  line.trim();
  if (!line.length()) return false;

  outPath = line;
  return true;
}

// -------------------- Index build / extend --------------------

static bool rebuildIndexWithProgress(long startPos, long stopAfterPages, bool showProgressScreen) {
  pageCount  = 0;
  idxLastPos = startPos;

  if (showProgressScreen) {
    display.clearBuffer();
    display.setRotation(ROTATION);
    computeTextArea();
    header();
    u8g2.drawUTF8(MARGIN_X, HEADER_H + 22, "Indexing pages...");
    footer(0);
    display.display();
  }

  long pos = startPos;
  long lastProgressPos = startPos;

  static const int UPDATE_EVERY_PAGES = 25;
  static const int UPDATE_EVERY_BYTES = 32 * 1024;

  while (pos < (long)bookSize && pageCount < MAX_PAGES) {
    pageIndex[pageCount] = pos;

    int used = paginateFrom(pos, false);
    if (used <= 0) break;

    pos += used;
    pageCount++;

    bool shouldUpdate =
      (pageCount % UPDATE_EVERY_PAGES == 0) ||
      (pos - lastProgressPos >= UPDATE_EVERY_BYTES) ||
      (pos >= (long)bookSize);

    if (showProgressScreen && shouldUpdate) {
      lastProgressPos = pos;
      int percent = (bookSize > 0) ? (int)((100.0 * pos) / bookSize) : 0;

      display.clearBuffer();
      display.setRotation(ROTATION);
      computeTextArea();
      header();

      u8g2.drawUTF8(MARGIN_X, HEADER_H + 22, "Indexing pages...");
      u8g2.drawUTF8(MARGIN_X, HEADER_H + 36, (String("Read: ") + String(percent) + "%").c_str());
      u8g2.drawUTF8(MARGIN_X, HEADER_H + 50, (String("Pages: ") + String(pageCount)).c_str());

      footer(pos);
      display.display();
    }

    if (stopAfterPages > 0 && pageCount >= stopAfterPages) {
      idxLastPos = pos;
      quickChecksum = computeQuickChecksum(book);
      savePageIndexCache(false);
      return false;
    }
  }

  idxLastPos = pos;
  quickChecksum = computeQuickChecksum(book);
  savePageIndexCache(true);
  return true;
}

static bool extendIndex(int pagesToAdd, bool silentQuick) {
  if (idxComplete) return true;
  if (pageCount <= 0) return false;

  long pos = idxLastPos;
  int added = 0;

  while (pos < (long)bookSize && pageCount < MAX_PAGES && added < pagesToAdd) {
    pageIndex[pageCount] = pos;

    int used = paginateFrom(pos, false);
    if (used <= 0) break;

    pos += used;
    pageCount++;
    added++;
  }

  idxLastPos = pos;
  if (pos >= (long)bookSize) idxComplete = true;

  if (!silentQuick) {
    long footerPos = (pageCount > 0 && curPage >= 0 && curPage < pageCount) ? pageIndex[curPage] : 0;

    display.clearBuffer();
    display.setRotation(ROTATION);
    computeTextArea();
    header();

    u8g2.drawUTF8(MARGIN_X, HEADER_H + 22, idxComplete ? "Index complete" : "Index extended");
    u8g2.drawUTF8(MARGIN_X, HEADER_H + 36, (String("Pages: ") + String(pageCount)).c_str());
    footer(footerPos);

    display.display();
    delay(120);
  }

  if (quickChecksum == 0) quickChecksum = computeQuickChecksum(book);
  savePageIndexCache(idxComplete);

  return idxComplete;
}

// -------------------- Book open --------------------

static String withNewExt(const String &path, const char *ext) {
  int dot = path.lastIndexOf('.');
  if (dot < 0) return path + String(ext);
  return path.substring(0, dot) + String(ext);
}

static bool openBookWithPath(const String &path, bool ignoreBookmark) {
  gBookPath     = path;
  gBookmarkPath = withNewExt(path, ".bmk");
  gIndexPath    = withNewExt(path, ".pgx");

  deriveTitleFromPath(path);

  if (book) book.close();
  book = sdOpenRead(gBookPath);

  display.clearBuffer();
  display.setRotation(ROTATION);

  u8g2.begin(display);
  u8g2.setFont(u8g2_font_helvR10_tf);
  u8g2.setFontMode(1);
  u8g2.setFontDirection(0);
  u8g2.setForegroundColor(UI_FG);
  u8g2.setBackgroundColor(UI_BG);

  if (!book) {
    computeTextArea();
    header();
    u8g2.drawUTF8(MARGIN_X, HEADER_H + 22, "Failed to open book file");
    display.display();
    return false;
  }

  saveLastBookPath(path);

  bookSize = book.size();
  computeTextArea();

  bool cacheOK = loadPageIndexCache();
  if (!cacheOK) {
    static const int INITIAL_PAGES = 120;
    rebuildIndexWithProgress(0, INITIAL_PAGES, true);
  }

  if (!ignoreBookmark && loadBookmark()) {
    // curPage set by loadBookmark()
  } else {
    curPage = 0;
  }

  // Pre-extend a bit ahead
  int target = imax(curPage + 50, 100);
  if (pageCount <= target && !idxComplete) extendIndex(target - pageCount + 1, true);

  renderCurrentPage(true);
  return true;
}

static void openDefaultOrLastBook() {
  String lastPath;
  if (loadLastBookPath(lastPath) && SD.exists(lastPath.c_str())) {
    if (openBookWithPath(lastPath, false)) return;
  }

  String fallback = String(BOOKS_DIR) + "/TestoJunkie.txt";
  if (SD.exists(fallback.c_str())) { openBookWithPath(fallback, false); return; }

  scanBooks();
  if (gBookCount > 0) openBookWithPath(gBooks[0], false);
  else {
    display.clearBuffer();
    display.setRotation(ROTATION);
    u8g2.begin(display);
    computeTextArea();
    header();
    u8g2.drawUTF8(MARGIN_X, HEADER_H + 22, "No .txt files in /books");
    display.display();
  }
}

// -------------------- Navigation --------------------

static void ensurePageVisible() {
  if (curPage < 0) curPage = 0;
  if (pageCount <= 0) return;
  if (curPage >= pageCount) curPage = pageCount - 1;

  if (curPage >= pageCount - 2 && !idxComplete) extendIndex(80, true);

  pageFlipsSinceFull++;
  renderCurrentPage();

  lastActivityMs = millis();
}

static void jumpToPercent(int percent, bool renderNow) {
  percent = imin(percent, 100);
  percent = imax(percent, 0);

  long targetPos = (long)((bookSize * (double)percent) / 100.0);

  int best = 0;
  for (int i = 0; i < pageCount; i++) {
    if (pageIndex[i] <= targetPos) best = i;
    else break;
  }

  curPage = best;
  if (renderNow) ensurePageVisible();
}

// -------------------- Menus --------------------

static void drawMenuLines(const char *title, const char *const *opts, int n, int sel) {
  display.clearBuffer();
  display.setRotation(ROTATION);

  computeTextArea();
  header();

  u8g2.drawUTF8(MARGIN_X, HEADER_H + 22, title);

  int baseY = HEADER_H + 44;
  for (int i = 0; i < n; i++) {
    String s = (i == sel) ? String("> ") + opts[i] : String("  ") + opts[i];
    u8g2.drawUTF8(MARGIN_X, baseY + i * 16, s.c_str());
  }
  display.display();
}

static void readerMenu() {
  const char *opts[2] = { "Jump to...", "Library" };
  const int COUNT = 2;
  int sel = 0;

  drawMenuLines("Reader Menu", opts, COUNT, sel);

  JoystickDir lastDirMenu = DIR_CENTER;

  while (true) {
    BtnEvent eA = pollButton(bNext);
    BtnEvent eB = pollButton(bPrev);
    BtnEvent eC = pollButton(bSave);

    if (eA == BTN_SHORT) { sel = (sel + 1) % COUNT; drawMenuLines("Reader Menu", opts, COUNT, sel); }
    if (eB == BTN_SHORT) { sel = (sel + COUNT - 1) % COUNT; drawMenuLines("Reader Menu", opts, COUNT, sel); }

    JoystickDir dir = mapDirForRotation(getJoystickDir());
    if (menuNavFromDir(dir, lastDirMenu, sel, COUNT)) {
      drawMenuLines("Reader Menu", opts, COUNT, sel);
    }

    if (eC == BTN_SHORT) {
      if (sel == 0) { percentMenu(); return; }
      libraryMenu();
      return;
    }

    if (eC == BTN_LONG) { renderCurrentPage(true); return; }

    delay(5);
  }
}

static void startupModeMenu() {
  const char *opts[3] = { "Reader", "Mouse", "Gamepad" };
  const int COUNT = 3;
  int sel = 0;

  drawMenuLines("Start Mode", opts, COUNT, sel);

  JoystickDir lastDirMenu = DIR_CENTER;

  while (true) {
    BtnEvent eA = pollButton(bNext);
    BtnEvent eB = pollButton(bPrev);
    BtnEvent eC = pollButton(bSave);

    if (eA == BTN_SHORT) { sel = (sel + 1) % COUNT; drawMenuLines("Start Mode", opts, COUNT, sel); }
    if (eB == BTN_SHORT) { sel = (sel + COUNT - 1) % COUNT; drawMenuLines("Start Mode", opts, COUNT, sel); }

    JoystickDir dir = mapDirForRotation(getJoystickDir());
    if (menuNavFromDir(dir, lastDirMenu, sel, COUNT)) {
      drawMenuLines("Start Mode", opts, COUNT, sel);
    }

    if (eC == BTN_SHORT) {
      lastActivityMs = millis();
      inIdle = false;

      if (sel == 0) { gMode = MODE_READER; openDefaultOrLastBook(); return; }
      if (sel == 1) { enterMouseMode(); return; }

      enterRemoteMode();
      return;
    }

    if (eC == BTN_LONG) {
      lastActivityMs = millis();
      inIdle = false;
      gMode = MODE_READER;
      openDefaultOrLastBook();
      return;
    }

    delay(5);
  }
}

static void menuPrincipal() {
  const char *opts[4] = { "Back", "Reader Menu", "Pingo Mouse", "Pingo Remote" };
  const int COUNT = 4;
  int sel = 0;

  drawMenuLines("Main Menu", opts, COUNT, sel);

  JoystickDir lastDirMenu = DIR_CENTER;

  while (true) {
    BtnEvent eA = pollButton(bNext);
    BtnEvent eB = pollButton(bPrev);
    BtnEvent eC = pollButton(bSave);

    if (eA == BTN_SHORT) { sel = (sel + 1) % COUNT; drawMenuLines("Main Menu", opts, COUNT, sel); }
    if (eB == BTN_SHORT) { sel = (sel + COUNT - 1) % COUNT; drawMenuLines("Main Menu", opts, COUNT, sel); }

    JoystickDir dir = mapDirForRotation(getJoystickDir());
    if (menuNavFromDir(dir, lastDirMenu, sel, COUNT)) {
      drawMenuLines("Main Menu", opts, COUNT, sel);
    }

    if (eC == BTN_SHORT) {
      if (sel == 0) { renderCurrentPage(true); return; }
      if (sel == 1) { readerMenu(); return; }
      if (sel == 2) { enterMouseMode(); return; }
      enterRemoteMode();
      return;
    }

    if (eC == BTN_LONG) { renderCurrentPage(true); return; }

    delay(5);
  }
}

static void percentMenu() {
  const char *opts[5] = { "0%", "10%", "25%", "50%", "75%" };
  const int percMap[5] = { 0, 10, 25, 50, 75 };
  const int COUNT = 5;

  int sel = 0;
  drawMenuLines("Jump to", opts, COUNT, sel);

  JoystickDir lastDirMenu = DIR_CENTER;

  while (true) {
    BtnEvent eA = pollButton(bNext);
    BtnEvent eB = pollButton(bPrev);
    BtnEvent eC = pollButton(bSave);

    if (eA == BTN_SHORT) { sel = (sel + 1) % COUNT; drawMenuLines("Jump to", opts, COUNT, sel); }
    if (eB == BTN_SHORT) { sel = (sel + COUNT - 1) % COUNT; drawMenuLines("Jump to", opts, COUNT, sel); }

    JoystickDir dir = mapDirForRotation(getJoystickDir());
    if (menuNavFromDir(dir, lastDirMenu, sel, COUNT)) {
      drawMenuLines("Jump to", opts, COUNT, sel);
    }

    if (eC == BTN_SHORT) { jumpToPercent(percMap[sel], true); return; }
    if (eC == BTN_LONG)  { menuPrincipal(); return; }

    delay(5);
  }
}

// -------------------- Library --------------------

static bool isTxt(const String &name) {
  int dot = name.lastIndexOf('.');
  if (dot < 0) return false;
  String ext = name.substring(dot);
  ext.toLowerCase();
  return (ext == ".txt");
}

static void scanBooks() {
  gBookCount = 0;

  if (SD.exists(BOOKS_DIR)) {
    File dir = SD.open(BOOKS_DIR);
    if (dir) {
      while (true) {
        File f = dir.openNextFile();
        if (!f) break;

        if (!f.isDirectory()) {
          String p = String(BOOKS_DIR) + "/" + String(f.name());
          if (isTxt(p) && gBookCount < MAX_BOOKS) gBooks[gBookCount++] = p;
        }
        f.close();
      }
      dir.close();
    }
  }

  // fallback: root
  if (gBookCount == 0) {
    File dir = SD.open("/");
    if (dir) {
      while (true) {
        File f = dir.openNextFile();
        if (!f) break;

        if (!f.isDirectory()) {
          String p = String("/") + String(f.name());
          if (isTxt(p) && gBookCount < MAX_BOOKS) gBooks[gBookCount++] = p;
        }
        f.close();
      }
      dir.close();
    }
  }
}

static void drawLibraryMenu(int topIndex, int selIndex) {
  display.clearBuffer();
  display.setRotation(ROTATION);

  computeTextArea();
  header();

  u8g2.drawUTF8(MARGIN_X, HEADER_H + 22, "Library");

  static const int VISIBLE = 7;
  int baseY = HEADER_H + 44;

  for (int i = 0; i < VISIBLE; i++) {
    int idx = topIndex + i;
    if (idx >= gBookCount) break;

    String path = gBooks[idx];
    int slash = path.lastIndexOf('/');
    String base = (slash >= 0) ? path.substring(slash + 1) : path;

    String line = (idx == selIndex) ? String("> ") + base : String("  ") + base;

    while (u8g2.getUTF8Width(line.c_str()) > (SW - 2 * MARGIN_X) && line.length() > 3) {
      line.remove(line.length() - 4);
      line += "...";
    }

    u8g2.drawUTF8(MARGIN_X, baseY + i * 16, line.c_str());
  }

  display.display();
}

static void libraryMenu() {
  scanBooks();

  if (gBookCount == 0) {
    display.clearBuffer();
    display.setRotation(ROTATION);

    computeTextArea();
    header();
    u8g2.drawUTF8(MARGIN_X, HEADER_H + 22, "No .txt books found");
    display.display();

    delay(600);
    renderCurrentPage(true);
    return;
  }

  static const int VISIBLE = 7;
  int sel = 0;
  int top = 0;

  drawLibraryMenu(top, sel);
  JoystickDir lastDirLib = DIR_CENTER;

  auto moveSel = [&](int delta) {
    int newSel = sel + delta;
    while (newSel < 0) newSel += gBookCount;
    while (newSel >= gBookCount) newSel -= gBookCount;
    sel = newSel;

    if (sel >= top + VISIBLE) top = sel - (VISIBLE - 1);
    if (sel < top) top = sel;
    if (top < 0) top = 0;

    drawLibraryMenu(top, sel);
  };

  while (true) {
    BtnEvent eA = pollButton(bNext);
    BtnEvent eB = pollButton(bPrev);
    BtnEvent eC = pollButton(bSave);

    if (eA == BTN_SHORT) moveSel(+1);
    if (eB == BTN_SHORT) moveSel(-1);

    JoystickDir dir = mapDirForRotation(getJoystickDir());
    if (dir != lastDirLib) {
      if (lastDirLib == DIR_CENTER) {
        if (dir == DIR_DOWN || dir == DIR_RIGHT) moveSel(+1);
        else if (dir == DIR_UP || dir == DIR_LEFT) moveSel(-1);
      }
      lastDirLib = dir;
    }

    if (eC == BTN_SHORT) { openBookWithPath(gBooks[sel], false); return; }
    if (eC == BTN_LONG)  { renderCurrentPage(true); return; }

    delay(5);
  }
}

// -------------------- BMP rendering (1-bit) --------------------

static bool drawBMP1bitFromSD_auto(const char *path, int x, int y, int *outW, int *outH) {
  File f = SD.open(path, FILE_READ);
  if (!f) return false;

  uint16_t bfType = f.read() | (f.read() << 8);
  if (bfType != 0x4D42) { f.close(); return false; }

  f.seek(10);
  uint32_t pixelOffset = f.read() | (f.read() << 8) | (f.read() << 16) | (f.read() << 24);

  uint32_t dibSize = f.read() | (f.read() << 8) | (f.read() << 16) | (f.read() << 24);
  if (dibSize < 40) { f.close(); return false; }

  int32_t bmpW = (int32_t)(f.read() | (f.read() << 8) | (f.read() << 16) | (f.read() << 24));
  int32_t bmpH = (int32_t)(f.read() | (f.read() << 8) | (f.read() << 16) | (f.read() << 24));

  f.seek(28);
  uint16_t bpp = f.read() | (f.read() << 8);

  f.seek(30);
  uint32_t comp = f.read() | (f.read() << 8) | (f.read() << 16) | (f.read() << 24);

  if (bpp != 1 || comp != 0 || bmpW <= 0 || bmpH == 0) { f.close(); return false; }

  uint32_t paletteOffset = 14 + dibSize;
  f.seek(paletteOffset);

  uint8_t b0 = f.read(), g0 = f.read(), r0 = f.read(); f.read();
  uint8_t b1 = f.read(), g1 = f.read(), r1 = f.read(); f.read();

  int i0 = (int)r0 + (int)g0 + (int)b0;
  int i1 = (int)r1 + (int)g1 + (int)b1;

  bool invertBits = !(i1 < i0);

  f.seek(pixelOffset);

  bool bottomUp = bmpH > 0;
  if (bmpH < 0) bmpH = -bmpH;

  int rowBits  = ((bmpW + 31) / 32) * 32;
  int rowBytes = rowBits / 8;

  if (outW) *outW = bmpW;
  if (outH) *outH = bmpH;

  for (int row = 0; row < bmpH; row++) {
    int srcRow = bottomUp ? (bmpH - 1 - row) : row;
    f.seek(pixelOffset + (uint32_t)srcRow * rowBytes);

    for (int colByte = 0; colByte < rowBytes; colByte++) {
      int b = f.read();

      for (int bit = 7; bit >= 0; bit--) {
        int col = colByte * 8 + (7 - bit);
        if (col >= bmpW) break;

        bool on = (b >> bit) & 0x1;
        if (invertBits) on = !on;

        int px = x + col;
        int py = y + row;

        if (px >= 0 && px < display.width() && py >= 0 && py < display.height()) {
          display.drawPixel(px, py, on ? EPD_BLACK : EPD_WHITE);
        }
      }
    }
  }

  f.close();
  return true;
}

// -------------------- Splash / Idle / Deep Sleep --------------------

static void showBootSplash() {
  const char *path = SD.exists(BOOT_BMP) ? BOOT_BMP : IDLE_BMP;

  display.clearBuffer();
  display.setRotation(ROTATION);
  display.fillRect(0, 0, display.width(), display.height(), EPD_WHITE);

  int w = 0, h = 0;
  if (drawBMP1bitFromSD_auto(path, 0, 0, &w, &h)) {
    display.clearBuffer();
    display.setRotation(ROTATION);
    display.fillRect(0, 0, display.width(), display.height(), EPD_WHITE);

    int x = (display.width()  - w) / 2;
    int y = (display.height() - h) / 2;
    drawBMP1bitFromSD_auto(path, x, y, nullptr, nullptr);
  }

  u8g2.begin(display);
  u8g2.setFont(u8g2_font_helvR12_tf);
  u8g2.setFontMode(1);
  u8g2.setForegroundColor(EPD_BLACK);
  u8g2.setBackgroundColor(EPD_WHITE);

  SW = display.width();
  SH = display.height();

  const char *title = "Pingo Reader";
  int tw = u8g2.getUTF8Width(title);
  int tx = imax((SW - tw) / 2, 2);
  int ty = SH - 20;
  u8g2.drawUTF8(tx, ty, title);

  display.display();
  delay(1800);
}

static void maybeShowIdle() {
  if (gMode != MODE_READER) return;
  if (inIdle) return;
  if (millis() - lastActivityMs < IDLE_TIMEOUT_MS) return;

  const char *path = SD.exists(IDLE_BMP) ? IDLE_BMP : BOOT_BMP;

  display.clearBuffer();
  display.setRotation(ROTATION);
  display.fillRect(0, 0, display.width(), display.height(), EPD_WHITE);

  int w = 0, h = 0;
  if (drawBMP1bitFromSD_auto(path, 0, 0, &w, &h)) {
    display.clearBuffer();
    display.setRotation(ROTATION);
    display.fillRect(0, 0, display.width(), display.height(), EPD_WHITE);

    int x = (display.width()  - w) / 2;
    int y = (display.height() - h) / 2;
    drawBMP1bitFromSD_auto(path, x, y, nullptr, nullptr);
  }

  u8g2.begin(display);
  u8g2.setFont(u8g2_font_helvR12_tf);
  u8g2.setFontMode(1);
  u8g2.setForegroundColor(EPD_BLACK);
  u8g2.setBackgroundColor(EPD_WHITE);

  SW = display.width();
  SH = display.height();

  const char *title = "Pingo Reader";
  int tw = u8g2.getUTF8Width(title);
  int tx = imax((SW - tw) / 2, 2);
  int ty = SH - 20;
  u8g2.drawUTF8(tx, ty, title);

  display.display();
  inIdle = true;
}

static void exitIdle() {
  if (!inIdle) {
    lastActivityMs = millis();
    return;
  }

  inIdle = false;
  lastActivityMs = millis();

  if (gMode == MODE_READER) renderCurrentPage(true);
  else if (gMode == MODE_MOUSE) showMouseScreen();
  else showRemoteScreen();
}

static void enterSuperIdle() {
  if (gMode != MODE_READER) return;

  saveBookmark();
  saveLastBookPath(gBookPath);

  stopAllBle();

  const char *path = SD.exists(IDLE_BMP) ? IDLE_BMP : BOOT_BMP;

  display.clearBuffer();
  display.setRotation(ROTATION);
  display.fillRect(0, 0, display.width(), display.height(), EPD_WHITE);

  int w = 0, h = 0;
  if (drawBMP1bitFromSD_auto(path, 0, 0, &w, &h)) {
    display.clearBuffer();
    display.setRotation(ROTATION);
    display.fillRect(0, 0, display.width(), display.height(), EPD_WHITE);

    int x = (display.width()  - w) / 2;
    int y = (display.height() - h) / 2;
    drawBMP1bitFromSD_auto(path, x, y, nullptr, nullptr);
  }

  u8g2.begin(display);
  u8g2.setFontMode(1);
  u8g2.setForegroundColor(EPD_BLACK);
  u8g2.setBackgroundColor(EPD_WHITE);

  SW = display.width();
  SH = display.height();

  u8g2.setFont(u8g2_font_helvR12_tf);
  const char *title = "Pingo Reader";
  int tw = u8g2.getUTF8Width(title);
  int tx = imax((SW - tw) / 2, 2);
  int ty = SH - 28;
  u8g2.drawUTF8(tx, ty, title);

  u8g2.setFont(u8g2_font_helvR10_tf);
  const char *hint = "Press middle button to wake";
  int hw = u8g2.getUTF8Width(hint);
  int hx = imax((SW - hw) / 2, 2);
  int hy = SH - 14;
  u8g2.drawUTF8(hx, hy, hint);

  display.display();
  delay(600);

  // Wake on BTN_C (GPIO12) LOW
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BTN_C, 0);
  esp_deep_sleep_start();
}

// -------------------- JoyWing helpers --------------------

static uint32_t readJoyButtonsMask() {
  if (!joyPresent) return 0xFFFFFFFF;
  return ss.digitalReadBulk(JOY_BUTTON_MASK);
}

static JoystickDir getJoystickDir() {
  if (!joyPresent) return DIR_CENTER;

  int x = ss.analogRead(JOY_X_CH);
  int y = ss.analogRead(JOY_Y_CH);

  if (JOY_SWAP_XY) {
    int tmp = x;
    x = y;
    y = tmp;
  }

  if (JOY_INVERT_X) x = (2 * centerX) - x;
  if (JOY_INVERT_Y) y = (2 * centerY) - y;

  if (x > centerX + JOY_DEADBAND) return DIR_RIGHT;
  if (x < centerX - JOY_DEADBAND) return DIR_LEFT;
  if (y > centerY + JOY_DEADBAND) return DIR_UP;
  if (y < centerY - JOY_DEADBAND) return DIR_DOWN;

  return DIR_CENTER;
}

static JoystickDir mapDirForRotation(JoystickDir d) {
  switch (ROTATION) {
    case 0:
      return d;

    case 1:
      if (d == DIR_UP) return DIR_RIGHT;
      if (d == DIR_RIGHT) return DIR_DOWN;
      if (d == DIR_DOWN) return DIR_LEFT;
      if (d == DIR_LEFT) return DIR_UP;
      return d;

    case 2:
      if (d == DIR_UP) return DIR_DOWN;
      if (d == DIR_DOWN) return DIR_UP;
      if (d == DIR_LEFT) return DIR_RIGHT;
      if (d == DIR_RIGHT) return DIR_LEFT;
      return d;

    case 3:
      if (d == DIR_UP) return DIR_LEFT;
      if (d == DIR_LEFT) return DIR_DOWN;
      if (d == DIR_DOWN) return DIR_RIGHT;
      if (d == DIR_RIGHT) return DIR_UP;
      return d;
  }
  return d;
}

// -------------------- Reader joystick navigation --------------------

static void readerHandleJoystick() {
  if (!joyPresent || gMode != MODE_READER) return;

  JoystickDir raw = getJoystickDir();
  JoystickDir dir = mapDirForRotation(raw);

  if (dir == readerLastDir) return;

  if (readerLastDir == DIR_CENTER) {
    if (dir == DIR_UP) {
      if (curPage > 0) { curPage--; ensurePageVisible(); }
    } else if (dir == DIR_DOWN) {
      if (curPage < pageCount - 1) { curPage++; ensurePageVisible(); }
    } else if (dir == DIR_RIGHT) {
      curPage = imin(curPage + 10, pageCount - 1);
      ensurePageVisible();
    } else if (dir == DIR_LEFT) {
      curPage = imax(curPage - 10, 0);
      ensurePageVisible();
    }
  }

  readerLastDir = dir;
}

// -------------------- BLE / mode helpers --------------------

static void stopAllBle() {
  if (bleMouseStarted || bleGamepadStarted) {
    NimBLEDevice::deinit(true);
    bleMouseStarted = false;
    bleGamepadStarted = false;
  }
}

static void enterReaderMode(bool fullRefresh) {
  stopAllBle();
  gMode = MODE_READER;
  inIdle = false;
  lastActivityMs = millis();
  renderCurrentPage(fullRefresh);
}

static void nextMode() {
  if (gMode == MODE_READER) enterMouseMode();
  else if (gMode == MODE_MOUSE) enterRemoteMode();
  else enterReaderMode(true);
}

// Long press SELECT (JoyWing): Reader -> Mouse, Mouse -> Remote
static void checkJoySelectLongPress() {
  if (!joyPresent) return;

  uint32_t buttons = readJoyButtonsMask();
  bool selPressed = !(buttons & (1UL << BUTTON_SEL));
  uint32_t now = millis();

  if (selPressed && !joySelLast) {
    joySelDownMs = now;
  } else if (selPressed && joySelLast) {
    if (joySelDownMs && (now - joySelDownMs >= JOY_SELECT_LONG_MS)) {
      if (gMode == MODE_READER) enterMouseMode();
      else if (gMode == MODE_MOUSE) enterRemoteMode();
      joySelDownMs = 0;
    }
  } else if (!selPressed && joySelLast) {
    joySelDownMs = 0;
  }

  joySelLast = selPressed;
}

// -------------------- Mouse screen / loop --------------------

static void showMouseScreen() {
  display.clearBuffer();
  display.setRotation(ROTATION);
  display.fillRect(0, 0, display.width(), display.height(), EPD_WHITE);

  const char *path = SD.exists(IDLE_BMP) ? IDLE_BMP : BOOT_BMP;
  int w = 0, h = 0;

  if (drawBMP1bitFromSD_auto(path, 0, 0, &w, &h)) {
    int x = (display.width()  - w) / 2;
    int y = (display.height() - h) / 2;

    display.clearBuffer();
    display.setRotation(ROTATION);
    display.fillRect(0, 0, display.width(), display.height(), EPD_WHITE);
    drawBMP1bitFromSD_auto(path, x, y, nullptr, nullptr);
  }

  u8g2.begin(display);
  u8g2.setFontMode(1);
  u8g2.setForegroundColor(EPD_BLACK);
  u8g2.setBackgroundColor(EPD_WHITE);

  SW = display.width();
  SH = display.height();

  u8g2.setFont(u8g2_font_helvR12_tf);
  const char *title = "Pingo Mouse";
  int tw = u8g2.getUTF8Width(title);
  int tx = imax((SW - tw) / 2, 2);
  int ty = SH - 20;
  u8g2.drawUTF8(tx, ty, title);

  // Sensitivity indicator (1..N)
  u8g2.setFont(u8g2_font_helvR10_tf);
  int level = (int)((mouseSpeed - MOUSE_SPEED_MIN) / MOUSE_SPEED_STEP + 0.5f) + 1;
  if (level < 1) level = 1;

  char buf[24];
  snprintf(buf, sizeof(buf), "Sens %d", level);
  u8g2.drawUTF8(4, 14, buf);

  display.display();
}

static void enterMouseMode() {
  stopAllBle();

  gMode = MODE_MOUSE;
  inIdle = false;
  lastActivityMs = millis();

  if (!bleMouseStarted) {
    bleMouse.begin();
    bleMouseStarted = true;
  }

  lastRightClick = false;
  lastLeftClick = false;

  showMouseScreen();
}

static void mouseLoopStep() {
  BtnEvent eSave = pollButton(bSave);

  // BTN_C long: jump to Remote
  if (eSave == BTN_LONG) { enterRemoteMode(); return; }

  if (!bleMouseStarted) { delay(10); return; }
  if (!bleMouse.isConnected()) { delay(20); return; }

  int rawX = joyPresent ? ss.analogRead(JOY_X_CH) : centerX;
  int rawY = joyPresent ? ss.analogRead(JOY_Y_CH) : centerY;
  int dX = rawX - centerX;
  int dY = rawY - centerY;

  uint32_t mask = joyPresent ? readJoyButtonsMask() : JOY_BUTTON_MASK;
  bool btnA = !(mask & (1UL << BUTTON_RIGHT)); // right click
  bool btnB = !(mask & (1UL << BUTTON_DOWN));  // left click
  bool btnY = !(mask & (1UL << BUTTON_UP));    // hold to scroll

  // Move when Y not pressed
  if (!btnY) {
    int dx = 0, dy = 0;

    if (abs(dX) > JOY_MOUSE_DEADBAND) {
      dx = (int)(-mouseSpeed * (float)dX);
      dx = imax(imin(dx, MOUSE_MAX_STEP), -MOUSE_MAX_STEP);
    }
    if (abs(dY) > JOY_MOUSE_DEADBAND) {
      dy = (int)(mouseSpeed * (float)dY);
      dy = imax(imin(dy, MOUSE_MAX_STEP), -MOUSE_MAX_STEP);
    }

    if (dx != 0 || dy != 0) bleMouse.move((signed char)dx, (signed char)dy, 0, 0);
  }

  // Scroll when Y pressed
  if (btnY) {
    int wheelV = 0, wheelH = 0;

    if (abs(dY) > JOY_SCROLL_DEADBAND) wheelV = (dY > 0) ? -1 : 1;
    if (abs(dX) > JOY_SCROLL_DEADBAND) wheelH = (dX > 0) ? 1 : -1;

    if (wheelV != 0 || wheelH != 0) bleMouse.move(0, 0, (signed char)wheelV, (signed char)wheelH);
  }

  // Mouse buttons
  if (btnA && !lastRightClick) bleMouse.press(MOUSE_RIGHT);
  else if (!btnA && lastRightClick) bleMouse.release(MOUSE_RIGHT);

  if (btnB && !lastLeftClick) bleMouse.press(MOUSE_LEFT);
  else if (!btnB && lastLeftClick) bleMouse.release(MOUSE_LEFT);

  lastRightClick = btnA;
  lastLeftClick  = btnB;

  // Adjust sensitivity with screen A/B
  BtnEvent eNext = pollButton(bNext);
  BtnEvent ePrev = pollButton(bPrev);

  bool changed = false;
  if (eNext == BTN_SHORT) {
    mouseSpeed += MOUSE_SPEED_STEP;
    if (mouseSpeed > MOUSE_SPEED_MAX) mouseSpeed = MOUSE_SPEED_MAX;
    changed = true;
  } else if (ePrev == BTN_SHORT) {
    mouseSpeed -= MOUSE_SPEED_STEP;
    if (mouseSpeed < MOUSE_SPEED_MIN) mouseSpeed = MOUSE_SPEED_MIN;
    changed = true;
  }

  if (changed) showMouseScreen();
  delay(15);
}

// -------------------- Remote screen / loop --------------------

static void showRemoteScreen() {
  display.clearBuffer();
  display.setRotation(ROTATION);
  display.fillRect(0, 0, display.width(), display.height(), EPD_WHITE);

  const char *path = SD.exists(BOOT_BMP) ? BOOT_BMP : IDLE_BMP;
  int w = 0, h = 0;

  if (drawBMP1bitFromSD_auto(path, 0, 0, &w, &h)) {
    int x = (display.width()  - w) / 2;
    int y = (display.height() - h) / 2;

    display.clearBuffer();
    display.setRotation(ROTATION);
    display.fillRect(0, 0, display.width(), display.height(), EPD_WHITE);
    drawBMP1bitFromSD_auto(path, x, y, nullptr, nullptr);
  }

  u8g2.begin(display);
  u8g2.setFont(u8g2_font_helvR12_tf);
  u8g2.setFontMode(1);
  u8g2.setForegroundColor(EPD_BLACK);
  u8g2.setBackgroundColor(EPD_WHITE);

  SW = display.width();
  SH = display.height();

  const char *title = "Pingo Remote";
  int tw = u8g2.getUTF8Width(title);
  int tx = imax((SW - tw) / 2, 2);
  int ty = SH - 20;
  u8g2.drawUTF8(tx, ty, title);

  display.display();
}

static void enterRemoteMode() {
  stopAllBle();

  gMode = MODE_REMOTE;
  inIdle = false;
  lastActivityMs = millis();

  if (!bleGamepadStarted) {
    bleGamepad.begin();
    bleGamepadStarted = true;
  }

  // Reset outputs
  bleGamepad.setHat1(HAT_CENTERED);
  for (int i = 1; i <= 8; i++) bleGamepad.release((uint16_t)i);

  for (int i = 0; i < 5; i++) lastButtonPressedRemote[i] = false;
  for (int i = 0; i < 3; i++) lastScreenBtnState[i] = false;

  showRemoteScreen();
}

static void remoteLoopStep() {
  // BTN_C long: Remote -> Reader (nextMode)
  BtnEvent eSave = pollButton(bSave);
  if (eSave == BTN_LONG) { nextMode(); return; }

  if (!bleGamepadStarted) { delay(50); return; }
  if (!bleGamepad.isConnected()) { delay(100); return; }

  // 1) Hat from joystick
  JoystickDir dir = mapDirForRotation(getJoystickDir());
  if (dir != lastDirRemote) {
    switch (dir) {
      case DIR_UP:    bleGamepad.setHat1(HAT_UP); break;
      case DIR_DOWN:  bleGamepad.setHat1(HAT_DOWN); break;
      case DIR_LEFT:  bleGamepad.setHat1(HAT_LEFT); break;
      case DIR_RIGHT: bleGamepad.setHat1(HAT_RIGHT); break;
      default:        bleGamepad.setHat1(HAT_CENTERED); break;
    }
    lastDirRemote = dir;
  }

  // 2) JoyWing buttons -> gamepad buttons 1..5
  uint32_t buttons = readJoyButtonsMask();

  bool curPressed[5];
  curPressed[0] = !(buttons & (1UL << BUTTON_RIGHT));
  curPressed[1] = !(buttons & (1UL << BUTTON_DOWN));
  curPressed[2] = !(buttons & (1UL << BUTTON_LEFT));
  curPressed[3] = !(buttons & (1UL << BUTTON_UP));
  curPressed[4] = !(buttons & (1UL << BUTTON_SEL));

  for (int i = 0; i < 5; i++) {
    if (curPressed[i] && !lastButtonPressedRemote[i]) bleGamepad.press(buttonIdRemote[i]);
    else if (!curPressed[i] && lastButtonPressedRemote[i]) bleGamepad.release(buttonIdRemote[i]);
    lastButtonPressedRemote[i] = curPressed[i];
  }

  // 3) Screen buttons -> gamepad buttons 6..8
  bool sA = (digitalRead(BTN_A) == LOW);
  bool sB = (digitalRead(BTN_B) == LOW);
  bool sC = (digitalRead(BTN_C) == LOW);
  bool curScreen[3] = { sA, sB, sC };

  for (int i = 0; i < 3; i++) {
    if (curScreen[i] && !lastScreenBtnState[i]) bleGamepad.press(screenButtonId[i]);
    else if (!curScreen[i] && lastScreenBtnState[i]) bleGamepad.release(screenButtonId[i]);
    lastScreenBtnState[i] = curScreen[i];
  }

  delay(10);
}

// -------------------- Reader loop --------------------

static void readerLoopStep() {
  if (!book) { delay(50); return; }

  BtnEvent eNext = pollButton(bNext);
  BtnEvent ePrev = pollButton(bPrev);
  BtnEvent eSave = pollButton(bSave);

  // A: next page / +10 pages (long)
  if (eNext == BTN_SHORT) {
    if (curPage < pageCount - 1) { curPage++; ensurePageVisible(); }
  } else if (eNext == BTN_LONG) {
    curPage = imin(curPage + 10, pageCount - 1);
    ensurePageVisible();
  }

  // B: prev page / -10 pages (long)
  if (ePrev == BTN_SHORT) {
    if (curPage > 0) { curPage--; ensurePageVisible(); }
  } else if (ePrev == BTN_LONG) {
    curPage = imax(curPage - 10, 0);
    ensurePageVisible();
  }

  // C: short => save bookmark
  // C: long  => menu
  if (eSave == BTN_SHORT) {
    saveBookmark();

    long footerPos = (pageCount > 0 && curPage >= 0 && curPage < pageCount) ? pageIndex[curPage] : 0;

    display.clearBuffer();
    display.setRotation(ROTATION);
    computeTextArea();
    header();
    u8g2.drawUTF8(MARGIN_X, HEADER_H + 22, "Bookmark saved");
    footer(footerPos);
    display.display();
    delay(300);

    renderCurrentPage(false);
    lastActivityMs = millis();
  } else if (eSave == BTN_LONG) {
    menuPrincipal();
    lastActivityMs = millis();
  }

  // Joystick paging
  readerHandleJoystick();

  // Idle screens
  maybeShowIdle();

  // Deep sleep after inactivity
  uint32_t now = millis();
  if (now - lastActivityMs > SUPER_IDLE_TIMEOUT_MS) {
    enterSuperIdle();
    return;
  }

  delay(5);
}

// -------------------- Setup / loop --------------------

void setup() {
  Serial.begin(115200);
  delay(120);

  pinMode(BTN_A, INPUT_PULLUP);
  pinMode(BTN_B, INPUT_PULLUP);
  pinMode(BTN_C, INPUT_PULLUP);

  if (!SD.begin(SD_CS)) {
    Serial.println("SD.begin failed");
    while (true) delay(1000);
  }

  display.begin(THINKINK_GRAYSCALE4);
  u8g2.begin(display);

  Serial.println("Init Joy FeatherWing...");
  if (!ss.begin(JOY_FEATHER_ADDR)) {
    Serial.println("Joy FeatherWing NOT found");
    joyPresent = false;
  } else {
    joyPresent = true;
    Serial.println("Joy FeatherWing OK");

    ss.pinModeBulk(JOY_BUTTON_MASK, INPUT_PULLUP);

    Serial.println("Calibrating joystick...");
    centerX = ss.analogRead(JOY_X_CH);
    centerY = ss.analogRead(JOY_Y_CH);

    for (int i = 0; i < 4; i++) {
      delay(5);
      centerX = (centerX + ss.analogRead(JOY_X_CH)) / 2;
      centerY = (centerY + ss.analogRead(JOY_Y_CH)) / 2;
    }

    Serial.print("Joystick center X=");
    Serial.print(centerX);
    Serial.print(" Y=");
    Serial.println(centerY);
  }

  showBootSplash();

  // Hold PREV during boot to clear last book
  bool clear = (digitalRead(PREV_BTN) == LOW);
  uint32_t t0 = millis();
  while (millis() - t0 < 800) {
    if (digitalRead(PREV_BTN) != LOW) { clear = false; break; }
  }
  if (clear) {
    sdRemoveIfExistsC(LAST_BOOK_FILE);
  }

  startupModeMenu();
  lastActivityMs = millis();
}

void loop() {
  // JoyWing SELECT long: Reader -> Mouse, Mouse -> Remote
  checkJoySelectLongPress();

  if (gMode == MODE_READER) readerLoopStep();
  else if (gMode == MODE_MOUSE) mouseLoopStep();
  else remoteLoopStep();
}