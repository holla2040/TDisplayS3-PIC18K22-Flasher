# TDisplayS3-PIC18K22-Flasher — design

Date: 2026-10-13. Status: approved for planning.

## Purpose

A standalone production jig: a LilyGO T-Display S3 (ESP32-S3) programs
PIC18(L)F2x/4xK22 microcontrollers over high-voltage ICSP, no PC in the loop.
It is generic (not tied to any one board) and is driven by a small text script
stored on the jig's FFat filesystem, so each board's EEPROM settings survive a
full reflash, kept per board as `settings-<userid>.hex`.

Sibling projects (same UI, web page, state machine; only the wire and device
layers differ):

- `~/r/s/st/stm32/TDisplayS3-STM32F1-Flasher` (SWD)
- `~/r/s/st/stm8/TDisplayS3-STM8S-Flasher` (SWIM)

## Target family

PIC18(L)F23K22/24K22/25K22/26K22/43K22/44K22/45K22/46K22, programming
specification DS41398. Facts from the MPLAB X 6.25 device pack
(`/opt/microchip/mplabx/v6.25/packs/Microchip/PIC18F-K_DFP/1.14.301/edc/PIC18F46K22.PIC`):

| Region | Byte address | 46K22 size |
|---|---|---|
| Program flash | 0x000000 | 64 KB (smaller parts: from the device table) |
| User ID | 0x200000 | 8 bytes |
| Config | 0x300000 | 14 bytes (0x300000–0x30000D) |
| Device ID | 0x3FFFFE | 2 bytes, mask 0xFFE0 for the part, low bits = revision |
| Data EEPROM | 0xF00000 | 1 KB (256 B on all parts except 26K22/46K22) |

Device table (device ID & 0xFFE0, from the pack; F / LF):

| Part | ID F / LF | Flash | EEPROM |
|---|---|---|---|
| 23K22 / 43K22 | 0x5740 / 0x5760, 0x5700 / 0x5720 | 8 KB | 256 B |
| 24K22 / 44K22 | 0x5640 / 0x5660, 0x5600 / 0x5620 | 16 KB | 256 B |
| 25K22 / 45K22 | 0x5540 / 0x5560, 0x5500 / 0x5520 | 32 KB | 256 B |
| 26K22 / 46K22 | 0x5440 / 0x5460, 0x5400 / 0x5420 | 64 KB | 1 KB |

- VPP 7.8–9.0 V at VDD 3.3 V (DS41398B D110: VDD + 4.5 V min, 9.0 V max; default 9.0). VDD 1.8–5.5 V (LF parts: 1.8–3.6 V).
- Write row 64 bytes. Waits: bulk erase 15 ms, row program 1 ms, config 5 ms,
  EEPROM byte 4 ms, user ID 5 ms.
- (Revised 2026-10-13.) The jig programs over LVP by default (`ICSP_LVP 1` in
  `config.h`; `ICSP_LVP 0` builds the original HV driver). LVP needs `LVP=ON`,
  the factory value; MCLRE does not matter, MCLR is always enabled in LVP mode.
  Every `firmware.hex` must keep `LVP=ON`: preflight refuses a `write` that
  clears it (`hex clears LVP`).
- The K22 has no factory unique ID. The per-board identity is the 8 user-ID
  bytes, programmed by the board's firmware build or by MPLAB.

## Hardware

LVP (the default, revised 2026-10-13): GPIO1 PGD, GPIO2 PGC, GPIO3 drives MCLR
directly, 3V3 to VDD, GND; five wires to a 5-pin ICSP header, nothing else (see
README "Wiring"). The table below is the original HV option (`ICSP_LVP 0`).

| T-Display S3 | Target | Notes |
|---|---|---|
| GPIO 1 | PGD | bidirectional |
| GPIO 2 | PGC | jig drives |
| GPIO 3 | MCLR low | N-MOSFET (AO3400A) pulls MCLR to GND through 100 Ω (PICkit 2 Q8 position). Default on: target held in reset. |
| GPIO 10 | MCLR high | N-MOSFET (AO3400A) drives a P-MOSFET (AO3401A) high-side switch putting the VPP rail on MCLR (PICkit 2 Q7/Q6 position) |
| 3V3 | VDD | the jig powers the target; no VDD sensing |
| 5V | boost input | USB 5 V feeds the on-board VPP boost |
| GND | GND | |

- Power is USB only. An on-board boost circuit (the user's existing design from
  another project; MT3608-class, LCSC C84817) makes the VPP rail from the
  T-Display's 5V pin, set to 8.5 V: inside 7.8–9.0 V with margin both ways.
  No external 9 V supply; never an unregulated converter (MCLR is almost no
  load, so its output would rise above 9.0 V).
- Firmware never turns on the VPP switch and the MCLR pull-down together
  (break-before-make, a few µs dead time).
- 3.3 V targets only (ESP32 pins aren't 5 V tolerant). 5 V targets are out of scope.
- Pins live in `config.h`.

## Script

`/flash.txt` runs on a KEY press (manual mode, the default since 2026-10-13)
or on target insertion (auto mode). Any `*.txt` script can also be run from the web page. One command per
line, `#` starts a comment, blank lines ignored, whitespace-separated tokens.

Default `/flash.txt` (created on first boot if missing):

```
read   userid  userid-{id}.hex      # chip erase wipes the user ID, so save it first
read   eeprom  settings-{id}.hex
erase  chip
write  firmware.hex
verify firmware.hex                 # before the settings overwrite any EEPROM data it carries
write  settings-{id}.hex
verify settings-{id}.hex
write  userid-{id}.hex              # last: an interrupted run leaves the ID blank, so the rerun stops
verify userid-{id}.hex
reset
```

### Commands

| Command | Effect |
|---|---|
| `read <flash\|userid\|config\|eeprom> <file>` | Read that region of the target and save it as Intel HEX on FFat (overwrites) |
| `erase chip` | Bulk erase: flash, user ID, config, EEPROM, code protection |
| `erase eeprom` | Bulk erase of the data EEPROM only |
| `write <file>` | Program every record of the HEX file into the region its address falls in |
| `verify <file>` | Read back every byte the HEX file covers and compare |
| `reset` | Leave programming mode; release MCLR so the target runs |

### Rules

- `{id}` expands to the 8 user-ID bytes as 16 uppercase hex digits, in
  address order (0x200000 first). It is read **once, when the script starts**,
  before any command runs, so it stays valid after `erase chip`.
- If any line uses `{id}` and the user ID is blank (all 0xFF), the script
  stops before touching the chip: orange `blank userid`. Otherwise all
  unnumbered boards would share one settings file.
- `write` / `verify` of a missing file is an error (a missing file never
  silently skips a step).
- A HEX record outside every region of the detected part is an error, checked
  when the file is loaded, before anything is written.
- The user ID is written last in the default script: a run cut short anywhere
  after `erase chip` leaves it blank, so the rerun stops at `blank userid`
  instead of reading the half-programmed EEPROM over the saved settings file.
- A `write` to the user ID first erases the user-ID block (DS41398B Table 3-1,
  0088h), so the last user-ID write wins. On a code-protected chip that erase
  also erases all program memory (DS41398B §3.1.1).
- A `write` whose file carries user-ID bytes (0x200000–0x200007) followed by a
  later `write` of another file with user-ID bytes stops the script before
  touching the chip: orange `firmware has userid` at the earlier line. A
  `firmware.hex` with IDLOC records would refill the user ID before the
  settings are written, so an interrupted run's rerun would save the
  half-written EEPROM as `settings-<id>.hex`. Build `firmware.hex` without them.
- The device ID is checked when the script starts and again on a `verify`
  mismatch. ICSP has no acknowledge and a missing target reads zeros, so a
  missing or different part is a comm error, never a FAIL.
- Config verify compares under the implemented-bits mask from the device table
  (unimplemented config bits read back as 0).
- `read eeprom` with EEPROM read protection set (`CPD` = 0 in CONFIG5H) stops
  the script: orange `EE locked`. The erase that follows would otherwise destroy
  settings it couldn't save.
- The script stops at the first error. Every stop is reported with its line
  number on screen and in the serial log.

### Outcomes (sibling contracts, unchanged)

- Red **FAIL** = a `verify` mismatch only.
- Comm errors (no target, entry failure, bad device ID mid-script) retry the
  whole script up to 3 times, then show an orange `comm error`.
- Script errors (syntax, missing file, blank userid, EE locked, out-of-range
  record, firmware has userid) show an orange notice with the line number, no retry.
- Green **PASS** = every line ran.
- Pass/fail counters, a 1 s result hold, KEY during the hold reruns, and the
  last result word stays on the home screen until the board is removed.
- Auto mode runs `/flash.txt` once per insertion (`flashed` set after any
  outcome, cleared after 3 "no target" probes in a row, about 3 s, or when a
  different user ID than the last run's shows on two probes in a row). After a
  run the last run's ID is unknown (the run may change it): it is taken from
  the first ID seen on two probes in a row, and that is not a new insertion. A
  board left on the jig is never reflashed.

## Detection

Once a second in auto mode (manual mode, the default, probes only at boot and
before each run): enter programming mode, read the device ID, exit.
Device ID 0x0000 or 0xFFFF = no target. Otherwise home line 1 shows the part
name from the device table (e.g. `PIC18F46K22`) or `unknown 0x5200` for an ID
not in the table, shown raw with its revision bits (and no script runs on it).

Detection is intrusive: the target is held in reset for the duration of each
probe (a few ms), once a second, the same tradeoff as the STM8 jig.

## Web page and HTTP API

- `GET /` — page: status, file list (name, size, download, delete), upload,
  `flash.txt` editor (load / save), a Run button per `*.txt`.
- `GET /status` — JSON: detected part, `{id}` of the seated board, pass/fail,
  auto, build, md5, last result.
- `GET /files` — JSON list of FFat files.
- `GET /file?name=X` — download. `POST /file` (multipart) — upload (any name).
  `DELETE /file?name=X` — delete.
- `POST /run?name=X.txt` — run a script on the seated board (the loop picks it
  up, as with upload-flash in the siblings).
- `POST /update` — jig firmware self-update (unchanged from the siblings).
- File names: `[A-Za-z0-9._-]`, max 31 chars, no paths; anything else gets 400.
- Uploading a file never runs anything by itself.
- Nothing is authenticated (same as the siblings): put jigs on an isolated network.

## Code layout

| File | Role |
|---|---|
| `TDisplayS3-PIC18K22-Flasher.ino` | Copied from the STM32 jig: UI, auto/manual, buttons, WiFiManager (non-blocking), OTA, mDNS (`PIC18Flasher.local`), web server, FFat. The single-image loader is replaced by the script runner. |
| `icsp.cpp/h` | Wire layer: LVP (default) or HV entry/exit sequence, 4-bit command + 16-bit payload frames, TBLRD*+ byte read, data-out shifting, timing constants. Direct GPIO register access, as in `swd.cpp`. |
| `pic18k22.cpp/h` | Device layer: device table (ID → name, flash size, EEPROM size, config masks), probe, region read, bulk erase (chip / EEPROM), 64-byte row write, user ID write, config byte write, EEPROM byte write. |
| `script.cpp/h` | Parser and runner over a small device interface (function pointers), so it runs on the host against a fake chip. |
| `ihx.cpp/h` | Sparse multi-region Intel HEX: parse into per-region buffers plus a written-byte mask; write a region back out as HEX (type 04 extended linear address records). |
| `webpage.h` | PROGMEM HTML page. |
| `config.h` | Pins, timing knobs. |
| `Makefile` | `bin`, `usb`, `ota`, `test`, `clean`. Build dir `/tmp/arduino-build-pic18` (separate from the siblings). |
| `README.md`, `CLAUDE.md`, `docs/architecture.md`, `LICENSE` (MIT), `.gitignore` | Same pattern as the siblings. |

## Testing

- `make test` (host gcc, no hardware):
  - HEX round trip (parse → write → parse) across all four regions, bad
    checksum and out-of-range records rejected.
  - Script runner against a fake chip: command order, `{id}` expansion, blank
    userid stops before any erase, EE locked stops before erase, missing or
    out-of-range file is an error before erase, firmware with user-ID records
    before a later user-ID write is an error before erase, only a verify mismatch with the
    target still there gives FAIL, an interrupted run's rerun keeps the saved settings.
- `make bin` compiles cleanly (arduino-cli, esp32 core 3.3.0).
- On hardware (the user, on a real K22 board):
  1. Probe shows the right part name and `{id}`.
  2. Load known EEPROM contents and a user ID with MPLAB, run `flash.txt`,
     confirm with MPLAB that EEPROM and user ID survived and the new firmware runs.
  3. A board with blank user ID gives `blank userid` and is untouched.

## Out of scope

5 V targets; PIC families other than K22; a jig-assigned serial number;
authentication; per-board logging beyond the settings files.
