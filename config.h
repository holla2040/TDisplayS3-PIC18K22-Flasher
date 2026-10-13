#pragma once

// --- Pin assignments (LilyGO T-Display S3; adjust to your jig wiring) ---
// HV ICSP, PICkit 2 style MCLR drive (see docs/design.md, Hardware).
#define PGD_PIN         1   // bidirectional data
#define PGC_PIN         2   // jig drives the clock
#define MCLR_LOW_PIN    3   // NPN pulls MCLR to GND; on by default (target held in reset)
#define VPP_ON_PIN      10  // NPN + PNP switch putting 9 V on MCLR

// ICSP clock half-period in ESP32 CPU cycles (240 MHz). 240 ≈ 1 µs; DS41398 min
// is 100 ns, so this is the cable-length knob. Raise it for long cables.
#define ICSP_HALF_PERIOD 240

// T-Display S3 user buttons (per LilyGO schematic: BOOT=IO0, KEY=IO14)
#define MODE_BUTTON     0   // short press: toggle auto/manual; hold 5 s: forget WiFi
#define FLASH_BUTTON    14  // manual mode: press to flash
