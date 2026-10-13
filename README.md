# TDisplayS3-PIC18K22-Flasher

Standalone production flasher for PIC18(L)F2xK22 / 4xK22 microcontrollers: a
LilyGO T-Display S3 (ESP32-S3) programs the target over high-voltage ICSP. No
PC, no PICkit on the line. A small text script on the jig says what to do
(save the board's EEPROM settings, erase, write the firmware, restore the
settings, verify). You load the firmware and the script to the jig over WiFi
once; after that an operator connects a board, the jig runs the script, and
the screen shows PASS or FAIL. The jig is generic: it isn't tied to one
board, and each board's EEPROM settings are kept on the jig per board, so they
survive a full reflash.

**Status: nothing has been run on hardware yet.** The firmware builds, and the
host tests pass (`make test`). The programming timing and command values were
taken from DS41398B by the implementer while writing the code; the circuit and
resistor values below are the PICkit 2 MCLR drive with values picked for this
jig. Plan a bring-up on a known board (see [Status](#status)).

Sibling projects (same screen, web page and state machine; only the wire and
device layers differ):
[TDisplayS3-STM32F1-Flasher](https://github.com/holla2040/TDisplayS3-STM32F1-Flasher) (SWD) and
[TDisplayS3-STM8S-Flasher](https://github.com/holla2040/TDisplayS3-STM8S-Flasher) (SWIM).

## What you need

| Item | Notes |
|---|---|
| LilyGO T-Display S3, LCD version (not AMOLED), 16 MB flash / 8 MB PSRAM | The build assumes 16 MB flash and OPI PSRAM. |
| Target with a PIC18(L)F K22 | Any of the 16 parts in [Supported targets](#supported-targets-and-limits), on your own board or a bare chip. 3.3 V only. |
| 9 V supply | Powers the VPP switch. DS41398 allows VPP 7.5-9.0 V; use 9 V and don't exceed it. |
| 2 x 2N3904 (or MMBT3904) NPN, 1 x 2N3906 (or MMBT3906) PNP | The MCLR drive; see [Wiring](#wiring). |
| Resistors: 100 Ω, 3 x 10 kΩ, 2 x 4.7 kΩ | R2, R4/R5/R6, R1/R3. The 10 kΩ pull-down R4 on GPIO10 is **required**. |
| 6 wires, or a pogo-pin / clip fixture | Keep them short. |
| 5 V USB-C supply for the jig | The jig powers the target from its 3V3 pin. |
| A computer with arduino-cli | Only to build the jig firmware. |

## Build the jig firmware

Tested with arduino-cli 1.3.1, esp32 core 3.3.0, TFT_eSPI 2.5.43 and
WiFiManager 2.0.17 (by tzapu). Other versions probably work; these are known good.

```
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.0
arduino-cli lib install "TFT_eSPI@2.5.43" "WiFiManager@2.0.17"
```

**TFT_eSPI needs one manual edit** or the build succeeds and the screen stays
black. In `<sketchbook>/libraries/TFT_eSPI/User_Setup_Select.h` (sketchbook is
usually `~/Arduino`), comment out `#include <User_Setup.h>` and uncomment
`#include <User_Setups/Setup206_LilyGo_T_Display_S3.h>`. The edit applies to
every sketch that uses TFT_eSPI and is lost when the library is updated.

Clone into a folder named exactly `TDisplayS3-PIC18K22-Flasher` (arduino-cli
requires the folder to match the .ino name; a GitHub ZIP unpacks to `...-main`,
so rename it). Then, on Linux/macOS:

```
make bin                        # compile into /tmp/arduino-build-pic18
make usb port=/dev/ttyACM0      # upload over USB
make test                       # optional: host-side tests of the HEX parser, script runner and part table (g++)
```

On Windows, or without make, use arduino-cli directly with the same board settings:

```
arduino-cli compile -b esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi --output-dir build .
arduino-cli upload  -b esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi -p COM5 --input-dir build .
```

Arduino IDE equivalent: board "ESP32S3 Dev Module", USB CDC On Boot: Enabled,
Flash Size: 16MB, Partition Scheme: "16M Flash (3MB APP/9.9MB FATFS)", PSRAM: OPI PSRAM.

If no serial port shows up for the first USB upload: hold BOOT, tap RST,
release BOOT, then upload. After that, updates can go over WiFi
([Updating the jig](#updating-the-jig)).

## Wiring

3.3 V target, high-voltage ICSP. Besides PGC, PGD and VDD, the jig has to
switch the target's MCLR pin between ground (reset), floating, and 9 V (HV
programming mode). That takes three transistors, the same arrangement as the
PICkit 2:

![T-Display S3 to PIC18(L)F K22 HV ICSP wiring](docs/wiring.png)

| T-Display S3 | Connects to |
|---|---|
| GPIO 1 | ICSP pin 4, PGD (bidirectional) |
| GPIO 2 | ICSP pin 5, PGC (the jig drives it) |
| GPIO 3 (`MCLR_LOW`) | R1 4.7 kΩ to the base of Q1 (NPN). Q1's collector goes through R2 100 Ω to MCLR, its emitter to GND: it holds the target in reset. On by default. |
| GPIO 10 (`VPP_ON`) | R3 4.7 kΩ to the base of Q2 (NPN), and **R4 10 kΩ from GPIO10 to GND**. Q2's collector goes through R5 10 kΩ to the base of Q3 (PNP). |
| 3V3 | ICSP pin 2, VDD: the jig powers the target |
| GND | ICSP pin 3, GND, and the circuit's ground, and the 9 V supply's negative: one common net |

Q3 is the high-side switch: emitter to +9 V, collector to MCLR, base pulled up
to 9 V by R6 10 kΩ and pulled down through R5 and Q2 when GPIO10 is high.
MCLR (Q3's collector, R2, and ICSP pin 1) is one net. ICSP pin 6 isn't used.
The pins are set in `config.h`.

- **GPIO10 needs the hardware pull-down (R4).** The pin floats while the
  ESP32-S3 boots, before the firmware runs. Without R4, Q2 and Q3 can turn on
  and put 9 V on MCLR. The firmware drives GPIO10 low first thing in setup.
- **MCLR floats for about 5 µs on every HV entry and exit.** The firmware
  turns one switch off, waits 5 µs, then turns the other on (break-before-make,
  so 9 V never meets the pull-down). During that gap the target's own pull-up
  can lift MCLR slightly. PGC and PGD are held low and the target's oscillator
  start-up takes far longer than 5 µs, so this shouldn't matter, but it hasn't been tested.
- **3.3 V targets only.** The ESP32 pins aren't 5 V tolerant. For LF parts VDD
  is limited to 3.6 V; 3.3 V is fine for both F and LF.
- **The target's MCLR pin must tolerate 9 V.** Don't leave a reset button, a
  voltage supervisor or a reset-IC output directly on MCLR on the target board:
  it would see 9 V. Isolate it with a Schottky diode, as the PICkit 2 guide
  advises. The ICSP header pinout is the standard one: 1 MCLR/VPP, 2 VDD, 3 GND,
  4 PGD, 5 PGC, 6 NC (LVP isn't used).
- Leave the target board's own power unplugged while it's on the jig: the jig
  powers VDD from its 3V3 pin.
- Keep wires short. If connections are flaky, raise `ICSP_HALF_PERIOD` in
  `config.h` (240 CPU cycles, about 1 µs per half clock, is the default; DS41398
  allows much faster).
- No target connected reads as `no target`: PGC and PGD have internal
  pull-downs, so the device ID reads 0x0000.
- The resistor values are mine, not from a tested board: they follow the PICkit 2
  arrangement, with base currents of about 0.5 mA from the 3.3 V GPIOs and
  about 0.8 mA into Q3's base.

## First boot and WiFi

WiFi is only needed to load files and to watch status from a browser.
Flashing works without it.

1. On first boot the jig opens an open WiFi access point named `PIC18Flasher`
   (no password); the screen's bottom line shows `AP: PIC18Flasher`.
2. Join it from a phone or laptop. The setup page should pop up; if not, browse
   to http://192.168.4.1/. Pick your network and enter its password.
   2.4 GHz WPA2-PSK only; WPA2-Enterprise (common on factory networks) isn't supported.
3. The jig joins the network and shows its IP address on the bottom line. Its
   web page is at that IP, or `http://PIC18Flasher.local/` where mDNS works
   (often not on Windows or corporate networks: use the IP).

The jig remembers the network. If that network is out of range at power-up,
the setup AP comes back by itself. To forget it deliberately, **hold BOOT for
5 seconds** while the jig is running: the screen shows `wifi forgotten` and the
jig restarts into the setup AP. (Don't hold BOOT while powering up: that puts
the ESP32-S3 into its USB download mode instead.)

More than one jig on the same network: give each a unique name by changing
`PROJECTNAME` in `TDisplayS3-PIC18K22-Flasher.ino` (e.g. `PIC18Flasher-2`)
before building. Otherwise their hostnames and setup APs collide.

## The script

The jig runs a text script from its own flash (FFat) on each board. On first
boot it writes this default `/flash.txt` if there isn't one:

```
read   userid  userid-{id}.hex      # chip erase wipes the user ID, so save it first
read   eeprom  settings-{id}.hex
erase  chip
write  firmware.hex
write  userid-{id}.hex              # after firmware, so this board's ID wins
write  settings-{id}.hex
verify firmware.hex
verify userid-{id}.hex
verify settings-{id}.hex
reset
```

`flash.txt` runs when a board is inserted (auto mode) or when KEY is pressed
(manual mode). Any `*.txt` on the jig can also be started from the web page
(**Run**) or with `POST /run`. Edit `flash.txt` on the web page, or upload a
replacement.

One command per line. `#` starts a comment, blank lines are ignored, tokens
are separated by spaces or tabs. File names are 1-31 characters from
`A-Z a-z 0-9 . _ -`, no folders.

| Command | Effect |
|---|---|
| `read <flash\|userid\|config\|eeprom> <file>` | Read that whole region of the target and save it as Intel HEX on the jig (overwrites). |
| `erase chip` | Bulk erase: flash, user ID, config, data EEPROM and code protection. |
| `erase eeprom` | Bulk erase of the data EEPROM only. |
| `write <file>` | Program every byte the HEX file contains, each into the region its address falls in. Doesn't erase first. |
| `verify <file>` | Read back every byte the HEX file contains and compare. |
| `reset` | Leave programming mode and release MCLR so the target runs. (The jig does this at the end of every script anyway.) |

Rules:

- **`{id}`** in a file name expands to the board's 8 user-ID bytes (0x200000
  first) as 16 uppercase hex digits. It is read **once, when the script starts**,
  before any command runs, so it still works after `erase chip`. The K22 has
  no factory serial number: the user ID is the board's identity, programmed by
  your firmware build or with MPLAB. The jig doesn't assign IDs.
- If any line uses `{id}` and the board's user ID is blank (all 0xFF), the
  script stops before touching the chip: orange `blank userid`. Otherwise all
  unnumbered boards would share one settings file.
- The whole script is checked before the first command runs: a syntax error
  anywhere stops it with the line number, and every file a `write` or `verify`
  names must exist on the jig or be saved by an earlier `read` in the same
  script. A missing file never silently skips a step.
- `read eeprom` on a board with EEPROM read protection set (`CPD` = 0 in
  CONFIG5H) stops the script: orange `EE locked`. The erase that follows
  would otherwise destroy settings the jig couldn't save.
- `verify` compares only the bytes the file contains. For the config words it
  compares only the implemented bits (unimplemented bits read back as 0).
- `erase chip` resets the config words to their defaults. To restore them,
  `firmware.hex` has to contain them (MPLAB X and XC8 hex files do).
- `write` programs flash and user ID in rows (64 bytes; 8 for the user ID), so
  it needs erased cells: run `erase chip` first, or the `verify` will report
  what didn't take. EEPROM bytes erase themselves as they're written.
- A HEX record outside the detected part's regions is an error ("0x...
  outside chip"). It is found when the `write` or `verify` line runs, which in the
  default script is **after** `erase chip`: check the file against your part
  before using it on boards.
- Only HEX record types 00, 01 and 04 are accepted.
- The script stops at the first error. Every stop is reported with its line
  number on screen and in the serial log.

### What the default script does to a board

1. Saves the user ID and the data EEPROM to `userid-<id>.hex` and
   `settings-<id>.hex` on the jig (the first time a board is seen this
   creates them; afterwards they are rewritten from the board each run).
2. Erases the whole chip.
3. Writes `firmware.hex`, then the saved user ID, then the saved settings.
4. Reads everything back and compares.
5. Releases the target, which runs the new firmware.

Writing the EEPROM takes at least 4 ms per byte (256 bytes about 1 s, 1 KB about
4 s), which dominates the run time on EEPROM-heavy boards. That figure is from
the timing constants in `icsp.h`, not a measurement.

## Making the files

- **`firmware.hex`** - your application as Intel HEX, from MPLAB X / XC8 or any
  tool that writes `.hex`. Include the config words if the board needs
  non-default ones. Upload it with that exact name (or edit `flash.txt` to
  use another).
- **Settings and user ID files** are made by the jig. A board with a blank user
  ID must have its ID programmed once (MPLAB with a PICkit, or a first-stage
  firmware build) before it can use the default script.
- The default script always overwrites `settings-<id>.hex` with what the board
  holds. To seed boards with EEPROM contents of your own, upload that file under
  the board's name and remove the `read eeprom` line from the script.

## Loading files onto the jig

Any of:

- The jig's web page: **Files** → choose or drop a file → **Upload**.
- `curl -F file=@firmware.hex http://<jig-ip>/file`
- `make file F=firmware.hex ip=<jig-ip>`

The file is stored under its own name, replaces any file of that name, and
survives power cycles and jig firmware updates. **Uploading never runs
anything.** A board already on the jig is not touched until the next insertion,
a KEY press (manual mode), or **Run**.

The Files list on the web page also downloads and deletes files; `GET /file?name=`
and `DELETE /file?name=` do the same. Download `settings-<id>.hex` and
`userid-<id>.hex` to inspect or archive a board's saved state.

## Production operation

### Operator steps

1. Power the jig. The home screen shows the board status, the board's user ID,
   the counters and the IP address.
2. Connect a board. Within about a second the jig detects it, shows the part
   name and user ID, then switches to the flash screen: the script name, the
   part, and the current script line.
3. Read the result bar, then disconnect the board. The result also stays on
   the home screen until the board is removed.
4. Connect the next board.

The jig runs the script on each board **once per connection**. A board left
connected is never rerun, whatever its result. To retry, disconnect and
reconnect it.

While idle, the jig probes for a board once a second: it enters HV programming
mode, reads the device ID and the user ID, and releases the target. **A board
left on the jig is therefore reset about twice a second** (once per HV session)
and runs only between probes: don't run functional tests while it's connected.

### Screen messages

| Screen | Meaning | What to do |
|---|---|---|
| `no target` | Nothing answered (device ID read 0x0000 or 0xFFFF). | Check the connection, power, MCLR circuit and 9 V. |
| `PIC18F46K22` (or another part name) | Board detected. | - |
| `id 0123456789ABCDEF` | The board's user ID: what `{id}` expands to. | - |
| `id blank` | User ID is all 0xFF. | Program an ID, or the default script stops with `blank userid`. |
| `unknown 0x5400` | A device ID that isn't in the table, shown with its revision bits. | Not a supported part; no script runs on it. |
| green `PASS` | Every script line ran; every `verify` matched. The board was released and is running. | Good board. |
| red `FAIL` with `verify 0x000104` | A `verify` found a mismatch; the address is on the line above. | Reject. A board that FAILs twice is bad. |
| orange `comm error no verdict` | Three attempts couldn't finish (no part answering at the start of a run, or an EEPROM write that never completed). | Reconnect once. If it repeats, set it aside and check the serial log. |
| orange `unknown part skipped` | The device ID isn't in the table. | Wrong part. |
| orange `blank userid` (`line N`) | A line uses `{id}` and the board's user ID is blank. Nothing was changed. | Program a user ID. |
| orange `EE locked` (`line N`) | `read eeprom` on a board with EEPROM read protection. Nothing was erased. | Unprotect it, or edit the script. |
| orange `syntax` / `no firmware.hex` (`line N`) | Script error or missing file; found before the chip was touched. | Fix the script or upload the file. |
| orange `0x...... outside chip` / `bad checksum` (`line N`) | A HEX file problem, found when that line ran. | Fix the file. See the warning in [The script](#the-script). |
| orange `no script flash.txt` | The script isn't on the jig. | Upload one. |
| `no target, check wiring` | Manual mode: KEY pressed with nothing connected. | - |
| `AP: PIC18Flasher` / `wifi: connecting` | Not on WiFi (bottom line). | Only matters for uploads; see [WiFi](#first-boot-and-wifi). |
| `updating flasher` | Jig firmware update in progress. | Wait; it restarts. |

Messages longer than 18 characters are cut on screen; the serial log has the
full text. FAIL means one thing only: a `verify` mismatch. Script errors (the
orange notices) are never FAIL and aren't retried.

**A communication failure in the middle of a run can look like FAIL.** ICSP has
no acknowledge: the jig clocks commands out and reads data back, but nothing
tells it a command was lost. A board that drops off during the write usually
reads back wrong and FAILs.

**If a run stops after `erase chip`** (power loss, board pulled, `comm error`
on the retry), the board's user ID and EEPROM are wiped, and rerunning stops at
`blank userid` because the ID is gone. The jig still has `userid-<id>.hex` and
`settings-<id>.hex` from the start of the run. Program the user ID back with
MPLAB (download the file from the web page), then rerun.

### Counters, buttons and mode

- `pass` and `fail` count script results since power-up, not boards: a board
  reconnected and run again counts again. They reset on power-cycle;
  record them first. `comm error` and the orange script errors count as neither.
- Auto mode (the default) runs `flash.txt` on every newly connected board. The
  mode isn't shown on the screen; the web page shows it. A short press of BOOT
  switches to manual mode, where nothing runs until KEY (GPIO14) is pressed. If
  boards are detected but never run, the jig is in manual mode: press BOOT
  once or power-cycle (it always starts in auto).
- KEY during the 1 s result display reruns the same board, in either mode.
- BOOT held 5 s forgets WiFi ([above](#first-boot-and-wifi)).

## Settings are per board

Each board's EEPROM settings live in `settings-<id>.hex` on the jig, named by
the board's own user ID. A script must only write a board's settings file
to a board with the same `{id}`: the default script guarantees that because
`{id}` is read from the board itself. If you write your own script, keep the
`settings-{id}.hex` name pattern; never hard-code another board's ID, and never
copy a settings file between boards.

## Supported targets and limits

The 16 parts the jig knows, by device ID (the ID register with its 5 revision bits masked):

| Parts | Device ID: F / LF | Program flash | Data EEPROM |
|---|---|---|---|
| PIC18(L)F23K22 | 0x5740 / 0x5760 | 8 KB | 256 B |
| PIC18(L)F43K22 | 0x5700 / 0x5720 | 8 KB | 256 B |
| PIC18(L)F24K22 | 0x5640 / 0x5660 | 16 KB | 256 B |
| PIC18(L)F44K22 | 0x5600 / 0x5620 | 16 KB | 256 B |
| PIC18(L)F25K22 | 0x5540 / 0x5560 | 32 KB | 256 B |
| PIC18(L)F45K22 | 0x5500 / 0x5520 | 32 KB | 256 B |
| PIC18(L)F26K22 | 0x5440 / 0x5460 | 64 KB | 1 KB |
| PIC18(L)F46K22 | 0x5400 / 0x5420 | 64 KB | 1 KB |

- The table is in `pic18k22.cpp`. Device IDs are from DS41398 Table 5-2 and sizes
  from the MPLAB X PIC18F-K_DFP 1.14.301 device files.
- **HV ICSP only.** Low-voltage programming needs both `LVP` = ON and
  `MCLRE` = EXTMCLR, and field firmware often turns one of them off, so the jig
  doesn't use it.
- **3.3 V targets only.** 5 V targets are out of scope.
- Other PIC families, a jig-assigned serial number, authentication and
  per-board logging beyond the saved settings files are out of scope.
- Regions the jig can read and write: flash, user ID (8 bytes at 0x200000),
  config (14 bytes at 0x300000, implemented bits only), data EEPROM (0xF00000).
- PASS checks what the script's `verify` lines check. The board isn't
  functionally tested.

## Updating the jig

The files on the jig and the WiFi settings survive a jig update.

- **Web page**: **Flasher firmware** → choose `TDisplayS3-PIC18K22-Flasher.ino.bin`
  from the build folder (not the `.merged.bin` or `.bootloader.bin`) → **Update flasher**.
  The page reports when the jig is back on the new firmware.
- **curl**: `curl -F firmware=@TDisplayS3-PIC18K22-Flasher.ino.bin http://<jig-ip>/update`
- **ArduinoOTA**: `make ota ip=<jig-ip>` (uses espota.py from the esp32 core; set
  `ESPOTA=` if yours lives elsewhere).
- **USB**: `make usb port=/dev/ttyACM0`.

ArduinoOTA from WSL2: espota needs the jig to connect back to the PC. Set
`networkingMode=mirrored` in `%USERPROFILE%\.wslconfig` (then `wsl --shutdown`),
allow inbound TCP 3233 in both the Windows firewall and the Hyper-V firewall
for WSL, and add `-P 3233` to the espota command. The web page method avoids all of this.

## Troubleshooting

The jig logs to USB serial at 115200 baud
(`arduino-cli monitor -p /dev/ttyACM0 -c baudrate=115200`). The wire layer
itself logs nothing: ICSP gives it nothing to report.

| Log line | Meaning |
|---|---|
| `wrote default /flash.txt` | First boot (or no script): the default script was created. |
| `detected: PIC18F46K22 0123456789ABCDEF` | Probe result changed (part name, then the user ID or `blank`). `detected: no target` when the board goes away. |
| `PASS: done` | Every script line ran. |
| `script: FAIL verify 0x000104 (line 7)` | The FAIL detail: address and script line. |
| `script: blank userid (line 1)` | An orange script error: the message and the line. Same form for `syntax`, `no <file>`, `EE locked` and the HEX errors. |
| `script: no part, comm error, retrying` | The probe saw a part, but at the start of the run nothing answered. |
| `script: comm error at line N, retrying` | A command failed (an EEPROM write didn't finish). |
| `flash: comm error, attempt n/3` | Retrying the whole script within one connection. |
| `manual: no target answered` / `run: no target answered` | KEY or **Run** with no board. |
| `uploaded <file>` | A file arrived over the web page / `/file`. |
| `mode: auto` / `mode: manual` | BOOT short press. |

`no target` with a board connected: check VDD (3V3 at ICSP pin 2), PGD/PGC
continuity, and that MCLR actually reaches the target (about 9 V on it while a
script runs). If MCLR sees 9 V while the jig is still booting, check R4.

Boards that show a part name but fail the verify: weak connections (raise
`ICSP_HALF_PERIOD`), a 3.3 V rail that sags under the board's load, or
code-protected flash/EEPROM from a previous life (`erase chip` clears code protection).

## Integration and security

- `GET /status` returns JSON, e.g.
  `{"detected":"PIC18F46K22","id":"0123456789ABCDEF","pass":12,"fail":0,"auto":true,"last":"PASS","build":"...","md5":"..."}`.
  `id` is the seated board's user ID (16 hex digits, `blank`, or empty when no
  board answers); `last` is the last result word until the board is removed;
  `md5` identifies the jig firmware. There's no per-board log or serial-number
  traceability beyond the saved files.
- `GET /files` lists the files on the jig (JSON name and size).
  `GET /file?name=X` downloads, `POST /file` (multipart upload, any
  valid name) stores, `DELETE /file?name=X` deletes.
  `POST /run?name=X.txt` runs a script on the seated board (404 if the file is
  missing, 409 if no board answers). A bad name gets 400.
- `POST /update` replaces the jig firmware.
- **Nothing is authenticated**: the setup AP, every endpoint above and
  ArduinoOTA are open to anyone on the same network. Anyone who can reach the
  jig can read or delete the saved settings, replace `firmware.hex` or the
  script, run scripts on a seated board, or replace the jig firmware. Put jigs
  on an isolated network.

## Status

- Builds with arduino-cli; `make test` passes (HEX parser, script runner
  against a fake chip, the 16-part table).
- The ICSP timing constants and command values were checked by the implementer
  against DS41398B (the programming specification) during development; P10 is
  200 µs per the document. No second person has re-checked them.
- **Not yet run on hardware.** On a known K22 board, bring it up in this order:
  1. The probe shows the right part name and user ID.
  2. Load known EEPROM contents and a user ID with MPLAB, run `flash.txt`, and
     confirm in MPLAB that the EEPROM and user ID survived and the new firmware runs.
  3. A board with a blank user ID shows `blank userid` and is untouched.

## Design

See [docs/architecture.md](docs/architecture.md) for how the ICSP driver, the
device layer and the script runner work, and [docs/design.md](docs/design.md)
for the original spec.

## License

MIT - see [LICENSE](LICENSE).
