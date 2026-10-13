#pragma once
#include <stdint.h>

// Bit-banged high-voltage ICSP host for PIC18(L)F2xK22/4xK22 (DS41398 §2.5, §2.7).
// Every frame is a 4-bit command + 16-bit payload, LSb first; the target latches
// PGD on the PGC falling edge. Host-clocked, so interrupts only stretch the clock.

// DS41398 §6.0 AC/DC timing (minimums)
#define P9_US   1000    // PGC high: flash row programming
#define P9A_US  5000    // PGC high: user ID and config programming
#define P10_US  200     // PGC low after programming (high-voltage discharge)
#define P11_US  15000   // self-timed bulk erase (X5/X6 parts; X3/X4 need 12 ms)
#define P11A_US 4000    // data EEPROM byte write

void     icsp_init();                    // MCLR_LOW on, VPP off, PGC/PGD low outputs
void     icsp_enter();                   // reset, PGC/PGD low, MCLR_LOW off, 5 µs, VPP on, 100 µs
void     icsp_exit();                    // VPP off, 5 µs, MCLR_LOW on, PGC/PGD inputs
void     icsp_release();                 // MCLR_LOW off: the target runs (call after icsp_exit)
void     icsp_core(uint16_t instr);      // command 0000 + 16-bit instruction
uint8_t  icsp_tblrd_postinc();           // command 1001, 8 dummy clocks, 8 clocks data out
uint8_t  icsp_shift_tablat();            // command 0010
void     icsp_tblwr(uint8_t cmd, uint16_t data);            // 1100/1101/1111 + 16-bit payload
void     icsp_start_prog(uint16_t data, uint32_t hold_us);  // 1111 + data, then NOP with 4th PGC high hold_us (P9), low P10
void     icsp_nop_hold(uint32_t us);     // NOP with PGD held low for us after the 4th PGC (bulk erase P11)
