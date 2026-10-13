// Host test for the K22 device table: make test
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "pic18k22.h"

static void test_lookup() {
  const pic_part *p = pic_lookup(0x5400);
  assert(p && !strcmp(p->name, "PIC18F46K22"));
  p = pic_lookup(0x5423);                       // low 5 bits = silicon revision
  assert(p && !strcmp(p->name, "PIC18LF46K22") && p->flash == 65536 && p->eeprom == 1024);
  assert(pic_lookup(0x5740)->eeprom == 256);
  assert(pic_lookup(0x1234) == nullptr);
  assert(pic_lookup(0x0000) == nullptr && pic_lookup(0xFFFF) == nullptr);
}

// All 16 parts from DS41398 Table 5-2, each found once with the right size.
static void test_table() {
  static const struct { uint16_t id; const char *name; uint32_t flash; } want[] = {
    {0x5740, "PIC18F23K22", 8192},  {0x5760, "PIC18LF23K22", 8192},
    {0x5700, "PIC18F43K22", 8192},  {0x5720, "PIC18LF43K22", 8192},
    {0x5640, "PIC18F24K22", 16384}, {0x5660, "PIC18LF24K22", 16384},
    {0x5600, "PIC18F44K22", 16384}, {0x5620, "PIC18LF44K22", 16384},
    {0x5540, "PIC18F25K22", 32768}, {0x5560, "PIC18LF25K22", 32768},
    {0x5500, "PIC18F45K22", 32768}, {0x5520, "PIC18LF45K22", 32768},
    {0x5440, "PIC18F26K22", 65536}, {0x5460, "PIC18LF26K22", 65536},
    {0x5400, "PIC18F46K22", 65536}, {0x5420, "PIC18LF46K22", 65536},
  };
  for (auto &w : want) {
    const pic_part *p = pic_lookup(w.id | 0x1F);
    assert(p && p->id == w.id && !strcmp(p->name, w.name) && p->flash == w.flash);
    assert(p->eeprom == (w.flash == 65536 ? 1024 : 256));
  }
}

int main() {
  test_lookup();
  test_table();
  assert(pic_part_detected() == nullptr);       // nothing probed yet
  puts("ok");
  return 0;
}
