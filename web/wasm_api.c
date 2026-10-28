/* WebAssembly surface for the AudioWorklet: one engine, fixed buffers, no pointers in JS except two views. */
#include "../engine/dk_engine.h"

#define EXPORT(name) __attribute__((export_name(#name)))
#define BLOCK 128

static dk_engine *eng;
static float out[BLOCK];
static uint8_t state[DK_STATE_MAX];
static uint32_t snap[DK_NI * 2 + 8];   /* step masks, hit counts, misc */

EXPORT(init) int init(double sr) { if (eng) dk_destroy(eng); eng = dk_create(sr); return eng != 0; }
EXPORT(out_ptr) float *out_ptr(void) { return out; }
EXPORT(state_ptr) uint8_t *state_ptr(void) { return state; }
EXPORT(snap_ptr) uint32_t *snap_ptr(void) { return snap; }
EXPORT(render) void render(int n) { dk_render(eng, out, 0, n > BLOCK ? BLOCK : n); }

EXPORT(hit) void hit(int inst, int vel) { dk_hit(eng, inst, vel); }
EXPORT(audition) void audition(int inst, int vel) { dk_audition(eng, inst, vel); }
EXPORT(gm) int gm(int note) { return dk_gm_note_to_inst(note); }
EXPORT(toggle_play) void toggle_play(void) { dk_toggle_play(eng); }
EXPORT(play) void play(void) { dk_play(eng); }
EXPORT(stop) void stop(void) { dk_stop(eng); }
EXPORT(toggle_record) void toggle_record(void) { dk_toggle_record(eng); }
EXPORT(clear) void clear(void) { dk_clear(eng); }
EXPORT(set_bpm) void set_bpm(int b) { dk_set_bpm(eng, b); }
EXPORT(set_metronome) void set_metronome(int on) { dk_set_metronome(eng, on); }
EXPORT(set_quantize) void set_quantize(int on) { dk_set_quantize(eng, on); }
EXPORT(set_volume) void set_volume(float g) { dk_set_volume(eng, g); }
EXPORT(toggle_step) void toggle_step(int inst, int step) { dk_toggle_step(eng, inst, step); }
EXPORT(save) int save(void) { return (int)dk_save(eng, state, sizeof state); }
EXPORT(load) int load(int len) { return dk_load(eng, state, (size_t)len); }
EXPORT(position) double position(void) { return dk_position(eng); }

/* snapshot for the UI: [0..9] step masks, [10..19] hit counts, then
   playing, recording, bpm, metronome, quantize, events, revision, level*1e6 */
EXPORT(snapshot) void snapshot(void) {
  for (int i = 0; i < DK_NI; i++) { snap[i] = dk_step_mask(eng, i); snap[DK_NI + i] = dk_hit_count(eng, i); }
  uint32_t *m = snap + DK_NI * 2;
  m[0] = dk_playing(eng); m[1] = dk_recording(eng); m[2] = dk_bpm(eng); m[3] = dk_metronome(eng);
  m[4] = dk_quantize(eng); m[5] = dk_event_count(eng); m[6] = dk_revision(eng); m[7] = (uint32_t)(dk_level(eng) * 1e6f);
}
