/*
 * ours-blanc Drum Kit engine — portable C (no I/O, no globals).
 *
 * Shared by the R36S libretro core, the web version (WebAssembly + AudioWorklet)
 * and the VST3 / AU / standalone plug-in. Ten synthesized drums, a 2-bar loop on a
 * 96-PPQ grid that both live recording and the 32-step sequencer write to, a
 * metronome, and an optional host transport (plug-ins follow the DAW's tempo/position).
 *
 * Threading: an engine is not thread-safe. Call everything from the audio thread,
 * or serialize access yourself.
 */
#ifndef DK_ENGINE_H
#define DK_ENGINE_H
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { DK_KICK, DK_SNARE, DK_CHH, DK_OHH, DK_TOM_HI, DK_TOM_MID, DK_TOM_LO, DK_CLAP, DK_CRASH, DK_RIDE, DK_NI };

#define DK_PPQ 96
#define DK_BEATS_PER_BAR 4
#define DK_BARS 2
#define DK_LOOP_TICKS (DK_PPQ * DK_BEATS_PER_BAR * DK_BARS) /* 768 */
#define DK_STEP_TICKS (DK_PPQ / 4)                          /* 24 = 16th note */
#define DK_STEPS (DK_LOOP_TICKS / DK_STEP_TICKS)             /* 32 */
#define DK_MAX_EVENTS 4096
#define DK_BPM_MIN 40
#define DK_BPM_MAX 240
#define DK_STATE_MAX (11 + DK_MAX_EVENTS * 4)

typedef struct dk_engine dk_engine;
typedef struct { uint16_t tick; uint8_t inst, vel; } dk_event;

extern const char *const dk_inst_name[DK_NI];   /* "KICK", "SNARE", "CL HAT", ... */
extern const uint32_t dk_inst_color[DK_NI];      /* 0xRRGGBB */

dk_engine *dk_create(double sample_rate);
void dk_destroy(dk_engine *e);
double dk_sample_rate(const dk_engine *e);

/* play a drum now (vel 1..127); while recording, the hit is also written to the loop */
void dk_hit(dk_engine *e, int inst, int vel);
/* play a drum without recording it */
void dk_audition(dk_engine *e, int inst, int vel);
/* map a General MIDI drum note to an instrument (-1 if unmapped) */
int dk_gm_note_to_inst(int note);

/* transport (internal) */
void dk_play(dk_engine *e);
void dk_stop(dk_engine *e);           /* also ends recording */
void dk_toggle_play(dk_engine *e);
void dk_toggle_record(dk_engine *e);  /* starts playback if needed */
void dk_set_recording(dk_engine *e, int on);
void dk_clear(dk_engine *e);          /* empties the loop, ends recording */

/* host transport: call once per audio block before dk_render.
 * host_playing = 0 hands the transport back to the engine (and stops it if the host just stopped). */
void dk_host_sync(dk_engine *e, int host_playing, double ppq_position, double bpm);

void dk_set_bpm(dk_engine *e, int bpm);
int dk_bpm(const dk_engine *e);
void dk_set_metronome(dk_engine *e, int on);
int dk_metronome(const dk_engine *e);
void dk_set_quantize(dk_engine *e, int on);
int dk_quantize(const dk_engine *e);
void dk_set_volume(dk_engine *e, float gain);   /* master, 0..1.5 */

/* sequencer view: a step is "on" when an event of that drum sits within ±half a step */
int dk_step_on(const dk_engine *e, int inst, int step);
uint32_t dk_step_mask(const dk_engine *e, int inst);  /* bit s = step s */
void dk_toggle_step(dk_engine *e, int inst, int step); /* auditions the drum when stopped */

/* state for UIs */
int dk_playing(const dk_engine *e);
int dk_recording(const dk_engine *e);
double dk_position(const dk_engine *e);      /* 0 .. DK_LOOP_TICKS */
int dk_event_count(const dk_engine *e);
const dk_event *dk_events(const dk_engine *e);
uint32_t dk_hit_count(const dk_engine *e, int inst); /* increments each time a drum sounds (for pad flashes) */
float dk_level(const dk_engine *e);           /* output peak, decaying */
uint32_t dk_revision(const dk_engine *e);     /* increments when the pattern or settings change */

/* render n frames; out_r may be NULL (mono) or equal to out_l */
void dk_render(dk_engine *e, float *out_l, float *out_r, int n);

/* pattern + settings: "OBDK" v2 (v1 from the first R36S release is still read) */
size_t dk_save(const dk_engine *e, uint8_t *buf, size_t cap);
int dk_load(dk_engine *e, const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif
