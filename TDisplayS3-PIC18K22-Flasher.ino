#include <WiFiClient.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <FFat.h>
#include <Update.h>
#include <TFT_eSPI.h>

#include "config.h"
#include "icsp.h"
#include "pic18k22.h"
#include "script.h"
#include "webpage.h"

#define PROJECTNAME "PIC18Flasher"
#define RESULT_HOLD_MS 1000   // pass/fail bar hold on the flash page

TFT_eSPI tft = TFT_eSPI();
WiFiManager wifiManager;
WebServer server(80);

// Scripts, HEX files and saved settings live in FFat; survive reboots.
#define DEFAULT_SCRIPT "flash.txt"   // run on insertion / KEY
static const char *UPLOAD_TMP = "/upload.tmp";
static File uploadFile;
static char uploadName[32];     // validated filename of the upload in progress
static int uploadCode = 400;    // 0 = upload is good; else the HTTP error the done handler sends
static const char *uploadWhy = "No file in the request\n";

static const char DEFAULT_FLASH_TXT[] =
  "read   userid  userid-{id}.hex      # chip erase wipes the user ID, so save it first\n"
  "read   eeprom  settings-{id}.hex\n"
  "erase  chip\n"
  "write  firmware.hex\n"
  "verify firmware.hex                 # before the settings overwrite any EEPROM data it carries\n"
  "write  settings-{id}.hex\n"
  "verify settings-{id}.hex\n"
  "write  userid-{id}.hex              # last: an interrupted run leaves the ID blank, so the rerun stops\n"
  "verify userid-{id}.hex\n"
  "reset\n";

static uint32_t passCount = 0, failCount = 0;

// Auto mode flashes once per insertion: set after every attempt (PASS, FAIL,
// skipped, or comm error), cleared after MISSES_GONE "no target" probes in a row,
// or when a different user ID than flashedId shows on two probes in a row (a board
// swapped faster than the miss rule sees). flashedId is the ID the run started on
// (no post-run probe: one bad read there would reflash a seated board). A run that
// changes the ID (blank after an interruption) gets one more run, which stops at
// "blank userid" without touching the chip.
// A comm-error board is NOT retried while it stays seated: every retry rewrites flash.
static bool flashed = false;
#define MISSES_GONE 3   // ~3 s: one bad probe (bounced pogo pin) must not reflash a seated board
static char flashedId[17] = "";   // user ID the last run started on
static char newId[17] = "";       // a boardId seen once; a second sighting makes a new board

// Last verdict, shown on the home screen until the board is removed.
static char lastResult[16] = "";
static uint16_t lastBg = TFT_BLACK;

// WiFi is secondary: the jig flashes with or without it. WiFiManager runs
// non-blocking; OTA + mDNS start whenever a connection eventually appears.
static bool netUp = false;

static uint32_t lastPoll = 0;
static bool autoMode = false;  // manual by default: runs only from FLASH_BUTTON or POST /run, no probing; MODE_BUTTON toggles
static char detected[32] = "no target";   // live probe result, home page line 1
static char boardId[17] = "";             // user ID of the seated board: 16 hex, "blank", or "" (unknown)

// Home page: status + waiting for the next target.
static void showHome() {
  tft.fillScreen(TFT_BLACK);
  tft.setCursor(0, 0, 2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.println(detected);                    // live detection
  if (boardId[0]) {
    char id[24];
    snprintf(id, sizeof(id), "id %s", boardId);
    if (tft.textWidth(id, 2) > tft.width()) tft.setTextSize(1);   // 19 chars may not fit at size 2
    tft.println(id);
    tft.setTextSize(2);
  } else tft.println();
  tft.printf("pass %lu fail %lu\n", passCount, failCount);
  if (lastResult[0]) {
    tft.setTextColor(TFT_WHITE, lastBg);
    tft.printf(" %s ", lastResult);
  }
  tft.setCursor(0, 4 * 32, 2);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  if (WiFi.status() == WL_CONNECTED) tft.print(WiFi.localIP());
  else tft.print(wifiManager.getConfigPortalActive() ? "AP: " PROJECTNAME : "wifi: connecting");

  // ponytail: mode letter hidden — the enclosure has no button access, so the
  // jig always runs auto. Toggle code stays; re-enable this if buttons return.
  // tft.setTextColor(TFT_CYAN, TFT_BLACK);
  // tft.setTextDatum(TR_DATUM);
  // tft.drawString(autoMode ? "A" : "M", tft.width() - 6, 0, 2);   // enclosure covers the right edge
  // tft.setTextDatum(TL_DATUM);
}

static bool wantRetest = false;
static bool wantRun = false;      // POST /run: loop() runs runName on the seated board
static char runName[32] = "";

// Debounced release: returns once the pin has read high for 50 ms straight.
// Every button-triggered test starts from here, so a single press can never
// span into the next page and fire twice.
static void waitRelease(uint8_t pin) {
  uint32_t hi = millis();
  while (millis() - hi < 50) {
    if (!digitalRead(pin)) hi = millis();
    delay(5);
  }
}

// Result line at the bottom of the flash page: hold RESULT_HOLD_MS, then home.
// FLASH_BUTTON during the hold cuts it short and requests a retest.
static void showResult(const char *word, uint16_t bg, const char *stage) {
  tft.setCursor(0, 4 * 32, 2);
  tft.setTextColor(TFT_WHITE, bg);
  tft.printf(" %s %s ", word, stage);
  Serial.printf("%s: %s\n", word, stage);
  strlcpy(lastResult, word, sizeof(lastResult));
  // Script messages can carry any token from the script ("no foo\"bar"): keep /status valid JSON.
  for (char *c = lastResult; *c; c++)
    if (*c == '"' || *c == '\\' || (unsigned char)*c < 0x20 || *c == 0x7F) *c = '?';
  lastBg = bg;
  uint32_t start = millis();
  while (millis() - start < RESULT_HOLD_MS) {
    if (!digitalRead(FLASH_BUTTON)) {
      waitRelease(FLASH_BUTTON);
      wantRetest = true;
      return;
    }
    delay(10);
  }
}

// File names are [A-Za-z0-9._-]{1,31}: no paths, so nothing escapes FFat's root.
static bool validName(const char *n) {
  size_t len = strlen(n);
  if (len < 1 || len > 31 || !strcmp(n, ".") || !strcmp(n, "..")) return false;
  for (; *n; n++)
    if (!isalnum((unsigned char)*n) && *n != '.' && *n != '_' && *n != '-') return false;
  return true;
}

// fs_ops for the script runner. Names come from script text, so they get the same check.
static char *fsLoad(const char *name) {
  if (!validName(name)) return nullptr;
  char path[40];
  snprintf(path, sizeof(path), "/%s", name);
  File f = FFat.open(path);
  if (!f || f.isDirectory()) return nullptr;
  size_t sz = f.size();
  char *buf = (char *)malloc(sz + 1);
  if (!buf) { f.close(); return nullptr; }
  size_t got = f.read((uint8_t *)buf, sz);
  f.close();
  buf[got] = 0;
  return buf;
}

static bool fsSave(const char *name, const char *text, size_t len) {
  if (!validName(name)) return false;
  char path[40];
  snprintf(path, sizeof(path), "/%s", name);
  File f = FFat.open(path, FILE_WRITE);
  if (!f) return false;
  size_t put = f.write((const uint8_t *)text, len);
  f.close();
  return put == len;
}

static const fs_ops fatfs = {fsLoad, fsSave};

static void handleRoot() {
  server.send(200, "text/html", PAGE);
}

static void handleStatus() {
  static String md5 = ESP.getSketchMD5();   // lets the page confirm a firmware update took
  char json[384];
  snprintf(json, sizeof(json),
    "{\"detected\":\"%s\",\"id\":\"%s\",\"pass\":%lu,\"fail\":%lu,\"auto\":%s,\"last\":\"%s\","
    "\"build\":\"" __DATE__ " " __TIME__ "\",\"md5\":\"%s\"}",
    detected, boardId, passCount, failCount, autoMode ? "true" : "false", lastResult, md5.c_str());
  server.send(200, "application/json", json);
}

// Flasher self-update: stream the .ino.bin into the spare OTA slot. Update
// rejects anything not starting with the ESP image magic byte.
static void handleUpdateChunk() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    tft.fillScreen(TFT_BLACK);
    tft.setCursor(0, 0, 2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.println("updating flasher");
    Serial.printf("firmware update: %s\n", up.filename.c_str());
    Update.begin(UPDATE_SIZE_UNKNOWN);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    Update.write(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END) {
    Update.end(true);
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
  }
}

static void handleUpdateDone() {
  if (Update.hasError() || !Update.isFinished()) {
    char msg[96];
    snprintf(msg, sizeof(msg), "Update failed: %s. The flasher kept its current firmware.",
             Update.errorString());
    Serial.println(msg);
    server.send(500, "text/plain", msg);
    Update.abort();
    showHome();
    return;
  }
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", "ok: restarting\n");
  Serial.println("firmware update ok, restarting");
  delay(300);
  ESP.restart();
}

// Returns the validated ?name= as an FFat path, or sends 400 and returns false.
static bool nameArg(char *path, size_t n) {
  String name = server.arg("name");
  if (!validName(name.c_str())) {
    server.send(400, "text/plain", "Bad file name: use 1-31 characters from A-Z a-z 0-9 . _ -\n");
    return false;
  }
  snprintf(path, n, "/%s", name.c_str());
  return true;
}

static void handleFiles() {
  String json = "[";
  File root = FFat.open("/");
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    const char *name = f.name();
    if (f.isDirectory() || !validName(name)) continue;   // hides anything the API couldn't address
    if (json.length() > 1) json += ',';
    json += "{\"name\":\"";
    json += name;
    json += "\",\"size\":";
    json += (unsigned)f.size();
    json += '}';
  }
  json += ']';
  server.send(200, "application/json", json);
}

static void handleFileGet() {
  char path[40];
  if (!nameArg(path, sizeof(path))) return;
  File f = FFat.open(path);
  if (!f || f.isDirectory()) { server.send(404, "text/plain", "No such file\n"); return; }
  server.sendHeader("Content-Disposition", String("attachment; filename=\"") + (path + 1) + "\"");   // validated name
  server.streamFile(f, "application/octet-stream");
  f.close();
}

static void handleFileDelete() {
  char path[40];
  if (!nameArg(path, sizeof(path))) return;
  if (!FFat.exists(path)) { server.send(404, "text/plain", "No such file\n"); return; }
  if (!FFat.remove(path)) { server.send(500, "text/plain", "Delete failed\n"); return; }
  server.send(200, "text/plain", "ok\n");
}

// Multipart upload: stream to /upload.tmp, rename to the field's filename in the
// done handler only when the whole body arrived. Never runs anything.
static void handleUploadChunk() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    if (!validName(up.filename.c_str()) || up.filename == "upload.tmp") {
      uploadCode = 400;
      uploadWhy = "Bad file name: use 1-31 characters from A-Z a-z 0-9 . _ -\n";
      return;
    }
    strlcpy(uploadName, up.filename.c_str(), sizeof(uploadName));
    uploadFile = FFat.open(UPLOAD_TMP, FILE_WRITE);
    uploadCode = uploadFile ? 0 : 500;
    uploadWhy = "Can't create the temporary file\n";
  } else if (up.status == UPLOAD_FILE_WRITE && !uploadCode) {
    if (uploadFile.write(up.buf, up.currentSize) != up.currentSize) {
      uploadCode = 500;
      uploadWhy = "Write failed: the flasher's filesystem is probably full\n";
    }
  } else if (up.status == UPLOAD_FILE_END || up.status == UPLOAD_FILE_ABORTED) {
    if (uploadFile) uploadFile.close();
    if (up.status == UPLOAD_FILE_ABORTED) { uploadCode = 500; uploadWhy = "Upload aborted\n"; }
  }
}

static void handleUploadDone() {
  if (uploadCode) {
    FFat.remove(UPLOAD_TMP);
    server.send(uploadCode, "text/plain", uploadWhy);
    uploadCode = 400; uploadWhy = "No file in the request\n";   // for the next request
    return;
  }
  char path[40];
  snprintf(path, sizeof(path), "/%s", uploadName);
  FFat.remove(path);
  bool ok = FFat.rename(UPLOAD_TMP, path);
  uploadCode = 400; uploadWhy = "No file in the request\n";   // a request with no file part must not reuse this one
  if (!ok) {
    FFat.remove(UPLOAD_TMP);
    server.send(500, "text/plain", "Rename failed\n");
    return;
  }
  server.send(200, "text/plain", String("ok: ") + uploadName + "\n");
  Serial.printf("uploaded %s\n", uploadName);
}

static bool probe();   // below

// Run a script on the seated board: the loop picks it up (wantRun) after this answers.
static void handleRun() {
  char path[40];
  if (!nameArg(path, sizeof(path))) return;
  size_t len = strlen(path);
  if (len < 5 || strcmp(path + len - 4, ".txt")) { server.send(400, "text/plain", "Only .txt scripts run\n"); return; }
  if (!FFat.exists(path)) { server.send(404, "text/plain", "No such file\n"); return; }
  if (!probe()) { server.send(409, "text/plain", "No board on the jig\n"); return; }   // safe: no script is running
  strlcpy(runName, path + 1, sizeof(runName));
  wantRun = true;
  server.send(200, "text/plain", "ok: running\n");
}

// Programming page: white on black, one line per phase, updated in place.
static const int LINE_H = 32;   // font 2 (16 px) at textSize 2

// Current script line on the flash page; text may carry a trailing # comment.
static void progress(int line, const char *text) {
  char cmd[24];
  snprintf(cmd, sizeof(cmd), "%d %s", line, text);
  char *hash = strchr(cmd, '#');
  if (hash) *hash = 0;
  tft.setCursor(0, 2 * LINE_H, 2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.printf("%-19.19s", cmd);
}

// Detail line (line 3) under the progress line.
static void note(const char *s, uint16_t fg) {
  tft.setCursor(0, 3 * LINE_H, 2);
  tft.setTextColor(fg, TFT_BLACK);
  tft.printf("%-19.19s", s);
}

// Target is present: run one script. Blocking is fine.
// Returns false on a communication hiccup: no verdict, caller just retries.
// FAIL means one thing only: a verify readback didn't match.
static bool runScript(const char *name) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(0, 0, 2);
  tft.print(name);
  tft.setCursor(0, LINE_H, 2);
  tft.print(detected);

  // Boards the jig won't touch get an orange notice, never a FAIL.
  if (!pic_part_detected()) {
    if (!strncmp(detected, "unknown", 7)) {
      showResult("unknown part", TFT_ORANGE, "skipped");
      return true;
    }
    Serial.println("script: no part, comm error, retrying");
    return false;
  }
  char *text = fsLoad(name);
  if (!text) {
    Serial.printf("script: no %s\n", name);
    showResult("no script", TFT_ORANGE, name);
    return true;
  }
  script_status st = script_run(text, &pic_dev, &fatfs, progress);
  free(text);
  pic_dev.reset();   // an error leaves programming mode open; this also releases the target

  char where[16] = "";
  if (st.line) snprintf(where, sizeof(where), "line %d", st.line);
  switch (st.result) {
    case SCRIPT_COMM:
      Serial.printf("script: comm error at line %d, retrying\n", st.line);
      return false;
    case SCRIPT_PASS:
      passCount++;
      showResult("PASS", TFT_DARKGREEN, "done");
      break;
    case SCRIPT_FAIL:
      failCount++;
      Serial.printf("script: FAIL %s (%s)\n", st.msg, where);
      note(st.msg, TFT_WHITE);
      showResult("FAIL", TFT_RED, "");
      break;
    default: {   // SCRIPT_ERROR: orange notice, no retry, not a FAIL
      Serial.printf("script: %s (%s)\n", st.msg, where);
      char word[19];
      strlcpy(word, st.msg, sizeof(word));
      note(where, TFT_ORANGE);
      showResult(word, TFT_ORANGE, "");
    }
  }
  return true;
}

// One verdicted run: up to 3 runScript attempts (each re-enters ICSP) on comm
// errors. Each retry re-probes first: a board that has gone gets an orange
// "no target" instead of more runs against nothing. If no attempt reaches a
// verdict, show an orange notice — NOT a red FAIL (that's reserved for verify
// mismatch) — instead of silently going home.
static void flashWithRetry(const char *name) {
  for (int attempt = 1; attempt <= 3; attempt++) {
    if (attempt > 1 && !probe()) {
      Serial.println("flash: target gone, not retrying");
      showResult("no target", TFT_ORANGE, "removed");
      return;
    }
    if (runScript(name)) return;
    Serial.printf("flash: comm error, attempt %d/3\n", attempt);
  }
  showResult("comm error", TFT_ORANGE, "no verdict");
}

// Run, honor retest presses during the result hold, then home.
static void runTests(const char *name) {
  char startId[sizeof(flashedId)];
  strlcpy(startId, boardId, sizeof(startId));   // from the probe that started this run
  flashWithRetry(name);
  while (wantRetest) {
    wantRetest = false;
    flashWithRetry(name);
  }
  flashed = true;   // whatever the outcome: no retry until the board is reseated
  strlcpy(flashedId, startId, sizeof(flashedId));
  newId[0] = 0;
  showHome();
}

// Refresh `detected` and `boardId`; true if a target answered. Only call between
// script runs: pic_probe() ends any open ICSP session.
static bool probe() {
  uint8_t u[8];
  strlcpy(detected, pic_probe(u), sizeof(detected));   // one HV session: device ID + user ID
  boardId[0] = 0;
  if (pic_part_detected()) {
    bool blank = true;
    for (int i = 0; i < 8; i++) {
      snprintf(boardId + 2 * i, 3, "%02X", u[i]);
      if (u[i] != 0xFF) blank = false;
    }
    if (blank) strcpy(boardId, "blank");
  }
  return strcmp(detected, "no target") != 0;
}

static void maybeStartNetwork() {
  if (netUp || WiFi.status() != WL_CONNECTED) return;
  netUp = true;
  MDNS.begin(PROJECTNAME);                 // PIC18Flasher.local
  ArduinoOTA.setHostname(PROJECTNAME);
  ArduinoOTA.begin();
  server.begin();
  MDNS.addService("http", "tcp", 80);
  Serial.printf("%s @ ", PROJECTNAME);
  Serial.println(WiFi.localIP());
  showHome();                           // refresh the wifi status line
}

void setup() {
  icsp_init();   // first: target held in reset (and HV: VPP_ON off) before anything else runs
  Serial.begin(115200);

  pinMode(MODE_BUTTON, INPUT_PULLUP);
  pinMode(FLASH_BUTTON, INPUT_PULLUP);

  pinMode(15, OUTPUT);        // T-Display S3: LCD power enable, required on battery
  digitalWrite(15, HIGH);

  tft.init();
  tft.setRotation(3);

  wifiManager.setConfigPortalBlocking(false);
  wifiManager.autoConnect(PROJECTNAME);    // returns immediately; flashing never waits on wifi

  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/files", HTTP_GET, handleFiles);
  server.on("/file", HTTP_GET, handleFileGet);
  server.on("/file", HTTP_DELETE, handleFileDelete);
  server.on("/file", HTTP_POST, handleUploadDone, handleUploadChunk);
  server.on("/run", HTTP_POST, handleRun);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateChunk);

  FFat.begin(true);                        // format on first boot
  if (!FFat.exists("/" DEFAULT_SCRIPT)) {
    File f = FFat.open("/" DEFAULT_SCRIPT, FILE_WRITE);
    if (f) {
      f.write((const uint8_t *)DEFAULT_FLASH_TXT, sizeof(DEFAULT_FLASH_TXT) - 1);
      f.close();
      Serial.println("wrote default /" DEFAULT_SCRIPT);
    }
  }

  probe();   // once, for home line 1; it also releases the target (manual mode doesn't poll)
  showHome();
}

void loop() {
  wifiManager.process();
  maybeStartNetwork();
  if (netUp) {
    ArduinoOTA.handle();
    server.handleClient();
  }

  // POST /run: run the named script now, same as a manual button press. handleRun
  // already saw a target; a board pulled since is not a FAIL — just log.
  if (wantRun) {
    wantRun = false;
    if (probe()) runTests(runName);
    else Serial.println("run: no target answered");
    return;
  }

  // Buttons idle high. KEY: press edge, 200 ms lockout. BOOT acts on hold
  // time: a short press (< 1 s) toggles auto/manual on release; held 5 s it
  // forgets WiFi and reboots into the setup AP. (BOOT can't be read at
  // power-up: holding it then puts the ESP32-S3 into ROM download mode.)
  static bool flashPrev = HIGH;
  static uint32_t lastEdge = 0, modeDown = 0;
  bool flashNow = digitalRead(FLASH_BUTTON);
  bool flashPressed = flashNow == LOW && flashPrev == HIGH && millis() - lastEdge >= 200;
  flashPrev = flashNow;
  if (flashPressed) lastEdge = millis();

  if (!digitalRead(MODE_BUTTON)) {
    if (!modeDown) modeDown = millis();
    else if (millis() - modeDown > 5000) {
      Serial.println("wifiManager reset");
      tft.fillScreen(TFT_BLACK);
      tft.setCursor(0, 0, 2);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.print("wifi forgotten");
      wifiManager.resetSettings();
      delay(1000);
      ESP.restart();
    }
  } else if (modeDown) {
    uint32_t held = millis() - modeDown;
    modeDown = 0;
    if (held >= 50 && held < 1000) {
      autoMode = !autoMode;
      Serial.printf("mode: %s\n", autoMode ? "auto" : "manual");
      showHome();
      return;
    }
  }

  // Manual test: press = probe + run flash.txt right now.
  if (!autoMode && flashPressed) {
    waitRelease(FLASH_BUTTON);   // test starts on release, not press
    if (!probe()) {
      Serial.println("manual: no target answered");
      showHome();
      tft.setCursor(0, 3 * 32, 2);
      tft.setTextColor(TFT_RED, TFT_BLACK);
      tft.print("no target, check wiring");
      return;
    }
    runTests(DEFAULT_SCRIPT);
    return;
  }

  // Auto mode only: live detection probe once a second. Each probe holds the
  // target in reset for a few ms, so manual mode doesn't probe and the target
  // runs undisturbed. A newly inserted target rolls straight into flash.txt
  // (once per insertion).
  if (!autoMode || millis() - lastPoll < 1000) return;
  lastPoll = millis();

  char prev[sizeof(detected) + sizeof(boardId)];
  snprintf(prev, sizeof(prev), "%s|%s", detected, boardId);
  static int misses = 0;   // consecutive "no target" probes
  if (!probe()) {
    newId[0] = 0;          // sightings must be consecutive
    if (misses < MISSES_GONE && ++misses == MISSES_GONE) {
      flashed = false;     // next insertion gets flashed
      lastResult[0] = 0;   // verdict was for the board just removed
      showHome();
    }
  } else {
    misses = 0;
    // Same-ID boards still need the 3-miss rule; two in a row so one misread ID can't reflash.
    if (autoMode && flashed && strcmp(boardId, flashedId)) {
      if (!strcmp(boardId, newId)) {
        Serial.printf("new board %s (was %s)\n", boardId, flashedId);
        flashed = false;
        lastResult[0] = 0;
      } else strlcpy(newId, boardId, sizeof(newId));
    } else newId[0] = 0;
    if (autoMode && !flashed) {
      runTests(DEFAULT_SCRIPT);
      return;
    }
  }
  char now[sizeof(prev)];
  snprintf(now, sizeof(now), "%s|%s", detected, boardId);
  if (strcmp(now, prev)) {
    Serial.printf("detected: %s %s\n", detected, boardId);
    showHome();
  }
}
