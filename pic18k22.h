#pragma once
#include <stdint.h>
#include "script.h"

// PIC18(L)F2xK22/4xK22 device layer over HV ICSP (DS41398).
struct pic_part { uint16_t id; const char *name; uint32_t flash; uint16_t eeprom; };

const pic_part *pic_lookup(uint16_t devid);    // devid & 0xFFE0; nullptr if unknown

// Enters programming mode, reads the device ID at 0x3FFFFE/F and, for a known part,
// the 8 user-ID bytes into uid (one session, so one target reset per probe), then
// leaves and releases MCLR. Returns "no target" (0x0000/0xFFFF), the part name, or
// "unknown 0x5200" (raw ID, revision bits included). Latches the part: fills
// pic_dev sizes, part_id and config mask.
const char *pic_probe(uint8_t uid[8]);
const pic_part *pic_part_detected();           // nullptr unless last probe matched the table

extern pic_ops pic_dev;                        // script backend
