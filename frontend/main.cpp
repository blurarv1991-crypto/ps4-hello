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
extern "C" {
#include <orbis/UserService.h>
#include <orbis/Pad.h>
}
#include "stb_easy_font.h"

typedef OrbisPadData ScePadData;

#ifndef SHM_ANON
#define SHM_ANON ((char *)1)
#endif

/* Mascaras de botones de ScePad (suposicion: valores del SDK) */
#define PB_L3 0x00000002u
#define PB_R3 0x00000004u
#define PB_OPTIONS 0x00000008u
#define PB_UP 0x00000010u
#define PB_RIGHT 0x00000020u
#define PB_DOWN 0x00000040u
#define PB_LEFT 0x00000080u
#define PB_L2 0x00000100u
#define PB_R2 0x00000200u
#define PB_L1 0x00000400u
#define PB_R1 0x00000800u
#define PB_TRIANGLE 0x00001000u
#define PB_CIRCLE 0x00002000u
#define PB_CROSS 0x00004000u
#define PB_SQUARE 0x00008000u
#define PB_TOUCH 0x00100000u

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
static SDL_Texture *g_tex = NULL;
static SDL_Texture *g_tex2 = NULL;
static int g_tw = 0, g_th = 0;
static int g_t2w = 0, g_t2h = 0;
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
static int g_frames = 0;
static volatile int g_traza = 0;
static volatile int g_dentro = 0;
static int g_fd = -1;
static int g_interp = 0;
static int g_jugando = 0;
static char g_rend[40] = "?";

/* ultimo cuadro del nucleo (para el escalado propio) */
static const void *g_dp = NULL;
static unsigned g_dw = 0, g_dh = 0;
static size_t g_dpitch = 0;
static uint16_t g_sbuf[1920 * 1080];

/* mando */
static int g_pad = -1;
static unsigned g_mask = 0;
static uint32_t g_btn = 0;
static uint32_t g_btn_prev = 0;
static int g_modo = 3;
static int g_cada = 3;

/* tiempos en microsegundos (acumulados) y medias en decimas de ms */
static double a_run = 0, a_upd = 0, a_cop = 0, a_pre = 0, a_esc = 0;
static int n_upd = 0, n_dib = 0;
static int m_run = 0, m_upd = 0, m_cop = 0, m_pre = 0, m_esc = 0;

static double ahora_us(void) {
  return (double)SDL_GetPerformanceCounter() * 1000000.0 / (double)SDL_GetPerformanceFrequency();
}

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
  if (g_jugando) return;
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

#define TRAZA(...) do { if (g_traza && !g_jugando && !g_dentro) { g_dentro = 1; esp(__VA_ARGS__); g_dentro = 0; } } while (0)

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
      if (contiene(v->key, "ReadaheadSectors")) {
        v->value = "0";
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
  if (!d || d == (const void *)-1 || w == 0 || h == 0) return;
  g_vnz = 1;
  g_dp = d;
  g_dw = w;
  g_dh = h;
  g_dpitch = p;

  if (!g_tex || g_tw != (int)w || g_th != (int)h) {
    if (g_tex) SDL_DestroyTexture(g_tex);
    Uint32 fmt = SDL_PIXELFORMAT_RGB565;
    if (g_pf == 0) fmt = SDL_PIXELFORMAT_ARGB1555;
    if (g_pf == 1) fmt = SDL_PIXELFORMAT_RGB888;
    g_tex = SDL_CreateTexture(R, fmt, SDL_TEXTUREACCESS_STREAMING, (int)w, (int)h);
    g_tw = (int)w;
    g_th = (int)h;
    esp("TEXTURA %dx%d fmt=%d ptr=%p", g_tw, g_th, g_pf, (void *)g_tex);
  }
  if (g_tex) {
    double t0 = ahora_us();
    SDL_UpdateTexture(g_tex, NULL, d, (int)p);
    a_upd += ahora_us() - t0;
    n_upd++;
  }
}
static void audio1(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audio2(const int16_t *d, size_t n) { (void)d; return n; }

/* Lee el mando y arma la mascara de botones de libretro */
static void poll(void) {
  if (g_pad < 0) return;
  ScePadData d;
  memset(&d, 0, sizeof(d));
  if (scePadReadState(g_pad, &d) != 0) return;
  uint32_t b = d.buttons;
  unsigned m = 0;
  if (b & PB_CROSS) m |= 1u << 0;
  if (b & PB_SQUARE) m |= 1u << 1;
  if (b & PB_TOUCH) m |= 1u << 2;
  if (b & PB_OPTIONS) m |= 1u << 3;
  if (b & PB_UP) m |= 1u << 4;
  if (b & PB_DOWN) m |= 1u << 5;
  if (b & PB_LEFT) m |= 1u << 6;
  if (b & PB_RIGHT) m |= 1u << 7;
  if (b & PB_CIRCLE) m |= 1u << 8;
  if (b & PB_TRIANGLE) m |= 1u << 9;
  if (b & PB_L1) m |= 1u << 10;
  if (b & PB_R1) m |= 1u << 11;
  if (b & PB_L2) m |= 1u << 12;
  if (b & PB_R2) m |= 1u << 13;
  /* stick izquierdo como cruceta */
  if (d.leftStick.x < 64) m |= 1u << 6;
  if (d.leftStick.x > 192) m |= 1u << 7;
  if (d.leftStick.y < 64) m |= 1u << 4;
  if (d.leftStick.y > 192) m |= 1u << 5;
  g_mask = m;
  g_btn = b;
  /* L3 cambia el modo de imagen, R3 cambia cada cuantos frames se dibuja */
  uint32_t nuevos = b & ~g_btn_prev;
  if (nuevos & PB_L3) g_modo = (g_modo + 1) % 4;
  if (nuevos & PB_R3) g_cada = (g_cada == 3) ? 2 : (g_cada == 2 ? 1 : 3);
  g_btn_prev = b;
}

static int16_t entrada(unsigned port, unsigned dev, unsigned idx, unsigned id) {
  (void)idx;
  if (port != 0 || dev != 1) return 0;
  if (id == 256) return (int16_t)(g_mask & 0xFFFF);
  if (id < 16) return (int16_t)((g_mask >> id) & 1u);
  return 0;
}

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
  int ini = nlog > 16 ? nlog - 16 : 0;
  for (int i = ini; i < nlog; i++) {
    snprintf(t, sizeof(t), "LOG: %.120s", logbuf[i % 16]);
    texto(10, y, t);
    y += 14;
  }
  SDL_RenderPresent(R);
}

/* Escalado propio por enteros (vecino mas cercano), solo 16 bits por pixel */
static int escalar_propio(int *ow, int *oh) {
  if (!g_dp || g_dw == 0 || g_dh == 0) return 0;
  if (g_pf != 2 && g_pf != 0) return 0;
  int w = (int)g_dw, h = (int)g_dh;
  int k = 1920 / w;
  int k2 = 1080 / h;
  if (k2 < k) k = k2;
  if (k < 1) k = 1;
  int W = w * k, H = h * k;
  const unsigned char *src = (const unsigned char *)g_dp;
  for (int y = 0; y < h; y++) {
    const uint16_t *s = (const uint16_t *)(src + (size_t)y * g_dpitch);
    uint16_t *d = g_sbuf + (size_t)(y * k) * W;
    for (int x = 0; x < w; x++) {
      uint16_t v = s[x];
      for (int j = 0; j < k; j++) d[x * k + j] = v;
    }
    for (int r = 1; r < k; r++) memcpy(d + (size_t)r * W, d, (size_t)W * 2);
  }
  if (!g_tex2 || g_t2w != W || g_t2h != H) {
    if (g_tex2) SDL_DestroyTexture(g_tex2);
    Uint32 fmt = (g_pf == 0) ? SDL_PIXELFORMAT_ARGB1555 : SDL_PIXELFORMAT_RGB565;
    g_tex2 = SDL_CreateTexture(R, fmt, SDL_TEXTUREACCESS_STREAMING, W, H);
    g_t2w = W;
    g_t2h = H;
    esp("TEXTURA2 %dx%d k=%d ptr=%p", W, H, k, (void *)g_tex2);
  }
  if (!g_tex2) return 0;
  SDL_UpdateTexture(g_tex2, NULL, g_sbuf, W * 2);
  *ow = W;
  *oh = H;
  return 1;
}

/* Pantalla de juego: imagen segun el modo, con tiempos */
static void dibujar_juego(void) {
  double t0 = ahora_us();
  SDL_SetRenderDrawColor(R, 0, 0, 0, 255);
  SDL_RenderClear(R);
  SDL_RenderSetScale(R, 1, 1);
  int hecho = 0;
  double te0 = ahora_us();
  if (g_modo == 3) {
    int W = 0, H = 0;
    if (escalar_propio(&W, &H)) {
      SDL_Rect dst = {(1920 - W) / 2, (1080 - H) / 2, W, H};
      SDL_RenderCopy(R, g_tex2, NULL, &dst);
      hecho = 1;
    }
  }
  a_esc += ahora_us() - te0;
  if (!hecho && g_tex && g_tw > 0 && g_th > 0) {
    int dw = g_tw, dh = g_th;
    if (g_modo == 1 || g_modo == 3) {
      int k = 1080 / g_th;
      if (k < 1) k = 1;
      dh = g_th * k;
      dw = dh * 4 / 3;
      if (dw > 1920) { dw = 1920; dh = dw * 3 / 4; }
    } else if (g_modo == 2) {
      dw = 1440;
      dh = 1080;
    }
    SDL_Rect dst = {(1920 - dw) / 2, (1080 - dh) / 2, dw, dh};
    SDL_RenderCopy(R, g_tex, NULL, &dst);
  }
  double t1 = ahora_us();
  SDL_RenderSetScale(R, 2, 2);
  char t[220];
  snprintf(t, sizeof(t), "F:%d run:%d.%d pre:%d.%d esc:%d.%d modo:%d cada:%d pad:%x %ux%u %s", g_frames,
           m_run / 10, m_run % 10, m_pre / 10, m_pre % 10, m_esc / 10, m_esc % 10, g_modo, g_cada, (unsigned)g_btn, g_vw, g_vh, g_rend);
  texto(5, 5, t);
  double t2 = ahora_us();
  SDL_RenderPresent(R);
  double t3 = ahora_us();
  a_cop += t1 - t0;
  a_pre += t3 - t2;
  n_dib++;
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
  R = SDL_CreateRenderer(w, -1, SDL_RENDERER_ACCELERATED);
  int acel = R ? 1 : 0;
  if (!R) R = SDL_CreateRenderer(w, -1, 0);
  if (!R) R = SDL_CreateRenderer(w, -1, SDL_RENDERER_SOFTWARE);
  g_main = SDL_ThreadID();
  mkdir("/data/swan", 0777);

  g_interp = existe("/data/swan/interp.txt") ? 1 : 0;
  g_fd = open("/data/swan/espia.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
  esp("ARRANQUE interp=%d", g_interp);

  SDL_RendererInfo ri;
  memset(&ri, 0, sizeof(ri));
  if (R && SDL_GetRendererInfo(R, &ri) == 0 && ri.name) {
    snprintf(g_rend, sizeof(g_rend), "%.30s", ri.name);
    esp("RENDERER acel_pedido_ok=%d nombre=%s flags=%x", acel, ri.name, (unsigned)ri.flags);
  } else {
    esp("RENDERER sin info (R=%p)", (void *)R);
  }

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

  /* Mando */
  int ru = sceUserServiceInitialize(NULL);
  int32_t uid = 0;
  int rg = sceUserServiceGetInitialUser(&uid);
  int rp = scePadInit();
  g_pad = scePadOpen(uid, 0, 0, NULL);
  esp("PAD userinit=%d getuser=%d uid=%d padinit=%d handle=%d", ru, rg, (int)uid, rp, g_pad);

  struct retro_system_info info;
  memset(&info, 0, sizeof(info));
  retro_get_system_info(&info);
  char t[160];
  snprintf(t, sizeof(t), "%s %s API %u", info.library_name ? info.library_name : "?", info.library_version ? info.library_version : "?", retro_api_version());
  paso(t);
  snprintf(t, sizeof(t), "fd=%d interp=%d pad=%d rend=%s", g_fd, g_interp, g_pad, g_rend);
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
  paso("4) load_game OK; mostrando imagen...");
  esp("INICIO frames (sin limite)");
  g_jugando = 1;
  int i = 0;
  for (;;) {
    double a = ahora_us();
    retro_run();
    a_run += ahora_us() - a;
    i++;
    g_frames = i;
    if (i % g_cada == 0) {
      SDL_PumpEvents();
      dibujar_juego();
    }
    if (i % 60 == 0) {
      m_run = (int)(a_run / 60.0 / 100.0);
      m_upd = n_upd ? (int)(a_upd / n_upd / 100.0) : 0;
      m_cop = n_dib ? (int)(a_cop / n_dib / 100.0) : 0;
      m_pre = n_dib ? (int)(a_pre / n_dib / 100.0) : 0;
      m_esc = n_dib ? (int)(a_esc / n_dib / 100.0) : 0;
      esp("frame %d run=%d upd=%d cop=%d pre=%d esc=%d modo=%d cada=%d pad=%x", i, m_run, m_upd, m_cop, m_pre, m_esc, g_modo, g_cada, (unsigned)g_btn);
      a_run = a_upd = a_cop = a_pre = a_esc = 0;
      n_upd = n_dib = 0;
    }
  }
  return 0;
}
