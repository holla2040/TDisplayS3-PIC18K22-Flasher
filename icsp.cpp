#include <Arduino.h>
#include "driver/gpio.h"
#include "soc/gpio_struct.h"
#include "esp_cpu.h"
#include "config.h"
#include "icsp.h"

// Microchip DS41398 (PIC18(L)F2XK22/4XK22 Flash Memory Programming Specification).

static const uint32_t PGD = 1UL << PGD_PIN, PGC = 1UL << PGC_PIN;
#if ICSP_LVP
static const uint32_t MCLR = 1UL << MCLR_PIN;
#else
static const uint32_t MCLR_LOW = 1UL << MCLR_LOW_PIN, VPP_ON = 1UL << VPP_ON_PIN;
#endif

static inline void half() {
  uint32_t t0 = esp_cpu_get_cycle_count();
  while (esp_cpu_get_cycle_count() - t0 < ICSP_HALF_PERIOD) {}
}
static inline void drive(bool on) {
  if (on) GPIO.enable_w1ts = PGD;
  else GPIO.enable_w1tc = PGD;
}

// Host sets PGD while PGC is low, then clocks; target latches it on the falling edge.
static void write_bits(uint32_t v, int n) {
  for (int i = 0; i < n; i++, v >>= 1) {
    if (v & 1) GPIO.out_w1ts = PGD;
    else GPIO.out_w1tc = PGD;
    GPIO.out_w1ts = PGC; half();
    GPIO.out_w1tc = PGC; half();
  }
}

// After the 8 operand clocks the target drives PGD: valid P14 after the rising
// edge, sampled just before the falling edge. Then hand PGD back to the host.
static uint8_t read_byte() {
  drive(false);
  half();                                        // P6: PGD turnaround, PGC low
  uint8_t v = 0;
  for (int i = 0; i < 8; i++) {
    GPIO.out_w1ts = PGC; half();
    if (GPIO.in & PGD) v |= 1 << i;
    GPIO.out_w1tc = PGC; half();
  }
  GPIO.out_w1tc = PGD;
  drive(true);
  return v;
}

#if ICSP_LVP
static bool in_session;                          // between icsp_enter() and icsp_exit()

static void pg_outputs() {
  gpio_config_t c = {};
  c.pin_bit_mask = 1ULL << PGD_PIN | 1ULL << PGC_PIN;
  c.mode = GPIO_MODE_INPUT_OUTPUT;               // read back while released
  c.pull_down_en = GPIO_PULLDOWN_ENABLE;         // no target => device ID reads 0x0000
  gpio_config(&c);
  GPIO.out_w1tc = PGC | PGD;
}

void icsp_init() {
  GPIO.out_w1tc = MCLR | PGC | PGD;              // level before direction: target held in reset
  gpio_config_t c = {};
  c.pin_bit_mask = 1ULL << MCLR_PIN;
  c.mode = GPIO_MODE_OUTPUT;
  gpio_config(&c);
  pg_outputs();
}

// DS41398 §2.6, Figure 2-14: MCLR VIH -> VIL, P18 >= 1 ms, 32-bit key 4D434850h
// MSb first (write_bits() is LSb first, so one bit at a time), P20, MCLR to VIH,
// P15 >= 400 µs before the first command. VDD is already up.
void icsp_enter() {
  GPIO.out_w1tc = PGC | PGD;
  GPIO.enable_w1ts = PGC | PGD;
  GPIO.out_w1ts = MCLR;                          // VIH first (no minimum hold)
  delayMicroseconds(5);
  GPIO.out_w1tc = MCLR;
  delayMicroseconds(1000 + 100);                 // P18
  const uint32_t key = 0x4D434850;
  for (int i = 31; i >= 0; i--) write_bits(key >> i, 1);
  GPIO.out_w1tc = PGD;
  delayMicroseconds(1);                          // P20 40 ns
  GPIO.out_w1ts = MCLR;
  delayMicroseconds(400 + 100);                  // P15
  in_session = true;
}

// Figure 2-15: P16 = 0 between the last PGC and MCLR falling.
void icsp_exit() {
  GPIO.out_w1tc = PGC | PGD;
  GPIO.out_w1tc = MCLR;
  GPIO.enable_w1tc = PGC | PGD;
  in_session = false;
}

void icsp_release() {
  if (in_session) return;                        // only from reset, never mid-session
  GPIO.out_w1ts = MCLR;
}
#else
void icsp_init() {
  // Levels before directions, so neither switch glitches on at boot. VPP_ON is
  // driven off first and MCLR_LOW only 5 µs later: break-before-make even if
  // GPIO10 floated high during boot.
  GPIO.out_w1tc = VPP_ON | PGC | PGD;
  GPIO.out_w1ts = MCLR_LOW;
  gpio_config_t c = {};
  c.pin_bit_mask = 1ULL << VPP_ON_PIN;
  c.mode = GPIO_MODE_OUTPUT;
  gpio_config(&c);
  delayMicroseconds(5);
  c.pin_bit_mask = 1ULL << MCLR_LOW_PIN;
  gpio_config(&c);
  c.pin_bit_mask = 1ULL << PGD_PIN | 1ULL << PGC_PIN;
  c.mode = GPIO_MODE_INPUT_OUTPUT;               // read back while released
  c.pull_down_en = GPIO_PULLDOWN_ENABLE;         // no target => device ID reads 0x0000
  gpio_config(&c);
  GPIO.out_w1tc = PGC | PGD;
}

// DS41398 Figure 2-12: VDD is already up (the jig powers the target), PGC/PGD
// low, then MCLR straight to VIHH. P12 is only 2 µs; the 100 µs covers the
// PNP switch rise.
void icsp_enter() {
  GPIO.out_w1tc = VPP_ON;
  delayMicroseconds(5);                          // break-before-make
  GPIO.out_w1ts = MCLR_LOW;                      // target may be running: reset it first
  GPIO.out_w1tc = PGC | PGD;
  GPIO.enable_w1ts = PGC | PGD;
  delayMicroseconds(100);
  GPIO.out_w1tc = MCLR_LOW;
  delayMicroseconds(5);                          // break-before-make
  GPIO.out_w1ts = VPP_ON;
  delayMicroseconds(100);
}

// Figure 2-13: P16 = 0 between the last PGC and MCLR falling.
void icsp_exit() {
  GPIO.out_w1tc = PGC | PGD;
  GPIO.out_w1tc = VPP_ON;
  delayMicroseconds(5);                          // break-before-make
  GPIO.out_w1ts = MCLR_LOW;
  GPIO.enable_w1tc = PGC | PGD;
}

void icsp_release() {
  if (GPIO.out & VPP_ON) return;                 // only from reset, never mid-session
  GPIO.out_w1tc = MCLR_LOW;
}
#endif

void icsp_core(uint16_t instr) {
  write_bits(0x0, 4);
  write_bits(instr, 16);
}

uint8_t icsp_tblrd_postinc() {
  write_bits(0x9, 4);
  write_bits(0, 8);
  return read_byte();
}

uint8_t icsp_shift_tablat() {
  write_bits(0x2, 4);
  write_bits(0, 8);
  return read_byte();
}

void icsp_tblwr(uint8_t cmd, uint16_t data) {
  write_bits(cmd, 4);
  write_bits(data, 16);
}

// Figure 3-5: programming is externally timed by holding the 4th PGC of the
// following NOP high for P9, then low for P10.
void icsp_start_prog(uint16_t data, uint32_t hold_us) {
  write_bits(0xF, 4);
  write_bits(data, 16);
  write_bits(0, 3);
  GPIO.out_w1tc = PGD;
  GPIO.out_w1ts = PGC; delayMicroseconds(hold_us);
  GPIO.out_w1tc = PGC; delayMicroseconds(P10_US);
  write_bits(0, 16);
}

// Figure 3-2: the self-timed erase starts on the 4th PGC; PGD must stay low until done.
void icsp_nop_hold(uint32_t us) {
  write_bits(0, 4);
  delayMicroseconds(us);
  write_bits(0, 16);
}
