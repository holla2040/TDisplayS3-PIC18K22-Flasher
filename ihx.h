#pragma once
#include <stdint.h>
#include <stddef.h>

enum { IHX_FLASH, IHX_USERID, IHX_CONFIG, IHX_EEPROM, IHX_NREGIONS };

// PIC18 address-space regions: flash 0x000000, userid 0x200000, config 0x300000,
// eeprom 0xF00000. mask[i]=1 means byte i was present in the file.
struct ihx_region { uint32_t base, size; uint8_t *data, *mask; };
struct ihx_image  { ihx_region r[IHX_NREGIONS]; };

// Caller fills base/size/data/mask (sizes from the detected part). Clears data to
// 0xFF and masks to 0 first. Returns false + message in err on bad checksum,
// unknown record type (only 00, 01, 04 accepted), or a byte outside every region
// ("0x200008 outside chip"). An EOF record doesn't stop parsing, so several
// ihx_write outputs can be concatenated and parsed in one go.
bool   ihx_parse(const char *text, ihx_image *img, char *err, size_t errlen);

// Compact binary (.pbin): "PICB", then segments of [u32 address LE][u16 length LE]
// [data], then a u32 CRC32 (zlib) of everything before it. Same regions as HEX.
// ihx_parse_any picks the format from the first bytes: "PICB" = compact binary,
// anything else = Intel HEX (buf must then be NUL-terminated). Errors as for
// ihx_parse, plus "bad CRC" and "malformed pbin".
bool     ihx_parse_any(const uint8_t *buf, size_t len, ihx_image *img, char *err, size_t errlen);
uint32_t ihx_crc32(const uint8_t *buf, size_t len);   // zlib CRC32

// Emits one region's masked bytes: type-04 record per 64 KB page, 16-byte type-00
// records (split at mask gaps), then ":00000001FF". Returns bytes written
// (0 = out too small).
size_t ihx_write(const ihx_image *img, int region, char *out, size_t outmax);
