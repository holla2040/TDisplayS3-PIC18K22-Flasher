#pragma once
#include <stdint.h>
#include <stddef.h>
#include "ihx.h"

// Device interface: the real one lives in pic18k22.cpp, tests use a fake chip.
// Calls return false on the few comm errors the wire can see. ICSP has no ACK:
// a missing target reads zeros, so devid() is how the runner tells "gone" from "wrong".
struct pic_ops {
  uint32_t size[IHX_NREGIONS];   // of the detected part
  uint16_t part_id;              // its device ID & 0xFFE0
  const uint8_t *config_mask;    // 14 bytes, implemented bits
  bool (*read)(int region, uint32_t off, uint8_t *buf, size_t n);
  // A user-ID write replaces the whole 8-byte block (erased first), so the last one wins.
  bool (*write)(int region, uint32_t off, const uint8_t *buf, const uint8_t *mask, size_t n);
  bool (*erase)(bool chip);      // true = chip, false = EEPROM only
  bool (*ee_locked)();           // CONFIG5H (0x300009) bit 7 == 0
  uint16_t (*devid)();           // raw device ID (0x3FFFFE/F); 0x0000/0xFFFF = no target
  void (*reset)();
};

struct fs_ops {
  char *(*load)(const char *name, size_t *len);          // malloc'd, NUL-terminated, *len = bytes (gunzipped if gzip); nullptr if missing
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
