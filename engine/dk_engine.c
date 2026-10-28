/* ours-blanc Drum Kit engine — see dk_engine.h */
#include "dk_engine.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_VOICES 32
#define CLICK_HI DK_NI
#define CLICK_LO (DK_NI + 1)
#define NSOUNDS (DK_NI + 2)

const char *const dk_inst_name[DK_NI] = { "KICK", "SNARE", "CL HAT", "OP HAT", "HI TOM", "MID TOM", "LOW TOM", "CLAP", "CRASH", "RIDE" };
const uint32_t dk_inst_color[DK_NI] = { 0xD2372B, 0xF2C230, 0x3DBB80, 0x2DB7BF, 0x9877D8, 0x7381F0, 0x5B86E0, 0xF58C3A, 0xE65B9F, 0x94B83A };

typedef struct { int id, pos; float gain; int on; } Voice;

struct dk_engine {
  double sr;
  float *smp[NSOUNDS];
  int smp_len[NSOUNDS];
  Voice voices[MAX_VOICES];

  dk_event ev[DK_MAX_EVENTS];
  int32_t skip[DK_MAX_EVENTS];   /* pass in which a forward-snapped hit must stay silent */
  int nev, next_idx;
  int playing, recording, metronome, quantize;
  double bpm, tick_pos;
  int32_t pass_no;
  int click_first, host_was_playing;
  float volume, level;
  uint32_t hits[DK_NI], revision;
  uint32_t rng;
};

/* ---------------------------------------------------------------- synthesis */
static float frand(dk_engine *e) {
  e->rng ^= e->rng << 13; e->rng ^= e->rng >> 17; e->rng ^= e->rng << 5;
  return (float)((int32_t)e->rng) / 2147483648.0f;
}

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

static float *alloc_smp(dk_engine *e, int id, double secs) {
  e->smp_len[id] = (int)(secs * e->sr);
  e->smp[id] = (float *)calloc((size_t)e->smp_len[id], sizeof(float));
  return e->smp[id];
}

/* one-pole high-pass coefficient tuned at 44.1 kHz, kept equivalent at other rates */
static float hp_coef(dk_engine *e, float a44) { return (float)pow(a44, 44100.0 / e->sr); }

static int synth_all(dk_engine *e) {
  const double SR = e->sr;
  float *b; int n; double ph;
  /* kick: pitch sweep 158 -> 48 Hz with a click */
  if (!(b = alloc_smp(e, DK_KICK, 0.5))) return 0;
  n = e->smp_len[DK_KICK]; ph = 0;
  for (int i = 0; i < n; i++) { double t = i / SR, f = 48 + 110 * exp(-t * 28);
    ph += 2 * M_PI * f / SR; b[i] = (float)(sin(ph) * exp(-t * 6.5)) + (t < 0.004 ? 0.6f * frand(e) * (float)(1 - t / 0.004) : 0); }
  normalize(b, n, 0.95f);
  /* snare: body tone + bright noise */
  if (!(b = alloc_smp(e, DK_SNARE, 0.3))) return 0;
  n = e->smp_len[DK_SNARE]; ph = 0; { float hp = 0, prev = 0, a = hp_coef(e, 0.85f);
  for (int i = 0; i < n; i++) { double t = i / SR; ph += 2 * M_PI * (185 + 40 * exp(-t * 40)) / SR;
    float nz = frand(e); hp = a * (hp + nz - prev); prev = nz;
    b[i] = (float)(0.55 * sin(ph) * exp(-t * 28) + 0.8 * hp * exp(-t * 16)); } }
  normalize(b, n, 0.8f);
  /* hats: high-passed metal + noise */
  for (int k = 0; k < 2; k++) {
    int id = k ? DK_OHH : DK_CHH; double dec = k ? 7.5 : 55;
    if (!(b = alloc_smp(e, id, k ? 0.6 : 0.09))) return 0;
    n = e->smp_len[id]; float hp = 0, prev = 0, hp2 = 0, prev2 = 0, a = hp_coef(e, 0.6f);
    for (int i = 0; i < n; i++) { double t = i / SR; float x = 0.7f * metal(t, 420) + 0.5f * frand(e);
      hp = a * (hp + x - prev); prev = x; hp2 = a * (hp2 + hp - prev2); prev2 = hp;
      b[i] = (float)(hp2 * exp(-t * dec) * (t < 0.002 ? t / 0.002 : 1)); }
    normalize(b, n, k ? 0.45f : 0.5f);
  }
  /* toms */
  static const double tf[3] = { 210, 150, 105 };
  for (int k = 0; k < 3; k++) {
    int id = DK_TOM_HI + k;
    if (!(b = alloc_smp(e, id, 0.55))) return 0;
    n = e->smp_len[id]; ph = 0;
    for (int i = 0; i < n; i++) { double t = i / SR; ph += 2 * M_PI * tf[k] * (1 + 0.6 * exp(-t * 18)) / SR;
      b[i] = (float)(sin(ph) * exp(-t * 7.5) + 0.15 * frand(e) * exp(-t * 60)); }
    normalize(b, n, 0.8f);
  }
  /* clap: band-passed noise in three quick bursts and a tail */
  if (!(b = alloc_smp(e, DK_CLAP, 0.4))) return 0;
  n = e->smp_len[DK_CLAP]; {
    double fc = 1200, q = 1.2, w0 = 2 * M_PI * fc / SR, al = sin(w0) / (2 * q), a0 = 1 + al;
    double b0 = al / a0, b2 = -al / a0, a1 = -2 * cos(w0) / a0, a2 = (1 - al) / a0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (int i = 0; i < n; i++) { double t = i / SR, x = frand(e);
      double y = b0 * x + b2 * x2 - a1 * y1 - a2 * y2; x2 = x1; x1 = x; y2 = y1; y1 = y;
      double env = 0; for (int k = 0; k < 3; k++) { double tt = t - k * 0.011; if (tt >= 0) env = fmax(env, exp(-tt * 180)); }
      env = fmax(env, t > 0.022 ? 0.7 * exp(-(t - 0.022) * 14) : 0);
      b[i] = (float)(y * env); } }
  normalize(b, n, 0.8f);
  /* crash: long metal + noise */
  if (!(b = alloc_smp(e, DK_CRASH, 2.0))) return 0;
  n = e->smp_len[DK_CRASH]; { float hp = 0, prev = 0, a = hp_coef(e, 0.7f);
  for (int i = 0; i < n; i++) { double t = i / SR; float x = 0.5f * metal(t, 300) + 0.8f * frand(e);
    hp = a * (hp + x - prev); prev = x; b[i] = (float)(hp * exp(-t * 2.0) * (t < 0.003 ? t / 0.003 : 1)); } }
  normalize(b, n, 0.5f);
  /* ride: metallic ping with a bell partial */
  if (!(b = alloc_smp(e, DK_RIDE, 1.3))) return 0;
  n = e->smp_len[DK_RIDE]; { float hp = 0, prev = 0, a = hp_coef(e, 0.7f);
  for (int i = 0; i < n; i++) { double t = i / SR; float x = 0.6f * metal(t, 520) + 0.15f * frand(e);
    hp = a * (hp + x - prev); prev = x;
    b[i] = (float)(hp * exp(-t * 3.2) + 0.25 * sin(2 * M_PI * 2900 * t) * exp(-t * 6)); } }
  normalize(b, n, 0.45f);
  /* metronome clicks */
  for (int k = 0; k < 2; k++) { int id = k ? CLICK_LO : CLICK_HI;
    if (!(b = alloc_smp(e, id, 0.03))) return 0;
    n = e->smp_len[id];
    for (int i = 0; i < n; i++) { double t = i / SR; b[i] = (float)(sin(2 * M_PI * (k ? 1000 : 1600) * t) * exp(-t * 180)); }
    normalize(b, n, 0.35f); }
  return 1;
}

/* ---------------------------------------------------------------- lifecycle */
dk_engine *dk_create(double sample_rate) {
  if (!(sample_rate >= 8000 && sample_rate <= 384000)) return NULL;
  dk_engine *e = (dk_engine *)calloc(1, sizeof *e);
  if (!e) return NULL;
  e->sr = sample_rate;
  e->rng = 0x12345678u;
  e->bpm = 100; e->metronome = 1; e->quantize = 1; e->volume = 1.0f;
  if (!synth_all(e)) { dk_destroy(e); return NULL; }
  return e;
}
void dk_destroy(dk_engine *e) {
  if (!e) return;
  for (int i = 0; i < NSOUNDS; i++) free(e->smp[i]);
  free(e);
}
double dk_sample_rate(const dk_engine *e) { return e->sr; }

/* ---------------------------------------------------------------- voices */
static float vel_gain(int vel) {
  if (vel < 1) vel = 1;
  if (vel > 127) vel = 127;
  return powf(vel / 127.0f, 1.3f);
}
static void sound(dk_engine *e, int id, float gain) {
  if (id == DK_CHH) for (int v = 0; v < MAX_VOICES; v++)   /* closed hat chokes open hat */
    if (e->voices[v].on && e->voices[v].id == DK_OHH) e->voices[v].on = 0;
  int slot = -1, oldest = 0, op = -1;
  for (int v = 0; v < MAX_VOICES; v++) {
    if (!e->voices[v].on) { slot = v; break; }
    if (e->voices[v].pos > op) { op = e->voices[v].pos; oldest = v; }
  }
  if (slot < 0) slot = oldest;
  e->voices[slot].id = id; e->voices[slot].pos = 0; e->voices[slot].gain = gain; e->voices[slot].on = 1;
  if (id < DK_NI) e->hits[id]++;
}

/* ---------------------------------------------------------------- loop events */
static int first_after(const dk_engine *e, double t) {   /* first event with tick > t */
  int i = 0; while (i < e->nev && e->ev[i].tick <= t) i++; return i;
}
static int first_at_or_after(const dk_engine *e, double t) {
  int i = 0; while (i < e->nev && e->ev[i].tick < t) i++; return i;
}
static int find_event(const dk_engine *e, int inst, int tick) {
  for (int i = 0; i < e->nev; i++) if (e->ev[i].inst == inst && e->ev[i].tick == tick) return i;
  return -1;
}
static void add_event(dk_engine *e, int inst, int tick, int vel, int32_t skip) {
  tick = ((tick % DK_LOOP_TICKS) + DK_LOOP_TICKS) % DK_LOOP_TICKS;
  if (find_event(e, inst, tick) >= 0 || e->nev >= DK_MAX_EVENTS) return;
  int i = e->nev;
  while (i > 0 && e->ev[i - 1].tick > tick) { e->ev[i] = e->ev[i - 1]; e->skip[i] = e->skip[i - 1]; i--; }
  e->ev[i].tick = (uint16_t)tick; e->ev[i].inst = (uint8_t)inst; e->ev[i].vel = (uint8_t)vel;
  e->skip[i] = skip; e->nev++;
  e->next_idx = e->playing ? first_after(e, e->tick_pos) : 0;
  e->revision++;
}
static void remove_event(dk_engine *e, int idx) {
  memmove(&e->ev[idx], &e->ev[idx + 1], (size_t)(e->nev - idx - 1) * sizeof(dk_event));
  memmove(&e->skip[idx], &e->skip[idx + 1], (size_t)(e->nev - idx - 1) * sizeof(int32_t));
  e->nev--;
  e->next_idx = e->playing ? first_after(e, e->tick_pos) : 0;
  e->revision++;
}

void dk_hit(dk_engine *e, int inst, int vel) {
  if (inst < 0 || inst >= DK_NI) return;
  if (vel < 1) vel = 1;
  if (vel > 127) vel = 127;
  sound(e, inst, vel_gain(vel));
  if (!(e->recording && e->playing)) return;
  double t = e->tick_pos;
  int tick = e->quantize ? (int)floor(t / DK_STEP_TICKS + 0.5) * DK_STEP_TICKS : (int)floor(t + 0.5);
  /* a hit snapped forward (still ahead of the playhead) must not sound again in this pass */
  int32_t skip = (tick > t && tick < DK_LOOP_TICKS) ? e->pass_no : -1;
  if (tick >= DK_LOOP_TICKS) { tick -= DK_LOOP_TICKS; skip = e->pass_no + 1; }
  add_event(e, inst, tick, vel, skip);
}

void dk_audition(dk_engine *e, int inst, int vel) {
  if (inst >= 0 && inst < DK_NI) sound(e, inst, vel_gain(vel));
}

int dk_gm_note_to_inst(int note) {
  switch (note) {
    case 35: case 36: return DK_KICK;
    case 37: case 38: case 40: return DK_SNARE;
    case 42: case 44: return DK_CHH;
    case 46: return DK_OHH;
    case 48: case 50: return DK_TOM_HI;
    case 45: case 47: return DK_TOM_MID;
    case 41: case 43: return DK_TOM_LO;
    case 39: return DK_CLAP;
    case 49: case 52: case 55: case 57: return DK_CRASH;
    case 51: case 53: case 59: return DK_RIDE;
    default: return -1;
  }
}

/* ---------------------------------------------------------------- transport */
/* x mod m in [0, m) — floor-based so the R36S build doesn't need glibc 2.38's fmod */
static double wrap(double x, double m) { return x - m * floor(x / m); }

static void start_at(dk_engine *e, double t) {
  e->playing = 1; e->tick_pos = t; e->next_idx = first_at_or_after(e, t); e->pass_no++;
  e->click_first = wrap(t, DK_PPQ) < 1e-6;   /* click the first beat only when starting on it */
}
void dk_play(dk_engine *e) { if (!e->playing) start_at(e, 0); }
void dk_stop(dk_engine *e) {
  if (e->recording) e->revision++;
  e->playing = 0; e->recording = 0; e->tick_pos = 0; e->next_idx = 0;
}
void dk_toggle_play(dk_engine *e) { if (e->playing) dk_stop(e); else dk_play(e); }
void dk_set_recording(dk_engine *e, int on) {
  on = !!on;
  if (on == e->recording) return;
  e->recording = on;
  if (on && !e->playing) dk_play(e);
  e->revision++;
}
void dk_toggle_record(dk_engine *e) { dk_set_recording(e, !e->recording); }
void dk_clear(dk_engine *e) { e->nev = 0; e->next_idx = 0; e->recording = 0; e->revision++; }

void dk_host_sync(dk_engine *e, int host_playing, double ppq, double bpm) {
  if (bpm > 0) { if (bpm < 20) bpm = 20; if (bpm > 400) bpm = 400; e->bpm = bpm; }
  if (host_playing) {
    double t = wrap(ppq * DK_PPQ, DK_LOOP_TICKS);
    if (!e->playing || !e->host_was_playing) start_at(e, t);
    else {
      double d = t - e->tick_pos;
      if (d > DK_LOOP_TICKS / 2) d -= DK_LOOP_TICKS;
      if (d < -DK_LOOP_TICKS / 2) d += DK_LOOP_TICKS;
      if (fabs(d) > 1.0) { e->tick_pos = t; e->next_idx = first_at_or_after(e, t); e->click_first = 0; }  /* locate / loop jump */
    }
    e->host_was_playing = 1;
  } else {
    if (e->host_was_playing) dk_stop(e);
    e->host_was_playing = 0;
  }
}

void dk_set_bpm(dk_engine *e, int bpm) {
  if (bpm < DK_BPM_MIN) bpm = DK_BPM_MIN;
  if (bpm > DK_BPM_MAX) bpm = DK_BPM_MAX;
  if (e->bpm != (double)bpm) { e->bpm = bpm; e->revision++; }
}
int dk_bpm(const dk_engine *e) { return (int)floor(e->bpm + 0.5); }
void dk_set_metronome(dk_engine *e, int on) { if (e->metronome != !!on) { e->metronome = !!on; e->revision++; } }
int dk_metronome(const dk_engine *e) { return e->metronome; }
void dk_set_quantize(dk_engine *e, int on) { if (e->quantize != !!on) { e->quantize = !!on; e->revision++; } }
int dk_quantize(const dk_engine *e) { return e->quantize; }
void dk_set_volume(dk_engine *e, float g) { e->volume = g < 0 ? 0 : g > 1.5f ? 1.5f : g; }

/* ---------------------------------------------------------------- sequencer view */
static int near_step(int tick, int step) { int d = tick - step * DK_STEP_TICKS; return d >= -DK_STEP_TICKS / 2 && d < DK_STEP_TICKS / 2; }
int dk_step_on(const dk_engine *e, int inst, int step) {
  for (int i = 0; i < e->nev; i++) if (e->ev[i].inst == inst && near_step(e->ev[i].tick, step)) return 1;
  /* the last half step wraps to step 0 */
  if (step == 0) for (int i = 0; i < e->nev; i++) if (e->ev[i].inst == inst && e->ev[i].tick >= DK_LOOP_TICKS - DK_STEP_TICKS / 2) return 1;
  return 0;
}
uint32_t dk_step_mask(const dk_engine *e, int inst) {
  uint32_t m = 0;
  for (int i = 0; i < e->nev; i++) if (e->ev[i].inst == inst) {
    int s = ((e->ev[i].tick + DK_STEP_TICKS / 2) / DK_STEP_TICKS) % DK_STEPS;
    m |= 1u << s;
  }
  return m;
}
void dk_toggle_step(dk_engine *e, int inst, int step) {
  if (inst < 0 || inst >= DK_NI || step < 0 || step >= DK_STEPS) return;
  int removed = 0;
  for (int i = e->nev - 1; i >= 0; i--) if (e->ev[i].inst == inst) {
    int s = ((e->ev[i].tick + DK_STEP_TICKS / 2) / DK_STEP_TICKS) % DK_STEPS;
    if (s == step) { remove_event(e, i); removed = 1; }
  }
  if (!removed) { add_event(e, inst, step * DK_STEP_TICKS, 127, -1); if (!e->playing) sound(e, inst, 1.0f); }
}

/* ---------------------------------------------------------------- queries */
int dk_playing(const dk_engine *e) { return e->playing; }
int dk_recording(const dk_engine *e) { return e->recording; }
double dk_position(const dk_engine *e) { return e->tick_pos; }
int dk_event_count(const dk_engine *e) { return e->nev; }
const dk_event *dk_events(const dk_engine *e) { return e->ev; }
uint32_t dk_hit_count(const dk_engine *e, int inst) { return inst >= 0 && inst < DK_NI ? e->hits[inst] : 0; }
float dk_level(const dk_engine *e) { return e->level; }
uint32_t dk_revision(const dk_engine *e) { return e->revision; }

/* ---------------------------------------------------------------- audio */
void dk_render(dk_engine *e, float *out_l, float *out_r, int n) {
  const double tps = e->bpm * DK_PPQ / 60.0 / e->sr;   /* ticks per sample */
  const float decay = (float)pow(0.9995, 44100.0 / e->sr);
  for (int s = 0; s < n; s++) {
    if (e->playing) {
      double prev = e->tick_pos;
      e->tick_pos += tps;
      if (e->tick_pos >= DK_LOOP_TICKS) { e->tick_pos -= DK_LOOP_TICKS; e->next_idx = 0; e->pass_no++; prev = -1; }
      while (e->next_idx < e->nev && e->ev[e->next_idx].tick <= e->tick_pos) {
        int i = e->next_idx++;
        if (e->skip[i] != e->pass_no) sound(e, e->ev[i].inst, 0.9f * vel_gain(e->ev[i].vel));
        else e->skip[i] = -1;
      }
      if (e->metronome) {                                  /* beat crossing */
        int pb = (int)floor(prev / DK_PPQ), nb = (int)floor(e->tick_pos / DK_PPQ);
        if (prev < 0 || e->click_first) pb = -1;
        if (nb != pb) sound(e, nb % DK_BEATS_PER_BAR == 0 ? CLICK_HI : CLICK_LO, 1.0f);
      }
      e->click_first = 0;
    }
    float mix = 0;
    for (int v = 0; v < MAX_VOICES; v++) {
      Voice *vo = &e->voices[v]; if (!vo->on) continue;
      mix += e->smp[vo->id][vo->pos] * vo->gain;
      if (++vo->pos >= e->smp_len[vo->id]) vo->on = 0;
    }
    mix = tanhf(mix * 0.9f) * e->volume;
    float a = fabsf(mix); e->level = a > e->level ? a : e->level * decay;
    out_l[s] = mix;
    if (out_r && out_r != out_l) out_r[s] = mix;
  }
}

/* ---------------------------------------------------------------- state */
#define MAGIC 0x4B44424Fu  /* "OBDK" little-endian */
static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static unsigned get16(const uint8_t *p) { return p[0] | (unsigned)p[1] << 8; }

size_t dk_save(const dk_engine *e, uint8_t *buf, size_t cap) {
  size_t need = 11 + (size_t)e->nev * 4;
  if (!buf || cap < need) return need;
  buf[0] = 'O'; buf[1] = 'B'; buf[2] = 'D'; buf[3] = 'K';
  put16(buf + 4, 2);
  put16(buf + 6, (unsigned)dk_bpm(e));
  buf[8] = (uint8_t)((e->metronome ? 1 : 0) | (e->quantize ? 2 : 0));
  put16(buf + 9, (unsigned)e->nev);
  uint8_t *p = buf + 11;
  for (int i = 0; i < e->nev; i++, p += 4) { put16(p, e->ev[i].tick); p[2] = e->ev[i].inst; p[3] = e->ev[i].vel; }
  return need;
}

int dk_load(dk_engine *e, const uint8_t *buf, size_t len) {
  if (!buf || len < 11 || memcmp(buf, "OBDK", 4) != 0) return 0;
  unsigned ver = get16(buf + 4), b = get16(buf + 6), fl = buf[8], n = get16(buf + 9);
  if (ver != 1 && ver != 2) return 0;
  size_t rec = ver == 1 ? 3 : 4;
  if (b >= DK_BPM_MIN && b <= DK_BPM_MAX) e->bpm = b;
  e->metronome = fl & 1; e->quantize = (fl & 2) != 0;
  e->nev = 0; e->next_idx = 0;
  const uint8_t *p = buf + 11;
  for (unsigned i = 0; i < n && i < DK_MAX_EVENTS && (size_t)(p - buf) + rec <= len; i++, p += rec) {
    unsigned t = get16(p), in = p[2], v = ver == 1 ? 127 : p[3];
    if (t < DK_LOOP_TICKS && in < DK_NI) add_event(e, (int)in, (int)t, v ? (int)v : 127, -1);
  }
  e->next_idx = e->playing ? first_after(e, e->tick_pos) : 0;
  e->revision++;
  return 1;
}
