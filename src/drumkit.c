/*
 * ours-blanc Drum Kit — a libretro core for the R36S (and any RetroArch).
 *
 * Pad mode: every button plays a drum. L1 = play/stop, R1 = loop record on/off
 * (hold R1 = clear the loop), R2 = step sequencer, START = metronome on/off.
 * Sequencer mode: D-pad moves the cursor, A toggles a step, B auditions the row,
 * X/Y = tempo up/down, L2 = record quantize on/off, R2 = back to the pads.
 *
 * Sounds are synthesized at start-up (no sample files). The loop is 2 bars of 4/4,
 * stored as events on a 96-PPQ grid shared by live recording and the sequencer.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "libretro.h"
#include "font8x8_basic.h"

#define SR 44100
#define FPS 60
#define SPF (SR / FPS)            /* 735 samples per frame */
#define W 640
#define H 480
#define PPQ 96
#define BEATS_PER_BAR 4
#define BARS 2
#define LOOP_TICKS (PPQ * BEATS_PER_BAR * BARS)   /* 768 */
#define STEP_TICKS (PPQ / 4)                       /* 16th note = 24 */
#define STEPS (LOOP_TICKS / STEP_TICKS)            /* 32 */
#define MAX_EVENTS 4096
#define MAX_VOICES 32
#define HOLD_CLEAR_FRAMES 36      /* hold R1 0.6 s to clear */

enum { KICK, SNARE, CHH, OHH, TOM_HI, TOM_MID, TOM_LO, CLAP, CRASH, RIDE, NI };
static const char *INST_NAME[NI] = { "KICK", "SNARE", "CL HAT", "OP HAT", "HI TOM", "MID TOM", "LOW TOM", "CLAP", "CRASH", "RIDE" };
static const char *INST_SHORT[NI] = { "KICK", "SNARE", "CL HH", "OP HH", "TOM 1", "TOM 2", "TOM 3", "CLAP", "CRASH", "RIDE" };
static const char *INST_KEY[NI]  = { "B", "A", "Y", "X", "UP", "LEFT", "DOWN", "RIGHT", "L2", "L3" };
static const unsigned INST_BTN[NI] = {
  RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_Y, RETRO_DEVICE_ID_JOYPAD_X,
  RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_DOWN, RETRO_DEVICE_ID_JOYPAD_RIGHT,
  RETRO_DEVICE_ID_JOYPAD_L2, RETRO_DEVICE_ID_JOYPAD_L3 };
static const uint32_t INST_COL[NI] = { 0xD2372B, 0xF2C230, 0x3DBB80, 0x2DB7BF, 0x9877D8, 0x7381F0, 0x5B86E0, 0xF58C3A, 0xE65B9F, 0x94B83A };

/* ---------- libretro callbacks ---------- */
static retro_environment_t env_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_log_printf_t log_cb;

/* ---------- sounds ---------- */
static float *smp[NI + 2];   /* + metronome hi/lo */
static int smp_len[NI + 2];
#define CLICK_HI NI
#define CLICK_LO (NI + 1)

static uint32_t rng = 0x12345678u;
static float frand(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return (float)((int32_t)rng) / 2147483648.0f; }

/* metallic 808-style source: six square waves at inharmonic ratios */
static float metal(double t, double base) {
  static const double r[6] = { 1.0, 1.4471, 1.6170, 1.9265, 2.5028, 2.6637 };
  float s = 0;
  for (int i = 0; i < 6; i++) s += ((t * base * r[i] - floor(t * base * r[i])) < 0.5) ? 1.f : -1.f;
  return s / 6.f;
}

static void normalize(float *b, int n, float peak) {
  float m = 0; for (int i = 0; i < n; i++) if (fabsf(b[i]) > m) m = fabsf(b[i]);
  if (m > 0) for (int i = 0; i < n; i++) b[i] *= peak / m;
}

static float *alloc_smp(int id, double secs) {
  smp_len[id] = (int)(secs * SR);
  smp[id] = calloc(smp_len[id], sizeof(float));
  return smp[id];
}

static void synth_all(void) {
  float *b; int n; double ph = 0;
  /* kick: pitch sweep 150 -> 48 Hz with a click */
  b = alloc_smp(KICK, 0.5); n = smp_len[KICK]; ph = 0;
  for (int i = 0; i < n; i++) { double t = (double)i / SR; double f = 48 + 110 * exp(-t * 28);
    ph += 2 * M_PI * f / SR; b[i] = (float)(sin(ph) * exp(-t * 6.5)) + (t < 0.004 ? 0.6f * frand() * (float)(1 - t / 0.004) : 0); }
  normalize(b, n, 0.95f);
  /* snare: body tone + bright noise */
  b = alloc_smp(SNARE, 0.3); n = smp_len[SNARE]; ph = 0; { float hp = 0, prev = 0;
  for (int i = 0; i < n; i++) { double t = (double)i / SR; ph += 2 * M_PI * (185 + 40 * exp(-t * 40)) / SR;
    float nz = frand(); hp = 0.85f * (hp + nz - prev); prev = nz;
    b[i] = (float)(0.55 * sin(ph) * exp(-t * 28) + 0.8 * hp * exp(-t * 16)); } }
  normalize(b, n, 0.8f);
  /* hats: high-passed metal + noise */
  for (int k = 0; k < 2; k++) {
    int id = k ? OHH : CHH; double dec = k ? 7.5 : 55;
    b = alloc_smp(id, k ? 0.6 : 0.09); n = smp_len[id]; float hp = 0, prev = 0, hp2 = 0, prev2 = 0;
    for (int i = 0; i < n; i++) { double t = (double)i / SR; float x = 0.7f * metal(t, 420) + 0.5f * frand();
      hp = 0.6f * (hp + x - prev); prev = x; hp2 = 0.6f * (hp2 + hp - prev2); prev2 = hp;
      b[i] = (float)(hp2 * exp(-t * dec) * (t < 0.002 ? t / 0.002 : 1)); }
    normalize(b, n, k ? 0.45f : 0.5f);
  }
  /* toms */
  static const double tf[3] = { 210, 150, 105 };
  for (int k = 0; k < 3; k++) {
    int id = TOM_HI + k; b = alloc_smp(id, 0.55); n = smp_len[id]; ph = 0;
    for (int i = 0; i < n; i++) { double t = (double)i / SR; ph += 2 * M_PI * tf[k] * (1 + 0.6 * exp(-t * 18)) / SR;
      b[i] = (float)(sin(ph) * exp(-t * 7.5) + 0.15 * frand() * exp(-t * 60)); }
    normalize(b, n, 0.8f);
  }
  /* clap: band-passed noise in three quick bursts and a tail */
  b = alloc_smp(CLAP, 0.4); n = smp_len[CLAP]; {
    double fc = 1200, q = 1.2, w0 = 2 * M_PI * fc / SR, al = sin(w0) / (2 * q), a0 = 1 + al;
    double b0 = al / a0, b2 = -al / a0, a1 = -2 * cos(w0) / a0, a2 = (1 - al) / a0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (int i = 0; i < n; i++) { double t = (double)i / SR, x = frand();
      double y = b0 * x + b2 * x2 - a1 * y1 - a2 * y2; x2 = x1; x1 = x; y2 = y1; y1 = y;
      double env = 0; for (int k = 0; k < 3; k++) { double tt = t - k * 0.011; if (tt >= 0) env = fmax(env, exp(-tt * 180)); }
      env = fmax(env, t > 0.022 ? 0.7 * exp(-(t - 0.022) * 14) : 0);
      b[i] = (float)(y * env); } }
  normalize(b, n, 0.8f);
  /* crash: long metal + noise */
  b = alloc_smp(CRASH, 2.0); n = smp_len[CRASH]; { float hp = 0, prev = 0;
  for (int i = 0; i < n; i++) { double t = (double)i / SR; float x = 0.5f * metal(t, 300) + 0.8f * frand();
    hp = 0.7f * (hp + x - prev); prev = x; b[i] = (float)(hp * exp(-t * 2.0) * (t < 0.003 ? t / 0.003 : 1)); } }
  normalize(b, n, 0.5f);
  /* ride: metallic ping with a bell partial */
  b = alloc_smp(RIDE, 1.3); n = smp_len[RIDE]; { float hp = 0, prev = 0;
  for (int i = 0; i < n; i++) { double t = (double)i / SR; float x = 0.6f * metal(t, 520) + 0.15f * frand();
    hp = 0.7f * (hp + x - prev); prev = x;
    b[i] = (float)(hp * exp(-t * 3.2) + 0.25 * sin(2 * M_PI * 2900 * t) * exp(-t * 6)); } }
  normalize(b, n, 0.45f);
  /* metronome clicks */
  for (int k = 0; k < 2; k++) { int id = k ? CLICK_LO : CLICK_HI; b = alloc_smp(id, 0.03); n = smp_len[id];
    for (int i = 0; i < n; i++) { double t = (double)i / SR; b[i] = (float)(sin(2 * M_PI * (k ? 1000 : 1600) * t) * exp(-t * 180)); }
    normalize(b, n, 0.35f); }
}

/* ---------- voices ---------- */
typedef struct { int id, pos, delay; float gain; bool on; } Voice;
static Voice voices[MAX_VOICES];
static int flash[NI];            /* frames left to light a pad */

static void play_sound(int id, int delay, float gain) {
  if (id == CHH || id == OHH) for (int v = 0; v < MAX_VOICES; v++)       /* closed hat chokes open hat */
    if (voices[v].on && voices[v].id == OHH && id == CHH) voices[v].on = false;
  int slot = -1, oldest = -1, op = -1;
  for (int v = 0; v < MAX_VOICES; v++) {
    if (!voices[v].on) { slot = v; break; }
    if (voices[v].pos > op) { op = voices[v].pos; oldest = v; }
  }
  if (slot < 0) slot = oldest;
  voices[slot] = (Voice){ id, 0, delay, gain, true };
  if (id < NI) flash[id] = 8;
}

/* ---------- loop / transport ---------- */
typedef struct { uint16_t tick; uint8_t inst; int32_t skip_pass; } Event;
static Event ev[MAX_EVENTS];
static int nev;
static bool playing, recording, metronome = true, quantize = true;
static int bpm = 100;
static double tick_pos;          /* 0 .. LOOP_TICKS */
static int next_idx;             /* next event to play */
static int32_t pass_no;          /* how many times the loop wrapped */
static bool dirty; static int dirty_frames;

static int first_after(double t) {         /* index of the first event with tick > t */
  int i = 0; while (i < nev && ev[i].tick <= t) i++; return i;
}
static int find_event(int inst, int tick) {
  for (int i = 0; i < nev; i++) if (ev[i].inst == inst && ev[i].tick == tick) return i;
  return -1;
}
static void mark_dirty(void) { dirty = true; dirty_frames = 30; }

static void add_event(int inst, int tick, int32_t skip_pass) {
  tick = ((tick % LOOP_TICKS) + LOOP_TICKS) % LOOP_TICKS;
  if (find_event(inst, tick) >= 0 || nev >= MAX_EVENTS) return;
  int i = nev; while (i > 0 && ev[i - 1].tick > tick) { ev[i] = ev[i - 1]; i--; }
  ev[i] = (Event){ (uint16_t)tick, (uint8_t)inst, skip_pass }; nev++;
  next_idx = first_after(tick_pos);
  mark_dirty();
}
static void remove_event(int idx) {
  memmove(&ev[idx], &ev[idx + 1], (nev - idx - 1) * sizeof(Event)); nev--;
  next_idx = first_after(tick_pos);
  mark_dirty();
}
static void clear_loop(void) { nev = 0; next_idx = 0; mark_dirty(); }

static bool click_first;
static void start_play(void) { playing = true; tick_pos = 0; next_idx = 0; pass_no++; click_first = true; }
static void stop_play(void) { playing = false; recording = false; tick_pos = 0; next_idx = 0; }

/* record a live hit at the current position */
static void record_hit(int inst) {
  double t = tick_pos;
  int tick = quantize ? (int)floor(t / STEP_TICKS + 0.5) * STEP_TICKS : (int)floor(t + 0.5);
  /* a hit snapped forward (still ahead of the playhead) must not sound again in this pass */
  int32_t skip = (tick > t && tick < LOOP_TICKS) ? pass_no : -1;
  if (tick >= LOOP_TICKS) { tick -= LOOP_TICKS; skip = pass_no + 1; }
  add_event(inst, tick, skip);
}

/* ---------- save ---------- */
static char save_path[1024];
static void save_pattern(void) {
  if (!save_path[0]) return;
  FILE *f = fopen(save_path, "wb"); if (!f) return;
  uint32_t magic = 0x4B44424F; /* "OBDK" */ uint16_t ver = 1, b = (uint16_t)bpm, n = (uint16_t)nev; uint8_t fl = (metronome ? 1 : 0) | (quantize ? 2 : 0);
  fwrite(&magic, 4, 1, f); fwrite(&ver, 2, 1, f); fwrite(&b, 2, 1, f); fwrite(&fl, 1, 1, f); fwrite(&n, 2, 1, f);
  for (int i = 0; i < nev; i++) { fwrite(&ev[i].tick, 2, 1, f); fwrite(&ev[i].inst, 1, 1, f); }
  fclose(f);
}
static void load_pattern(void) {
  if (!save_path[0]) return;
  FILE *f = fopen(save_path, "rb"); if (!f) return;
  uint32_t magic = 0; uint16_t ver = 0, b = 0, n = 0; uint8_t fl = 0;
  if (fread(&magic, 4, 1, f) == 1 && magic == 0x4B44424F && fread(&ver, 2, 1, f) == 1 && ver == 1 &&
      fread(&b, 2, 1, f) == 1 && fread(&fl, 1, 1, f) == 1 && fread(&n, 2, 1, f) == 1) {
    if (b >= 40 && b <= 240) bpm = b;
    metronome = fl & 1; quantize = (fl & 2) != 0;
    nev = 0;
    for (int i = 0; i < n && i < MAX_EVENTS; i++) { uint16_t t; uint8_t in;
      if (fread(&t, 2, 1, f) != 1 || fread(&in, 1, 1, f) != 1) break;
      if (t < LOOP_TICKS && in < NI) add_event(in, t, -1); }
  }
  fclose(f); dirty = false;
}

/* ---------- audio ---------- */
static int16_t abuf[SPF * 2];
static float out_level;
static void render_audio(void) {
  double tps = (double)bpm * PPQ / 60.0 / SR;     /* ticks per sample */
  for (int s = 0; s < SPF; s++) {
    if (playing) {
      double prev = tick_pos;
      tick_pos += tps;
      if (tick_pos >= LOOP_TICKS) { tick_pos -= LOOP_TICKS; next_idx = 0; pass_no++; prev = -1; }
      while (next_idx < nev && ev[next_idx].tick <= tick_pos) {
        if (ev[next_idx].skip_pass != pass_no) play_sound(ev[next_idx].inst, 0, 0.9f);
        else ev[next_idx].skip_pass = -1;
        next_idx++;
      }
      if (metronome) {                              /* beat crossing */
        int pb = (int)floor(prev / PPQ), nb = (int)floor(tick_pos / PPQ);
        if (prev < 0 || click_first) pb = -1;
        click_first = false;
        if (nb != pb) play_sound(nb % BEATS_PER_BAR == 0 ? CLICK_HI : CLICK_LO, 0, 1.0f);
      }
    }
    float mix = 0;
    for (int v = 0; v < MAX_VOICES; v++) {
      Voice *vo = &voices[v]; if (!vo->on) continue;
      if (vo->delay > 0) { vo->delay--; continue; }
      mix += smp[vo->id][vo->pos] * vo->gain;
      if (++vo->pos >= smp_len[vo->id]) vo->on = false;
    }
    mix = tanhf(mix * 0.9f);
    float a = fabsf(mix); out_level = a > out_level ? a : out_level * 0.9995f;
    int16_t o = (int16_t)(mix * 30000);
    abuf[s * 2] = abuf[s * 2 + 1] = o;
  }
  audio_batch_cb(abuf, SPF);
}

/* ---------- drawing ---------- */
static uint32_t fb[W * H];
#define BG 0x101216
#define INK 0xECEEF2
#define MUTED 0x8C93A1
#define LINE 0x2B3039
#define SURF 0x191C22
#define RED 0xF0503F

static void rect(int x, int y, int w, int h, uint32_t c) {
  for (int j = y < 0 ? 0 : y; j < y + h && j < H; j++)
    for (int i = x < 0 ? 0 : x; i < x + w && i < W; i++) fb[j * W + i] = c;
}
static void frame_rect(int x, int y, int w, int h, int t, uint32_t c) {
  rect(x, y, w, t, c); rect(x, y + h - t, w, t, c); rect(x, y, t, h, c); rect(x + w - t, y, t, h, c);
}
static uint32_t mixc(uint32_t a, uint32_t b, float k) {
  int r = (int)(((a >> 16) & 255) * (1 - k) + ((b >> 16) & 255) * k);
  int g = (int)(((a >> 8) & 255) * (1 - k) + ((b >> 8) & 255) * k);
  int bl = (int)((a & 255) * (1 - k) + (b & 255) * k);
  return (uint32_t)(r << 16 | g << 8 | bl);
}
static int text(const char *s, int x, int y, int sc, uint32_t c) {
  for (; *s; s++, x += 8 * sc) {
    unsigned char ch = (unsigned char)*s; if (ch > 127) ch = '?';
    for (int r = 0; r < 8; r++) for (int b = 0; b < 8; b++)
      if (font8x8_basic[ch][r] & (1 << b)) rect(x + b * sc, y + r * sc, sc, sc, c);
  }
  return x;
}
static int text_w(const char *s, int sc) { return (int)strlen(s) * 8 * sc; }
static void text_c(const char *s, int cx, int y, int sc, uint32_t c) { text(s, cx - text_w(s, sc) / 2, y, sc, c); }

static bool seq_mode;
static int cur_row, cur_col;

static void draw_header(void) {
  rect(0, 0, W, 44, SURF); rect(0, 44, W, 1, LINE);
  text("OURS-BLANC", 16, 8, 1, MUTED);
  text(seq_mode ? "SEQUENCER" : "DRUM KIT", 16, 20, 2, INK);
  char b[64]; snprintf(b, sizeof b, "%d BPM", bpm); text(b, 236, 14, 2, INK);
  int x = 380;
  if (recording) { rect(x, 12, 18, 18, RED); x = text("REC", x + 24, 14, 2, RED) + 14; }
  else if (playing) { for (int i = 0; i < 16; i++) { int hh = 18 - i * 18 / 16; rect(x + i, 12 + (18 - hh) / 2, 1, hh, INK); } x = text("PLAY", x + 24, 14, 2, INK) + 14; }
  else { rect(x, 13, 16, 16, MUTED); x = text("STOP", x + 24, 14, 2, MUTED) + 14; }
  text(metronome ? "CLICK ON" : "CLICK OFF", 552, 10, 1, metronome ? INK : MUTED);
  text(quantize ? "QUANT 1/16" : "QUANT OFF", 552, 24, 1, quantize ? INK : MUTED);
}

/* loop strip: 32 steps, beats and bars marked, playhead, recorded hits as dots */
static void draw_strip(int y, int h) {
  int x0 = 24, w = W - 48;
  rect(x0, y, w, h, SURF);
  for (int s = 0; s <= STEPS; s++) {
    int x = x0 + w * s / STEPS;
    uint32_t c = s % 16 == 0 ? MUTED : s % 4 == 0 ? 0x4A505C : LINE;
    rect(x, y, 1, h, c);
  }
  for (int i = 0; i < nev; i++) {
    int x = x0 + (int)((double)w * ev[i].tick / LOOP_TICKS);
    int yy = y + 3 + (h - 8) * ev[i].inst / (NI - 1);
    rect(x - 1, yy, 4, 3, INST_COL[ev[i].inst]);
  }
  if (playing) rect(x0 + (int)(w * tick_pos / LOOP_TICKS), y - 3, 2, h + 6, recording ? RED : INK);
  frame_rect(x0, y, w, h, 1, LINE);
}

/* pads laid out like the controller: d-pad cluster left, face buttons right, shoulders on top */
static void pad(int i, int cx, int cy, int w, int h) {
  float k = flash[i] / 8.0f;
  uint32_t base = mixc(BG, INST_COL[i], 0.22f + 0.6f * k);
  rect(cx - w / 2, cy - h / 2, w, h, base);
  frame_rect(cx - w / 2, cy - h / 2, w, h, 2, INST_COL[i]);
  text_c(INST_SHORT[i], cx, cy - 12, 2, INK);
  text_c(INST_KEY[i], cx, cy + 10, 1, mixc(INK, INST_COL[i], 0.5f));
}
static void draw_pads(void) {
  draw_strip(58, 46);
  const int pw = 96, ph = 64, dx = 102, dy = 72;
  pad(CRASH, 56, 146, pw, 44);  pad(RIDE, 158, 146, pw, 44);
  int lx = 154, rx = 486, cy = 290;
  pad(TOM_HI, lx, cy - dy, pw, ph); pad(TOM_MID, lx - dx, cy, pw, ph);
  pad(CLAP, lx + dx, cy, pw, ph);  pad(TOM_LO, lx, cy + dy, pw, ph);
  pad(OHH, rx, cy - dy, pw, ph);    pad(CHH, rx - dx, cy, pw, ph);
  pad(SNARE, rx + dx, cy, pw, ph); pad(KICK, rx, cy + dy, pw, ph);
  rect(0, 412, W, 1, LINE);
  text("L1 PLAY/STOP   R1 REC ON/OFF (HOLD: CLEAR)", 24, 424, 1, INK);
  text("R2 SEQUENCER   START CLICK ON/OFF   SELECT+START QUIT", 24, 440, 1, MUTED);
  char b[64]; snprintf(b, sizeof b, "%d HITS IN LOOP  (2 BARS)", nev); text(b, 24, 458, 1, MUTED);
}

static bool step_on(int row, int step) {
  for (int i = 0; i < nev; i++) if (ev[i].inst == row) {
    int d = ev[i].tick - step * STEP_TICKS; if (d >= -STEP_TICKS / 2 && d < STEP_TICKS / 2) return true;
  }
  return false;
}
static void draw_seq(void) {
  const int lx = 16, gx = 100, gy = 64, cw = 16, ch = 31;
  int play_step = playing ? (int)(tick_pos / STEP_TICKS) : -1;
  for (int r = 0; r < NI; r++) {
    int y = gy + r * ch;
    text(INST_NAME[r], lx, y + 11, 1, r == cur_row ? INK : MUTED);
    for (int s = 0; s < STEPS; s++) {
      int x = gx + s * cw + (s / 4) * 2 + (s / 16) * 4;
      bool on = step_on(r, s);
      uint32_t c = on ? INST_COL[r] : (s / 4) % 2 ? 0x1C2027 : 0x22262E;
      if (s == play_step) c = on ? mixc(INST_COL[r], 0xFFFFFF, 0.5f) : 0x3A404B;
      rect(x, y + 2, cw - 3, ch - 5, c);
      if (r == cur_row && s == cur_col) frame_rect(x - 2, y, cw + 1, ch - 1, 2, INK);
    }
  }
  rect(0, 382, W, 1, LINE);
  text("D-PAD MOVE   A STEP ON/OFF   B LISTEN   X/Y TEMPO +/-", 16, 394, 1, INK);
  text("L1 PLAY/STOP   R1 REC (HOLD: CLEAR)   L2 QUANTIZE   R2 PADS", 16, 410, 1, INK);
  char b[96]; snprintf(b, sizeof b, "STEP %d/%d  BAR %d  BEAT %d", cur_col + 1, STEPS, cur_col / 16 + 1, (cur_col % 16) / 4 + 1);
  text(b, 16, 432, 1, MUTED);
  text("RECORDED HITS SNAP TO THE NEAREST STEP HERE", 16, 448, 1, MUTED);
}

static void draw(void) {
  for (int i = 0; i < W * H; i++) fb[i] = BG;
  draw_header();
  if (seq_mode) draw_seq(); else draw_pads();
  /* output meter (pad screen) */
  if (!seq_mode) { int m = (int)(out_level * 120); rect(W - 14, 52 + 120 - m, 6, m, out_level > 0.95f ? RED : 0x3DBB80); }
  video_cb(fb, W, H, W * sizeof(uint32_t));
}

/* ---------- input ---------- */
static bool held[16], prev_held[16];
static int r1_hold, repeat_cnt[16];
static bool r1_consumed;
#define PRESSED(b) (held[b] && !prev_held[b])
#define RELEASED(b) (!held[b] && prev_held[b])

static bool repeat_fire(unsigned b) {          /* press, then auto-repeat after 0.33 s every 0.1 s */
  if (!held[b]) { repeat_cnt[b] = 0; return false; }
  int c = repeat_cnt[b]++;
  return c == 0 || (c >= 20 && (c - 20) % 6 == 0);
}

static void handle_input(void) {
  input_poll_cb();
  for (unsigned b = 0; b < 16; b++) { prev_held[b] = held[b]; held[b] = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, b) != 0; }

  /* L1: play / stop */
  if (PRESSED(RETRO_DEVICE_ID_JOYPAD_L)) { if (playing) stop_play(); else start_play(); }
  /* R1: tap = record on/off, hold = clear */
  if (held[RETRO_DEVICE_ID_JOYPAD_R]) {
    if (PRESSED(RETRO_DEVICE_ID_JOYPAD_R)) { r1_hold = 0; r1_consumed = false; }
    if (++r1_hold == HOLD_CLEAR_FRAMES) { clear_loop(); recording = false; r1_consumed = true; }
  } else if (RELEASED(RETRO_DEVICE_ID_JOYPAD_R) && !r1_consumed) {
    recording = !recording;
    if (recording && !playing) start_play();
    if (!recording) mark_dirty();
  }
  /* R2: pads <-> sequencer */
  if (PRESSED(RETRO_DEVICE_ID_JOYPAD_R2)) { seq_mode = !seq_mode; memset(repeat_cnt, 0, sizeof repeat_cnt); }
  /* START: metronome (SELECT+START is RetroArch's quit, so ignore START while SELECT is held) */
  if (PRESSED(RETRO_DEVICE_ID_JOYPAD_START) && !held[RETRO_DEVICE_ID_JOYPAD_SELECT]) { metronome = !metronome; mark_dirty(); }

  if (!seq_mode) {
    for (int i = 0; i < NI; i++) if (PRESSED(INST_BTN[i])) {
      play_sound(i, 0, 1.0f);
      if (recording && playing) record_hit(i);
    }
  } else {
    if (repeat_fire(RETRO_DEVICE_ID_JOYPAD_UP)) cur_row = (cur_row + NI - 1) % NI;
    if (repeat_fire(RETRO_DEVICE_ID_JOYPAD_DOWN)) cur_row = (cur_row + 1) % NI;
    if (repeat_fire(RETRO_DEVICE_ID_JOYPAD_LEFT)) cur_col = (cur_col + STEPS - 1) % STEPS;
    if (repeat_fire(RETRO_DEVICE_ID_JOYPAD_RIGHT)) cur_col = (cur_col + 1) % STEPS;
    if (repeat_fire(RETRO_DEVICE_ID_JOYPAD_X) && bpm < 240) { bpm++; mark_dirty(); }
    if (repeat_fire(RETRO_DEVICE_ID_JOYPAD_Y) && bpm > 40) { bpm--; mark_dirty(); }
    if (PRESSED(RETRO_DEVICE_ID_JOYPAD_A)) {
      bool removed = false;
      for (int i = nev - 1; i >= 0; i--) if (ev[i].inst == cur_row) {
        int d = ev[i].tick - cur_col * STEP_TICKS;
        if (d >= -STEP_TICKS / 2 && d < STEP_TICKS / 2) { remove_event(i); removed = true; }
      }
      if (!removed) { add_event(cur_row, cur_col * STEP_TICKS, -1); if (!playing) play_sound(cur_row, 0, 1.0f); }
    }
    if (PRESSED(RETRO_DEVICE_ID_JOYPAD_B)) play_sound(cur_row, 0, 1.0f);
    if (PRESSED(RETRO_DEVICE_ID_JOYPAD_L2)) { quantize = !quantize; mark_dirty(); }
  }
}

/* ---------- libretro API ---------- */
void retro_set_environment(retro_environment_t cb) {
  env_cb = cb;
  bool no_game = true; cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
  struct retro_log_callback l; if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &l)) log_cb = l.log;
}
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }
unsigned retro_api_version(void) { return RETRO_API_VERSION; }
void retro_set_controller_port_device(unsigned port, unsigned device) { (void)port; (void)device; }

void retro_get_system_info(struct retro_system_info *info) {
  memset(info, 0, sizeof *info);
  info->library_name = "ours-blanc Drum Kit";
  info->library_version = "1.0.0";
  info->need_fullpath = false;
  info->valid_extensions = "";
}
void retro_get_system_av_info(struct retro_system_av_info *info) {
  memset(info, 0, sizeof *info);
  info->timing.fps = FPS; info->timing.sample_rate = SR;
  info->geometry.base_width = W; info->geometry.base_height = H;
  info->geometry.max_width = W; info->geometry.max_height = H;
  info->geometry.aspect_ratio = 4.0f / 3.0f;
}

void retro_init(void) {
  synth_all();
  const char *dir = NULL;
  if (env_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir && *dir)
    snprintf(save_path, sizeof save_path, "%s/ours-blanc-drumkit.dat", dir);
  load_pattern();
}
void retro_deinit(void) {
  if (dirty) save_pattern();
  for (int i = 0; i < NI + 2; i++) { free(smp[i]); smp[i] = NULL; }
}
void retro_reset(void) { stop_play(); }

bool retro_load_game(const struct retro_game_info *game) {
  (void)game;
  enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
  if (!env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) return false;
  struct retro_input_descriptor desc[] = {
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Kick" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Snare / Step on-off" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Closed hat / Tempo -" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "Open hat / Tempo +" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "High tom" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Mid tom" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Low tom" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Clap" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "Crash / Quantize" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3, "Ride" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "Play / Stop" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "Loop record (hold: clear)" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "Sequencer" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Metronome" },
    { 0 } };
  env_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);
  return true;
}
bool retro_load_game_special(unsigned t, const struct retro_game_info *i, size_t n) { (void)t; (void)i; (void)n; return false; }
void retro_unload_game(void) { if (dirty) save_pattern(); }
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

void retro_run(void) {
  handle_input();
  render_audio();
  for (int i = 0; i < NI; i++) if (flash[i] > 0) flash[i]--;
  if (dirty && --dirty_frames <= 0) { save_pattern(); dirty = false; }
  draw();
}

size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void *d, size_t s) { (void)d; (void)s; return false; }
bool retro_unserialize(const void *d, size_t s) { (void)d; (void)s; return false; }
void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned i, bool e, const char *c) { (void)i; (void)e; (void)c; }
