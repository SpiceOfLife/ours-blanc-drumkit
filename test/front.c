// Scripted libretro frontend for testing: frontend <core.so> <script> <outdir>
// script lines: "<frame> <button,button,...|-> [snap:<name>]"  (buttons held during that frame)
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "../src/libretro.h"
static const void *fb; static unsigned fw, fh; static size_t fp;
static int16_t *audio; static size_t na, cap;
static uint32_t pad; static const char *outdir;
static bool env(unsigned cmd, void *d) {
  switch (cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: return *(enum retro_pixel_format*)d == RETRO_PIXEL_FORMAT_XRGB8888;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char**)d = outdir; return true;
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME: case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS: return true;
    default: return false;
  }
}
static void video(const void *d, unsigned w, unsigned h, size_t p) { if (d) { fb = d; fw = w; fh = h; fp = p; } }
static size_t abatch(const int16_t *d, size_t n) { if (na + n * 2 > cap) { cap = (na + n * 2) * 2; audio = realloc(audio, cap * 2); } memcpy(audio + na, d, n * 4); na += n * 2; return n; }
static void asample(int16_t l, int16_t r) { (void)l; (void)r; }
static void poll(void) {}
static int16_t state(unsigned port, unsigned dev, unsigned idx, unsigned id) { (void)idx; return port == 0 && dev == RETRO_DEVICE_JOYPAD && (pad >> id & 1); }
static const char *NAMES[16] = { "B","Y","SELECT","START","UP","DOWN","LEFT","RIGHT","A","X","L","R","L2","R2","L3","R3" };
static void snap(const char *name) {
  char p[512]; snprintf(p, sizeof p, "%s/%s.ppm", outdir, name);
  FILE *f = fopen(p, "wb"); fprintf(f, "P6\n%u %u\n255\n", fw, fh);
  for (unsigned y = 0; y < fh; y++) for (unsigned x = 0; x < fw; x++) { uint32_t c = ((const uint32_t*)((const char*)fb + y * fp))[x]; fputc(c >> 16 & 255, f); fputc(c >> 8 & 255, f); fputc(c & 255, f); }
  fclose(f);
}
#define L(n) n##_t n = (n##_t)dlsym(h, #n)
typedef void (*retro_init_t)(void); typedef void (*retro_deinit_t)(void); typedef bool (*retro_load_game_t)(const struct retro_game_info*);
typedef void (*retro_run_t)(void); typedef void (*retro_unload_game_t)(void); typedef void (*retro_set_environment_t)(retro_environment_t);
typedef void (*retro_set_video_refresh_t)(retro_video_refresh_t); typedef void (*retro_set_audio_sample_t)(retro_audio_sample_t);
typedef void (*retro_set_audio_sample_batch_t)(retro_audio_sample_batch_t); typedef void (*retro_set_input_poll_t)(retro_input_poll_t);
typedef void (*retro_set_input_state_t)(retro_input_state_t);
int main(int argc, char **argv) {
  outdir = argv[3];
  void *h = dlopen(argv[1], RTLD_NOW); if (!h) { puts(dlerror()); return 1; }
  L(retro_set_environment); L(retro_set_video_refresh); L(retro_set_audio_sample); L(retro_set_audio_sample_batch);
  L(retro_set_input_poll); L(retro_set_input_state); L(retro_init); L(retro_deinit); L(retro_load_game); L(retro_run); L(retro_unload_game);
  retro_set_environment(env); retro_set_video_refresh(video); retro_set_audio_sample(asample); retro_set_audio_sample_batch(abatch);
  retro_set_input_poll(poll); retro_set_input_state(state);
  retro_init(); if (!retro_load_game(NULL)) { puts("load failed"); return 2; }
  FILE *s = fopen(argv[2], "r"); char line[256]; int frame = 0;
  while (fgets(line, sizeof line, s)) {
    int fr; char btn[128] = "-", extra[128] = "";
    if (sscanf(line, "%d %127s %127s", &fr, btn, extra) < 2) continue;
    while (frame < fr) { pad = 0; retro_run(); frame++; }
    pad = 0;
    if (strcmp(btn, "-")) { char *t = strtok(btn, ","); while (t) { for (int i = 0; i < 16; i++) if (!strcmp(t, NAMES[i])) pad |= 1u << i; t = strtok(NULL, ","); } }
    retro_run(); frame++;
    if (!strncmp(extra, "snap:", 5)) snap(extra + 5);
  }
  retro_unload_game(); retro_deinit();
  char p[512]; snprintf(p, sizeof p, "%s/audio.raw", outdir); FILE *a = fopen(p, "wb"); fwrite(audio, 2, na, a); fclose(a);
  printf("frames=%d audio_samples=%zu\n", frame, na / 2);
  return 0;
}
