// Host test for script.cpp against a fake chip: make test
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "script.h"

enum { FLASH_SZ = 0x8000, UID_SZ = 8, CFG_SZ = 14, EE_SZ = 256 };

// ---- fake chip: static arrays per region, call log, fail the Nth call ----
static uint8_t chip[IHX_NREGIONS][FLASH_SZ];
static uint8_t cfg_mask[CFG_SZ];
static char logbuf[256];
static int calls, fail_at;                  // fail_at: 1-based call number that returns false (0 = never)
static bool locked, corrupt_flash_104;
static const uint32_t SIZES[IHX_NREGIONS] = {FLASH_SZ, UID_SZ, CFG_SZ, EE_SZ};

static void logit(const char *s) { strcat(logbuf, s); strcat(logbuf, " "); }
static bool tick() { return ++calls != fail_at; }

static bool f_read(int r, uint32_t off, uint8_t *buf, size_t n) {
  char t[8]; snprintf(t, sizeof t, "r%d", r); logit(t);
  if (!tick()) return false;
  memcpy(buf, chip[r] + off, n);
  return true;
}
static bool f_write(int r, uint32_t off, const uint8_t *buf, const uint8_t *mask, size_t n) {
  char t[8]; snprintf(t, sizeof t, "w%d", r); logit(t);
  if (!tick()) return false;
  for (size_t i = 0; i < n; i++) {
    if (!mask[i]) continue;
    chip[r][off + i] = r == IHX_CONFIG ? buf[i] & cfg_mask[i] : buf[i];   // unimplemented bits read 0
  }
  if (corrupt_flash_104 && r == IHX_FLASH) chip[IHX_FLASH][0x104] ^= 0x01;
  return true;
}
static bool f_erase(bool c) {
  logit(c ? "Ec" : "Ee");
  if (!tick()) return false;
  memset(chip[IHX_EEPROM], 0xFF, EE_SZ);
  if (c) {
    memset(chip[IHX_FLASH], 0xFF, FLASH_SZ);
    memset(chip[IHX_USERID], 0xFF, UID_SZ);
    for (int i = 0; i < CFG_SZ; i++) chip[IHX_CONFIG][i] = 0xFF & cfg_mask[i];
  }
  return true;
}
static bool f_locked() { return locked; }
static void f_reset() { logit("X"); }

static const pic_ops DEV_TEMPLATE = {{FLASH_SZ, UID_SZ, CFG_SZ, EE_SZ}, cfg_mask, f_read, f_write, f_erase, f_locked, f_reset};

// ---- in-memory fs ----
struct file { char name[40]; char *text; };
static file files[16];

static file *find(const char *name) {
  for (auto &f : files) if (f.text && !strcmp(f.name, name)) return &f;
  return nullptr;
}
static char *fs_load(const char *name) { file *f = find(name); return f ? strdup(f->text) : nullptr; }
static bool fs_save(const char *name, const char *text, size_t len) {
  file *f = find(name);
  for (int i = 0; !f && i < 16; i++) if (!files[i].text) f = &files[i];
  if (!f) return false;
  free(f->text);
  f->text = strndup(text, len);
  snprintf(f->name, sizeof f->name, "%s", name);
  return true;
}
static const fs_ops FS = {fs_load, fs_save};

// Appends one HEX record with a computed checksum.
static void rec(char *o, int type, unsigned addr, const uint8_t *d, int n) {
  unsigned sum = n + (addr >> 8) + (addr & 0xFF) + type;
  o += strlen(o);
  o += sprintf(o, ":%02X%04X%02X", n, addr & 0xFFFF, type);
  for (int i = 0; i < n; i++) { o += sprintf(o, "%02X", d[i]); sum += d[i]; }
  sprintf(o, "%02X\n", (-sum) & 0xFF);
}

static const uint8_t UID[8] = {1, 2, 3, 4, 5, 6, 7, 8};
static const char *const DEFAULT_SCRIPT =
  "read   userid  userid-{id}.hex      # chip erase wipes the user ID, so save it first\n"
  "read   eeprom  settings-{id}.hex\n"
  "erase  chip\n"
  "write  firmware.hex\n"
  "write  userid-{id}.hex              # after firmware, so this board's ID wins\n"
  "write  settings-{id}.hex\n"
  "verify firmware.hex\n"
  "verify userid-{id}.hex\n"
  "verify settings-{id}.hex\n"
  "reset\n";

static void set_file(const char *name, const char *text) { fs_save(name, text, strlen(text)); }

static void reset_world() {
  for (auto &f : files) { free(f.text); f.text = nullptr; }
  memset(chip, 0xFF, sizeof chip);
  memcpy(chip[IHX_USERID], UID, 8);
  for (int i = 0; i < EE_SZ; i++) chip[IHX_EEPROM][i] = (uint8_t)(i * 5 + 3);
  memset(cfg_mask, 0xFF, sizeof cfg_mask);
  logbuf[0] = 0;
  calls = fail_at = 0;
  locked = corrupt_flash_104 = false;
  // firmware.hex: flash 0x000..0x1FF, config bytes 0..1
  static char hex[4096];
  uint8_t fl[16], c[2] = {0x28, 0x1F}, p30[2] = {0x00, 0x30};
  hex[0] = 0;
  for (int a = 0; a < 0x200; a += 16) {
    for (int i = 0; i < 16; i++) fl[i] = (uint8_t)((a + i) * 7 + 1);
    rec(hex, 0, a, fl, 16);
  }
  rec(hex, 4, 0, p30, 2);
  rec(hex, 0, 0, c, 2);
  strcat(hex, ":00000001FF\n");
  set_file("firmware.hex", hex);
}

static script_status run(const char *script) {
  pic_ops dev = DEV_TEMPLATE;
  return script_run(script, &dev, &FS, nullptr);
}

static void expect(script_status s, script_result r, int line, const char *msg) {
  if (s.result != r || s.line != line || strcmp(s.msg, msg)) {
    fprintf(stderr, "got result=%d line=%d msg='%s', want result=%d line=%d msg='%s'\n",
            s.result, s.line, s.msg, r, line, msg);
    abort();
  }
}

static void check_default_flow(const char *script) {
  reset_world();
  uint8_t ee[EE_SZ];
  memcpy(ee, chip[IHX_EEPROM], EE_SZ);
  expect(run(script), SCRIPT_PASS, 0, "");
  assert(find("settings-0102030405060708.hex"));
  assert(find("userid-0102030405060708.hex"));
  assert(!strcmp(logbuf, "r1 r1 r3 Ec w0 w2 w1 w3 r0 r2 r1 r3 X "));   // id, read userid, read eeprom, erase, write..., verify..., reset
  assert(!memcmp(chip[IHX_EEPROM], ee, EE_SZ));
  assert(!memcmp(chip[IHX_USERID], UID, 8));
  assert(chip[IHX_FLASH][0x104] == (uint8_t)(0x104 * 7 + 1));
}

static void test_script_default_flow() { check_default_flow(DEFAULT_SCRIPT); }

static void test_script_crlf() {
  char crlf[1024] = "";
  for (const char *p = DEFAULT_SCRIPT; *p; p++) {
    if (*p == '\n') strcat(crlf, "  \t\r");
    strncat(crlf, p, 1);
  }
  check_default_flow(crlf);
}

static void test_script_blank_id() {
  reset_world();
  memset(chip[IHX_USERID], 0xFF, UID_SZ);
  expect(run(DEFAULT_SCRIPT), SCRIPT_ERROR, 1, "blank userid");
  assert(!strcmp(logbuf, "r1 "));   // only the id read, no erase
}

static void test_script_blank_id_after_failed_run() {
  reset_world();
  fail_at = 5;   // r1 id, r1 userid, r3 eeprom, Ec, w0 fails: erase already wiped the userid
  expect(run(DEFAULT_SCRIPT), SCRIPT_COMM, 4, "");
  char saved[8192];
  strcpy(saved, find("settings-0102030405060708.hex")->text);
  fail_at = 0;
  logbuf[0] = 0;
  expect(run(DEFAULT_SCRIPT), SCRIPT_ERROR, 1, "blank userid");
  assert(!strstr(logbuf, "E"));
  assert(!strcmp(saved, find("settings-0102030405060708.hex")->text));
}

static void test_script_ee_locked() {
  reset_world();
  locked = true;
  expect(run(DEFAULT_SCRIPT), SCRIPT_ERROR, 2, "EE locked");
  assert(!strstr(logbuf, "E"));
}

static void test_script_missing_file() {
  reset_world();
  free(find("firmware.hex")->text);
  find("firmware.hex")->text = nullptr;
  expect(run(DEFAULT_SCRIPT), SCRIPT_ERROR, 4, "no firmware.hex");
  assert(!strcmp(logbuf, "r1 "));   // caught in preflight: only the id read, no erase
  // a file produced by an earlier read in the same script is fine
  logbuf[0] = 0;
  expect(run("read userid u-{id}.hex\nwrite u-{id}.hex\n"), SCRIPT_PASS, 0, "");
  // ...but not one produced only later, or under a different name
  logbuf[0] = 0;
  expect(run("write u.hex\nread userid u.hex\n"), SCRIPT_ERROR, 1, "no u.hex");
  expect(run("read userid a.hex\nverify b.hex\n"), SCRIPT_ERROR, 2, "no b.hex");
  assert(!logbuf[0]);
}

static void test_script_verify_mismatch() {
  reset_world();
  corrupt_flash_104 = true;
  expect(run(DEFAULT_SCRIPT), SCRIPT_FAIL, 7, "verify 0x000104");
}

static void test_script_comm() {
  reset_world();
  fail_at = 2;   // id read ok, "read userid" fails
  expect(run(DEFAULT_SCRIPT), SCRIPT_COMM, 1, "");
}

static void test_script_config_mask() {
  reset_world();
  cfg_mask[0] = 0x00;   // firmware wants 0x28; chip reads 0x00; the mask says don't care
  expect(run("write firmware.hex\nverify firmware.hex\n"), SCRIPT_PASS, 0, "");
  assert(chip[IHX_CONFIG][0] == 0x00 && chip[IHX_CONFIG][1] == 0x1F);
  chip[IHX_CONFIG][1] ^= 0x01;   // an implemented bit differs: that must fail
  expect(run("verify firmware.hex\n"), SCRIPT_FAIL, 1, "verify 0x300001");
}

static void test_script_syntax() {
  reset_world();
  expect(run("frobnicate\n"), SCRIPT_ERROR, 1, "syntax");
  // wrong arg counts and bad words are caught before any chip access, even after valid lines
  const char *bad[] = {"erase\n", "erase flash\n", "write\n", "write a b\n", "read flash\n",
                       "read nothing f.hex\n", "reset now\n", "verify\n", "read a b c d\n"};
  for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) {
    char s[64] = "erase chip\n";
    strcat(s, bad[i]);
    logbuf[0] = 0;
    expect(run(s), SCRIPT_ERROR, 2, "syntax");
    assert(!logbuf[0]);
  }
}

static void test_script_read_regions() {
  reset_world();
  static const char *s = "# only comments and blanks\n\nread flash f.hex\nread config c.hex\nread eeprom e.hex\n";
  expect(run(s), SCRIPT_PASS, 0, "");
  assert(find("f.hex") && find("c.hex") && find("e.hex"));
  expect(run("erase chip\nwrite f.hex\nwrite c.hex\nwrite e.hex\nverify f.hex\nverify c.hex\nverify e.hex\n"),
         SCRIPT_PASS, 0, "");
}

static int prog_n, prog_last;
static char prog_text[64];
static void prog(int line, const char *text) { prog_n++; prog_last = line; snprintf(prog_text, sizeof prog_text, "%s", text); }

static void test_script_progress() {
  reset_world();
  pic_ops dev = DEV_TEMPLATE;
  script_status s = script_run("# c\n\n  erase   chip  # x\nreset\n", &dev, &FS, prog);
  expect(s, SCRIPT_PASS, 0, "");
  assert(prog_n == 2 && prog_last == 4 && !strcmp(prog_text, "reset"));
}

int main() {
  test_script_default_flow();
  test_script_blank_id();
  test_script_blank_id_after_failed_run();
  test_script_ee_locked();
  test_script_missing_file();
  test_script_verify_mismatch();
  test_script_comm();
  test_script_config_mask();
  test_script_crlf();
  test_script_syntax();
  test_script_read_regions();
  test_script_progress();
  puts("ok");
  return 0;
}
