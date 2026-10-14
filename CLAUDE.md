# TDisplayS3-PIC18K22-Flasher

Standalone production-jig flasher for PIC18(L)F2xK22 / 4xK22: a LilyGO T-Display
S3 (ESP32-S3) programs the target over low-voltage ICSP (DS41398; HV is a build option), no PC in the
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

- `TDisplayS3-PIC18K22-Flasher.ino` — UI (home page, flash page), state machine, wifi/OTA/mDNS/webserver, FFat file API, `runScript` / `flashWithRetry` / `runTests`, `probe()`. Manual mode default: no polling, runs start only from FLASH_BUTTON or POST /run, so a programmed target runs undisturbed (mode letter hidden). MODE_BUTTON (GPIO0): short press toggles auto/manual, held 5 s forgets WiFi (can't be read at power-up: boot strap). FLASH_BUTTON (GPIO14): manual run; runs start on debounced release.
- `icsp.cpp/h` — bit-banged ICSP wire driver: LVP entry/exit (key 4D434850h MSb first) or, with `ICSP_LVP 0`, HV entry/exit, 4-bit command + 16-bit payload frames (LSb first), TBLRD*+ and TABLAT data-out, start-programming hold (P9/P9A then P10), bulk-erase hold (P11+P10). Cycle-counted half period, direct GPIO register access.
- `pic18k22.cpp/h` — device table (16 parts, ID & 0xFFE0 -> name, flash, EEPROM), `pic_probe` (device ID + user ID in one ICSP session), and `pic_dev` (the script backend): read, write (a user-ID write bulk-erases the user-ID block first, 0088h), erase, ee_locked, devid, reset; plus `part_id` and sizes latched by the probe. Everything touching the wire is under `#ifndef HOST_TEST`.
- `script.cpp/h` — script parser and runner over `pic_ops` / `fs_ops` function pointers (no Arduino APIs, so it builds on the host).
- `ihx.cpp/h` — sparse multi-region Intel HEX parse/write (flash, user ID, config, EEPROM; per-byte written mask); `ihx_parse_any` also reads compact binary `.pbin` ("PICB", [u32 addr][u16 len][data]..., CRC32). `fsLoad` in the .ino inflates gzip files (ROM `tinfl`, decompressor on the heap: the loop stack is too small) before parsing, so scripts can name `.hex`, `.pbin` or `.pbin.gz`.
- `tools/hex2pbin.py` — HEX -> `.pbin` + `.pbin.gz`; refuses a HEX without CONFIG4L or with LVP off.
- `webpage.h` — the web page (PROGMEM HTML).
- `config.h` — `ICSP_LVP` (1 = LVP, the default; 0 = HV); pins: PGD=GPIO1, PGC=GPIO2, GPIO3 = MCLR (LVP: `MCLR_PIN`, driven directly; HV: `MCLR_LOW_PIN`), HV only: VPP_ON=GPIO10; `ICSP_HALF_PERIOD` (clock knob); buttons.
- `tests/` — test_ihx.c, test_script.c, test_parts.c (run by `make test`).
- `docs/wiring-lvp.svg` / `.png` — LVP wiring (the default: five wires to a 5-pin ICSP header); `docs/wiring.svg` / `.png` — HV wiring (PICkit-2-style MCLR drive).

## Hardware

LVP (the default build, `ICSP_LVP 1`): five wires to the target's 5-pin ICSP header:
GPIO3 to MCLR (pin 1, driven directly push-pull: low = reset, high = run), 3V3 to
VDD (pin 2), GND (pin 3), GPIO1 PGD (pin 4), GPIO2 PGC (pin 5). No transistors and no
target MCLR pull-up needed: with LVP = 1 the PIC's MCLR is always enabled and its
internal weak pull-up is always on (DS40001412G §10.6.3, §24.9 note 3). If the target
is powered from its own supply, leave pin 2 off (four wires). PGC/PGD have internal
pull-downs so no target reads device ID 0x0000 (= "no target"). 3.3 V targets only, no
VDD sensing. **GPIO3 is an ESP32-S3 strapping pin** (JTAG source select), ignored unless
the `STRAP_JTAG_SEL` eFuse is burned (off by default); it floats until `icsp_init()`
(first line of `setup()`), so MCLR is undefined for ~300 ms at jig boot.

HV (`ICSP_LVP 0`, the exception) needs the 8.5 V boost and Q1-Q3 described in README
"Wiring"; the firmware keeps that code under `#else`.

## Behavior contracts (user-specified, don't regress)

- FAIL means a `verify` mismatch ONLY. No target, comm errors and script errors are never shown as FAIL. Comm errors (`SCRIPT_COMM`) retry the whole script up to 3 times (`flashWithRetry`), then an orange `comm error no verdict`. Script errors (`SCRIPT_ERROR`: syntax, missing file, blank userid, EE locked, HEX problems, firmware has userid) are an orange notice with the line number, no retry, counted as neither pass nor fail.
- **Settings files are per board; never write a settings file to a board whose `{id}` differs.** `{id}` = the board's 8 user-ID bytes (0x200000) as 16 uppercase hex digits, read ONCE when the script starts (before any command, so it survives `erase chip`) from the seated board itself. Scripts must keep the `settings-{id}.hex` / `userid-{id}.hex` pattern; no hard-coded IDs.
- If any script line uses `{id}` and the user ID is blank (all 0xFF), stop before touching the chip: `blank userid`.
- The whole script is checked before the first command runs (syntax; every name must expand; every `write`/`verify` file must be produced by an earlier `read`, or exist and parse against the detected part: out-of-range records and bad checksums are caught here; so is a `write` carrying user-ID bytes followed by a later user-ID `write`: `firmware has userid` at the earlier line). A missing file is an error, never a skipped step.
- If the script has a `read eeprom` and the board has EEPROM read protection (CONFIG5H CPD = 0), it stops before any line runs: `EE locked`. Never erase after a failed settings save.
- The device ID is checked at script start and on every verify mismatch (`devid()` vs `part_id`). Gone or different part = `SCRIPT_COMM` `no target`, never FAIL. `flashWithRetry` re-probes before each retry and stops with orange `no target removed` if the board is gone.
- Default `flash.txt` order: read userid / read eeprom / erase chip / write+verify firmware.hex / write+verify settings-{id}.hex / write+verify userid-{id}.hex / reset. **The user ID goes last** so a run cut short after the erase leaves it blank and the rerun stops at `blank userid` instead of saving the half-written EEPROM over `settings-<id>.hex`. Firmware is verified before the settings overwrite any EEPROM data it carries. Keep this order in the .ino, docs/design.md, README.md, docs/architecture.md and tests/test_script.c.
- The script stops at the first error and reports the line number on screen and in the serial log.
- Home line 1 = detected target (`no target` / part name / `unknown 0x....`): probed once at boot and before every run; in auto mode also polled every 1 s (one ICSP session: enter, read device ID and user ID, exit, release MCLR). Manual mode (default) never polls, so the target runs undisturbed after a run. Unknown parts are skipped with an orange notice and no script runs on them. Probe only between script runs: `pic_probe()` ends any open ICSP session.
- Config verify compares under the implemented-bits mask only. Config bytes are written with CONFIG6H (WRTC) last.
- Result holds on the flash page 1 s (pass and fail); FLASH_BUTTON during the hold reruns immediately. The last result word then stays on the home screen (line 4) until the board is removed.
- Auto mode runs `flash.txt` once per insertion (`flashed`, set after ANY outcome including comm error, cleared together with `lastResult` after `MISSES_GONE` = 3 "no target" probes in a row, ~3 s, OR when a user ID different from `flashedId` shows on two consecutive probes: a quick swap to a different board is run). `runTests` sets `flashedId` to the ID the run started on (no post-run probe, so one bad read can't reflash a seated board, and a swap during the result hold is still caught); a run that changes the ID (blank after an interruption) gets one more run, which stops at `blank userid` without touching the chip. A board left on the jig is never reflashed, even across one bad probe or one misread ID.
- Uploading a file never runs anything. `POST /run` (only `*.txt`, else 400) is the only HTTP way to start a script; the loop runs it (`wantRun`).
- WiFi is secondary: flashing must never wait on it (WiFiManager non-blocking). Nothing is authenticated (same as the siblings).
- **MANDATORY: never compile or flash target firmware without `LVP = ON`.** Every target build sets `#pragma config LVP = ON`; before any hex is uploaded or flashed (jig or PICkit), decode CONFIG4L (0x300006) and confirm bit 2 = 1. A hex with LVP off is never flashed, no exceptions (user, 2026-10-13).
- **LVP is the programming mode** (user decision 2026-10-13): HV is the exception (the user builds the 8.5 V boost), never the default. Every `firmware.hex` must keep `LVP` = 1: preflight refuses a `write` whose CONFIG4L clears LVP (`hex clears LVP`, `SCRIPT_ERROR`, before any device call). LVP can be changed only in HV mode, so a cleared bit would lock the board out of LVP.

## Current state / caveats

- **Hardware status (2026-10-13), LVP on Trace R0 boards (PIC18F46K22):** probe, `read config`/`read userid`/`read flash`, `erase chip`, `write`, `verify` and `reset` all work; blink and LED test images programmed by the jig run (and read back byte-identical). Still untested on hardware: the full default `flash.txt` with a real user ID and EEPROM, EEPROM writes, user-ID writes, the `{id}` settings flow, pulling a board mid-run, auto mode, and **a `firmware.hex` with CP0 = 0 and CPD = 0: do the bits take effect before reset?** (decides whether firmware.hex may ever carry protection bits). Trace R0 boards had LEDs D4-D6 assembled backwards (fixed by hand on the bench boards).
- The DS41398 values (command encodings, P9 1 ms, P9A 5 ms, **P10 200 µs**, P11 15 ms, P11A 4 ms, device IDs, config mask) were checked by the implementer against DS41398B. The user-ID bulk-erase option 0088h was re-checked against the PDF (Table 3-1, page 13) in the final review fix wave; the rest has no second reader.
- ICSP has no acknowledge, so the wire layer can't detect lost commands. `SCRIPT_COMM` comes from: the device ID check (start of a run, or after a verify mismatch: target gone or different), the `{id}` read, or an EEPROM write whose WR bit never clears. A contact that drops and recovers between write and verify can still end as FAIL.
- Any stop after `erase chip` and before `write userid-{id}.hex` leaves the user ID blank: the retry/rerun stops at `blank userid`, and the jig's `userid-<id>.hex` / `settings-<id>.hex` are the recovery copies (README: one-off script naming them explicitly). A `firmware.hex` carrying IDLOC records would refill the ID early and defeat this, so preflight rejects any `write` with user-ID bytes followed by a later user-ID `write`: `firmware has userid` (`SCRIPT_ERROR`) at the earlier line, before any device call.
- **Config protection bits (hardware check pending, Task 6):** `firmware.hex` must not set CP0-3, CPB, CPD, WRT0-3, WRTB, WRTC or WRTD until bring-up shows config bits only take effect after reset. The script writes config together with the firmware, before `verify firmware.hex` and the settings write; if CP/CPD/WRTD act in-session, verify fails, the settings write is blocked, and every CPD board becomes one-shot (`EE locked` next time). If they do act in-session, the fix is for `write` to queue the config region and `verify` to skip it, then write and verify the queued config at `reset` / script end. Also: the user-ID block erase (0088h, before every user-ID write) on a code-protected chip erases all program memory (DS41398B §3.1.1).
- `write` doesn't erase flash: flash rows need erased cells (the default script erases the chip first). A user-ID write erases the user-ID block itself. Rows are padded with 0xFF where the HEX leaves gaps.
- `read eeprom`/`write` EEPROM costs >= 4 ms per byte (P11A), so a 1 KB part takes seconds.
- In auto mode, detection resets the target once a second while a part is present (one ICSP session reads device ID and user ID): a connected chip only runs between probes. Manual mode (default) doesn't poll.
- Remaining gap: two boards with the **same** user ID (e.g. both blank) swapped in under ~3 s look like one board; the second isn't run and the first's result still shows. Different-ID swaps are caught by the ID-change rule (~2 s after seating).
- `lastResult` is 15 characters and screen messages 18: long error text is cut on screen / in /status; the serial log has it all.
- `flash.txt` is only recreated at boot if missing; deleting it makes auto mode show `no script` once per insertion.
