#pragma once
#include <stdint.h>
#include "script.h"

// PIC18(L)F2xK22/4xK22 device layer over HV ICSP (DS41398).
struct pic_part { uint16_t id; const char *name; uint32_t flash; uint16_t eeprom; };

const pic_part *pic_lookup(uint16_t devid);    // devid & 0xFFE0; nullptr if unknown

// Enters programming mode, reads the device ID at 0x3FFFFE/F, leaves and releases
// MCLR. Returns "no target" (0x0000/0xFFFF), the part name, or "unknown 0x5400".
// Latches the part: fills pic_dev sizes and config mask.
const char *pic_probe();
const pic_part *pic_part_detected();           // nullptr unless last probe matched the table

extern pic_ops pic_dev;                        // script backend
