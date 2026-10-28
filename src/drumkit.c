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
#include "../engine/dk_engine.h"

#define SR 44100
#define FPS 60
#define SPF (SR / FPS)            /* 735 samples per frame */
#define W 640
#define H 480
#define LOOP_TICKS DK_LOOP_TICKS
#define STEP_TICKS DK_STEP_TICKS
#define STEPS DK_STEPS
#define HOLD_CLEAR_FRAMES 36      /* hold R1 0.6 s to clear */

enum { KICK, SNARE, CHH, OHH, TOM_HI, TOM_MID, TOM_LO, CLAP, CRASH, RIDE, NI };
#define INST_NAME dk_inst_name
static const char *INST_SHORT[NI] = { "KICK", "SNARE", "CL HH", "OP HH", "TOM 1", "TOM 2", "TOM 3", "CLAP", "CRASH", "RIDE" };
static const char *INST_KEY[NI]  = { "B", "A", "Y", "X", "UP", "LEFT", "DOWN", "RIGHT", "L2", "L3" };
static const unsigned INST_BTN[NI] = {
  RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_Y, RETRO_DEVICE_ID_JOYPAD_X,
  RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_DOWN, RETRO_DEVICE_ID_JOYPAD_RIGHT,
  RETRO_DEVICE_ID_JOYPAD_L2, RETRO_DEVICE_ID_JOYPAD_L3 };
#define INST_COL dk_inst_color

/* ---------- libretro callbacks ---------- */
static retro_environment_t env_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_log_printf_t log_cb;

/* ---------- engine ---------- */
static dk_engine *eng;
static int flash[NI];            /* frames left to light a pad */
static uint32_t seen_hits[NI];
static uint32_t saved_rev; static int dirty_frames;

/* ---------- save ---------- */
static char save_path[1024];
static void save_pattern(void) {
  if (!save_path[0] || !eng) return;
  static uint8_t buf[DK_STATE_MAX];
  size_t n = dk_save(eng, buf, sizeof buf);
  FILE *f = fopen(save_path, "wb"); if (!f) return;
  fwrite(buf, 1, n, f); fclose(f);
  saved_rev = dk_revision(eng);
}
static void load_pattern(void) {
  if (!save_path[0]) return;
  FILE *f = fopen(save_path, "rb"); if (!f) return;
  static uint8_t buf[DK_STATE_MAX];
  size_t n = fread(buf, 1, sizeof buf, f); fclose(f);
  dk_load(eng, buf, n);
  saved_rev = dk_revision(eng);
}

/* ---------- audio ---------- */
static float fl_buf[SPF];
static int16_t abuf[SPF * 2];
static void render_audio(void) {
  dk_render(eng, fl_buf, NULL, SPF);
  for (int s = 0; s < SPF; s++) { int16_t o = (int16_t)(fl_buf[s] * 30000); abuf[s * 2] = abuf[s * 2 + 1] = o; }
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
  char b[64]; snprintf(b, sizeof b, "%d BPM", dk_bpm(eng)); text(b, 236, 14, 2, INK);
  int x = 380;
  if (dk_recording(eng)) { rect(x, 12, 18, 18, RED); x = text("REC", x + 24, 14, 2, RED) + 14; }
  else if (dk_playing(eng)) { for (int i = 0; i < 16; i++) { int hh = 18 - i * 18 / 16; rect(x + i, 12 + (18 - hh) / 2, 1, hh, INK); } x = text("PLAY", x + 24, 14, 2, INK) + 14; }
  else { rect(x, 13, 16, 16, MUTED); x = text("STOP", x + 24, 14, 2, MUTED) + 14; }
  text(dk_metronome(eng) ? "CLICK ON" : "CLICK OFF", 552, 10, 1, dk_metronome(eng) ? INK : MUTED);
  text(dk_quantize(eng) ? "QUANT 1/16" : "QUANT OFF", 552, 24, 1, dk_quantize(eng) ? INK : MUTED);
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
  const dk_event *ev = dk_events(eng); int nev = dk_event_count(eng);
  for (int i = 0; i < nev; i++) {
    int x = x0 + (int)((double)w * ev[i].tick / LOOP_TICKS);
    int yy = y + 3 + (h - 8) * ev[i].inst / (NI - 1);
    rect(x - 1, yy, 4, 3, INST_COL[ev[i].inst]);
  }
  if (dk_playing(eng)) rect(x0 + (int)(w * dk_position(eng) / LOOP_TICKS), y - 3, 2, h + 6, dk_recording(eng) ? RED : INK);
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
  char b[64]; snprintf(b, sizeof b, "%d HITS IN LOOP  (2 BARS)", dk_event_count(eng)); text(b, 24, 458, 1, MUTED);
}

static void draw_seq(void) {
  const int lx = 16, gx = 100, gy = 64, cw = 16, ch = 31;
  int play_step = dk_playing(eng) ? (int)(dk_position(eng) / STEP_TICKS) : -1;
  for (int r = 0; r < NI; r++) {
    int y = gy + r * ch;
    text(INST_NAME[r], lx, y + 11, 1, r == cur_row ? INK : MUTED);
    for (int s = 0; s < STEPS; s++) {
      int x = gx + s * cw + (s / 4) * 2 + (s / 16) * 4;
      bool on = (dk_step_mask(eng, r) >> s) & 1;
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
  if (!seq_mode) { float lv = dk_level(eng); int m = (int)(lv * 120); rect(W - 14, 52 + 120 - m, 6, m, lv > 0.95f ? RED : 0x3DBB80); }
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
  if (PRESSED(RETRO_DEVICE_ID_JOYPAD_L)) dk_toggle_play(eng);
  /* R1: tap = record on/off, hold = clear */
  if (held[RETRO_DEVICE_ID_JOYPAD_R]) {
    if (PRESSED(RETRO_DEVICE_ID_JOYPAD_R)) { r1_hold = 0; r1_consumed = false; }
    if (++r1_hold == HOLD_CLEAR_FRAMES) { dk_clear(eng); r1_consumed = true; }
  } else if (RELEASED(RETRO_DEVICE_ID_JOYPAD_R) && !r1_consumed) {
    dk_toggle_record(eng);
  }
  /* R2: pads <-> sequencer */
  if (PRESSED(RETRO_DEVICE_ID_JOYPAD_R2)) { seq_mode = !seq_mode; memset(repeat_cnt, 0, sizeof repeat_cnt); }
  /* START: metronome (SELECT+START is RetroArch's quit, so ignore START while SELECT is held) */
  if (PRESSED(RETRO_DEVICE_ID_JOYPAD_START) && !held[RETRO_DEVICE_ID_JOYPAD_SELECT]) dk_set_metronome(eng, !dk_metronome(eng));

  if (!seq_mode) {
    for (int i = 0; i < NI; i++) if (PRESSED(INST_BTN[i])) {
      dk_hit(eng, i, 127);
    }
  } else {
    if (repeat_fire(RETRO_DEVICE_ID_JOYPAD_UP)) cur_row = (cur_row + NI - 1) % NI;
    if (repeat_fire(RETRO_DEVICE_ID_JOYPAD_DOWN)) cur_row = (cur_row + 1) % NI;
    if (repeat_fire(RETRO_DEVICE_ID_JOYPAD_LEFT)) cur_col = (cur_col + STEPS - 1) % STEPS;
    if (repeat_fire(RETRO_DEVICE_ID_JOYPAD_RIGHT)) cur_col = (cur_col + 1) % STEPS;
    if (repeat_fire(RETRO_DEVICE_ID_JOYPAD_X)) dk_set_bpm(eng, dk_bpm(eng) + 1);
    if (repeat_fire(RETRO_DEVICE_ID_JOYPAD_Y)) dk_set_bpm(eng, dk_bpm(eng) - 1);
    if (PRESSED(RETRO_DEVICE_ID_JOYPAD_A)) dk_toggle_step(eng, cur_row, cur_col);
    if (PRESSED(RETRO_DEVICE_ID_JOYPAD_B)) dk_audition(eng, cur_row, 127);
    if (PRESSED(RETRO_DEVICE_ID_JOYPAD_L2)) dk_set_quantize(eng, !dk_quantize(eng));
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
  eng = dk_create(SR);
  if (!eng) { if (log_cb) log_cb(RETRO_LOG_ERROR, "drumkit: out of memory\n"); return; }
  const char *dir = NULL;
  if (env_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir && *dir)
    snprintf(save_path, sizeof save_path, "%s/ours-blanc-drumkit.dat", dir);
  load_pattern();
}
void retro_deinit(void) {
  if (eng && dk_revision(eng) != saved_rev) save_pattern();
  dk_destroy(eng); eng = NULL;
}
void retro_reset(void) { dk_stop(eng); }

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
void retro_unload_game(void) { if (dk_revision(eng) != saved_rev) save_pattern(); }
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

void retro_run(void) {
  handle_input();
  render_audio();
  for (int i = 0; i < NI; i++) {
    if (flash[i] > 0) flash[i]--;
    uint32_t h = dk_hit_count(eng, i); if (h != seen_hits[i]) { seen_hits[i] = h; flash[i] = 8; }
  }
  /* save 0.5 s after the last change */
  static uint32_t last_rev;
  if (dk_revision(eng) != last_rev) { last_rev = dk_revision(eng); dirty_frames = 30; }
  if (dk_revision(eng) != saved_rev && --dirty_frames <= 0) save_pattern();
  draw();
}

size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void *d, size_t s) { (void)d; (void)s; return false; }
bool retro_unserialize(const void *d, size_t s) { (void)d; (void)s; return false; }
void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned i, bool e, const char *c) { (void)i; (void)e; (void)c; }
