/* Engine tests that the libretro front end can't reach: other sample rates, host sync,
 * velocity, GM mapping, state v1/v2. Build: make engine-test */
#include "../engine/dk_engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int fails;
#define CHECK(c, ...) do { if (c) { printf("PASS "); } else { printf("FAIL "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)

/* count onsets: windows of 'win' samples whose peak jumps above thr after being quiet */
static float peak(const float *b, int n) { float m = 0; for (int i = 0; i < n; i++) if (fabsf(b[i]) > m) m = fabsf(b[i]); return m; }

int main(void) {
  /* sample rates */
  double rates[] = { 44100, 48000, 96000 };
  for (int r = 0; r < 3; r++) {
    dk_engine *e = dk_create(rates[r]);
    float buf[512]; float pk[DK_NI];
    for (int i = 0; i < DK_NI; i++) {
      dk_hit(e, i, 127); float m = 0;
      for (int k = 0; k < 20; k++) { dk_render(e, buf, NULL, 512); float p = peak(buf, 512); if (p > m) m = p; }
      for (int k = 0; k < 400; k++) dk_render(e, buf, NULL, 512);   /* let it ring out */
      pk[i] = m;
    }
    int ok = 1; for (int i = 0; i < DK_NI; i++) if (pk[i] < 0.2f || pk[i] > 1.0f) ok = 0;
    CHECK(ok, "all drums sound at %.0f Hz (kick %.2f, ride %.2f)", rates[r], pk[0], pk[9]);
    dk_destroy(e);
  }

  /* velocity */
  {
    dk_engine *e = dk_create(48000); float b[4800];
    dk_audition(e, DK_SNARE, 127); dk_render(e, b, NULL, 4800); float hi = peak(b, 4800);
    for (int k = 0; k < 20; k++) dk_render(e, b, NULL, 4800);
    dk_audition(e, DK_SNARE, 40); dk_render(e, b, NULL, 4800); float lo = peak(b, 4800);
    CHECK(lo < hi * 0.5f && lo > 0.02f, "velocity 40 is quieter than 127 (%.2f vs %.2f)", lo, hi);
    dk_destroy(e);
  }

  /* GM map */
  CHECK(dk_gm_note_to_inst(36) == DK_KICK && dk_gm_note_to_inst(38) == DK_SNARE && dk_gm_note_to_inst(42) == DK_CHH &&
        dk_gm_note_to_inst(46) == DK_OHH && dk_gm_note_to_inst(50) == DK_TOM_HI && dk_gm_note_to_inst(47) == DK_TOM_MID &&
        dk_gm_note_to_inst(43) == DK_TOM_LO && dk_gm_note_to_inst(39) == DK_CLAP && dk_gm_note_to_inst(49) == DK_CRASH &&
        dk_gm_note_to_inst(51) == DK_RIDE && dk_gm_note_to_inst(60) == -1, "General MIDI drum map");

  /* host sync: follow the host position, play steps at the host's beats */
  {
    const double sr = 48000; dk_engine *e = dk_create(sr);
    dk_set_metronome(e, 0);
    for (int s = 0; s < DK_STEPS; s += 4) dk_toggle_step(e, DK_KICK, s);   /* quarter-note kicks */
    CHECK(dk_step_mask(e, DK_KICK) == 0x11111111u, "step mask 0x%08x", dk_step_mask(e, DK_KICK));
    float b[480]; double bpm = 120, ppq = 3.5;  /* start mid-bar, on the "and" of beat 4 */
    int hits = 0; uint32_t last = dk_hit_count(e, DK_KICK); double first_hit_ppq = -1;
    for (int blk = 0; blk < 400; blk++) {                      /* 4 s = 8 beats */
      dk_host_sync(e, 1, ppq, bpm);
      dk_render(e, b, NULL, 480);
      uint32_t h = dk_hit_count(e, DK_KICK);
      if (h != last) { hits += (int)(h - last); last = h; if (first_hit_ppq < 0) first_hit_ppq = ppq; }
      ppq += 480 / sr * bpm / 60;
    }
    CHECK(hits == 8, "host-synced kicks: %d in 8 beats (expect 8)", hits);
    CHECK(fabs(first_hit_ppq - 3.99) < 0.02, "first kick on beat 5 (block at ppq %.3f)", first_hit_ppq);
    CHECK(dk_bpm(e) == 120, "engine tempo follows host: %d", dk_bpm(e));
    /* host locates back to 0 → kick at once */
    last = dk_hit_count(e, DK_KICK);
    dk_host_sync(e, 1, 0.0, bpm); dk_render(e, b, NULL, 480);
    CHECK(dk_hit_count(e, DK_KICK) == last + 1, "locate to bar 1 plays the downbeat");
    dk_host_sync(e, 0, 0, bpm);
    CHECK(!dk_playing(e), "host stop stops the engine");
    dk_play(e); dk_host_sync(e, 0, 0, bpm); dk_render(e, b, NULL, 480);
    CHECK(dk_playing(e), "internal transport runs while the host is stopped");
    dk_destroy(e);
  }

  /* recording against the host clock */
  {
    const double sr = 44100; dk_engine *e = dk_create(sr); float b[441];
    dk_set_metronome(e, 0); dk_set_recording(e, 1);
    double ppq = 0; int hit_done = 0;
    for (int blk = 0; blk < 100; blk++) {
      dk_host_sync(e, 1, ppq, 100);
      if (!hit_done && ppq >= 1.02) { dk_hit(e, DK_SNARE, 90); hit_done = 1; }
      dk_render(e, b, NULL, 441); ppq += 441 / sr * 100 / 60;
    }
    const dk_event *ev = dk_events(e);
    CHECK(dk_event_count(e) == 1 && ev[0].tick == DK_PPQ && ev[0].vel == 90, "hit near beat 2 is quantized to tick %d (vel %d)",
          dk_event_count(e) ? ev[0].tick : -1, dk_event_count(e) ? ev[0].vel : -1);
    dk_destroy(e);
  }

  /* state v2 round trip and v1 compatibility */
  {
    dk_engine *a = dk_create(44100), *b = dk_create(48000);
    dk_set_bpm(a, 133); dk_set_quantize(a, 0); dk_toggle_step(a, DK_RIDE, 7); dk_toggle_step(a, DK_CLAP, 31);
    uint8_t buf[DK_STATE_MAX]; size_t n = dk_save(a, buf, sizeof buf);
    CHECK(dk_load(b, buf, n) && dk_bpm(b) == 133 && !dk_quantize(b) && dk_step_mask(b, DK_RIDE) == 1u << 7 &&
          dk_step_mask(b, DK_CLAP) == 1u << 31, "state v2 round trip (%zu bytes)", n);
    uint8_t v1[] = { 'O', 'B', 'D', 'K', 1, 0, 90, 0, 1, 2, 0, 48, 0, DK_KICK, 0, 0, DK_SNARE };
    CHECK(dk_load(b, v1, sizeof v1) && dk_bpm(b) == 90 && dk_event_count(b) == 2 && dk_events(b)[0].vel == 127, "reads v1 files from the R36S release");
    CHECK(!dk_load(b, (const uint8_t *)"nonsense", 8), "rejects garbage");
    dk_destroy(a); dk_destroy(b);
  }

  printf(fails ? "%d FAILED\n" : "ALL PASSED\n", fails);
  return fails != 0;
}
