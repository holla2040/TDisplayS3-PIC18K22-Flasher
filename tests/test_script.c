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
static int gone_at;                         // from this call on the target is pulled: reads 0, writes do nothing
static bool locked, corrupt_flash_104, corrupt_ee_5;
static uint16_t devid_val;                  // raw device ID the chip answers with
static const uint32_t SIZES[IHX_NREGIONS] = {FLASH_SZ, UID_SZ, CFG_SZ, EE_SZ};

static void logit(const char *s) { strcat(logbuf, s); strcat(logbuf, " "); }
static bool gone() { return gone_at && calls >= gone_at; }
static bool tick() { return ++calls != fail_at; }

static bool f_read(int r, uint32_t off, uint8_t *buf, size_t n) {
  char t[8]; snprintf(t, sizeof t, "r%d", r); logit(t);
  if (!tick()) return false;
  if (gone()) memset(buf, 0, n);   // PGD pull-down: a missing target reads zeros, no error
  else memcpy(buf, chip[r] + off, n);
  return true;
}
// A user-ID write replaces the whole 8-byte block (pic_dev erases it first), so the last one wins.
static bool f_write(int r, uint32_t off, const uint8_t *buf, const uint8_t *mask, size_t n) {
  char t[8]; snprintf(t, sizeof t, "w%d", r); logit(t);
  if (!tick()) return false;
  if (gone()) return true;
  if (r == IHX_USERID) memset(chip[r], 0xFF, UID_SZ);
  for (size_t i = 0; i < n; i++) {
    if (!mask[i]) continue;
    chip[r][off + i] = r == IHX_CONFIG ? buf[i] & cfg_mask[i] : buf[i];   // unimplemented bits read 0
  }
  if (corrupt_flash_104 && r == IHX_FLASH) chip[IHX_FLASH][0x104] ^= 0x01;
  if (corrupt_ee_5 && r == IHX_EEPROM) chip[IHX_EEPROM][5] ^= 0x01;
  return true;
}
static bool f_erase(bool c) {
  logit(c ? "Ec" : "Ee");
  if (!tick()) return false;
  if (gone()) return true;
  memset(chip[IHX_EEPROM], 0xFF, EE_SZ);
  if (c) {
    memset(chip[IHX_FLASH], 0xFF, FLASH_SZ);
    memset(chip[IHX_USERID], 0xFF, UID_SZ);
    for (int i = 0; i < CFG_SZ; i++) chip[IHX_CONFIG][i] = 0xFF & cfg_mask[i];
  }
  return true;
}
static bool f_locked() { return locked; }
static uint16_t f_devid() { logit("D"); ++calls; return gone() ? 0x0000 : devid_val; }
static void f_reset() { logit("X"); }

enum { PART_ID = 0x5540 };   // PIC18F25K22: 32 KB flash, 256 B EEPROM
static const pic_ops DEV_TEMPLATE = {{FLASH_SZ, UID_SZ, CFG_SZ, EE_SZ}, PART_ID, cfg_mask,
                                     f_read, f_write, f_erase, f_locked, f_devid, f_reset};

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
static const char *const DEFAULT_SCRIPT =   // same as the sketch's DEFAULT_FLASH_TXT
  "read   userid  userid-{id}.hex      # chip erase wipes the user ID, so save it first\n"
  "read   eeprom  settings-{id}.hex\n"
  "erase  chip\n"
  "write  firmware.hex\n"
  "verify firmware.hex                 # before the settings overwrite any EEPROM data it carries\n"
  "write  settings-{id}.hex\n"
  "verify settings-{id}.hex\n"
  "write  userid-{id}.hex              # last: an interrupted run leaves the ID blank, so the rerun stops\n"
  "verify userid-{id}.hex\n"
  "reset\n";

static void set_file(const char *name, const char *text) { fs_save(name, text, strlen(text)); }

static void reset_world() {
  for (auto &f : files) { free(f.text); f.text = nullptr; }
  memset(chip, 0xFF, sizeof chip);
  memcpy(chip[IHX_USERID], UID, 8);
  for (int i = 0; i < EE_SZ; i++) chip[IHX_EEPROM][i] = (uint8_t)(i * 5 + 3);
  memset(cfg_mask, 0xFF, sizeof cfg_mask);
  logbuf[0] = 0;
  calls = fail_at = gone_at = 0;
  locked = corrupt_flash_104 = corrupt_ee_5 = false;
  devid_val = PART_ID | 0x03;   // revision bits don't matter
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
  // devid, id, read userid, read eeprom, erase, write+verify firmware, settings, userid, reset
  assert(!strcmp(logbuf, "D r1 r1 r3 Ec w0 w2 r0 r2 w3 r3 w1 r1 X "));
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
  assert(!strcmp(logbuf, "D r1 "));   // only the devid and id reads, no erase
}

static void test_script_blank_id_after_failed_run() {
  reset_world();
  fail_at = 6;   // D, r1 id, r1 userid, r3 eeprom, Ec, w0 fails: erase already wiped the userid
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
  assert(!strcmp(logbuf, "D r1 "));   // checked up front: not even "read userid" ran
  assert(!find("userid-0102030405060708.hex"));
  // no "read eeprom" in the script: the lock doesn't matter
  expect(run("read userid u.hex\n"), SCRIPT_PASS, 0, "");
}

static void test_script_missing_file() {
  reset_world();
  free(find("firmware.hex")->text);
  find("firmware.hex")->text = nullptr;
  expect(run(DEFAULT_SCRIPT), SCRIPT_ERROR, 4, "no firmware.hex");
  assert(!strcmp(logbuf, "D r1 "));   // caught in preflight: only the devid and id reads, no erase
  // a file produced by an earlier read in the same script is fine
  logbuf[0] = 0;
  expect(run("read userid u-{id}.hex\nwrite u-{id}.hex\n"), SCRIPT_PASS, 0, "");
  // ...but not one produced only later, or under a different name
  logbuf[0] = 0;
  expect(run("write u.hex\nread userid u.hex\n"), SCRIPT_ERROR, 1, "no u.hex");
  expect(run("read userid a.hex\nverify b.hex\n"), SCRIPT_ERROR, 2, "no b.hex");
  assert(!strcmp(logbuf, "D D "));
}

static void test_script_verify_mismatch() {
  reset_world();
  corrupt_flash_104 = true;
  expect(run(DEFAULT_SCRIPT), SCRIPT_FAIL, 5, "verify 0x000104");
  assert(strstr(logbuf, "r0 D "));   // the mismatch re-checked the target, which is still there
}

static void test_script_comm() {
  reset_world();
  fail_at = 3;   // devid and id read ok, "read userid" fails
  expect(run(DEFAULT_SCRIPT), SCRIPT_COMM, 1, "");
}

// No target, or not the part the sizes came from: comm error before anything is read or saved.
static void test_script_devid_start() {
  const uint16_t bad[] = {0x0000, 0xFFFF, 0x5400, 0x5560};
  for (uint16_t id : bad) {
    reset_world();
    devid_val = id;
    expect(run(DEFAULT_SCRIPT), SCRIPT_COMM, 0, "no target");
    assert(!strcmp(logbuf, "D "));
    assert(!find("userid-0102030405060708.hex") && !find("userid-0000000000000000.hex"));
  }
}

// Board pulled mid-run: verify reads zeros. That's a comm error, not a FAIL.
static void test_script_verify_target_gone() {
  reset_world();
  gone_at = 8;   // D r1 r1 r3 Ec w0 w2, then "verify firmware.hex" reads a missing chip
  expect(run(DEFAULT_SCRIPT), SCRIPT_COMM, 5, "no target");
  assert(strstr(logbuf, "r0 D "));
}

// C1: an interrupted run must never let a rerun overwrite the saved settings with
// what the half-programmed board holds. userid goes last, so the rerun sees it blank.
static void test_script_rerun_keeps_settings() {
  for (int how = 0; how < 2; how++) {
    reset_world();
    if (how == 0) fail_at = 12;   // D r1 r1 r3 Ec w0 w2 r0 r2 w3 r3, then the userid write fails
    else corrupt_ee_5 = true;     // or an EEPROM byte didn't take: verify settings FAILs
    script_status s = run(DEFAULT_SCRIPT);
    if (how == 0) expect(s, SCRIPT_COMM, 8, "");
    else expect(s, SCRIPT_FAIL, 7, "verify 0xF00005");
    char saved[8192];
    strcpy(saved, find("settings-0102030405060708.hex")->text);
    fail_at = 0;
    corrupt_ee_5 = false;
    logbuf[0] = 0;
    expect(run(DEFAULT_SCRIPT), SCRIPT_ERROR, 1, "blank userid");
    assert(!strcmp(logbuf, "D r1 "));
    assert(!strcmp(saved, find("settings-0102030405060708.hex")->text));
  }
}

// Appends records to firmware.hex: a user ID (if uid) and EEPROM defaults.
static void firmware_extra(bool uid) {
  char hex[8192];
  strcpy(hex, find("firmware.hex")->text);
  *strstr(hex, ":00000001FF") = 0;   // drop the EOF, append more records
  uint8_t p20[2] = {0x00, 0x20}, pf0[2] = {0x00, 0xF0}, id[8] = {9, 9, 9, 9, 9, 9, 9, 9}, d[16];
  memset(d, 0x42, sizeof d);
  if (uid) {
    rec(hex, 4, 0, p20, 2);
    rec(hex, 0, 0, id, 8);
  }
  rec(hex, 4, 0, pf0, 2);
  rec(hex, 0, 0, d, 16);
  strcat(hex, ":00000001FF\n");
  set_file("firmware.hex", hex);
}

// I3: firmware.hex carrying EEPROM defaults (no user ID) still PASSes,
// and the board keeps its own ID and settings.
static void test_script_firmware_with_ee() {
  reset_world();
  uint8_t ee[EE_SZ];
  memcpy(ee, chip[IHX_EEPROM], EE_SZ);
  firmware_extra(false);
  expect(run(DEFAULT_SCRIPT), SCRIPT_PASS, 0, "");
  assert(!strcmp(logbuf, "D r1 r1 r3 Ec w0 w2 w3 r0 r2 r3 w3 r3 w1 r1 X "));
  assert(!memcmp(chip[IHX_USERID], UID, 8));
  assert(!memcmp(chip[IHX_EEPROM], ee, EE_SZ));
}

// firmware.hex with user-ID records before a later user-ID write would refill the
// ID early, so an interrupted run's rerun would save half-written EEPROM as the
// board's settings. Stopped in preflight, at the firmware line, before any erase.
static void test_script_firmware_with_uid() {
  reset_world();
  firmware_extra(true);
  expect(run(DEFAULT_SCRIPT), SCRIPT_ERROR, 4, "firmware has userid");
  assert(!strcmp(logbuf, "D r1 "));
  assert(!find("settings-0102030405060708.hex") && !find("userid-0102030405060708.hex"));
  // also when the later user-ID write is a file on FFat rather than one a read made
  logbuf[0] = 0;
  set_file("u.hex", find("firmware.hex")->text);
  expect(run("erase chip\nwrite firmware.hex\nwrite u.hex\n"), SCRIPT_ERROR, 2, "firmware has userid");
  assert(!strcmp(logbuf, "D "));
  // with no later user-ID write it is the only ID source: fine
  expect(run("erase chip\nwrite firmware.hex\nverify firmware.hex\n"), SCRIPT_PASS, 0, "");
  // nor the other way round: the firmware's ID would overwrite the board's
  expect(run("read userid u-{id}.hex\nwrite u-{id}.hex\nwrite firmware.hex\n"), SCRIPT_ERROR, 2, "firmware has userid");
  // a verify of it doesn't count
  expect(run("read userid u-{id}.hex\nverify firmware.hex\nwrite u-{id}.hex\n"), SCRIPT_PASS, 0, "");
}

// I1: a record beyond the detected part is caught before anything changes the chip.
static void test_script_out_of_range() {
  reset_world();
  char hex[8192];
  strcpy(hex, find("firmware.hex")->text);
  *strstr(hex, ":00000001FF") = 0;
  uint8_t d[2] = {0, 0}, p0[2] = {0, 0};
  rec(hex, 4, 0, p0, 2);
  rec(hex, 0, FLASH_SZ, d, 2);   // 0x8000: fine on a 64 KB part, not on this 32 KB one
  strcat(hex, ":00000001FF\n");
  set_file("firmware.hex", hex);
  expect(run(DEFAULT_SCRIPT), SCRIPT_ERROR, 4, "0x008000 outside chip");
  assert(!strcmp(logbuf, "D r1 "));
  // a bad checksum too
  reset_world();
  set_file("firmware.hex", ":0100000000FE\n:00000001FF\n");
  expect(run(DEFAULT_SCRIPT), SCRIPT_ERROR, 4, "bad checksum");
  assert(!strcmp(logbuf, "D r1 "));
}

// Task 2 minor #2: a read name that won't fit once {id} expands is caught before the erase.
static void test_script_read_expand() {
  reset_world();
  char s[160];
  snprintf(s, sizeof s, "erase chip\nread eeprom %s{id}.hex\n",
           "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");   // 50 + 16 + 4 > 63
  expect(run(s), SCRIPT_ERROR, 2, "syntax");
  assert(!strcmp(logbuf, "D r1 "));
}

static void test_script_erase_eeprom() {
  reset_world();
  chip[IHX_FLASH][0] = 0x12;
  expect(run("erase eeprom\n"), SCRIPT_PASS, 0, "");
  assert(!strcmp(logbuf, "D Ee "));
  for (int i = 0; i < EE_SZ; i++) assert(chip[IHX_EEPROM][i] == 0xFF);
  assert(chip[IHX_FLASH][0] == 0x12 && !memcmp(chip[IHX_USERID], UID, 8));
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
  test_script_devid_start();
  test_script_verify_target_gone();
  test_script_rerun_keeps_settings();
  test_script_firmware_with_ee();
  test_script_firmware_with_uid();
  test_script_out_of_range();
  test_script_read_expand();
  test_script_erase_eeprom();
  test_script_config_mask();
  test_script_crlf();
  test_script_syntax();
  test_script_read_regions();
  test_script_progress();
  puts("ok");
  return 0;
}
