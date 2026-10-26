#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "stb_easy_font.h"

extern "C" {
struct retro_system_info {
  const char *library_name;
  const char *library_version;
  const char *valid_extensions;
  bool need_fullpath;
  bool block_extract;
};
struct retro_variable {
  const char *key;
  const char *value;
};
struct retro_log_callback {
  void (*log)(int level, const char *fmt, ...);
};
unsigned retro_api_version(void);
void retro_get_system_info(struct retro_system_info *info);
void retro_set_environment(bool (*cb)(unsigned, void *));
void retro_set_video_refresh(void (*cb)(const void *, unsigned, unsigned, size_t));
void retro_set_audio_sample(void (*cb)(int16_t, int16_t));
void retro_set_audio_sample_batch(size_t (*cb)(const int16_t *, size_t));
void retro_set_input_poll(void (*cb)(void));
void retro_set_input_state(int16_t (*cb)(unsigned, unsigned, unsigned, unsigned));
void retro_init(void);
}

static SDL_Renderer *R;
static char lineas[12][100];
static int nl = 0;
static char logbuf[6][110];
static int nlog = 0;
static int g_cmd = 0;
static int g_ncmd = 0;

static void log_core(int nivel, const char *fmt, ...) {
  (void)nivel;
  char *d = logbuf[nlog % 6];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(d, sizeof(logbuf[0]), fmt, ap);
  va_end(ap);
  for (char *p = d; *p; p++) {
    if (*p == '\n' || *p == '\r') *p = ' ';
  }
  nlog++;
}

static bool entorno(unsigned cmd, void *data) {
  g_cmd = (int)cmd;
  g_ncmd++;
  switch (cmd) {
    case 3: *(bool *)data = true; return true;
    case 6: return true;
    case 9: *(const char **)data = "/data/swan"; return true;
    case 10: return true;
    case 15: ((retro_variable *)data)->value = NULL; return false;
    case 16: return true;
    case 17: *(bool *)data = false; return true;
    case 27: ((retro_log_callback *)data)->log = log_core; return true;
    case 31: *(const char **)data = "/data/swan"; return true;
    default: return false;
  }
}

static void video(const void *d, unsigned w, unsigned h, size_t p) { (void)d; (void)w; (void)h; (void)p; }
static void audio1(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audio2(const int16_t *d, size_t n) { (void)d; return n; }
static void poll(void) {}
static int16_t entrada(unsigned a, unsigned b, unsigned c, unsigned d) { (void)a; (void)b; (void)c; (void)d; return 0; }

static void texto(float x, float y, const char *s) {
  static char buf[60000];
  int n = stb_easy_font_print(x, y, (char *)s, NULL, buf, sizeof(buf));
  SDL_SetRenderDrawColor(R, 255, 255, 255, 255);
  for (int i = 0; i < n; i++) {
    float *v = (float *)(buf + i * 64);
    SDL_Rect q = {(int)v[0], (int)v[1], (int)(v[8] - v[0]), (int)(v[9] - v[1])};
    SDL_RenderFillRect(R, &q);
  }
}

static void dibujar(void) {
  SDL_SetRenderDrawColor(R, 0, 0, 80, 255);
  SDL_RenderClear(R);
  SDL_RenderSetScale(R, 3, 3);
  float y = 10;
  for (int i = 0; i < nl; i++) {
    texto(10, y, lineas[i]);
    y += 14;
  }
  char t[120];
  snprintf(t, sizeof(t), "Ultimo cmd entorno: %d (total %d)", g_cmd, g_ncmd);
  texto(10, y, t);
  y += 14;
  int ini = nlog > 6 ? nlog - 6 : 0;
  for (int i = ini; i < nlog; i++) {
    snprintf(t, sizeof(t), "LOG: %.80s", logbuf[i % 6]);
    texto(10, y, t);
    y += 14;
  }
  SDL_RenderPresent(R);
}

static void esperar(int frames) {
  for (int i = 0; i < frames; i++) {
    dibujar();
    SDL_Delay(16);
  }
}

static void paso(const char *s) {
  snprintf(lineas[nl], sizeof(lineas[0]), "%.90s", s);
  if (nl < 11) nl++;
  esperar(100);
}

int main(void) {
  SDL_Init(SDL_INIT_VIDEO);
  SDL_Window *w = SDL_CreateWindow("swan", 0, 0, 1920, 1080, 0);
  R = SDL_CreateRenderer(w, -1, 0);

  struct retro_system_info info;
  memset(&info, 0, sizeof(info));
  retro_get_system_info(&info);
  char t[128];
  snprintf(t, sizeof(t), "%s %s API %u", info.library_name ? info.library_name : "?", info.library_version ? info.library_version : "?", retro_api_version());
  paso(t);

  paso("1) antes de set_environment");
  retro_set_environment(entorno);
  retro_set_video_refresh(video);
  retro_set_audio_sample(audio1);
  retro_set_audio_sample_batch(audio2);
  retro_set_input_poll(poll);
  retro_set_input_state(entrada);
  paso("2) antes de retro_init");
  retro_init();
  paso("3) retro_init OK");
  esperar(1800);
  SDL_Quit();
  return 0;
}
