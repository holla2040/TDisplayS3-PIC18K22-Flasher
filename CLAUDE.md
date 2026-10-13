# TDisplayS3-PIC18K22-Flasher

Standalone production-jig flasher for PIC18(L)F2xK22 / 4xK22: a LilyGO T-Display
S3 (ESP32-S3) programs the target over high-voltage ICSP (DS41398), no PC in the
loop. Driven by a text script (`/flash.txt`) on the jig's FFat, so each board's
EEPROM settings survive a full reflash. Same UI, web page and state machine as
the siblings TDisplayS3-STM32F1-Flasher and TDisplayS3-STM8S-Flasher; only the
wire and device layers and the script runner differ. Arduino sketch built with
arduino-cli (esp32 core 3.3.0), TFT_eSPI (Setup206, 170x320), WiFiManager,
ArduinoOTA. See docs/architecture.md for the design as built, docs/design.md for the spec.

## Build / deploy

- `make bin` — compile into /tmp/arduino-build-pic18 (own dir: the siblings use /tmp/arduino-build and -stm32)
- `make test` — host tests with g++ (HEX parser, script runner on a fake chip, part table); no hardware
- `make ota` / `make usb` — deploy to PIC18Flasher.local / /dev/ttyACM0 (`make usb` writes to the port: don't run it while a sibling production jig might be on it)
- `make file F=path/to/firmware.hex` — push a script or HEX file to the running flasher's FFat

Serial debug: 115200. The jig hosts http://PIC18Flasher.local/ (live status, file
list / upload / download / delete, `flash.txt` editor, Run per `*.txt`, flasher
firmware update). Files live in FFat at the root; names are `[A-Za-z0-9._-]`, max 31
chars. flashing-shop.md is a local, untracked note (.gitignore).

## Layout

- `TDisplayS3-PIC18K22-Flasher.ino` — UI (home page, flash page), state machine, wifi/OTA/mDNS/webserver, FFat file API, `runScript` / `flashWithRetry` / `runTests`, `probe()`. Auto mode default (no buttons on the enclosure; mode letter hidden). MODE_BUTTON (GPIO0): short press toggles auto/manual, held 5 s forgets WiFi (can't be read at power-up: boot strap). FLASH_BUTTON (GPIO14): manual run; runs start on debounced release.
- `icsp.cpp/h` — bit-banged HV ICSP wire driver: HV entry/exit, 4-bit command + 16-bit payload frames (LSb first), TBLRD*+ and TABLAT data-out, start-programming hold (P9/P9A then P10), bulk-erase hold (P11+P10). Cycle-counted half period, direct GPIO register access.
- `pic18k22.cpp/h` — device table (16 parts, ID & 0xFFE0 -> name, flash, EEPROM), `pic_probe`, and `pic_dev` (the script backend): read, write, erase, ee_locked, reset. Everything touching the wire is under `#ifndef HOST_TEST`.
- `script.cpp/h` — script parser and runner over `pic_ops` / `fs_ops` function pointers (no Arduino APIs, so it builds on the host).
- `ihx.cpp/h` — sparse multi-region Intel HEX parse/write (flash, user ID, config, EEPROM; per-byte written mask).
- `webpage.h` — the web page (PROGMEM HTML).
- `config.h` — pins: PGD=GPIO1, PGC=GPIO2, MCLR_LOW=GPIO3, VPP_ON=GPIO10; `ICSP_HALF_PERIOD` (clock knob); buttons.
- `tests/` — test_ihx.c, test_script.c, test_parts.c (run by `make test`).
- `docs/wiring.svg` / `wiring.png` — the wiring diagram (T-Display S3, PICkit-2-style MCLR drive, ICSP header).

## Hardware

MCLR is driven PICkit-2 style: NPN (Q1) on GPIO3 pulls MCLR to GND through 100 Ω
(`MCLR_LOW`, on by default: target held in reset); NPN (Q2) on GPIO10 drives a PNP
(Q3) high-side switch from the 9 V rail to MCLR (`VPP_ON`). **GPIO10 needs a
hardware pull-down (10 kΩ): the pin floats while the ESP32 boots.** The firmware
never turns both switches on together: 5 µs break-before-make, MCLR floats for
that long. PGC/PGD have internal pull-downs so no target reads device ID 0x0000
(= "no target"). The jig powers the target from 3V3 (3.3 V targets only, no VDD
sensing). The target's MCLR pin must tolerate 9 V.

## Behavior contracts (user-specified, don't regress)

- FAIL means a `verify` mismatch ONLY. No target, comm errors and script errors are never shown as FAIL. Comm errors (`SCRIPT_COMM`) retry the whole script up to 3 times (`flashWithRetry`), then an orange `comm error no verdict`. Script errors (`SCRIPT_ERROR`: syntax, missing file, blank userid, EE locked, HEX problems) are an orange notice with the line number, no retry, counted as neither pass nor fail.
- **Settings files are per board; never write a settings file to a board whose `{id}` differs.** `{id}` = the board's 8 user-ID bytes (0x200000) as 16 uppercase hex digits, read ONCE when the script starts (before any command, so it survives `erase chip`) from the seated board itself. Scripts must keep the `settings-{id}.hex` / `userid-{id}.hex` pattern; no hard-coded IDs.
- If any script line uses `{id}` and the user ID is blank (all 0xFF), stop before touching the chip: `blank userid`.
- The whole script is checked before the first command runs (syntax; every `write`/`verify` file must exist or be produced by an earlier `read`). A missing file is an error, never a skipped step.
- `read eeprom` with EEPROM read protection (CONFIG5H CPD = 0) stops the script: `EE locked`. Never erase after a failed settings save.
- The script stops at the first error and reports the line number on screen and in the serial log.
- Home line 1 = live detected target (`no target` / part name / `unknown 0x....`), polled every 1 s in both modes (enter HV, read device ID and user ID, exit, release MCLR). Unknown parts are skipped with an orange notice and no script runs on them. Probe only between script runs: `pic_probe()` ends any open ICSP session.
- Config verify compares under the implemented-bits mask only. Config bytes are written with CONFIG6H (WRTC) last.
- Result holds on the flash page 1 s (pass and fail); FLASH_BUTTON during the hold reruns immediately. The last result word then stays on the home screen (line 4) until the board is removed.
- Auto mode runs `flash.txt` once per insertion (`flashed`, set after ANY outcome including comm error, cleared when the probe stops seeing the board). A board left on the jig is never reflashed.
- Uploading a file never runs anything. `POST /run` is the only HTTP way to start a script; the loop runs it (`wantRun`).
- WiFi is secondary: flashing must never wait on it (WiFiManager non-blocking). Nothing is authenticated (same as the siblings).
- HV only. No LVP (needs LVP and MCLRE both on; field firmware often disables one).

## Current state / caveats

- **Nothing has been run on hardware yet** (as of 2026-10-13). `make bin` and `make test` pass. HTTP handlers, screen text widths and the ICSP bit timing are unexercised on a real jig/chip. Bring-up order: probe shows the right part and `{id}`; known EEPROM and user ID loaded with MPLAB survive `flash.txt` and the new firmware runs; a blank-user-ID board gives `blank userid` and is untouched.
- The DS41398 values (command encodings, P9 1 ms, P9A 5 ms, **P10 200 µs**, P11 15 ms, P11A 4 ms, device IDs, config mask) were checked by the implementer against DS41398B. No second reader has re-checked them against the PDF.
- ICSP has no acknowledge, so the wire layer can't detect lost commands. `SCRIPT_COMM` only comes from: no part answering at the start of a run, or an EEPROM write whose WR bit never clears. A board lost mid-write normally ends as a `verify` mismatch.
- A HEX file with a record outside the part's regions is rejected when its `write`/`verify` line runs, which is after `erase chip` in the default script (the pre-scan only checks that files exist).
- A comm error after `erase chip` makes the retry stop at `blank userid` (the erase wiped the ID); the board is left erased and the jig's `userid-<id>.hex` / `settings-<id>.hex` are the recovery copies.
- `write` doesn't erase: flash and user-ID rows need erased cells (the default script erases the chip first). Rows are padded with 0xFF where the HEX leaves gaps.
- `read eeprom`/`write` EEPROM costs >= 4 ms per byte (P11A), so a 1 KB part takes seconds.
- Detection resets the target about twice a second while a part is present (one HV session for the device ID, a second for the user ID): a connected chip only runs between probes.
- `lastResult` is 15 characters and screen messages 18: long error text is cut on screen / in /status; the serial log has it all.
- `flash.txt` is only recreated at boot if missing; deleting it makes auto mode show `no script` once per insertion.
