#ifndef HOST_TEST
#include <Arduino.h>
#include "icsp.h"
#endif
#include <stdio.h>
#include "pic18k22.h"

// Device IDs: DS41398 Table 5-2 (& 0xFFE0; low 5 bits are the revision).
// Sizes: MPLAB X PIC18F-K_DFP 1.14.301 .PIC files.
static const pic_part parts[] = {
  {0x5740, "PIC18F23K22", 8192, 256},   {0x5760, "PIC18LF23K22", 8192, 256},
  {0x5700, "PIC18F43K22", 8192, 256},   {0x5720, "PIC18LF43K22", 8192, 256},
  {0x5640, "PIC18F24K22", 16384, 256},  {0x5660, "PIC18LF24K22", 16384, 256},
  {0x5600, "PIC18F44K22", 16384, 256},  {0x5620, "PIC18LF44K22", 16384, 256},
  {0x5540, "PIC18F25K22", 32768, 256},  {0x5560, "PIC18LF25K22", 32768, 256},
  {0x5500, "PIC18F45K22", 32768, 256},  {0x5520, "PIC18LF45K22", 32768, 256},
  {0x5440, "PIC18F26K22", 65536, 1024}, {0x5460, "PIC18LF26K22", 65536, 1024},
  {0x5400, "PIC18F46K22", 65536, 1024}, {0x5420, "PIC18LF46K22", 65536, 1024},
};

static const pic_part *detected;

const pic_part *pic_lookup(uint16_t devid) {
  devid &= 0xFFE0;
  for (auto &p : parts)
    if (p.id == devid) return &p;
  return nullptr;
}

const pic_part *pic_part_detected() { return detected; }

#ifndef HOST_TEST

// Access-bank SFR instructions (DS41398 Tables 3-x, 4-x).
#define MOVLW(k)      (0x0E00 | ((k) & 0xFF))
#define MOVWF_TBLPTRU 0x6EF8
#define MOVWF_TBLPTRH 0x6EF7
#define MOVWF_TBLPTRL 0x6EF6
#define MOVWF_EEADR   0x6EA9
#define MOVWF_EEADRH  0x6EAA
#define MOVWF_EEDATA  0x6EA8
#define MOVWF_TABLAT  0x6EF5
#define MOVF_EEDATA_W 0x50A8
#define MOVF_EECON1_W 0x50A6
#define BSF_EEPGD 0x8EA6
#define BCF_EEPGD 0x9EA6
#define BSF_CFGS  0x8CA6
#define BCF_CFGS  0x9CA6
#define BSF_WREN  0x84A6
#define BCF_WREN  0x94A6
#define BSF_WR    0x82A6
#define BSF_RD    0x80A6
#define NOP_      0x0000      // NOP is taken by esp32-hal.h

static const uint32_t BASE[IHX_NREGIONS] = {0x000000, 0x200000, 0x300000, 0xF00000};

// Implemented bits of CONFIG1L..CONFIG7H (DS41398 Table 5-1, pack edc:impl).
// CP/WRT/EBTR (offsets 8, 10, 12) have 2 blocks on 8/16 KB parts, 4 on 32/64 KB.
static uint8_t cfg_mask[14] = {0x00, 0xFF, 0x1F, 0x3F, 0x00, 0xBF, 0xC5, 0x00, 0x0F, 0xC0, 0x0F, 0xE0, 0x0F, 0x40};

static bool active;                              // in programming mode: a script is one session

static void session() {
  if (!active) icsp_enter();
  active = true;
}

static void set_ptr(uint32_t a) {
  icsp_core(MOVLW(a >> 16)); icsp_core(MOVWF_TBLPTRU);
  icsp_core(MOVLW(a >> 8));  icsp_core(MOVWF_TBLPTRH);
  icsp_core(MOVLW(a));       icsp_core(MOVWF_TBLPTRL);
}

static void set_eeadr(uint32_t a) {
  icsp_core(MOVLW(a)); icsp_core(MOVWF_EEADR);
  icsp_core(MOVLW(a >> 8)); icsp_core(MOVWF_EEADRH);
}

// Table 4-2. TABLAT shifts out as the second byte of the 16-bit frame.
static uint8_t ee_read(uint32_t a) {
  set_eeadr(a);
  icsp_core(BSF_RD);
  icsp_core(MOVF_EEDATA_W); icsp_core(MOVWF_TABLAT); icsp_core(NOP_);
  return icsp_shift_tablat();
}

static bool in_range(int r, uint32_t off, size_t n) {
  return r >= 0 && r < IHX_NREGIONS && off + n <= pic_dev.size[r];
}

static bool dev_read(int r, uint32_t off, uint8_t *buf, size_t n) {
  if (!in_range(r, off, n)) return false;
  session();
  if (r == IHX_EEPROM) {
    icsp_core(BCF_EEPGD); icsp_core(BCF_CFGS);
    for (size_t i = 0; i < n; i++) buf[i] = ee_read(off + i);
  } else {
    set_ptr(BASE[r] + off);                      // Table 4-1
    for (size_t i = 0; i < n; i++) buf[i] = icsp_tblrd_postinc();
  }
  return true;
}

// Tables 3-5 / 3-8: load the write buffer 2 bytes at a time, the last word starts
// programming. Rows the mask doesn't touch are skipped; gaps are padded with 0xFF,
// which leaves erased cells alone.
static void write_rows(int r, uint32_t off, const uint8_t *buf, const uint8_t *mask, size_t n) {
  uint32_t row = r == IHX_USERID ? 8 : 64;
  icsp_core(BSF_EEPGD); icsp_core(BCF_CFGS); icsp_core(BSF_WREN);
  for (uint32_t a = off - off % row; a < off + n; a += row) {
    uint8_t b[64];
    bool any = false;
    for (uint32_t j = 0; j < row; j++) {
      uint32_t k = a + j - off;
      bool in = a + j >= off && k < n && mask[k];
      b[j] = in ? buf[k] : 0xFF;
      any |= in;
    }
    if (!any) continue;
    set_ptr(BASE[r] + a);
    for (uint32_t j = 0; j < row - 2; j += 2) icsp_tblwr(0xD, b[j] | b[j + 1] << 8);
    icsp_start_prog(b[row - 2] | b[row - 1] << 8, r == IHX_USERID ? P9A_US : P9_US);
  }
  icsp_core(BCF_WREN);
}

// Table 3-9: one byte per start-programming command, address set each time
// (even addresses take the payload LSB, odd the MSB, so send it in both).
// CONFIG6H (WRTC) goes last: once set, no other config byte can be written.
static void write_config(uint32_t off, const uint8_t *buf, const uint8_t *mask, size_t n) {
  static const uint8_t order[14] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 13, 11};
  icsp_core(BSF_EEPGD); icsp_core(BSF_CFGS); icsp_core(BSF_WREN);
  for (uint8_t a : order) {
    uint32_t k = a - off;
    if (a < off || k >= n || !mask[k] || !cfg_mask[a]) continue;
    set_ptr(BASE[IHX_CONFIG] + a);
    icsp_start_prog(buf[k] | buf[k] << 8, P9A_US);
  }
  icsp_core(BCF_WREN); icsp_core(BCF_CFGS);
}

// Table 3-7: per byte, set WR, two NOPs (the write starts on the 4th PGC of the
// second), then poll WR until hardware clears it.
static bool write_eeprom(uint32_t off, const uint8_t *buf, const uint8_t *mask, size_t n) {
  icsp_core(BCF_EEPGD); icsp_core(BCF_CFGS);
  for (size_t i = 0; i < n; i++) {
    if (!mask[i]) continue;
    set_eeadr(off + i);
    icsp_core(MOVLW(buf[i])); icsp_core(MOVWF_EEDATA);
    icsp_core(BSF_WREN);
    icsp_core(BSF_WR); icsp_core(NOP_); icsp_core(NOP_);
    delayMicroseconds(P11A_US);
    bool busy = true;
    for (int t = 0; t < 10 && busy; t++) {
      if (t) delayMicroseconds(1000);
      icsp_core(MOVF_EECON1_W); icsp_core(MOVWF_TABLAT); icsp_core(NOP_);
      busy = icsp_shift_tablat() & 0x02;
    }
    delayMicroseconds(P10_US);
    icsp_core(BCF_WREN);
    if (busy) return false;
  }
  return true;
}

static bool dev_write(int r, uint32_t off, const uint8_t *buf, const uint8_t *mask, size_t n) {
  if (!in_range(r, off, n)) return false;
  session();
  if (r == IHX_CONFIG) write_config(off, buf, mask, n);
  else if (r == IHX_EEPROM) return write_eeprom(off, buf, mask, n);
  else write_rows(r, off, buf, mask, n);
  return true;
}

// Table 3-2 / 3-1: 3C0005h:3C0004h = 0F8Fh chip, 0084h data EEPROM only.
static bool dev_erase(bool chip) {
  session();
  set_ptr(0x3C0005); icsp_tblwr(0xC, chip ? 0x0F0F : 0x0000);
  set_ptr(0x3C0004); icsp_tblwr(0xC, chip ? 0x8F8F : 0x8484);
  icsp_core(NOP_);
  icsp_nop_hold(P11_US + P10_US);
  return true;
}

static bool dev_ee_locked() {
  uint8_t c5h;
  if (!dev_read(IHX_CONFIG, 9, &c5h, 1)) return false;
  return !(c5h & 0x80);                          // CONFIG5H CPD = 0
}

static void dev_reset() {
  if (active) icsp_exit();
  active = false;
  delayMicroseconds(100);
  icsp_release();
}

pic_ops pic_dev = {{0, 0, 0, 0}, cfg_mask, dev_read, dev_write, dev_erase, dev_ee_locked, dev_reset};

const char *pic_probe() {
  static char unknown[16];
  session();
  set_ptr(0x3FFFFE);                             // Figure 5-1
  uint16_t id = icsp_tblrd_postinc();
  id |= icsp_tblrd_postinc() << 8;
  dev_reset();
  detected = pic_lookup(id);
  uint8_t blocks = detected && detected->flash >= 32768 ? 0x0F : 0x03;
  cfg_mask[8] = cfg_mask[10] = cfg_mask[12] = blocks;
  pic_dev.size[IHX_FLASH] = detected ? detected->flash : 0;
  pic_dev.size[IHX_USERID] = detected ? 8 : 0;
  pic_dev.size[IHX_CONFIG] = detected ? 14 : 0;
  pic_dev.size[IHX_EEPROM] = detected ? detected->eeprom : 0;
  if (id == 0x0000 || id == 0xFFFF) return "no target";
  if (!detected) {
    snprintf(unknown, sizeof unknown, "unknown 0x%04X", id);
    return unknown;
  }
  return detected->name;
}

#endif
