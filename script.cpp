#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "script.h"

enum cmd_op { OP_NONE, OP_READ, OP_ERASE_CHIP, OP_ERASE_EE, OP_WRITE, OP_VERIFY, OP_RESET };

struct cmd { cmd_op op; int region; const char *arg; };   // arg points into the line buffer

enum { LINE_MAX_LEN = 128, NAME_MAX_LEN = 64 };

static const char *const REGION_WORD[IHX_NREGIONS] = {"flash", "userid", "config", "eeprom"};
static const uint32_t REGION_BASE[IHX_NREGIONS] = {0x000000, 0x200000, 0x300000, 0xF00000};

static script_status status(script_result r, int line, const char *fmt = "", ...) {
  script_status s = {r, line, ""};
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s.msg, sizeof s.msg, fmt, ap);
  va_end(ap);
  return s;
}

// Copies the next script line into buf with the comment, \r and edge blanks removed.
// False at the end of the script; *fit = false if the line was too long for buf.
static bool next_line(const char **p, char *buf, size_t n, bool *fit) {
  if (!**p) return false;
  const char *nl = strchr(*p, '\n');
  size_t len = nl ? (size_t)(nl - *p) : strlen(*p);
  *fit = len < n;
  memcpy(buf, *p, *fit ? len : n - 1);
  buf[*fit ? len : n - 1] = 0;
  *p = nl ? nl + 1 : *p + len;
  char *hash = strchr(buf, '#');
  if (hash) *hash = 0;
  char *e = buf + strlen(buf);
  while (e > buf && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
  char *b = buf;
  while (*b == ' ' || *b == '\t') b++;
  memmove(buf, b, e - b + 1);
  return true;
}

// Splits line in place (tokens are NUL-terminated inside it) and checks the syntax.
static bool parse(char *line, cmd *c) {
  char *t[4];
  int n = 0;
  for (char *p = line; *p; ) {
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) break;
    if (n == 4) return false;
    t[n++] = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) *p++ = 0;
  }
  *c = {OP_NONE, 0, nullptr};
  if (n == 0) return true;
  if (!strcmp(t[0], "read") && n == 3) {
    for (int r = 0; r < IHX_NREGIONS; r++)
      if (!strcmp(t[1], REGION_WORD[r])) { *c = {OP_READ, r, t[2]}; return true; }
  } else if (!strcmp(t[0], "erase") && n == 2) {
    if (!strcmp(t[1], "chip")) { c->op = OP_ERASE_CHIP; return true; }
    if (!strcmp(t[1], "eeprom")) { c->op = OP_ERASE_EE; return true; }
  } else if (!strcmp(t[0], "write") && n == 2) {
    *c = {OP_WRITE, 0, t[1]};
    return true;
  } else if (!strcmp(t[0], "verify") && n == 2) {
    *c = {OP_VERIFY, 0, t[1]};
    return true;
  } else if (!strcmp(t[0], "reset") && n == 1) {
    c->op = OP_RESET;
    return true;
  }
  return false;
}

// Copies src to out, replacing {id}. False if it doesn't fit.
static bool expand(const char *src, const char *id, char *out, size_t n) {
  size_t o = 0, idlen = strlen(id);
  while (*src) {
    const char *rep = nullptr;
    size_t len = 1;
    if (!strncmp(src, "{id}", 4)) { rep = id; len = idlen; }
    if (o + len >= n) return false;
    if (rep) memcpy(out + o, rep, len); else out[o] = *src;
    o += len;
    src += rep ? 4 : 1;
  }
  out[o] = 0;
  return true;
}

struct ctx {
  const pic_ops *dev;
  const fs_ops *fs;
  ihx_image img;
  uint8_t *scratch;   // read-back buffer, big enough for the largest region
};

static bool any_mask(const ihx_region *g) { return g->size && memchr(g->mask, 1, g->size); }

static script_status do_read(ctx *c, int line, const cmd *k, const char *name) {
  const pic_ops *d = c->dev;
  if (k->region == IHX_EEPROM && d->ee_locked()) return status(SCRIPT_ERROR, line, "EE locked");
  ihx_region *g = &c->img.r[k->region];
  if (!d->read(k->region, 0, g->data, g->size)) return status(SCRIPT_COMM, line);
  memset(g->mask, 1, g->size);
  size_t outmax = (size_t)g->size * 3 + 512;   // 44 chars per 16 bytes plus page/EOF records
  char *out = (char *)malloc(outmax);
  if (!out) return status(SCRIPT_ERROR, line, "no memory");
  size_t n = ihx_write(&c->img, k->region, out, outmax);
  bool ok = n && c->fs->save(name, out, n);
  free(out);
  return ok ? status(SCRIPT_PASS, line) : status(SCRIPT_ERROR, line, "save %s", name);
}

// Loads and parses a HEX file into c->img (nothing is written to the chip yet).
static script_status load_hex(ctx *c, int line, const char *name) {
  char *txt = c->fs->load(name);
  if (!txt) return status(SCRIPT_ERROR, line, "no %s", name);
  script_status s = status(SCRIPT_PASS, line);
  if (!ihx_parse(txt, &c->img, s.msg, sizeof s.msg)) s.result = SCRIPT_ERROR;
  free(txt);
  return s;
}

static script_status do_write(ctx *c, int line, const char *name) {
  script_status s = load_hex(c, line, name);
  if (s.result != SCRIPT_PASS) return s;
  for (int r = 0; r < IHX_NREGIONS; r++) {
    ihx_region *g = &c->img.r[r];
    if (any_mask(g) && !c->dev->write(r, 0, g->data, g->mask, g->size)) return status(SCRIPT_COMM, line);
  }
  return s;
}

static script_status do_verify(ctx *c, int line, const char *name) {
  script_status s = load_hex(c, line, name);
  if (s.result != SCRIPT_PASS) return s;
  for (int r = 0; r < IHX_NREGIONS; r++) {
    ihx_region *g = &c->img.r[r];
    if (!any_mask(g)) continue;
    if (!c->dev->read(r, 0, c->scratch, g->size)) return status(SCRIPT_COMM, line);
    for (uint32_t i = 0; i < g->size; i++) {
      if (!g->mask[i]) continue;
      uint8_t diff = g->data[i] ^ c->scratch[i];
      if (r == IHX_CONFIG && i < 14) diff &= c->dev->config_mask[i];   // unimplemented bits don't count
      if (diff) return status(SCRIPT_FAIL, line, "verify 0x%06X", (unsigned)(g->base + i));
    }
  }
  return s;
}

static script_status exec(ctx *c, const char *script, const char *id,
                          void (*progress)(int, const char *)) {
  const char *p = script;
  char raw[LINE_MAX_LEN], line[LINE_MAX_LEN], name[NAME_MAX_LEN];
  bool fit;
  for (int n = 1; next_line(&p, raw, sizeof raw, &fit); n++) {
    cmd k;
    strcpy(line, raw);
    parse(line, &k);   // already validated by script_run
    if (k.op == OP_NONE) continue;
    if (progress) progress(n, raw);
    if (k.arg && !expand(k.arg, id, name, sizeof name)) return status(SCRIPT_ERROR, n, "syntax");
    script_status s = status(SCRIPT_PASS, n);
    switch (k.op) {
      case OP_READ:       s = do_read(c, n, &k, name); break;
      case OP_ERASE_CHIP: if (!c->dev->erase(true)) s = status(SCRIPT_COMM, n); break;
      case OP_ERASE_EE:   if (!c->dev->erase(false)) s = status(SCRIPT_COMM, n); break;
      case OP_WRITE:      s = do_write(c, n, name); break;
      case OP_VERIFY:     s = do_verify(c, n, name); break;
      case OP_RESET:      c->dev->reset(); break;
      default: break;
    }
    if (s.result != SCRIPT_PASS) return s;
  }
  return status(SCRIPT_PASS, 0);
}

// Every write/verify file must exist on fs or be saved by an earlier read in the script,
// so a missing firmware.hex is caught before erase chip rather than after.
static script_status preflight(const char *script, const fs_ops *fs, const char *id) {
  const char *p = script;
  char raw[LINE_MAX_LEN], line[LINE_MAX_LEN], name[NAME_MAX_LEN], prev[NAME_MAX_LEN];
  bool fit;
  for (int n = 1; next_line(&p, raw, sizeof raw, &fit); n++) {
    cmd k;
    strcpy(line, raw);
    parse(line, &k);
    if (k.op != OP_WRITE && k.op != OP_VERIFY) continue;
    if (!expand(k.arg, id, name, sizeof name)) return status(SCRIPT_ERROR, n, "syntax");
    bool made = false;
    const char *q = script;
    for (int m = 1; m < n && next_line(&q, raw, sizeof raw, &fit); m++) {
      cmd j;
      strcpy(line, raw);
      parse(line, &j);
      if (j.op == OP_READ && expand(j.arg, id, prev, sizeof prev) && !strcmp(prev, name)) made = true;
    }
    if (made) continue;
    char *txt = fs->load(name);
    if (!txt) return status(SCRIPT_ERROR, n, "no %s", name);
    free(txt);
  }
  return status(SCRIPT_PASS, 0);
}

script_status script_run(const char *script, const pic_ops *dev, const fs_ops *fs,
                         void (*progress)(int line, const char *text)) {
  // Check the whole script first so a typo on line 9 can't leave the chip erased.
  const char *p = script;
  char raw[LINE_MAX_LEN], line[LINE_MAX_LEN];
  bool fit;
  int id_line = 0;
  for (int n = 1; next_line(&p, raw, sizeof raw, &fit); n++) {
    cmd k;
    strcpy(line, raw);
    if (!fit || !parse(line, &k)) return status(SCRIPT_ERROR, n, "syntax");
    if (!id_line && strstr(raw, "{id}")) id_line = n;
  }

  // {id} is read once, up front, because erase chip wipes the user ID.
  char id[17] = "";
  if (id_line) {
    uint8_t u[8];
    if (!dev->read(IHX_USERID, 0, u, sizeof u)) return status(SCRIPT_COMM, id_line);
    bool blank = true;
    for (int i = 0; i < 8; i++) {
      snprintf(id + 2 * i, 3, "%02X", u[i]);
      if (u[i] != 0xFF) blank = false;
    }
    if (blank) return status(SCRIPT_ERROR, id_line, "blank userid");
  }

  script_status pre = preflight(script, fs, id);
  if (pre.result != SCRIPT_PASS) return pre;

  // One allocation: data + mask per region, then the read-back buffer.
  size_t total = 0, biggest = 0;
  for (int r = 0; r < IHX_NREGIONS; r++) {
    total += 2 * (size_t)dev->size[r];
    if (dev->size[r] > biggest) biggest = dev->size[r];
  }
  uint8_t *mem = (uint8_t *)malloc(total + biggest);
  if (!mem) return status(SCRIPT_ERROR, 0, "no memory");
  ctx c = {dev, fs, {}, mem + total};
  uint8_t *q = mem;
  for (int r = 0; r < IHX_NREGIONS; r++) {
    c.img.r[r] = {REGION_BASE[r], dev->size[r], q, q + dev->size[r]};
    q += 2 * (size_t)dev->size[r];
  }
  script_status s = exec(&c, script, id, progress);
  free(mem);
  return s;
}
