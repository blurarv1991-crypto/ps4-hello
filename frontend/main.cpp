#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
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
struct retro_game_info {
  const char *path;
  const void *data;
  size_t size;
  const char *meta;
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
bool retro_load_game(const struct retro_game_info *game);
void retro_run(void);
}

static SDL_Renderer *R;
static SDL_threadID g_main = 0;
static char lineas[16][100];
static int nl = 0;
static char logbuf[10][130];
static int nlog = 0;
static int g_cmd = 0;
static int g_ncmd = 0;
static unsigned g_vw = 0, g_vh = 0;
static int g_vc = 0, g_vnz = 0;
static int g_frames = 0, g_ms = 0;

static void dibujar(void);

static void log_core(int nivel, const char *fmt, ...) {
  (void)nivel;
  char *d = logbuf[nlog % 10];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(d, sizeof(logbuf[0]), fmt, ap);
  va_end(ap);
  for (char *p = d; *p; p++) {
    if (*p == '\n' || *p == '\r') *p = ' ';
  }
  nlog++;
  if (SDL_ThreadID() == g_main) {
    dibujar();
    SDL_Delay(150);
  }
}

static bool contiene(const char *s, const char *sub) {
  if (!s) return false;
  size_t n = strlen(sub);
  for (; *s; s++) {
    size_t i = 0;
    while (i < n && s[i] && ((s[i] | 32) == (sub[i] | 32))) i++;
    if (i == n) return true;
  }
  return false;
}

static bool entorno(unsigned cmd, void *data) {
  g_cmd = (int)cmd;
  g_ncmd++;
  switch (cmd) {
    case 3: *(bool *)data = true; return true;
    case 6: return true;
    case 8: return true;
    case 9: *(const char **)data = "/data/swan"; return true;
    case 10: return true;
    case 11: return true;
    case 15: {
      retro_variable *v = (retro_variable *)data;
      if (contiene(v->key, "fastmem")) {
        v->value = "Disabled";
        return true;
      }
      v->value = NULL;
      return false;
    }
    case 16: return true;
    case 17: *(bool *)data = false; return true;
    case 27: ((retro_log_callback *)data)->log = log_core; return true;
    case 31: *(const char **)data = "/data/swan"; return true;
    default: return false;
  }
}

static void video(const void *d, unsigned w, unsigned h, size_t p) {
  g_vw = w;
  g_vh = h;
  g_vc++;
  if (d) {
    const unsigned char *b = (const unsigned char *)d;
    size_t n = (size_t)h * p;
    if (n > 400000) n = 400000;
    int nz = 0;
    for (size_t i = 0; i < n; i += 7) {
      if (b[i]) { nz = 1; break; }
    }
    g_vnz = nz;
  }
}
static void audio1(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audio2(const int16_t *d, size_t n) { (void)d; return n; }
static void poll(void) {}
static int16_t entrada(unsigned a, unsigned b, unsigned c, unsigned d) { (void)a; (void)b; (void)c; (void)d; return 0; }

static bool existe(const char *p) {
  FILE *f = fopen(p, "rb");
  if (!f) return false;
  fclose(f);
  return true;
}

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
  SDL_RenderSetScale(R, 2, 2);
  float y = 10;
  char t[200];
  for (int i = 0; i < nl; i++) {
    texto(10, y, lineas[i]);
    y += 14;
  }
  snprintf(t, sizeof(t), "Ultimo cmd entorno: %d (total %d)", g_cmd, g_ncmd);
  texto(10, y, t);
  y += 14;
  snprintf(t, sizeof(t), "Frames: %d  ms/frame: %d  video: %ux%u  px!=0: %d", g_frames, g_ms, g_vw, g_vh, g_vnz);
  texto(10, y, t);
  y += 14;
  int ini = nlog > 10 ? nlog - 10 : 0;
  for (int i = ini; i < nlog; i++) {
    snprintf(t, sizeof(t), "LOG: %.120s", logbuf[i % 10]);
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
  if (nl < 15) nl++;
  esperar(100);
}

static void diag(const char *nombre) {
  char p[120], t[160];
  snprintf(p, sizeof(p), "/data/swan/%s", nombre);
  FILE *f = fopen(p, "rb");
  if (!f) return;
  fseek(f, 0, SEEK_END);
  long long n = (long long)ftell(f);
  fclose(f);
  struct stat sb;
  memset(&sb, 0, sizeof(sb));
  int r = stat(p, &sb);
  snprintf(t, sizeof(t), "D) %s ftell=%lld stat=%d size=%lld sz=%d", nombre, n, r, (long long)sb.st_size, (int)sizeof(sb));
  paso(t);
}

int main(void) {
  SDL_Init(SDL_INIT_VIDEO);
  SDL_Window *w = SDL_CreateWindow("swan", 0, 0, 1920, 1080, 0);
  R = SDL_CreateRenderer(w, -1, 0);
  g_main = SDL_ThreadID();
  mkdir("/data/swan", 0777);

  struct retro_system_info info;
  memset(&info, 0, sizeof(info));
  retro_get_system_info(&info);
  char t[160];
  snprintf(t, sizeof(t), "%s %s API %u", info.library_name ? info.library_name : "?", info.library_version ? info.library_version : "?", retro_api_version());
  paso(t);

  retro_set_environment(entorno);
  retro_set_video_refresh(video);
  retro_set_audio_sample(audio1);
  retro_set_audio_sample_batch(audio2);
  retro_set_input_poll(poll);
  retro_set_input_state(entrada);
  paso("1) retro_init...");
  retro_init();
  paso("2) retro_init OK");

  diag("scph7502-eu.bin");
  diag("scph7502.bin");
  diag("juego.bin");

  const char *juegos[] = {"/data/swan/juego.chd", "/data/swan/juego.cue", "/data/swan/juego.pbp", "/data/swan/juego.iso", "/data/swan/juego.bin"};
  const char *ruta = NULL;
  for (int i = 0; i < 5; i++) {
    if (existe(juegos[i])) { ruta = juegos[i]; break; }
  }
  if (!ruta) {
    paso("Sin juego en /data/swan");
    esperar(1800);
    SDL_Quit();
    return 0;
  }
  snprintf(t, sizeof(t), "Juego: %s", ruta);
  paso(t);

  paso("3) retro_load_game...");
  struct retro_game_info gi;
  memset(&gi, 0, sizeof(gi));
  gi.path = ruta;
  bool ok = retro_load_game(&gi);
  if (!ok) {
    paso("3) load_game DEVOLVIO FALSE");
    esperar(1800);
    SDL_Quit();
    return 0;
  }
  paso("4) load_game OK; ejecutando 600 frames...");
  unsigned t0 = SDL_GetTicks();
  for (int i = 0; i < 600; i++) {
    retro_run();
    g_frames = i + 1;
    g_ms = (int)((SDL_GetTicks() - t0) / (unsigned)(i + 1));
    dibujar();
  }
  paso("5) 600 frames terminados");
  esperar(1800);
  SDL_Quit();
  return 0;
}
