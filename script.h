#pragma once
#include <stdint.h>
#include <stddef.h>
#include "ihx.h"

// Device interface: the real one lives in pic18k22.cpp, tests use a fake chip.
// Every call returns false on a comm error.
struct pic_ops {
  uint32_t size[IHX_NREGIONS];   // of the detected part
  const uint8_t *config_mask;    // 14 bytes, implemented bits
  bool (*read)(int region, uint32_t off, uint8_t *buf, size_t n);
  bool (*write)(int region, uint32_t off, const uint8_t *buf, const uint8_t *mask, size_t n);
  bool (*erase)(bool chip);      // true = chip, false = EEPROM only
  bool (*ee_locked)();           // CONFIG5H (0x300009) bit 7 == 0
  void (*reset)();
};

struct fs_ops {
  char *(*load)(const char *name);                       // malloc'd, NUL-terminated; nullptr if missing
  bool  (*save)(const char *name, const char *text, size_t len);
};

enum script_result { SCRIPT_PASS, SCRIPT_FAIL, SCRIPT_COMM, SCRIPT_ERROR };

// line is the 1-based script line of the failing command (0 on PASS).
// msg e.g. "blank userid", "verify 0x000104".
struct script_status { script_result result; int line; char msg[48]; };

// Runs the whole script once. progress(line, text) is called before each command.
// Stops at the first error. No Arduino APIs, so it builds on the host.
script_status script_run(const char *script, const pic_ops *dev, const fs_ops *fs,
                         void (*progress)(int line, const char *text));
