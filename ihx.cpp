#include <stdio.h>
#include <string.h>
#include "ihx.h"

static int hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

static int hex_byte(const char *p) {
  int hi = hex_nibble(p[0]);
  if (hi < 0) return -1;   // also stops us reading past the NUL
  int lo = hex_nibble(p[1]);
  return lo < 0 ? -1 : (hi << 4) | lo;
}

static bool fail(char *err, size_t errlen, const char *fmt, uint32_t v) {
  if (err && errlen) snprintf(err, errlen, fmt, (unsigned)v);
  return false;
}

// Stores one byte at absolute address `a`, or false if no region covers it.
static bool put(ihx_image *img, uint32_t a, uint8_t b) {
  for (int r = 0; r < IHX_NREGIONS; r++) {
    ihx_region *g = &img->r[r];
    if (a >= g->base && a - g->base < g->size) {
      g->data[a - g->base] = b;
      g->mask[a - g->base] = 1;
      return true;
    }
  }
  return false;
}

bool ihx_parse(const char *text, ihx_image *img, char *err, size_t errlen) {
  for (int r = 0; r < IHX_NREGIONS; r++) {
    memset(img->r[r].data, 0xFF, img->r[r].size);
    memset(img->r[r].mask, 0, img->r[r].size);
  }
  uint32_t ext = 0;     // extended linear address (record type 04) << 16

  for (const char *p = text; *p; ) {
    while (*p && *p != ':') p++;
    if (!*p) break;
    p++;

    // Each read stops at the first bad/short field, so we never look past the NUL.
    int count = hex_byte(p), ah = -1, al = -1, type = -1;
    if (count >= 0 && (ah = hex_byte(p + 2)) >= 0 && (al = hex_byte(p + 4)) >= 0)
      type = hex_byte(p + 6);
    if (type < 0) return fail(err, errlen, "malformed record", 0);

    uint8_t sum = count + ah + al + type;
    const char *data = p + 8;
    if (type != 0x00 && type != 0x01 && type != 0x04)
      return fail(err, errlen, "unsupported record type %02X", type);
    if (type == 0x04 && count != 2) return fail(err, errlen, "bad type 04 record", 0);
    if (type == 0x01 && count != 0) return fail(err, errlen, "bad EOF record", 0);

    uint32_t addr = ext | (ah << 8) | al;
    uint8_t buf[256];   // count is a byte, so this holds any record
    for (int i = 0; i < count; i++) {
      int b = hex_byte(data + i * 2);
      if (b < 0) return fail(err, errlen, "malformed record", 0);
      sum += b;
      buf[i] = b;
    }
    int cksum = hex_byte(data + count * 2);
    if (cksum < 0 || (uint8_t)(sum + cksum) != 0) return fail(err, errlen, "bad checksum", 0);

    if (type == 0x00) {
      for (int i = 0; i < count; i++)
        if (!put(img, addr + i, buf[i])) return fail(err, errlen, "0x%06X outside chip", addr + i);
    } else if (type == 0x04) {
      ext = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16);
    }
    // 0x01 (EOF): keep scanning so concatenated per-region dumps parse in one call
    p = data + count * 2 + 2;
  }
  return true;   // no EOF record — tolerate, some tools omit it
}

size_t ihx_write(const ihx_image *img, int region, char *out, size_t outmax) {
  const ihx_region *g = &img->r[region];
  char *o = out;
  size_t left = outmax;
  int64_t page = -1;    // last type-04 page emitted
  uint32_t i = 0;

  // Appends one finished record line; false if it doesn't fit (with NUL).
  auto emit = [&](int type, uint32_t addr, const uint8_t *d, int n) {
    char line[16 + 2 * 16 + 4];
    unsigned sum = n + (addr >> 8 & 0xFF) + (addr & 0xFF) + type;
    int len = snprintf(line, sizeof line, ":%02X%04X%02X", n, (unsigned)(addr & 0xFFFF), type);
    for (int k = 0; k < n; k++) { len += snprintf(line + len, sizeof line - len, "%02X", d[k]); sum += d[k]; }
    len += snprintf(line + len, sizeof line - len, "%02X\n", (unsigned)(-sum & 0xFF));
    if ((size_t)len + 1 > left) return false;
    memcpy(o, line, len + 1);
    o += len; left -= len;
    return true;
  };

  while (i < g->size) {
    if (!g->mask[i]) { i++; continue; }
    uint32_t abs = g->base + i;
    uint32_t n = 1;     // run of present bytes, max 16, not crossing a 64 KB page
    while (n < 16 && i + n < g->size && g->mask[i + n] && ((abs + n) & 0xFFFF)) n++;
    if ((int64_t)(abs >> 16) != page) {
      page = abs >> 16;
      uint8_t pg[2] = {(uint8_t)(page >> 8), (uint8_t)page};
      if (!emit(0x04, 0, pg, 2)) return 0;
    }
    if (!emit(0x00, abs & 0xFFFF, g->data + i, n)) return 0;
    i += n;
  }
  if (!emit(0x01, 0, nullptr, 0)) return 0;
  return o - out;
}
