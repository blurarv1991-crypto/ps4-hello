#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include "stb_easy_font.h"

#ifndef SHM_ANON
#define SHM_ANON ((char *)1)
#endif

static long long tam_ruta(const char *p) {
  FILE *f = fopen(p, "rb");
  if (!f) return -1;
  fseek(f, 0, SEEK_END);
  long long n = (long long)ftell(f);
  fclose(f);
  return n;
}

extern "C" {
int __real_stat(const char *p, struct stat *s);
int __real_lstat(const char *p, struct stat *s);
int __real_fstat(int fd, struct stat *s);

int __wrap_stat(const char *p, struct stat *s) {
  int r = __real_stat(p, s);
  if (r == 0 && !S_ISDIR(s->st_mode)) {
    long long n = tam_ruta(p);
    if (n >= 0) s->st_size = n;
  }
  return r;
}

int __wrap_lstat(const char *p, struct stat *s) {
  int r = __real_lstat(p, s);
  if (r == 0 && !S_ISDIR(s->st_mode)) {
    long long n = tam_ruta(p);
    if (n >= 0) s->st_size = n;
  }
  return r;
}

int __wrap_fstat(int fd, struct stat *s) {
  int r = __real_fstat(fd, s);
  if (r == 0 && !S_ISDIR(s->st_mode)) {
    off_t cur = lseek(fd, 0, SEEK_CUR);
    off_t fin = lseek(fd, 0, SEEK_END);
    if (cur >= 0 && fin >= 0) {
      s->st_size = fin;
      lseek(fd, cur, SEEK_SET);
    }
  }
  return r;
}
}

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
static char lineas[24][100];
static int nl = 0;
static char logbuf[16][130];
static int nlog = 0;
static int g_cmd = 0;
static int g_ncmd = 0;
static unsigned g_vw = 0, g_vh = 0;
static int g_vc = 0, g_vnz = 0;
static int g_pf = -1;
static int g_frames = 0, g_ms = 0;
static volatile int g_traza = 0;
static volatile int g_dentro = 0;
static int g_fd = -1;
static int g_interp = 0;

static void dibujar(void);

/* Escribe una linea en espia.txt sin dibujar */
static void esp(const char *fmt, ...) {
  if (g_fd < 0) return;
  char b[300];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(b, sizeof(b) - 1, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n > (int)sizeof(b) - 2) n = (int)sizeof(b) - 2;
  b[n++] = '\n';
  write(g_fd, b, (size_t)n);
  fsync(g_fd);
}

static void al_fallo(int sig, siginfo_t *si, void *ctx) {
  (void)ctx;
  esp("FALLO senal=%d addr=%p", sig, si ? si->si_addr : (void *)0);
  _exit(1);
}

static void log_core(int nivel, const char *fmt, ...) {
  (void)nivel;
  char tmp[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  for (char *p = tmp; *p; p++) {
    if (*p == '\n' || *p == '\r') *p = ' ';
  }
  esp("LOG: %s", tmp);
  if (g_traza) return;
  char *d = logbuf[nlog % 16];
  snprintf(d, sizeof(logbuf[0]), "%s", tmp);
  nlog++;
  if (SDL_ThreadID() == g_main) {
    dibujar();
    SDL_Delay(150);
  }
}

#define TRAZA(...) do { if (g_traza && !g_dentro) { g_dentro = 1; esp(__VA_ARGS__); g_dentro = 0; } } while (0)

extern "C" {
int __real_shm_open(const char *p, int fl, mode_t m);
int __real_shm_unlink(const char *p);
int __real_ftruncate(int fd, off_t n);
void *__real_mmap(void *a, size_t l, int pr, int fl, int fd, off_t o);
int __real_munmap(void *a, size_t l);
int __real_mprotect(void *a, size_t l, int pr);
int __real_madvise(void *a, size_t l, int b);

int __wrap_shm_open(const char *p, int fl, mode_t m) {
  TRAZA("T> shm_open %s fl=%x", (p == (const char *)1) ? "ANON" : p, fl);
  int r = __real_shm_open(p, fl, m);
  TRAZA("T< shm_open = %d", r);
  return r;
}

int __wrap_shm_unlink(const char *p) {
  TRAZA("T> shm_unlink %s", (p == (const char *)1) ? "ANON" : p);
  int r = __real_shm_unlink(p);
  TRAZA("T< shm_unlink = %d", r);
  return r;
}

int __wrap_ftruncate(int fd, off_t n) {
  TRAZA("T> ftruncate fd=%d len=%llu", fd, (unsigned long long)n);
  int r = __real_ftruncate(fd, n);
  TRAZA("T< ftruncate = %d", r);
  return r;
}

void *__wrap_mmap(void *a, size_t l, int pr, int fl, int fd, off_t o) {
  TRAZA("T> mmap addr=%p len=%llu pr=%d fl=%x fd=%d off=%llu", a, (unsigned long long)l, pr, fl, fd, (unsigned long long)o);
  void *r = __real_mmap(a, l, pr, fl, fd, o);
  TRAZA("T< mmap = %p", r);
  return r;
}

int __wrap_munmap(void *a, size_t l) {
  TRAZA("T> munmap addr=%p len=%llu", a, (unsigned long long)l);
  int r = __real_munmap(a, l);
  TRAZA("T< munmap = %d", r);
  return r;
}

int __wrap_mprotect(void *a, size_t l, int pr) {
  TRAZA("T> mprotect addr=%p len=%llu pr=%d", a, (unsigned long long)l, pr);
  int r = __real_mprotect(a, l, pr);
  TRAZA("T< mprotect = %d", r);
  return r;
}

int __wrap_madvise(void *a, size_t l, int b) {
  TRAZA("T> madvise addr=%p len=%llu adv=%d", a, (unsigned long long)l, b);
  int r = __real_madvise(a, l, b);
  TRAZA("T< madvise = %d", r);
  return r;
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
    case 10:
      g_pf = *(int *)data;
      esp("ENV10 formato pixel pedido=%d", g_pf);
      return true;
    case 11: return true;
    case 15: {
      retro_variable *v = (retro_variable *)data;
      if (!g_traza) esp("ENV15 clave=%s", v->key ? v->key : "(null)");
      if (contiene(v->key, "fastmem")) {
        v->value = "Disabled";
        return true;
      }
      if (contiene(v->key, "GPU_Renderer")) {
        v->value = "Software";
        return true;
      }
      if (contiene(v->key, "GPU_UseThread")) {
        v->value = "false";
        return true;
      }
      if (contiene(v->key, "Logging_LogLevel")) {
        v->value = "Debug";
        return true;
      }
      if (contiene(v->key, "ReadaheadSectors")) {
        v->value = "0";
        if (!g_traza) esp("  -> respondido 0 (sin lectura anticipada)");
        return true;
      }
      if (g_interp && contiene(v->key, "executionmode")) {
        v->value = "Interpreter";
        return true;
      }
      v->value = NULL;
      return false;
    }
    case 16: return true;
    case 17: *(bool *)data = false; return true;
    case 27: ((retro_log_callback *)data)->log = log_core; return true;
    case 31: *(const char **)data = "/data/swan"; return true;
    default:
      return false;
  }
}

static void video(const void *d, unsigned w, unsigned h, size_t p) {
  if (g_vc < 3 || w != g_vw || h != g_vh) {
    esp("VIDEO llamada %d: %ux%u pitch=%llu datos=%p", g_vc + 1, w, h, (unsigned long long)p, d);
  }
  g_vw = w;
  g_vh = h;
  g_vc++;
  if (d && d != (const void *)-1) {
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
  snprintf(t, sizeof(t), "Llamadas video: %d  formato pixel: %d", g_vc, g_pf);
  texto(10, y, t);
  y += 14;
  int ini = nlog > 16 ? nlog - 16 : 0;
  for (int i = ini; i < nlog; i++) {
    snprintf(t, sizeof(t), "LOG: %.120s", logbuf[i % 16]);
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
  if (nl < 23) nl++;
  esperar(60);
}

int main(void) {
  SDL_Init(SDL_INIT_VIDEO);
  SDL_Window *w = SDL_CreateWindow("swan", 0, 0, 1920, 1080, 0);
  R = SDL_CreateRenderer(w, -1, 0);
  g_main = SDL_ThreadID();
  mkdir("/data/swan", 0777);

  g_interp = existe("/data/swan/interp.txt") ? 1 : 0;
  g_fd = open("/data/swan/espia.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
  esp("ARRANQUE interp=%d", g_interp);

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  /* Suposicion: el puntero a la funcion es el primer campo de sigaction */
  *(void **)&sa = (void *)al_fallo;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  int s1 = sigaction(SIGSEGV, &sa, NULL);
  int s2 = sigaction(SIGILL, &sa, NULL);
  int s3 = sigaction(SIGBUS, &sa, NULL);
  esp("sigaction SEGV=%d ILL=%d BUS=%d", s1, s2, s3);

  struct retro_system_info info;
  memset(&info, 0, sizeof(info));
  retro_get_system_info(&info);
  char t[160];
  snprintf(t, sizeof(t), "%s %s API %u", info.library_name ? info.library_name : "?", info.library_version ? info.library_version : "?", retro_api_version());
  paso(t);
  snprintf(t, sizeof(t), "fd=%d interp=%d sig=%d,%d,%d", g_fd, g_interp, s1, s2, s3);
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

  paso("3) load_game (pantalla quieta)...");
  struct retro_game_info gi;
  memset(&gi, 0, sizeof(gi));
  gi.path = ruta;
  esp("INICIO retro_load_game");
  g_traza = 1;
  bool ok = retro_load_game(&gi);
  esp("FIN retro_load_game = %d", ok ? 1 : 0);
  if (!ok) {
    g_traza = 0;
    paso("3) load_game DEVOLVIO FALSE");
    esperar(1800);
    SDL_Quit();
    return 0;
  }
  paso("4) load_game OK; ejecutando 600 frames...");
  esp("INICIO frames (espia sigue activo)");
  unsigned t0 = SDL_GetTicks();
  for (int i = 0; i < 600; i++) {
    esp("frame %d ini t=%u", i + 1, SDL_GetTicks() - t0);
    retro_run();
    g_frames = i + 1;
    g_ms = (int)((SDL_GetTicks() - t0) / (unsigned)(i + 1));
    dibujar();
  }
  g_traza = 0;
  esp("FIN 600 frames");
  paso("5) 600 frames terminados");
  esperar(1800);
  SDL_Quit();
  return 0;
}
