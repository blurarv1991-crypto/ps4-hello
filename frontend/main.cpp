#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include "stb_easy_font.h"

#ifndef SHM_ANON
#define SHM_ANON ((const char *)1)
#endif

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
static char lineas[16][100];
static int nl = 0;
static char logbuf[6][110];
static int nlog = 0;
static int g_cmd = 0;
static int g_ncmd = 0;
static unsigned g_vw = 0, g_vh = 0;
static int g_vc = 0, g_vnz = 0;
static int g_frames = 0, g_ms = 0;

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
    case 8: return true;
    case 9: *(const char **)data = "/data/swan"; return true;
    case 10: return true;
    case 11: return true;
    case 15: ((retro_variable *)data)->value = NULL; return false;
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
  SDL_RenderSetScale(R, 3, 3);
  float y = 10;
  char t[160];
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
  if (nl < 15) nl++;
  esperar(100);
}

static void pruebas(void) {
  char t[160];
  unsigned mb[4] = {64, 128, 256, 512};
  strcpy(t, "T1) RAM:");
  for (int i = 0; i < 4; i++) {
    size_t n = (size_t)mb[i] * 1024 * 1024;
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    char s[24];
    if (p != MAP_FAILED) {
      snprintf(s, sizeof(s), " %uMB:si", mb[i]);
      munmap(p, n);
    } else {
      snprintf(s, sizeof(s), " %uMB:no", mb[i]);
    }
    strncat(t, s, sizeof(t) - strlen(t) - 1);
  }
  paso(t);

  paso("T2) mmap normal + mprotect a ejecutable...");
  void *d = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (d == MAP_FAILED) {
    paso("T2) FALLO el mmap normal");
  } else {
    ((unsigned char *)d)[0] = 0xC3;
    int r = mprotect(d, 65536, PROT_READ | PROT_EXEC);
    snprintf(t, sizeof(t), "T2) mprotect a ejecutable devolvio %d", r);
    paso(t);
    if (r == 0) {
      paso("T2) ejecutando codigo...");
      ((void (*)(void))d)();
      paso("T2) OK: codigo ejecutado");
    }
  }

  paso("T3) mmap directo con permiso de ejecucion...");
  void *c = mmap(NULL, 65536, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (c == MAP_FAILED) {
    paso("T3) FALLO: no da memoria ejecutable");
  } else {
    ((unsigned char *)c)[0] = 0xC3;
    paso("T3) ejecutando codigo...");
    ((void (*)(void))c)();
    paso("T3) OK: codigo ejecutado");
  }

  paso("T4) memoria compartida shm_open...");
  int fd = shm_open(SHM_ANON, O_RDWR, 0600);
  if (fd < 0) {
    paso("T4) shm_open FALLO");
  } else {
    int rf = ftruncate(fd, 1 << 20);
    void *m = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    snprintf(t, sizeof(t), "T4) shm_open OK, ftruncate=%d, mmap=%s", rf, m == MAP_FAILED ? "FALLO" : "OK");
    paso(t);
  }
}

int main(void) {
  SDL_Init(SDL_INIT_VIDEO);
  SDL_Window *w = SDL_CreateWindow("swan", 0, 0, 1920, 1080, 0);
  R = SDL_CreateRenderer(w, -1, 0);
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

  pruebas();

  const char *bios[] = {"scph5501.bin", "scph1001.bin", "scph5500.bin", "scph5502.bin", "scph7001.bin", "scph7502.bin"};
  char b[160] = "BIOS:";
  int hay = 0;
  for (int i = 0; i < 6; i++) {
    char p[96];
    snprintf(p, sizeof(p), "/data/swan/%s", bios[i]);
    if (existe(p)) {
      strncat(b, " ", sizeof(b) - strlen(b) - 1);
      strncat(b, bios[i], sizeof(b) - strlen(b) - 1);
      hay = 1;
    }
  }
  if (!hay) strncat(b, " ninguno", sizeof(b) - strlen(b) - 1);
  paso(b);

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
