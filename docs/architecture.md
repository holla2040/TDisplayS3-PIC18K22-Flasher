# Architecture

How the jig works, as built. For setup and operation see the [README](../README.md);
for the original spec, [design.md](design.md).

Status: the code builds and the host tests pass. Nothing has run on hardware.
The ICSP values below (command codes, timings, device IDs) were checked by the
implementer against DS41398B; nobody else has re-checked them yet.

## Design principle

Programming follows Microchip's DS41398 (PIC18(L)F2X/4XK22 Flash Memory
Programming Specification), high-voltage ICSP only. A jig programs one chip
family, so the driver is a small direct implementation of the spec's sequences
rather than a port of a programmer. What to do to a board is not compiled in:
a text script on the jig's FFat decides it, and the script runner is separate
from the chip so it can be tested on the host against a fake chip.

## Layers

```
┌───────────────────────────────────────────────────────────────┐
│ TDisplayS3-PIC18K22-Flasher.ino                               │
│  probe every 1 s → run flash.txt once per insertion →         │
│  PASS / FAIL / orange notice; screen, buttons, WiFiManager,   │
│  ArduinoOTA, web server, FFat file API                        │
├───────────────────────┬───────────────────┬───────────────────┤
│ script.cpp            │ pic18k22.cpp      │ webpage.h         │
│  parse, {id},         │  device table,    │  / /status /files │
│  preflight, run;      │  probe, read /    │  /file /run       │
│  talks to pic_ops +   │  write / erase,   │  /update          │
│  fs_ops only          │  pic_dev          │                   │
│ ihx.cpp               │                   │ FFat: flash.txt,  │
│  multi-region HEX     │                   │  *.hex, *.txt     │
├───────────────────────┴───────────────────┴───────────────────┤
│ icsp.cpp — HV ICSP wire driver                                │
│  HV entry / exit, 4-bit command + 16-bit frames, data out,    │
│  programming and bulk-erase holds                             │
├───────────────────────────────────────────────────────────────┤
│ GPIO1 PGD · GPIO2 PGC · GPIO3 MCLR_LOW · GPIO10 VPP_ON        │
│ (MCLR drive: NPN pull-down, NPN + PNP high-side 9 V switch)   │
└───────────────────────────────────────────────────────────────┘
```

## ICSP wire driver (`icsp.cpp`)

Bit-banged with direct register access (`GPIO.out_w1ts/w1tc`, `enable_w1ts/w1tc`,
`GPIO.in`), and each half clock is `ICSP_HALF_PERIOD` ESP32 cycles counted with
`esp_cpu_get_cycle_count()`. At the default 240 cycles (240 MHz) that is about
1 µs per half period; DS41398 allows 100 ns, so it is the cable-length knob.
The jig is the only clock source, so an interrupt can only stretch a clock
phase, never corrupt a bit.

### Pins and MCLR drive

| Pin | Role |
|---|---|
| GPIO1 `PGD_PIN` | PGD, bidirectional (input/output with an internal pull-down) |
| GPIO2 `PGC_PIN` | PGC, driven by the jig (internal pull-down) |
| GPIO3 `MCLR_LOW_PIN` | NPN: MCLR to GND through 100 Ω. High = target held in reset. |
| GPIO10 `VPP_ON_PIN` | NPN driving a PNP high-side switch: 9 V onto MCLR. High = HV on. |

`icsp_init()` sets the levels before the directions (VPP off, MCLR_LOW on, PGC/PGD
low), so neither switch glitches on at boot. GPIO10 still floats before that
runs, which is why the hardware has a 10 kΩ pull-down on it.

### HV entry and exit (DS41398 §2.5, Figures 2-12 and 2-13)

The jig powers VDD from 3V3, so VDD is already up. `icsp_enter()`:

1. VPP off, MCLR_LOW on (the target may be running: reset it first), PGC/PGD
   low and driven.
2. Wait 100 µs.
3. MCLR_LOW off.
4. Wait 5 µs. **MCLR floats here**: neither switch drives it (break-before-make,
   so 9 V is never connected to the 100 Ω pull-down).
5. VPP on. Wait 100 µs (the spec's P12 is 2 µs before the first command; the
   rest covers the PNP switch's rise).

`icsp_exit()`: PGC/PGD low, VPP off, wait 5 µs (floating again), MCLR_LOW on,
PGC/PGD released to inputs (P16 = 0: no PGC edge between the last frame and MCLR falling).

`icsp_release()` turns MCLR_LOW off so the target runs. It does nothing while
VPP_ON is set, so it can't be used mid-session. After `icsp_exit()` the target is
held in reset until something releases it: `icsp_init()` leaves it held, and the
device layer releases it after every probe and at `reset`.

### Command frame

Every frame is a 4-bit command followed by a 16-bit payload, sent LSb first
(DS41398 §2.7). The jig puts each bit on PGD while PGC is high; the target
latches it on the PGC falling edge.

| Command | Function | Call |
|---|---|---|
| `0000` | Core instruction: the payload is a PIC18 instruction, executed by the target | `icsp_core(instr)` |
| `1001` | Table read, post-increment: 8 dummy clocks, then the byte shifts out | `icsp_tblrd_postinc()` |
| `0010` | Shift out TABLAT: same shape; the data is the second byte of the frame | `icsp_shift_tablat()` |
| `1100` | Table write | `icsp_tblwr(0xC, data)` |
| `1101` | Table write, post-increment by 2 | `icsp_tblwr(0xD, data)` |
| `1111` | Table write and start programming | `icsp_start_prog(data, hold_us)` |

**Data out.** After the 8 operand clocks the target drives PGD. The jig releases
PGD, waits one half period (P6 turnaround, PGC low), then clocks 8 bits, sampling
PGD just before each falling edge (the data is valid P14 after the rising
edge), and takes PGD back. Reads are LSb first.

**Start programming** (`icsp_start_prog`, Figure 3-5). Programming is externally
timed. After the `1111` frame the jig sends a NOP command (`0000`): three PGC
clocks, then the 4th PGC is held high for P9 (flash rows) or P9A (user ID and
config), then PGC goes low for P10, then 16 more zero bits finish the NOP.

**Bulk erase** (`icsp_nop_hold`, Figure 3-2). The erase is self-timed. After the
4th PGC of a NOP command the jig keeps PGC and PGD low for P11 + P10 (the document's
Figure 3-1 says "Delay P11 + P10"), then sends the 16 zero payload bits.

### Timing constants (`icsp.h`, DS41398 §6.0)

| Constant | Value | Used for |
|---|---|---|
| `P9_US` | 1000 µs | PGC high, flash row programming |
| `P9A_US` | 5000 µs | PGC high, user ID and config programming |
| `P10_US` | **200 µs** | PGC low after programming (high-voltage discharge) |
| `P11_US` | 15000 µs | Self-timed bulk erase (15 ms for the X5/X6 parts; X3/X4 need 12 ms, so 15 ms covers all 16 parts) |
| `P11A_US` | 4000 µs | Data EEPROM byte write wait, then WR is polled |

P10 is 200 µs in DS41398B; the original plan said 100 µs and the document won.
The erase supply-voltage requirement (D111, VDD >= 2.7 V on LF parts) is met by the jig's 3V3.

## Device layer (`pic18k22.cpp`)

### Device table and probe

`pic_lookup(id)` masks the ID with 0xFFE0 (the low 5 bits are the silicon
revision) and finds one of 16 entries: name, flash size, EEPROM size. IDs are
from DS41398 Table 5-2, sizes from the MPLAB X PIC18F-K_DFP 1.14.301 device
files.

`pic_probe()` enters programming mode, reads the device ID at 0x3FFFFE/F
(`TBLPTR` = 0x3FFFFE, two `TBLRD*+`), leaves, and releases MCLR. It returns
`no target` for 0x0000 or 0xFFFF (PGC/PGD have internal pull-downs, so an empty
jig reads zeros), the part name, or `unknown 0x5403` (the raw ID, revision bits
included). It latches the detected part into `pic_dev`: region sizes (flash,
user ID 8, config 14, EEPROM) and the config mask. The ino then reads the user
ID in a second session for the home screen.

### Session

One script is one ICSP session. Each device operation calls `session()`, which
enters programming mode only if the previous operation hasn't already. The
session ends at `pic_dev.reset()` (exit, 100 µs, release MCLR) or at the next
`pic_probe()`, so the probe can only run between scripts.

### Memory operations

Instruction encodings are the access-bank forms from DS41398 Tables 3-x and 4-x
(`MOVLW`, `MOVWF` to `TBLPTRU/H/L`, `EEADR`, `EEADRH`, `EEDATA`, `TABLAT`, `BSF`/`BCF`
on `EECON1` bits, `MOVF EEDATA,W`). Addresses are PIC18 byte addresses:

| Region | Base | Size (46K22) |
|---|---|---|
| Flash | 0x000000 | 64 KB (8/16/32 KB on smaller parts) |
| User ID | 0x200000 | 8 B |
| Config | 0x300000 | 14 B (CONFIG1L..CONFIG7H) |
| Device ID | 0x3FFFFE | 2 B |
| Data EEPROM | 0xF00000 in the HEX files | 1 KB (256 B on the other parts) |

**Read** flash, user ID and config: set `TBLPTR` to base + offset (three `MOVLW` /
`MOVWF` pairs), then one `TBLRD*+` per byte. **Read** EEPROM: `BCF EEPGD`,
`BCF CFGS`, then per byte set `EEADR`/`EEADRH`, `BSF RD`, `MOVF EEDATA,W`,
`MOVWF TABLAT`, `NOP`, shift out TABLAT.

**Write flash and user ID** (Tables 3-5 and 3-8): `BSF EEPGD`, `BCF CFGS`,
`BSF WREN`. For each row (64 bytes; 8 for the user ID) that holds at least one
byte the HEX file supplies: set `TBLPTR` to the row, send the row's words with
`1101` (post-increment), and the last word with `1111` and the P9 (flash) or
P9A (user ID) hold. Bytes of a row the file doesn't cover are padded with
0xFF, which leaves erased cells alone. Rows nothing touches are skipped.
`BCF WREN` at the end. There is no erase here: the cells have to be blank.

**Write config** (Table 3-9): `BSF EEPGD`, `BSF CFGS`, `BSF WREN`, then one byte at
a time in the order 0..10, 12, 13, **11 last**: set `TBLPTR` to that byte, send
`1111` with the byte in both halves of the payload (even addresses take the low
byte, odd the high), P9A hold. CONFIG6H (offset 11, `WRTC`) goes last because
once WRTC is set no other config byte can be written. Bytes the HEX doesn't
cover, and the three offsets with no implemented bits (0, 4, 7), are skipped.
Config mask per byte: `00 FF 1F 3F 00 BF C5 00 xx C0 xx E0 xx 40`, where `xx` (the
CP, WRT and EBTR bytes at offsets 8, 10, 12) is 0x0F on 32/64 KB parts and 0x03
on 8/16 KB parts.

**Write EEPROM** (Table 3-7): `BCF EEPGD`, `BCF CFGS`; per masked byte set
`EEADR`/`EEADRH` and `EEDATA`, `BSF WREN`, `BSF WR`, two `NOP`s (the write starts on
the 4th PGC of the second), wait P11A (4 ms), then poll the WR bit (read
`EECON1` through TABLAT) up to 10 more times at 1 ms, wait P10, `BCF WREN`. If WR
is still set the write returns an error (a comm error to the script runner).

**Bulk erase** (Tables 3-1 and 3-2): `TBLPTR` = 0x3C0005, table write `1100` of
0F0F (chip) or 0000 (EEPROM only); `TBLPTR` = 0x3C0004, `1100` of 8F8F or 8484;
a NOP, then the P11 + P10 hold. The combined codes are 0F8Fh (chip: flash,
user ID, config, EEPROM, code protection) and 0084h (data EEPROM only).

**EEPROM locked**: `ee_locked()` reads CONFIG5H (0x300009) and returns true when
bit 7 (`CPD`) is 0. A failed read returns false, which is all the signature allows.

**Reset**: if a session is open, `icsp_exit()`; wait 100 µs; `icsp_release()`.

## HEX handling (`ihx.cpp`)

`ihx_parse` fills four regions (the caller supplies base, size, data and mask
buffers sized from the detected part), clears data to 0xFF and the mask to 0,
and accepts record types 00, 01 and 04 only. A byte outside every region is an
error (`0x200008 outside chip`), as are a bad checksum and malformed records. An
EOF record doesn't stop parsing, so several `ihx_write` outputs concatenated
parse as one. `ihx_write` emits one region's masked bytes: a type-04 record per
64 KB page, 16-byte data records split at mask gaps, then `:00000001FF`.

## Script runner (`script.cpp`)

The runner knows nothing about the chip beyond `pic_ops` (read, write, erase,
`ee_locked`, reset, region sizes, config mask) and `fs_ops` (load, save), so
`make test` drives it with a fake chip and a fake filesystem.

### Phases of `script_run`

1. **Syntax pass over the whole script.** Each line is split into at most four
   tokens and checked against the grammar (`read <region> <file>`, `erase chip|eeprom`,
   `write <file>`, `verify <file>`, `reset`). A bad line, or one over 127
   characters, stops everything: `SCRIPT_ERROR` `syntax` with its line number.
   Nothing has touched the chip yet. The first line that contains `{id}` is
   remembered.
2. **`{id}`.** If any line uses it, the 8 user-ID bytes are read once and
   formatted as 16 uppercase hex digits. A read failure is `SCRIPT_COMM`. All
   0xFF stops with `blank userid` (`SCRIPT_ERROR`) at the first `{id}` line. This
   happens before any command, so `erase chip` can't take the ID away.
3. **Preflight.** Every `write` and `verify` file (after `{id}` expansion) must exist
   on the filesystem, or be saved by an earlier `read` in the same script;
   otherwise `no <file>`. This catches a missing `firmware.hex` before `erase chip`.
   It only checks existence, not content.
4. **One allocation** (data + mask for each region of the detected part, plus a
   read-back buffer as big as the biggest region), then the commands run in order
   with a progress callback before each (line number and text, used by the flash page).

### Commands

- `read <region> <file>`: if the region is EEPROM and `ee_locked()`, `EE locked`
  (before anything is read or erased). Otherwise read the whole region, mark every
  byte present, write it out with `ihx_write`, save to the file (`save <file>` on
  failure).
- `erase chip` / `erase eeprom`: `erase(true/false)`.
- `write <file>`: load and parse the HEX file (any HEX error is `SCRIPT_ERROR` with
  the parser's message), then for each region (flash, user ID, config, EEPROM) with
  at least one masked byte, `write(region, 0, data, mask, size)`. A device
  error is `SCRIPT_COMM`.
- `verify <file>`: load and parse, then per region read the whole region back
  and compare every masked byte. For the config region the difference is masked
  with `config_mask` (unimplemented bits read as 0). A difference is
  `SCRIPT_FAIL` with `verify 0x<address>` (the first mismatching byte). A read
  error is `SCRIPT_COMM`.
- `reset`: `reset()`.

The runner stops at the first non-pass status. The status carries the result
(`SCRIPT_PASS`, `SCRIPT_FAIL`, `SCRIPT_COMM`, `SCRIPT_ERROR`), the 1-based line
(0 on PASS, or for errors that aren't about a line) and a 47-character message.

## Orchestration (`TDisplayS3-PIC18K22-Flasher.ino`)

The jig powers the target, so there is no VDD sensing: a successful probe
(device ID read, not 0x0000/0xFFFF) is the target-present detection.

```
        probe every 1 s (HV session: device ID; second session: user ID)
            │
 no target ◀┤ target present, auto mode, not yet run
 (clear     ▼
  flag,     runScript("flash.txt"): script_run, then pic_dev.reset()
  clear     │      comm error → up to 3 attempts of the whole script
  result)   ▼
   PASS (green) │ FAIL (red, verify mismatch only) │ orange: comm error,
                │                                   │ script error, unknown part
                ▼
     result held 1 s on the flash page (KEY = rerun),
     then on the home screen until the board is removed
                ▼
     flag set: no rerun until the board is disconnected
```

- **FAIL means only a verify mismatch.** No target, comm errors and script errors
  never show as FAIL.
- **Retry.** `runScript` returns false on `SCRIPT_COMM` (or when the probe saw a
  part but none answers at run time) and `flashWithRetry` runs the whole script
  again, up to 3 times, then shows orange `comm error no verdict`. Because the
  script restarts, a comm error after `erase chip` makes the retry stop at
  `blank userid`: the erase wiped the ID. The saved `userid-<id>.hex` is the recovery copy.
- **Script errors** show an orange notice (the message, cut to 18 characters, with
  `line N` on the detail line), aren't retried, and aren't counted as pass or fail.
- **Unknown parts** (`unknown 0x....`) get an orange `unknown part skipped` and
  no script.
- **Once per insertion**: the flag is set after any outcome and cleared when the
  probe stops seeing a board. A board left on the jig is never rerun.
- **After every run**, `pic_dev.reset()` closes any session left open by an error
  and releases MCLR so the target runs.
- **HTTP-triggered runs.** `POST /run` checks the name and file, probes (safe: no
  script is running), and sets `wantRun`; `loop()` probes again and runs the
  script. It never runs in the handler.
- **WiFi is secondary**: WiFiManager runs non-blocking and flashing never waits
  on the network.

## Storage and upload

FFat on the 9.9 MB FATFS partition holds `flash.txt`, the firmware and settings
HEX files, and any other script. File names are `[A-Za-z0-9._-]`, 1-31 characters,
checked on every path (HTTP and script `fs_ops`), so nothing can name a folder.

- First boot formats FFat if needed and writes the default `flash.txt` if it is missing.
- `POST /file` streams the multipart body to `/upload.tmp` and renames it to the
  upload's own file name only when the whole body arrived. The file isn't
  interpreted at upload time. A file named `upload.tmp` is refused.
- `GET /file?name=X` downloads, `DELETE /file?name=X` deletes, `GET /files` lists.
- `GET /status` reports the detected part, the seated board's user ID, counters,
  mode, the last result word, and the build and md5.

## Not built (possible additions)

5 V targets, LVP, other PIC families, a jig-assigned serial number, writing a
user ID the jig made up, authentication on the web endpoints, per-board logs
beyond the saved settings files, and wire-level error detection (ICSP gives the
host no acknowledge, so the driver can't tell a lost command from a good one).

## References

- [DS41398B](https://ww1.microchip.com/downloads/en/DeviceDoc/41398B.pdf): PIC18(L)F2X/4XK22 Flash Memory Programming Specification
- MPLAB X PIC18F-K_DFP 1.14.301 device files (`edc/PIC18F46K22.PIC` and the others): device IDs, memory sizes, config bit masks
- The Microchip PICkit 2 user guide: the MCLR drive circuit the jig copies
