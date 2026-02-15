#include "launcher_icons.h"

#include <string.h>

namespace {

constexpr int kIconCount = 4;
constexpr int kMainW = 46;
constexpr int kMainH = 46;
constexpr int kSideW = 24;
constexpr int kSideH = 24;

uint8_t gMainData[kIconCount][kMainW * kMainH];
uint8_t gSideData[kIconCount][kSideW * kSideH];
lv_image_dsc_t gMainDsc[kIconCount];
lv_image_dsc_t gSideDsc[kIconCount];
bool gInitialized = false;

inline void clear(uint8_t *buf, int len) {
  memset(buf, 0, static_cast<size_t>(len));
}

inline void put(uint8_t *buf, int w, int h, int x, int y, uint8_t v = 255) {
  if (x < 0 || y < 0 || x >= w || y >= h) {
    return;
  }
  buf[y * w + x] = v;
}

void fillRect(uint8_t *buf, int w, int h, int x, int y, int rw, int rh, uint8_t v = 255) {
  for (int yy = y; yy < y + rh; ++yy) {
    for (int xx = x; xx < x + rw; ++xx) {
      put(buf, w, h, xx, yy, v);
    }
  }
}

void drawRect(uint8_t *buf, int w, int h, int x, int y, int rw, int rh, int t = 1) {
  for (int i = 0; i < t; ++i) {
    fillRect(buf, w, h, x + i, y + i, rw - (i * 2), 1);
    fillRect(buf, w, h, x + i, y + rh - 1 - i, rw - (i * 2), 1);
    fillRect(buf, w, h, x + i, y + i, 1, rh - (i * 2));
    fillRect(buf, w, h, x + rw - 1 - i, y + i, 1, rh - (i * 2));
  }
}

void drawLine(uint8_t *buf, int w, int h, int x0, int y0, int x1, int y1, int t = 1) {
  int dx = abs(x1 - x0);
  int sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0);
  int sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;

  while (true) {
    const int hs = t / 2;
    for (int yy = y0 - hs; yy <= y0 + hs; ++yy) {
      for (int xx = x0 - hs; xx <= x0 + hs; ++xx) {
        put(buf, w, h, xx, yy);
      }
    }
    if (x0 == x1 && y0 == y1) {
      break;
    }
    const int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

void drawCircle(uint8_t *buf, int w, int h, int cx, int cy, int r, int t = 1, uint8_t v = 255) {
  if (r <= 0) {
    return;
  }

  for (int rr = r; rr > r - t; --rr) {
    int x = rr;
    int y = 0;
    int err = 0;
    while (x >= y) {
      put(buf, w, h, cx + x, cy + y, v);
      put(buf, w, h, cx + y, cy + x, v);
      put(buf, w, h, cx - y, cy + x, v);
      put(buf, w, h, cx - x, cy + y, v);
      put(buf, w, h, cx - x, cy - y, v);
      put(buf, w, h, cx - y, cy - x, v);
      put(buf, w, h, cx + y, cy - x, v);
      put(buf, w, h, cx + x, cy - y, v);
      ++y;
      if (err <= 0) {
        err += 2 * y + 1;
      } else {
        --x;
        err += 2 * (y - x) + 1;
      }
    }
  }
}

void fillCircle(uint8_t *buf, int w, int h, int cx, int cy, int r, uint8_t v = 255) {
  if (r <= 0) {
    return;
  }

  const int rr = r * r;
  for (int y = -r; y <= r; ++y) {
    for (int x = -r; x <= r; ++x) {
      if ((x * x) + (y * y) <= rr) {
        put(buf, w, h, cx + x, cy + y, v);
      }
    }
  }
}

void drawAppMarketIcon(uint8_t *buf, int w, int h) {
  clear(buf, w * h);

  const int cx = w / 2;
  const int boxW = (w * 24) / 46;
  const int boxH = (h * 13) / 46;
  const int boxX = cx - (boxW / 2);
  const int boxY = (h * 22) / 46;

  drawRect(buf, w, h, boxX, boxY, boxW, boxH, 2);
  drawRect(buf, w, h, boxX + 3, boxY - 5, boxW - 6, 4, 1);

  const int stemTop = (h * 8) / 46;
  const int stemBottom = boxY - 2;
  drawLine(buf, w, h, cx, stemTop, cx, stemBottom, 2);

  for (int i = 0; i < 5; ++i) {
    fillRect(buf, w, h, cx - i, stemBottom + i, i * 2 + 1, 1);
  }
}

void drawSettingsIcon(uint8_t *buf, int w, int h) {
  clear(buf, w * h);

  const int cx = w / 2;
  const int cy = h / 2;
  int outerR = (w * 10) / 46;
  if (outerR < 5) {
    outerR = 5;
  }
  int innerR = (w * 4) / 46;
  if (innerR < 2) {
    innerR = 2;
  }
  int toothLen = (w * 4) / 46;
  if (toothLen < 2) {
    toothLen = 2;
  }
  int toothW = (w * 4) / 46;
  if (toothW < 2) {
    toothW = 2;
  }
  const int diag = (outerR * 7) / 10;

  fillCircle(buf, w, h, cx, cy, outerR, 255);
  fillCircle(buf, w, h, cx, cy, innerR, 0);

  // Cardinal teeth
  fillRect(buf, w, h, cx - (toothW / 2), cy - outerR - toothLen + 1, toothW, toothLen);
  fillRect(buf, w, h, cx - (toothW / 2), cy + outerR, toothW, toothLen);
  fillRect(buf, w, h, cx - outerR - toothLen + 1, cy - (toothW / 2), toothLen, toothW);
  fillRect(buf, w, h, cx + outerR, cy - (toothW / 2), toothLen, toothW);

  // Diagonal teeth
  fillRect(buf, w, h, cx - diag - (toothW / 2), cy - diag - (toothW / 2), toothW, toothW);
  fillRect(buf, w, h, cx + diag - (toothW / 2), cy - diag - (toothW / 2), toothW, toothW);
  fillRect(buf, w, h, cx - diag - (toothW / 2), cy + diag - (toothW / 2), toothW, toothW);
  fillRect(buf, w, h, cx + diag - (toothW / 2), cy + diag - (toothW / 2), toothW, toothW);

  drawCircle(buf, w, h, cx, cy, outerR, 1, 255);
  if (outerR > 2) {
    drawCircle(buf, w, h, cx, cy, outerR - 2, 1, 0);
  }
}

void drawFileExplorerIcon(uint8_t *buf, int w, int h) {
  clear(buf, w * h);

  const int fw = (w * 30) / 46;
  const int fh = (h * 18) / 46;
  const int fx = (w - fw) / 2;
  const int fy = (h * 18) / 46;

  drawRect(buf, w, h, fx, fy, fw, fh, 2);

  const int tabW = (w * 12) / 46;
  const int tabH = (h * 5) / 46;
  drawRect(buf, w, h, fx + 2, fy - tabH + 1, tabW, tabH, 1);

  fillRect(buf, w, h, fx + 4, fy + 6, fw - 8, 2);
}

void drawOpenClawIcon(uint8_t *buf, int w, int h) {
  clear(buf, w * h);

  const int cx = w / 2;
  const int cy = (h * 24) / 46;
  const int nodeR = (w * 3) / 46;

  const int lx = (w * 12) / 46;
  const int ly = (h * 15) / 46;
  const int rx = (w * 34) / 46;
  const int ry = (h * 15) / 46;
  const int bx = cx;
  const int by = (h * 34) / 46;

  drawLine(buf, w, h, cx, cy, lx, ly, 2);
  drawLine(buf, w, h, cx, cy, rx, ry, 2);
  drawLine(buf, w, h, cx, cy, bx, by, 2);
  drawLine(buf, w, h, lx, ly, rx, ry, 1);

  drawCircle(buf, w, h, cx, cy, nodeR + 1, 2);
  fillRect(buf, w, h, cx - 1, cy - 1, 3, 3);
  drawCircle(buf, w, h, lx, ly, nodeR, 2);
  drawCircle(buf, w, h, rx, ry, nodeR, 2);
  drawCircle(buf, w, h, bx, by, nodeR, 2);
}

void buildSet(uint8_t *mainBuf, uint8_t *sideBuf) {
  drawAppMarketIcon(mainBuf + (0 * kMainW * kMainH), kMainW, kMainH);
  drawSettingsIcon(mainBuf + (1 * kMainW * kMainH), kMainW, kMainH);
  drawFileExplorerIcon(mainBuf + (2 * kMainW * kMainH), kMainW, kMainH);
  drawOpenClawIcon(mainBuf + (3 * kMainW * kMainH), kMainW, kMainH);

  drawAppMarketIcon(sideBuf + (0 * kSideW * kSideH), kSideW, kSideH);
  drawSettingsIcon(sideBuf + (1 * kSideW * kSideH), kSideW, kSideH);
  drawFileExplorerIcon(sideBuf + (2 * kSideW * kSideH), kSideW, kSideH);
  drawOpenClawIcon(sideBuf + (3 * kSideW * kSideH), kSideW, kSideH);
}

void setupDsc(lv_image_dsc_t &dsc, const uint8_t *data, int w, int h) {
  dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  dsc.header.cf = LV_COLOR_FORMAT_A8;
  dsc.header.flags = 0;
  dsc.header.w = static_cast<uint16_t>(w);
  dsc.header.h = static_cast<uint16_t>(h);
  dsc.header.stride = static_cast<uint16_t>(w);
  dsc.header.reserved_2 = 0;
  dsc.data_size = static_cast<uint32_t>(w * h);
  dsc.data = data;
  dsc.reserved = nullptr;
  dsc.reserved_2 = nullptr;
}

}  // namespace

bool initLauncherIcons() {
  if (gInitialized) {
    return true;
  }

  buildSet(&gMainData[0][0], &gSideData[0][0]);

  for (int i = 0; i < kIconCount; ++i) {
    setupDsc(gMainDsc[i], &gMainData[i][0], kMainW, kMainH);
    setupDsc(gSideDsc[i], &gSideData[i][0], kSideW, kSideH);
  }

  gInitialized = true;
  return true;
}

bool launcherIconsReady() {
  return gInitialized;
}

const lv_image_dsc_t *getLauncherIcon(LauncherIconId id, LauncherIconVariant variant) {
  if (!gInitialized) {
    return nullptr;
  }

  const int iconIndex = static_cast<int>(id);
  if (iconIndex < 0 || iconIndex >= kIconCount) {
    return nullptr;
  }

  if (variant == LauncherIconVariant::Side) {
    return &gSideDsc[iconIndex];
  }
  return &gMainDsc[iconIndex];
}
