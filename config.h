#pragma once

// --- Programming mode ---
// 1 = low-voltage ICSP (the default): four wires plus VDD, GPIO3 drives MCLR
//     directly. The target must have LVP = 1 (factory default on K22 parts).
// 0 = high-voltage ICSP: needs the 8.5 V boost and Q1-Q3 (README "High-voltage option").
#ifndef ICSP_LVP
#define ICSP_LVP 1
#endif

// --- Pin assignments (LilyGO T-Display S3; adjust to your jig wiring) ---
#define PGD_PIN         1   // bidirectional data
#define PGC_PIN         2   // jig drives the clock
#if ICSP_LVP
#define MCLR_PIN        3   // drives the target's MCLR directly (high = run, low = reset)
#else
// HV, PICkit 2 style MCLR drive (see docs/design.md, Hardware).
#define MCLR_LOW_PIN    3   // N-MOSFET pulls MCLR to GND; on by default (target held in reset)
#define VPP_ON_PIN      10  // N-MOSFET + P-MOSFET switch putting 9 V on MCLR
#endif

// ICSP clock half-period in ESP32 CPU cycles (240 MHz). 240 ≈ 1 µs; DS41398 min
// is 100 ns, so this is the cable-length knob. Raise it for long cables.
#define ICSP_HALF_PERIOD 240

// T-Display S3 user buttons (per LilyGO schematic: BOOT=IO0, KEY=IO14)
#define MODE_BUTTON     0   // short press: toggle auto/manual; hold 5 s: forget WiFi
#define FLASH_BUTTON    14  // manual mode: press to flash
