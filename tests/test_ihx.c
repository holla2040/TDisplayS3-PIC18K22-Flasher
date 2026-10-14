// Host test for ihx.cpp: make test
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "ihx.h"

enum { FLASH_SZ = 65536, UID_SZ = 8, CFG_SZ = 14, EE_SZ = 1024 };

struct img_buf {
  uint8_t d0[FLASH_SZ], m0[FLASH_SZ], d1[UID_SZ], m1[UID_SZ];
  uint8_t d2[CFG_SZ], m2[CFG_SZ], d3[EE_SZ], m3[EE_SZ];
  ihx_image img;
};

static void setup(img_buf *b, uint32_t flash_sz = FLASH_SZ) {
  b->img.r[IHX_FLASH]  = (ihx_region){0x000000, flash_sz, b->d0, b->m0};
  b->img.r[IHX_USERID] = (ihx_region){0x200000, UID_SZ,   b->d1, b->m1};
  b->img.r[IHX_CONFIG] = (ihx_region){0x300000, CFG_SZ,   b->d2, b->m2};
  b->img.r[IHX_EEPROM] = (ihx_region){0xF00000, EE_SZ,    b->d3, b->m3};
}

// Appends one record with a computed checksum.
static void rec(char *o, int type, unsigned addr, const uint8_t *d, int n) {
  unsigned sum = n + (addr >> 8) + (addr & 0xFF) + type;
  o += strlen(o);
  o += sprintf(o, ":%02X%04X%02X", n, addr & 0xFFFF, type);
  for (int i = 0; i < n; i++) { o += sprintf(o, "%02X", d[i]); sum += d[i]; }
  sprintf(o, "%02X\n", (-sum) & 0xFF);
}

static void test_ihx_roundtrip() {
  static img_buf a, b;
  static char hex[8192], out[4][8192], all[32768];
  setup(&a);
  uint8_t p20[2] = {0x00, 0x20}, p30[2] = {0x00, 0x30}, pF0[2] = {0x00, 0xF0};
  uint8_t f[4] = {0xEF, 0x04, 0xF0, 0x00}, u[8] = {1, 2, 3, 4, 5, 6, 7, 8}, c = 0x28, e = 0x5A;
  hex[0] = 0;
  rec(hex, 0, 0x0000, f, 4);
  rec(hex, 4, 0, p20, 2);
  rec(hex, 0, 0x0000, u, 8);
  rec(hex, 4, 0, p30, 2);
  rec(hex, 0, 0x0001, &c, 1);
  rec(hex, 4, 0, pF0, 2);
  rec(hex, 0, 0x03FF, &e, 1);
  strcat(hex, ":00000001FF\n");
  char err[64];
  assert(ihx_parse(hex, &a.img, err, sizeof err));
  assert(a.d0[0] == 0xEF && a.d0[3] == 0x00 && a.m0[3] == 1 && a.m0[4] == 0 && a.d0[4] == 0xFF);
  assert(a.d1[7] == 8 && a.m1[0] == 1 && a.d2[1] == 0x28 && a.m2[0] == 0 && a.d3[0x3FF] == 0x5A);

  all[0] = 0;
  for (int r = 0; r < IHX_NREGIONS; r++) {
    size_t n = ihx_write(&a.img, r, out[r], sizeof out[r]);
    assert(n > 0 && n == strlen(out[r]));
    strcat(all, out[r]);
  }
  setup(&b);
  memset(b.d0, 0x11, sizeof b.d0);   // parse must clear these
  memset(b.m0, 1, sizeof b.m0);
  assert(ihx_parse(all, &b.img, err, sizeof err));
  assert(!memcmp(a.d0, b.d0, sizeof a.d0) && !memcmp(a.m0, b.m0, sizeof a.m0));
  assert(!memcmp(a.d1, b.d1, sizeof a.d1) && !memcmp(a.m1, b.m1, sizeof a.m1));
  assert(!memcmp(a.d2, b.d2, sizeof a.d2) && !memcmp(a.m2, b.m2, sizeof a.m2));
  assert(!memcmp(a.d3, b.d3, sizeof a.d3) && !memcmp(a.m3, b.m3, sizeof a.m3));

  // Too small an output buffer reports 0.
  assert(ihx_write(&a.img, IHX_FLASH, out[0], 10) == 0);
}

static void test_ihx_xc8_layout() {
  static img_buf a;
  setup(&a);
  uint8_t cfg[14], uid[8];
  for (int i = 0; i < 14; i++) cfg[i] = 0x10 + i;
  for (int i = 0; i < 8; i++) uid[i] = 0xA0 + i;
  char hex[512] = ":020000040030CA\n", err[64];
  rec(hex, 0, 0x0000, cfg, 14);
  strcat(hex, ":020000040020DA\n");
  rec(hex, 0, 0x0000, uid, 8);
  strcat(hex, ":00000001FF\n");
  assert(ihx_parse(hex, &a.img, err, sizeof err));
  for (int i = 0; i < 14; i++) assert(a.m2[i] == 1 && a.d2[i] == cfg[i]);
  for (int i = 0; i < 8; i++) assert(a.m1[i] == 1 && a.d1[i] == uid[i]);
}

static void test_ihx_gap_rejected() {
  static img_buf a;
  setup(&a);
  uint8_t v = 0x55;
  char hex[128] = ":020000040020DA\n", err[64];
  rec(hex, 0, 0x0008, &v, 1);
  strcat(hex, ":00000001FF\n");
  assert(!ihx_parse(hex, &a.img, err, sizeof err));
  assert(strstr(err, "0x200008"));
}

static void test_ihx_bad_checksum() {
  static img_buf a;
  setup(&a);
  char err[64];
  assert(!ihx_parse(":0100000055AB\n:00000001FF\n", &a.img, err, sizeof err));   // correct is AA
  assert(ihx_parse(":0100000055AA\n:00000001FF\n", &a.img, err, sizeof err));
  assert(!ihx_parse(":020000020000FC\n:00000001FF\n", &a.img, err, sizeof err)); // type 02 rejected
}

static void test_ihx_small_part() {
  static img_buf a;
  setup(&a, 8192);
  uint8_t v = 0x55;
  char hex[128] = "", err[64];
  rec(hex, 0, 0x2000, &v, 1);
  strcat(hex, ":00000001FF\n");
  assert(!ihx_parse(hex, &a.img, err, sizeof err));
  assert(strstr(err, "0x002000"));
}

static void test_ihx_truncated() {
  static img_buf a;
  setup(&a);
  char err[64];
  // Heap copies so ASan catches any read past the NUL.
  const char *cases[] = {":", ":0", ":01", ":010", ":0100", ":01000000", ":0100000055", ":0400000001020"};
  for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
    char *s = strdup(cases[i]);
    assert(!ihx_parse(s, &a.img, err, sizeof err));
    free(s);
  }
}

// Compact binary: "PICB", [u32 addr][u16 len][data]..., CRC32. Builds one in buf.
static size_t pbin(uint8_t *buf, const uint32_t *addr, const uint8_t *const *data, const uint16_t *n, int segs) {
  size_t p = 4;
  memcpy(buf, "PICB", 4);
  for (int i = 0; i < segs; i++) {
    for (int k = 0; k < 4; k++) buf[p++] = addr[i] >> 8 * k;
    buf[p++] = n[i]; buf[p++] = n[i] >> 8;
    memcpy(buf + p, data[i], n[i]); p += n[i];
  }
  uint32_t c = ihx_crc32(buf, p);
  for (int k = 0; k < 4; k++) buf[p++] = c >> 8 * k;
  return p;
}

static void test_pbin() {
  static img_buf a, b;
  static uint8_t buf[256];
  char err[48];
  assert(ihx_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926);   // zlib check value
  setup(&a); setup(&b);
  uint8_t f[4] = {0xEF, 0x04, 0xF0, 0x00}, c[2] = {0x28, 0x85}, e = 0x5A;
  const uint32_t addr[3] = {0x000000, 0x300001, 0xF00003};
  const uint8_t *const data[3] = {f, c, &e};
  const uint16_t n[3] = {4, 2, 1};
  size_t len = pbin(buf, addr, data, n, 3);
  assert(ihx_parse_any(buf, len, &a.img, err, sizeof err));
  // same image as the equivalent HEX
  char hex[512] = "";
  uint8_t p30[2] = {0x00, 0x30}, pF0[2] = {0x00, 0xF0};
  rec(hex, 0, 0, f, 4); rec(hex, 4, 0, p30, 2); rec(hex, 0, 1, c, 2);
  rec(hex, 4, 0, pF0, 2); rec(hex, 0, 3, &e, 1); strcat(hex, ":00000001FF\n");
  assert(ihx_parse_any((const uint8_t *)hex, strlen(hex), &b.img, err, sizeof err));
  assert(!memcmp(&a, &b, offsetof(img_buf, img)));
  // corrupt one data byte: bad CRC
  buf[11] ^= 1;
  assert(!ihx_parse_any(buf, len, &a.img, err, sizeof err) && !strcmp(err, "bad CRC"));
  buf[11] ^= 1;
  // a length running past the end (CRC recomputed so only the framing is wrong)
  const uint16_t big[1] = {200};
  len = pbin(buf, addr, data, n, 1);
  buf[8] = big[0];
  uint32_t cc = ihx_crc32(buf, len - 4);
  for (int k = 0; k < 4; k++) buf[len - 4 + k] = cc >> 8 * k;
  assert(!ihx_parse_any(buf, len, &a.img, err, sizeof err) && !strcmp(err, "malformed pbin"));
  // an address outside every region
  const uint32_t out[1] = {0x200008};
  len = pbin(buf, out, data, n, 1);
  assert(!ihx_parse_any(buf, len, &a.img, err, sizeof err) && !strcmp(err, "0x200008 outside chip"));
}

int main() {
  test_ihx_roundtrip();
  test_ihx_xc8_layout();
  test_ihx_gap_rejected();
  test_ihx_bad_checksum();
  test_ihx_small_part();
  test_ihx_truncated();
  test_pbin();
  puts("ok");
  return 0;
}
