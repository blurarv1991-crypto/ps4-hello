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

/* Audio: declaraciones propias enlazadas a los simbolos reales de la PS4 */
extern "C" {
int mia_aud_init(void) __asm__("sceAudioOutInit");
int mia_aud_open(int uid, int tipo, int indice, unsigned len, unsigned freq, unsigned param) __asm__("sceAudioOutOpen");
int mia_aud_out(int handle, const void *datos) __asm__("sceAudioOutOutput");
}

/* Carpetas: funciones del nucleo de la PS4 (libkernel) para listar /data/swan/juegos */
extern "C" {
int mia_k_open(const char *ruta, int flags, int modo) __asm__("sceKernelOpen");
int mia_k_getdents(int fd, char *buf, int n) __asm__("sceKernelGetdents");
int mia_k_mprotect(const void *addr, size_t len, int prot) __asm__("sceKernelMprotect");
int mia_k_close(int fd) __asm__("sceKernelClose");
}

typedef OrbisPadData ScePadData;

#ifndef SHM_ANON
#define SHM_ANON ((char *)1)
#endif

/* Mascaras de botones de ScePad (todas verificadas en la PS4; SHARE = 0x800000 no se usa) */
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
struct retro_system_av_info {
  unsigned base_width;
  unsigned base_height;
  unsigned max_width;
  unsigned max_height;
  float aspect_ratio;
  double fps;
  double sample_rate;
};
unsigned retro_api_version(void);
void retro_get_system_info(struct retro_system_info *info);
void retro_get_system_av_info(struct retro_system_av_info *info);
void retro_set_environment(bool (*cb)(unsigned, void *));
void retro_set_video_refresh(void (*cb)(const void *, unsigned, unsigned, size_t));
void retro_set_audio_sample(void (*cb)(int16_t, int16_t));
void retro_set_audio_sample_batch(size_t (*cb)(const int16_t *, size_t));
void retro_set_input_poll(void (*cb)(void));
void retro_set_input_state(int16_t (*cb)(unsigned, unsigned, unsigned, unsigned));
void retro_init(void);
bool retro_load_game(const struct retro_game_info *game);
void retro_run(void);
size_t retro_serialize_size(void);
bool retro_serialize(void *data, size_t size);
bool retro_unserialize(const void *data, size_t size);
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
static int g_cached = 0;
static int g_jugando = 0;
static char g_rend[40] = "?";
static int g_kreal = 1;

/* ultimo cuadro del nucleo (para el escalado propio) */
static const void *g_dp = NULL;
static unsigned g_dw = 0, g_dh = 0;
static size_t g_dpitch = 0;
static uint16_t g_sbuf[1920 * 1080];

/* copia del ultimo cuadro, para dibujarlo en otro hilo */
static uint16_t g_snap[1920 * 1080];
static unsigned g_sw = 0, g_sh = 0;
static int g_dib_estado = 0; /* 0 libre, 1 cuadro listo, 2 dibujando */
static int g_dib_hilo = 0;

/* mando */
static int g_pad = -1;
static unsigned g_mask = 0;
static uint32_t g_btn = 0;
static uint32_t g_btn_prev = 0;
static int g_modo = 3;
static int g_asp = 1; /* 1 = corregir a 4:3, 0 = pixeles cuadrados (L2+R2+L3 cambia) */
static int g_xmap[1920];
static int g_xmap_w = 0, g_xmap_W = 0;

/* menu de opciones dentro del juego (L3) */
static volatile int g_menu_toggle = 0;
static volatile int g_menu_vis = 0;
static int g_menu_sel = 0, g_menu_n = 0;
static char g_ml[10][72];
static char g_msg[80] = "";
static int g_jit_pedido = 0; /* 1 si existe /data/swan/jit.txt (se aplica al reiniciar la app) */
static int g_ranura = 1;
static char g_juego_id[96] = "juego";
static unsigned char g_estbuf[40 * 1024 * 1024];
static int g_cada = 3;

/* audio */
#define AQ_N 32768
#define AQ_MASK (AQ_N - 1)
#define GRANO 256
static int g_aud = -1;
static double g_srate = 44100.0;
static int16_t g_aq[AQ_N * 2];
static unsigned g_ah = 0, g_at = 0;
static double g_rpos = 0.0;
static int16_t g_pl = 0, g_pr = 0;
static unsigned long long g_ain = 0;
static int m_ain = 0;
static int g_aout_ok = 0;

/* tiempos en microsegundos (acumulados) y medias en decimas de ms */
static double a_run = 0, a_upd = 0, a_cop = 0, a_pre = 0, a_esc = 0;
static int n_upd = 0, n_dib = 0;
static int m_run = 0, m_upd = 0, m_cop = 0, m_pre = 0, m_esc = 0;
static double a_aud = 0;
static int n_blq = 0;
static int m_aud = 0, m_tot = 0;
static int g_hilo_ok = 0;
static unsigned g_hblq = 0, g_hsil = 0;

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

/* JIT (solo con /data/swan/jit.txt): zonas que ya se marcaron RWX con sceKernelMprotect */
static int g_jit = 0;
static unsigned long g_jit_ini[8];
static unsigned long g_jit_fin[8];
static int g_jit_n = 0;
static int g_jit_logs = 0;
static int jit_dentro(unsigned long a, unsigned long l) {
  for (int i = 0; i < g_jit_n; i++)
    if (a >= g_jit_ini[i] && a + l <= g_jit_fin[i]) return 1;
  return 0;
}

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
  if (g_jit) {
    unsigned long ini = (unsigned long)a;
    unsigned long fin = ini + (unsigned long)l;
    if (jit_dentro(ini, (unsigned long)l)) {
      /* Ya es RWX: se deja asi (tambien si el nucleo pide quitar ejecucion) */
      if (g_jit_logs < 30) { g_jit_logs++; esp("JIT mprotect %p +%llu pr=%d: zona ya RWX, se ignora", a, (unsigned long long)l, pr); }
      return 0;
    }
    if (pr & 4) {
      unsigned long ri = ini & ~16383UL;
      unsigned long rf = (fin + 16383UL) & ~16383UL;
      esp("JIT mprotect %p +%llu pr=%d -> sceKernelMprotect(%p, %llu, RWX)", a, (unsigned long long)l, pr, (void *)ri, (unsigned long long)(rf - ri));
      int r = mia_k_mprotect((void *)ri, (size_t)(rf - ri), 7);
      esp("JIT sceKernelMprotect = %d (%#x)", r, (unsigned)r);
      if (r == 0) {
        if (g_jit_n < 8) { g_jit_ini[g_jit_n] = ri; g_jit_fin[g_jit_n] = rf; g_jit_n++; }
        return 0;
      }
      esp("JIT: fallo, se prueba el mprotect normal");
    }
  }
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
      /* Tarjeta de memoria 1: el propio nucleo la guarda en /data/swan/<codigo>_1.mcd */
      if (contiene(v->key, "Card1Type")) {
        v->value = "PerGame";
        return true;
      }
      if (g_jit && contiene(v->key, "executionmode")) {
        esp("JIT: pidiendo modo Recompiler al nucleo");
        v->value = "Recompiler";
        return true;
      }
      if (g_cached && contiene(v->key, "executionmode")) {
        v->value = "CachedInterpreter";
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
}

/* ---- audio: remuestreo lineal a 48000 Hz y cola circular ---- */
static void aq_push(int16_t l, int16_t r) {
  unsigned ah = __atomic_load_n(&g_ah, __ATOMIC_ACQUIRE);
  unsigned at = g_at;
  if (at - ah >= AQ_N) return; /* cola llena: se descarta */
  unsigned i = at & AQ_MASK;
  g_aq[i * 2] = l;
  g_aq[i * 2 + 1] = r;
  __atomic_store_n(&g_at, at + 1, __ATOMIC_RELEASE);
}

static void audio_entrada(int16_t l, int16_t r) {
  g_ain++;
  double paso = g_srate / 48000.0;
  while (g_rpos < 1.0) {
    double a = g_rpos;
    int16_t ol = (int16_t)(g_pl + (l - g_pl) * a);
    int16_t orr = (int16_t)(g_pr + (r - g_pr) * a);
    aq_push(ol, orr);
    g_rpos += paso;
  }
  g_rpos -= 1.0;
  g_pl = l;
  g_pr = r;
}

static void audio1(int16_t l, int16_t r) { audio_entrada(l, r); }
static size_t audio2(const int16_t *d, size_t n) {
  for (size_t i = 0; i < n; i++) audio_entrada(d[i * 2], d[i * 2 + 1]);
  return n;
}

/* Envia a la PS4 los bloques completos que haya en la cola */
static void audio_enviar(void) {
  if (g_aud < 0) return;
  int veces = 0;
  static int16_t grano[GRANO * 2];
  while (g_at - g_ah >= GRANO && veces < 8) {
    for (int i = 0; i < GRANO; i++) {
      unsigned k = (g_ah + (unsigned)i) & AQ_MASK;
      grano[i * 2] = g_aq[k * 2];
      grano[i * 2 + 1] = g_aq[k * 2 + 1];
    }
    g_ah += GRANO;
    double ta = ahora_us();
    int r = mia_aud_out(g_aud, grano);
    a_aud += ahora_us() - ta;
    n_blq++;
    if (r < 0 && !g_aout_ok) {
      esp("AUDIO output error %x", (unsigned)r);
      g_aout_ok = -1;
    } else if (r >= 0 && g_aout_ok == 0) {
      g_aout_ok = 1;
      esp("AUDIO output primer envio OK (ret=%d)", r);
    }
    veces++;
  }
}

/* Hilo de audio: lee la cola circular y llama a sceAudioOutOutput (que bloquea).
   Si falta audio manda un bloque de silencio para mantener el ritmo. */
static int hilo_audio(void *arg) {
  (void)arg;
  static int16_t grano[GRANO * 2];
  for (;;) {
    unsigned at = __atomic_load_n(&g_at, __ATOMIC_ACQUIRE);
    unsigned ah = g_ah;
    int hay = (at - ah >= GRANO);
    if (hay) {
      for (int i = 0; i < GRANO; i++) {
        unsigned k = (ah + (unsigned)i) & AQ_MASK;
        grano[i * 2] = g_aq[k * 2];
        grano[i * 2 + 1] = g_aq[k * 2 + 1];
      }
      __atomic_store_n(&g_ah, ah + GRANO, __ATOMIC_RELEASE);
    } else {
      memset(grano, 0, sizeof(grano));
    }
    int r = mia_aud_out(g_aud, grano);
    if (hay) __atomic_fetch_add(&g_hblq, 1u, __ATOMIC_RELAXED);
    else __atomic_fetch_add(&g_hsil, 1u, __ATOMIC_RELAXED);
    if (r < 0 && !g_aout_ok) {
      esp("AUDIO hilo output error %x", (unsigned)r);
      g_aout_ok = -1;
    } else if (r >= 0 && g_aout_ok == 0) {
      g_aout_ok = 1;
      esp("AUDIO hilo primer envio OK (ret=%d)", r);
    }
  }
  return 0;
}

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
  /* L3 cambia la escala, R3 cambia cada cuantos frames se dibuja */
  uint32_t nuevos = b & ~g_btn_prev;
  /* L2+R2 mantenidos y L3: cambia entre 4:3 y pixeles cuadrados.
     L3 solo: abre/cierra el menu de opciones */
  if ((nuevos & PB_L3) && (b & PB_L2) && (b & PB_R2)) g_asp = !g_asp;
  else if (nuevos & PB_L3) g_menu_toggle = 1;
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

/* ---- Suavizado de imagen (0 = No, 1 = Bajo, 2 = Alto) ----
   Bajo: interpolacion "nitida" solo en horizontal (arregla los pixeles de distinto ancho del 4:3);
         en vertical el factor ya es entero, asi que no hace falta mezclar.
   Alto: bordes mas suaves (mezcla el doble de ancha) en horizontal y vertical. */
static int g_suav = 0;
static uint16_t g_hrow[2][1920]; /* dos filas ya estiradas (sin buffer grande) */
static int g_hrow_y[2];
static int g_hi0[1920], g_hi1[1920];
static unsigned char g_hw[1920];
static int g_h_w = 0, g_h_W = 0, g_h_s = -1;
static int g_vi0[1080], g_vi1[1080];
static unsigned char g_svw[1080];
static int g_v_h = 0, g_v_H = 0, g_v_s = -1;

/* Tabla para pasar de n pixeles a N: indice i0, indice i1 y peso (0..32) de i1.
   nitido=0: vecino mas cercano. 1: bilineal nitido. 2: bilineal suave (borde mas ancho). */
static void suav_tabla(int n, int N, int nitido, int *i0, int *i1, unsigned char *wt) {
  for (int o = 0; o < N; o++) {
    if (!nitido) {
      int a = (int)(((long long)o * n) / N);
      if (a > n - 1) a = n - 1;
      i0[o] = a; i1[o] = a; wt[o] = 0;
      continue;
    }
    float u = ((float)o + 0.5f) * (float)n / (float)N - 0.5f;
    if (u <= 0.0f) { i0[o] = 0; i1[o] = 0; wt[o] = 0; continue; }
    int a = (int)u;
    if (a >= n - 1) { i0[o] = n - 1; i1[o] = n - 1; wt[o] = 0; continue; }
    float f = u - (float)a;
    float sc = (float)N / (float)n;
    if (nitido == 2) { sc *= 0.5f; if (sc < 1.0f) sc = 1.0f; }
    float fp = (f - 0.5f) * sc + 0.5f;
    if (fp < 0.0f) fp = 0.0f;
    if (fp > 1.0f) fp = 1.0f;
    i0[o] = a; i1[o] = a + 1; wt[o] = (unsigned char)(fp * 32.0f + 0.5f);
  }
}

/* Mezcla dos pixeles de 16 bits con peso w (0..32) de b. m: mascara RGB565 o 1555 */
static inline uint16_t mezcla(uint16_t a, uint16_t b, unsigned w, uint32_t m) {
  uint32_t x = ((uint32_t)a | ((uint32_t)a << 16)) & m;
  uint32_t y = ((uint32_t)b | ((uint32_t)b << 16)) & m;
  uint32_t r = ((x * (32u - w) + y * w) >> 5) & m;
  return (uint16_t)(r | (r >> 16));
}

/* Escalado propio por enteros (vecino mas cercano), solo 16 bits por pixel.
   kpedido: 2, 3 o 0 (= el maximo que cabe en 1920x1080) */
static int escalar_propio(int kpedido, int *ow, int *oh) {
  if (g_sw == 0 || g_sh == 0) return 0;
  if (g_pf != 2 && g_pf != 0) return 0;
  int w = (int)g_sw, h = (int)g_sh;
  int kmax = 1920 / w;
  int k2 = 1080 / h;
  if (k2 < kmax && !g_asp) kmax = k2;
  if (g_asp) kmax = k2;
  if (kmax < 1) kmax = 1;
  int k = (kpedido <= 0 || kpedido > kmax) ? kmax : kpedido;
  g_kreal = k;
  int H = h * k;
  int W = w * k;
  int asp = 0;
  if (g_asp) {
    int Wa = (H * 4 + 1) / 3;
    if (Wa >= 1 && Wa <= 1920) { W = Wa; asp = 1; }
  }
  if (!asp) {
    /* pixeles cuadrados: factor entero igual en ancho y alto */
    int kk = 1920 / w;
    if (1080 / h < kk) kk = 1080 / h;
    if (kk < 1) kk = 1;
    if (k > kk) k = kk;
    g_kreal = k;
    W = w * k;
    H = h * k;
  }
  int sv = g_suav;
  if (sv && h <= 576 && W <= 1920 && H <= 1080) {
    uint32_t m = (g_pf == 0) ? 0x03E07C1Fu : 0x07E0F81Fu;
    if (g_h_w != w || g_h_W != W || g_h_s != sv) {
      suav_tabla(w, W, sv == 2 ? 2 : 1, g_hi0, g_hi1, g_hw);
      g_h_w = w;
      g_h_W = W;
      g_h_s = sv;
    }
    if (g_v_h != h || g_v_H != H || g_v_s != sv) {
      suav_tabla(h, H, sv == 2 ? 2 : 0, g_vi0, g_vi1, g_svw);
      g_v_h = h;
      g_v_H = H;
      g_v_s = sv;
    }
    g_hrow_y[0] = -1;
    g_hrow_y[1] = -1;
    for (int oy = 0; oy < H; oy++) {
      uint16_t *d = g_sbuf + (size_t)oy * W;
      const uint16_t *r[2];
      int ys[2] = {g_vi0[oy], g_vi1[oy]};
      for (int q = 0; q < 2; q++) {
        int y = ys[q];
        uint16_t *hr = g_hrow[y & 1];
        if (g_hrow_y[y & 1] != y) {
          const uint16_t *sr = g_snap + (size_t)y * (size_t)w;
          for (int x = 0; x < W; x++) {
            unsigned wx = g_hw[x];
            hr[x] = wx ? mezcla(sr[g_hi0[x]], sr[g_hi1[x]], wx, m) : sr[g_hi0[x]];
          }
          g_hrow_y[y & 1] = y;
        }
        r[q] = hr;
      }
      unsigned wt = g_svw[oy];
      if (!wt) {
        memcpy(d, r[0], (size_t)W * 2);
      } else {
        for (int x = 0; x < W; x++) d[x] = mezcla(r[0][x], r[1][x], wt, m);
      }
    }
  } else {
  if (g_xmap_w != w || g_xmap_W != W) {
    for (int x = 0; x < W; x++) g_xmap[x] = (int)(((long long)x * w) / W);
    g_xmap_w = w;
    g_xmap_W = W;
  }
  for (int y = 0; y < h; y++) {
    const uint16_t *s = g_snap + (size_t)y * (size_t)w;
    uint16_t *d = g_sbuf + (size_t)(y * k) * W;
    for (int x = 0; x < W; x++) d[x] = s[g_xmap[x]];
    for (int r = 1; r < k; r++) memcpy(d + (size_t)r * W, d, (size_t)W * 2);
  }
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

/* Pantalla de juego: modo 0 original, 1 = 2x, 2 = 3x, 3 = maximo */
static void dibujar_juego(void) {
  double t0 = ahora_us();
  SDL_SetRenderDrawColor(R, 0, 0, 0, 255);
  SDL_RenderClear(R);
  SDL_RenderSetScale(R, 1, 1);
  int hecho = 0;
  if (g_modo == 0 && g_sw > 0 && g_sh > 0) {
    if (!g_tex || g_tw != (int)g_sw || g_th != (int)g_sh) {
      if (g_tex) SDL_DestroyTexture(g_tex);
      Uint32 fmt = (g_pf == 0) ? SDL_PIXELFORMAT_ARGB1555 : SDL_PIXELFORMAT_RGB565;
      g_tex = SDL_CreateTexture(R, fmt, SDL_TEXTUREACCESS_STREAMING, (int)g_sw, (int)g_sh);
      g_tw = (int)g_sw;
      g_th = (int)g_sh;
      esp("TEXTURA %dx%d fmt=%d ptr=%p", g_tw, g_th, g_pf, (void *)g_tex);
    }
    if (g_tex) {
      double tu = ahora_us();
      SDL_UpdateTexture(g_tex, NULL, g_snap, (int)g_sw * 2);
      a_upd += ahora_us() - tu;
      n_upd++;
    }
  }
  double te0 = ahora_us();
  if (g_modo >= 1) {
    int kp = (g_modo == 1) ? 2 : (g_modo == 2 ? 3 : 0);
    int W = 0, H = 0;
    if (escalar_propio(kp, &W, &H)) {
      SDL_Rect dst = {(1920 - W) / 2, (1080 - H) / 2, W, H};
      SDL_RenderCopy(R, g_tex2, NULL, &dst);
      hecho = 1;
    }
  }
  a_esc += ahora_us() - te0;
  if (!hecho && g_tex && g_tw > 0 && g_th > 0) {
    if (g_modo == 0) g_kreal = 1;
    SDL_Rect dst = {(1920 - g_tw) / 2, (1080 - g_th) / 2, g_tw, g_th};
    SDL_RenderCopy(R, g_tex, NULL, &dst);
  }
  if (g_menu_vis) {
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(R, 0, 0, 40, 220);
    SDL_Rect pn = {420, 180, 1080, 720};
    SDL_RenderFillRect(R, &pn);
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_NONE);
    SDL_RenderSetScale(R, 3, 3);
    texto(150, 60, "OPCIONES  (L3 o O: cerrar)");
    int mn = g_menu_n;
    for (int mi = 0; mi < mn && mi < 10; mi++) {
      char ml[90];
      snprintf(ml, sizeof(ml), "%s%s", mi == g_menu_sel ? "> " : "  ", g_ml[mi]);
      texto(150, 82 + mi * 18, ml);
    }
    char mg[90];
    snprintf(mg, sizeof(mg), "%s", g_msg);
    texto(150, 82 + mn * 18 + 10, mg);
  }
  double t1 = ahora_us();
  SDL_RenderSetScale(R, 2, 2);
  char t[240];
  snprintf(t, sizeof(t), "F:%d run:%d.%d pre:%d.%d k:%d pad:%x au:%d in/s:%d q:%u", g_frames,
           m_run / 10, m_run % 10, m_pre / 10, m_pre % 10, g_kreal, (unsigned)g_btn, g_aud, m_ain, g_at - g_ah);
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

/* Copia el ultimo cuadro del nucleo a g_snap (solo 16 bits por pixel) */
static int tomar_cuadro(void) {
  if (!g_dp || g_dw == 0 || g_dh == 0) return 0;
  if (g_pf != 2 && g_pf != 0) return 0;
  if ((size_t)g_dw * (size_t)g_dh > (size_t)(1920 * 1080)) return 0;
  const unsigned char *src = (const unsigned char *)g_dp;
  for (unsigned y = 0; y < g_dh; y++) {
    memcpy(g_snap + (size_t)y * g_dw, src + (size_t)y * g_dpitch, (size_t)g_dw * 2);
  }
  g_sw = g_dw;
  g_sh = g_dh;
  return 1;
}

/* Hilo de dibujo: cuando hay un cuadro listo lo escala, lo sube y hace Present */
static int hilo_dibujo(void *arg) {
  (void)arg;
  esp("DIBUJO hilo: arranca");
  int primero = 1;
  for (;;) {
    if (__atomic_load_n(&g_dib_estado, __ATOMIC_ACQUIRE) == 1) {
      __atomic_store_n(&g_dib_estado, 2, __ATOMIC_RELAXED);
      if (primero) esp("DIBUJO hilo: primer cuadro, empieza");
      dibujar_juego();
      if (primero) {
        esp("DIBUJO hilo: primer cuadro, terminado");
        primero = 0;
      }
      __atomic_store_n(&g_dib_estado, 0, __ATOMIC_RELEASE);
    } else {
      /* espera sin llamar a ninguna libreria (por si fallan en este hilo) */
      __builtin_ia32_pause();
    }
  }
  return 0;
}

/* ---------------- menu de juegos ---------------- */
#define MAX_JUEGOS 64
#define CARPETA_JUEGOS "/data/swan/juegos"
static char g_jn[MAX_JUEGOS][128];
static int g_nj = 0;

static int minus(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int cmp_ci(const char *a, const char *b) {
  while (*a && *b && minus((unsigned char)*a) == minus((unsigned char)*b)) {
    a++;
    b++;
  }
  return minus((unsigned char)*a) - minus((unsigned char)*b);
}

static const char *extension(const char *n) {
  const char *p = NULL;
  for (; *n; n++) {
    if (*n == '.') p = n;
  }
  return p ? p + 1 : "";
}

static int ext_juego(const char *n) {
  const char *e = extension(n);
  return !cmp_ci(e, "cue") || !cmp_ci(e, "chd") || !cmp_ci(e, "pbp") || !cmp_ci(e, "iso") || !cmp_ci(e, "bin");
}

/* a y b con extension de 3 letras: mismo nombre sin la extension */
static int mismo_nombre(const char *a, const char *b) {
  size_t la = strlen(a), lb = strlen(b);
  if (la != lb || la < 4) return 0;
  for (size_t i = 0; i + 3 < la; i++) {
    if (minus((unsigned char)a[i]) != minus((unsigned char)b[i])) return 0;
  }
  return 1;
}

static int listar_juegos(void) {
  g_nj = 0;
  int fd = mia_k_open(CARPETA_JUEGOS, 0, 0);
  esp("MENU carpeta %s open -> %d (0x%x)", CARPETA_JUEGOS, fd, (unsigned)fd);
  if (fd < 0) return 0;
  static char buf[16384];
  int total_bytes = 0;
  for (;;) {
    int n = mia_k_getdents(fd, buf, (int)sizeof(buf));
    if (n <= 0) {
      if (n < 0) esp("MENU getdents error %d (0x%x)", n, (unsigned)n);
      break;
    }
    total_bytes += n;
    int pos = 0;
    while (pos + 8 <= n) {
      uint32_t fileno = 0;
      uint16_t reclen = 0;
      memcpy(&fileno, buf + pos, 4);
      memcpy(&reclen, buf + pos + 4, 2);
      unsigned tipo = (unsigned char)buf[pos + 6];
      unsigned namlen = (unsigned char)buf[pos + 7];
      const char *nm = buf + pos + 8;
      if (reclen < 8 || pos + (int)reclen > n) break;
      if (fileno != 0 && tipo != 4 && namlen > 0 && namlen < 128 && nm[0] != '.' && ext_juego(nm) &&
          !contiene(nm, "(track") && g_nj < MAX_JUEGOS) {
        snprintf(g_jn[g_nj], sizeof(g_jn[0]), "%.127s", nm);
        g_nj++;
      }
      pos += reclen;
    }
  }
  mia_k_close(fd);
  esp("MENU getdents bytes=%d archivos=%d", total_bytes, g_nj);

  /* oculta los .bin que tienen un .cue con el mismo nombre */
  int k = 0;
  for (int i = 0; i < g_nj; i++) {
    int ocultar = 0;
    if (!cmp_ci(extension(g_jn[i]), "bin")) {
      for (int j = 0; j < g_nj; j++) {
        if (j != i && !cmp_ci(extension(g_jn[j]), "cue") && mismo_nombre(g_jn[i], g_jn[j])) {
          ocultar = 1;
          break;
        }
      }
    }
    if (!ocultar) {
      if (k != i) memcpy(g_jn[k], g_jn[i], sizeof(g_jn[0]));
      k++;
    }
  }
  g_nj = k;

  /* orden alfabetico */
  for (int i = 1; i < g_nj; i++) {
    char tmp[128];
    memcpy(tmp, g_jn[i], sizeof(tmp));
    int j = i - 1;
    while (j >= 0 && cmp_ci(g_jn[j], tmp) > 0) {
      memcpy(g_jn[j + 1], g_jn[j], sizeof(tmp));
      j--;
    }
    memcpy(g_jn[j + 1], tmp, sizeof(tmp));
  }
  return g_nj;
}

static void dibujar_menu(int sel, int top) {
  SDL_SetRenderDrawColor(R, 0, 0, 80, 255);
  SDL_RenderClear(R);
  SDL_RenderSetScale(R, 2, 2);
  char t[200];
  snprintf(t, sizeof(t), "SwanStation PS4 - elige un juego (%d)", g_nj);
  texto(10, 10, t);
  texto(10, 26, "Arriba/Abajo: mover     X: jugar");
  float y = 52;
  const int VISIBLES = 28;
  if (top > 0) {
    texto(10, y, "   ...");
  }
  y += 14;
  for (int i = top; i < g_nj && i < top + VISIBLES; i++) {
    char nombre[128];
    snprintf(nombre, sizeof(nombre), "%.127s", g_jn[i]);
    char *p = NULL;
    for (char *q = nombre; *q; q++) {
      if (*q == '.') p = q;
    }
    if (p) *p = 0;
    snprintf(t, sizeof(t), "%s %.90s", (i == sel) ? ">" : " ", nombre);
    texto(10, y, t);
    y += 14;
  }
  if (top + VISIBLES < g_nj) texto(10, y, "   ...");
  SDL_RenderPresent(R);
}

/* Muestra el menu y devuelve 1 con la ruta elegida en out; 0 si no hay juegos en la carpeta */
static int elegir_juego(char *out, size_t outn) {
  int n = listar_juegos();
  if (n <= 0) return 0;

  int sel = 0;
  char ult[160];
  ult[0] = 0;
  FILE *f = fopen("/data/swan/ultimo.txt", "rb");
  if (f) {
    if (fgets(ult, sizeof(ult), f)) {
      for (char *q = ult; *q; q++) {
        if (*q == '\n' || *q == '\r') *q = 0;
      }
    }
    fclose(f);
  }
  for (int i = 0; i < n; i++) {
    if (ult[0] && !cmp_ci(g_jn[i], ult)) sel = i;
  }

  if (g_pad >= 0) {
    uint32_t prev = 0xFFFFFFFFu;
    int dir_prev = 0;
    unsigned t_sig = 0;
    int top = 0;
    {
      ScePadData d0;
      memset(&d0, 0, sizeof(d0));
      if (scePadReadState(g_pad, &d0) == 0) prev = d0.buttons;
    }
    for (;;) {
      SDL_PumpEvents();
      ScePadData d;
      memset(&d, 0, sizeof(d));
      uint32_t b = 0;
      int y = 128;
      if (scePadReadState(g_pad, &d) == 0) {
        b = d.buttons;
        y = d.leftStick.y;
      }
      int dir = 0;
      if ((b & PB_DOWN) || y > 192) dir = 1;
      if ((b & PB_UP) || y < 64) dir = -1;
      unsigned ahora = SDL_GetTicks();
      if (dir != 0) {
        if (dir != dir_prev) {
          sel = (sel + dir + n) % n;
          t_sig = ahora + 350;
        } else if (ahora >= t_sig) {
          sel = (sel + dir + n) % n;
          t_sig = ahora + 90;
        }
      }
      dir_prev = dir;
      uint32_t nuevos = b & ~prev;
      prev = b;
      if (nuevos & PB_CROSS) break;
      if (sel < top) top = sel;
      if (sel >= top + 28) top = sel - 27;
      dibujar_menu(sel, top);
      SDL_Delay(16);
    }
  }

  snprintf(out, outn, "%s/%s", CARPETA_JUEGOS, g_jn[sel]);
  f = fopen("/data/swan/ultimo.txt", "wb");
  if (f) {
    fputs(g_jn[sel], f);
    fclose(f);
  }
  esp("MENU elegido (%d de %d): %s", sel + 1, n, out);
  return 1;
}

/* ---------------- guardado de estado y menu de opciones ---------------- */
static void estado_ruta(char *out, size_t n, int slot) {
  snprintf(out, n, "/data/swan/estado_%s_%d.sta", g_juego_id, slot);
}

static void estado_guardar(void) {
  size_t sz = retro_serialize_size();
  esp("ESTADO guardar: ranura=%d tam=%u", g_ranura, (unsigned)sz);
  if (sz == 0) { snprintf(g_msg, sizeof(g_msg), "El nucleo no da tamano de estado"); return; }
  if (sz > sizeof(g_estbuf)) { snprintf(g_msg, sizeof(g_msg), "Estado demasiado grande (%u KB)", (unsigned)(sz / 1024)); return; }
  if (!retro_serialize(g_estbuf, sz)) { snprintf(g_msg, sizeof(g_msg), "Fallo al crear el estado"); esp("ESTADO retro_serialize FALLO"); return; }
  char p[220];
  estado_ruta(p, sizeof(p), g_ranura);
  FILE *f = fopen(p, "wb");
  if (!f) { snprintf(g_msg, sizeof(g_msg), "No se pudo crear el archivo"); esp("ESTADO fopen escritura FALLO %s", p); return; }
  size_t w = fwrite(g_estbuf, 1, sz, f);
  fclose(f);
  if (w != sz) { snprintf(g_msg, sizeof(g_msg), "Escritura incompleta"); esp("ESTADO escritura %u de %u", (unsigned)w, (unsigned)sz); return; }
  snprintf(g_msg, sizeof(g_msg), "Guardado en ranura %d (%u KB)", g_ranura, (unsigned)(sz / 1024));
  esp("ESTADO guardado OK %s", p);
}

static void estado_cargar(void) {
  char p[220];
  estado_ruta(p, sizeof(p), g_ranura);
  long long n = tam_ruta(p);
  esp("ESTADO cargar: %s tam=%lld", p, n);
  if (n <= 0) { snprintf(g_msg, sizeof(g_msg), "Ranura %d vacia", g_ranura); return; }
  if ((unsigned long long)n > sizeof(g_estbuf)) { snprintf(g_msg, sizeof(g_msg), "Archivo demasiado grande"); return; }
  FILE *f = fopen(p, "rb");
  if (!f) { snprintf(g_msg, sizeof(g_msg), "No se pudo abrir el archivo"); return; }
  size_t r = fread(g_estbuf, 1, (size_t)n, f);
  fclose(f);
  if (r != (size_t)n) { snprintf(g_msg, sizeof(g_msg), "Lectura incompleta"); return; }
  if (!retro_unserialize(g_estbuf, (size_t)n)) { snprintf(g_msg, sizeof(g_msg), "El nucleo rechazo el estado"); esp("ESTADO retro_unserialize FALLO"); return; }
  snprintf(g_msg, sizeof(g_msg), "Cargado de ranura %d", g_ranura);
  esp("ESTADO cargado OK");
}

static void jit_conmutar(void) {
  if (g_jit_pedido) {
    unlink("/data/swan/jit.txt");
    g_jit_pedido = 0;
  } else {
    int fd = open("/data/swan/jit.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) close(fd);
    g_jit_pedido = existe("/data/swan/jit.txt") ? 1 : 0;
  }
  esp("MENU: JIT pedido para el proximo arranque = %d", g_jit_pedido);
  snprintf(g_msg, sizeof(g_msg), "JIT %s: reinicia la app para aplicarlo", g_jit_pedido ? "activado" : "desactivado");
}

static void suav_cambiar(int d) {
  g_suav = (g_suav + d + 3) % 3;
  FILE *f = fopen("/data/swan/suav.txt", "wb");
  if (f) { fputc('0' + g_suav, f); fclose(f); }
  esp("MENU: suavizado = %d", g_suav);
  snprintf(g_msg, sizeof(g_msg), "Suavizado cambiado (no aplica con escala Original)");
}

static void menu_refrescar(void) {
  const char *esc[4] = {"Original", "2x", "3x", "Maxima"};
  snprintf(g_ml[0], sizeof(g_ml[0]), "Guardar estado");
  snprintf(g_ml[1], sizeof(g_ml[1]), "Cargar estado");
  snprintf(g_ml[2], sizeof(g_ml[2]), "Ranura: %d  (izq/der)", g_ranura);
  snprintf(g_ml[3], sizeof(g_ml[3]), "Escala: %s  (izq/der)", esc[g_modo & 3]);
  snprintf(g_ml[4], sizeof(g_ml[4]), "Aspecto: %s", g_asp ? "4:3" : "Cuadrado");
  const char *sv[3] = {"No", "Bajo", "Alto"};
  snprintf(g_ml[5], sizeof(g_ml[5]), "Suavizado: %s  (izq/der)", sv[g_suav % 3]);
  snprintf(g_ml[6], sizeof(g_ml[6]), "Recompilador JIT: %s (al reiniciar)", g_jit_pedido ? "Si" : "No");
  snprintf(g_ml[7], sizeof(g_ml[7]), "Continuar");
  g_menu_n = 8;
}

static void menu_pedir_dibujo(void) {
  if (g_dib_hilo) {
    if (__atomic_load_n(&g_dib_estado, __ATOMIC_ACQUIRE) == 0)
      __atomic_store_n(&g_dib_estado, 1, __ATOMIC_RELEASE);
  } else {
    dibujar_juego();
  }
}

/* El juego queda en pausa (no se llama a retro_run) mientras el menu esta abierto */
static void pausa_menu(void) {
  esp("MENU opciones: abre");
  g_menu_sel = 0;
  g_msg[0] = 0;
  menu_refrescar();
  g_menu_vis = 1;
  poll();
  g_menu_toggle = 0;
  unsigned prev = g_mask;
  for (;;) {
    poll();
    if (g_menu_toggle) { g_menu_toggle = 0; break; }
    unsigned m = g_mask;
    unsigned nw = m & ~prev;
    prev = m;
    int salir = 0;
    if (nw & (1u << 4)) g_menu_sel = (g_menu_sel + g_menu_n - 1) % g_menu_n;
    if (nw & (1u << 5)) g_menu_sel = (g_menu_sel + 1) % g_menu_n;
    int dx = ((nw & (1u << 7)) ? 1 : 0) - ((nw & (1u << 6)) ? 1 : 0);
    if (dx != 0) {
      if (g_menu_sel == 2) g_ranura = ((g_ranura - 1 + dx + 4) % 4) + 1;
      else if (g_menu_sel == 3) g_modo = (g_modo + dx + 4) % 4;
      else if (g_menu_sel == 4) g_asp = !g_asp;
      else if (g_menu_sel == 5) suav_cambiar(dx);
      else if (g_menu_sel == 6) jit_conmutar();
    }
    if (nw & (1u << 8)) salir = 1;
    if (nw & (1u << 0)) {
      if (g_menu_sel == 0) estado_guardar();
      else if (g_menu_sel == 1) estado_cargar();
      else if (g_menu_sel == 2) g_ranura = (g_ranura % 4) + 1;
      else if (g_menu_sel == 3) g_modo = (g_modo + 1) % 4;
      else if (g_menu_sel == 4) g_asp = !g_asp;
      else if (g_menu_sel == 5) suav_cambiar(1);
      else if (g_menu_sel == 6) jit_conmutar();
      else if (g_menu_sel == 7) salir = 1;
    }
    menu_refrescar();
    if (salir) break;
    SDL_PumpEvents();
    menu_pedir_dibujo();
    SDL_Delay(30);
  }
  g_menu_vis = 0;
  esp("MENU opciones: cierra");
}

/* ---------------- prueba de memoria ejecutable (JIT), solo con jit_prueba.txt ----------------
   Imita lo que hace EmuSuNi (port/ps4_n64.c): array estatico alineado a 16 KiB, sceKernelMprotect con
   lectura+escritura+ejecucion y una funcion minima ejecutada ahi. Cada paso se escribe en el log ANTES
   de hacerlo: si la app se cierra, la ultima linea JIT dice donde. */
#define JIT_PRUEBA_TAM (64 * 1024)
static unsigned char g_jit_prueba[JIT_PRUEBA_TAM] __attribute__((aligned(16384)));

static void jit_prueba(void) {
  esp("JIT prueba: array en %p (alineado a 16 KiB: %s), %d KiB", (void *)g_jit_prueba,
      (((unsigned long)g_jit_prueba) & 16383u) == 0 ? "si" : "NO", JIT_PRUEBA_TAM / 1024);
  esp("JIT prueba: distancia al codigo del frontend = %lld MiB",
      (long long)(((long)g_jit_prueba - (long)&existe) / 1048576));
  esp("JIT prueba: llamando a sceKernelMprotect(RWX)...");
  int r = mia_k_mprotect(g_jit_prueba, JIT_PRUEBA_TAM, 7);
  esp("JIT prueba: sceKernelMprotect = %d (%#x)", r, (unsigned)r);
  if (r != 0) {
    esp("JIT prueba: FIN, la PS4 no da memoria ejecutable por esta via");
    return;
  }
  static const unsigned char codigo[] = {0xB8, 0x36, 0x4E, 0x00, 0x00, 0xC3}; /* mov eax,0x4E36; ret */
  memcpy(g_jit_prueba, codigo, sizeof(codigo));
  esp("JIT prueba: ejecutando la funcion de prueba (si el log acaba aqui, ejecutar ahi cierra la app)...");
  int v = ((int (*)(void))g_jit_prueba)();
  esp("JIT prueba: la funcion devolvio %#x (esperado 0x4e36): %s", (unsigned)v, v == 0x4E36 ? "FUNCIONA" : "resultado raro");
}

int main(void) {
  SDL_Init(SDL_INIT_VIDEO);
  SDL_Window *w = SDL_CreateWindow("swan", 0, 0, 1920, 1080, 0);
  R = SDL_CreateRenderer(w, -1, 0);
  if (!R) R = SDL_CreateRenderer(w, -1, SDL_RENDERER_SOFTWARE);
  g_main = SDL_ThreadID();
  mkdir("/data/swan", 0777);

  /* Por defecto: Cached Interpreter. Con /data/swan/puro.txt: interprete puro */
  g_interp = existe("/data/swan/puro.txt") ? 1 : 0;
  g_cached = g_interp ? 0 : 1;
  g_fd = open("/data/swan/espia.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
  esp("ARRANQUE interp=%d cached=%d", g_interp, g_cached);
  if (existe("/data/swan/jit.txt")) {
    g_jit = 1;
    g_interp = 0;
    g_cached = 0;
    esp("JIT activado por jit.txt (modo Recompiler)");
  }
  g_jit_pedido = g_jit;

  SDL_RendererInfo ri;
  memset(&ri, 0, sizeof(ri));
  if (R && SDL_GetRendererInfo(R, &ri) == 0 && ri.name) {
    snprintf(g_rend, sizeof(g_rend), "%.30s", ri.name);
    esp("RENDERER nombre=%s flags=%x", ri.name, (unsigned)ri.flags);
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

  /* Audio: puerto principal, 256 muestras por bloque, 48000 Hz, S16 estereo.
     Suposicion: tipo 0 = MAIN y formato 1 = S16_STEREO */
  int ra = mia_aud_init();
  int ids[3] = {0xFF, (int)uid, 0};
  for (int k = 0; k < 3; k++) {
    g_aud = mia_aud_open(ids[k], 0, 0, GRANO, 48000, 1);
    esp("AUDIO init=%d open(uid=%d) -> %d (0x%x)", ra, ids[k], g_aud, (unsigned)g_aud);
    if (g_aud >= 0) break;
  }

  struct retro_system_info info;
  memset(&info, 0, sizeof(info));
  retro_get_system_info(&info);
  char t[160];
  snprintf(t, sizeof(t), "%s %s API %u", info.library_name ? info.library_name : "?", info.library_version ? info.library_version : "?", retro_api_version());
  paso(t);
  snprintf(t, sizeof(t), "fd=%d interp=%d pad=%d audio=%d rend=%s", g_fd, g_interp, g_pad, g_aud, g_rend);
  paso(t);

  if (existe("/data/swan/jit_prueba.txt")) jit_prueba();
  retro_set_environment(entorno);
  retro_set_video_refresh(video);
  retro_set_audio_sample(audio1);
  retro_set_audio_sample_batch(audio2);
  retro_set_input_poll(poll);
  retro_set_input_state(entrada);
  paso("1) retro_init...");
  retro_init();
  paso("2) retro_init OK");

  char ruta_menu[300];
  const char *ruta = NULL;
  if (elegir_juego(ruta_menu, sizeof(ruta_menu))) ruta = ruta_menu;
  if (!ruta) {
    /* sin carpeta de juegos: se usa juego.* en /data/swan, como antes */
    const char *juegos[] = {"/data/swan/juego.chd", "/data/swan/juego.cue", "/data/swan/juego.pbp", "/data/swan/juego.iso", "/data/swan/juego.bin"};
    for (int i = 0; i < 5; i++) {
      if (existe(juegos[i])) { ruta = juegos[i]; break; }
    }
  }
  if (!ruta) {
    paso("Sin juegos: pon archivos en /data/swan/juegos");
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

  struct retro_system_av_info av;
  memset(&av, 0, sizeof(av));
  retro_get_system_av_info(&av);
  if (av.sample_rate > 8000.0 && av.sample_rate < 96000.0) g_srate = av.sample_rate;
  esp("AV fps=%.2f sample_rate=%.1f base=%ux%u", av.fps, av.sample_rate, av.base_width, av.base_height);

  {
    const char *b = ruta;
    for (const char *q = ruta; *q; q++) {
      if (*q == '/') b = q + 1;
    }
    snprintf(g_juego_id, sizeof(g_juego_id), "%s", b);
    char *pt = NULL;
    for (char *q = g_juego_id; *q; q++) {
      if (*q == '.') pt = q;
    }
    if (pt) *pt = 0;
    for (char *q = g_juego_id; *q; q++) {
      char c = *q;
      int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
      if (!ok) *q = '_';
    }
    if (!g_juego_id[0]) snprintf(g_juego_id, sizeof(g_juego_id), "juego");
    esp("ESTADO id del juego: %s", g_juego_id);
  }
  paso("4) load_game OK; mostrando imagen...");
  esp("INICIO frames (sin limite)");
  g_jugando = 1;
  int i = 0;
  unsigned long long ain_prev = 0;
  unsigned t_prev = SDL_GetTicks();
  unsigned hb_prev = 0, hs_prev = 0;
  if (g_aud >= 0) {
    SDL_Thread *th = SDL_CreateThread(hilo_audio, "audio", NULL);
    g_hilo_ok = th ? 1 : 0;
    esp("AUDIO hilo %s", g_hilo_ok ? "creado" : "NO se pudo crear (audio en el bucle principal)");
  }
  /* El hilo de dibujo se usa siempre, salvo que exista /data/swan/sin_hilo.txt */
  int quiere_hilo = existe("/data/swan/sin_hilo.txt") ? 0 : 1;
  /* Con /data/swan/sin_aspecto.txt se arranca con pixeles cuadrados */
  if (existe("/data/swan/sin_aspecto.txt")) g_asp = 0;
  {
    FILE *fs = fopen("/data/swan/suav.txt", "rb");
    if (fs) {
      int c = fgetc(fs);
      fclose(fs);
      if (c >= '0' && c <= '2') g_suav = c - '0';
    }
    esp("SUAVIZADO inicial=%d", g_suav);
  }
  esp("DIBUJO hilo pedido=%d pf=%d", quiere_hilo, g_pf);
  if (quiere_hilo && (g_pf == 2 || g_pf == 0)) {
    SDL_Thread *td = SDL_CreateThread(hilo_dibujo, "dibujo", NULL);
    g_dib_hilo = td ? 1 : 0;
    esp("DIBUJO hilo %s", g_dib_hilo ? "creado" : "NO se pudo crear (dibujo en el bucle principal)");
  }
  g_cada = g_dib_hilo ? 2 : 3;
  for (;;) {
    double a = ahora_us();
    retro_run();
    a_run += ahora_us() - a;
    if (!g_hilo_ok) audio_enviar();
    if (g_menu_toggle) {
      g_menu_toggle = 0;
      pausa_menu();
    }
    i++;
    g_frames = i;
    if (i % g_cada == 0) {
      SDL_PumpEvents();
      if (g_dib_hilo) {
        if (__atomic_load_n(&g_dib_estado, __ATOMIC_ACQUIRE) == 0 && tomar_cuadro())
          __atomic_store_n(&g_dib_estado, 1, __ATOMIC_RELEASE);
      } else if (tomar_cuadro()) {
        dibujar_juego();
      }
    }
    /* Ritmo: el audio marca el reloj. Si hay mas de ~50 ms acumulados, se espera
       (tope 40 ms por frame, por si el hilo de audio se atasca). */
    if (g_hilo_ok) {
      int esperas = 0;
      while (esperas < 40 &&
             (__atomic_load_n(&g_at, __ATOMIC_ACQUIRE) - __atomic_load_n(&g_ah, __ATOMIC_ACQUIRE)) > GRANO * 10) {
        SDL_Delay(1);
        esperas++;
      }
    }
    if (i % 60 == 0) {
      unsigned ahora = SDL_GetTicks();
      unsigned dt = ahora - t_prev;
      if (dt > 0) m_ain = (int)((g_ain - ain_prev) * 1000ULL / dt);
      ain_prev = g_ain;
      t_prev = ahora;
      m_run = (int)(a_run / 60.0 / 100.0);
      m_upd = n_upd ? (int)(a_upd / n_upd / 100.0) : 0;
      m_cop = n_dib ? (int)(a_cop / n_dib / 100.0) : 0;
      m_pre = n_dib ? (int)(a_pre / n_dib / 100.0) : 0;
      m_esc = n_dib ? (int)(a_esc / n_dib / 100.0) : 0;
      m_aud = (int)(a_aud / 60.0 / 100.0);
      m_tot = (int)(dt * 10u / 60u);
      unsigned hb = g_hblq, hs = g_hsil;
      unsigned dblq = g_hilo_ok ? (hb - hb_prev) : (unsigned)n_blq;
      unsigned dsil = hs - hs_prev;
      hb_prev = hb;
      hs_prev = hs;
      esp("frame %d run=%d cop=%d pre=%d esc=%d dib=%d blq=%u sil=%u tot=%d k=%d cada=%d au=%d in/s=%d q=%u aok=%d", i, m_run, m_cop, m_pre, m_esc, n_dib, dblq, dsil, m_tot, g_kreal, g_cada, g_aud, m_ain, g_at - g_ah, g_aout_ok);
      a_aud = 0;
      n_blq = 0;
      a_run = a_upd = a_cop = a_pre = a_esc = 0;
      n_upd = n_dib = 0;
    }
  }
  return 0;
}
