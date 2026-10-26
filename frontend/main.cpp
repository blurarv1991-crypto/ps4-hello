#include <SDL2/SDL.h>
#include <stdio.h>
#include "stb_easy_font.h"

extern "C" {
struct retro_system_info {
  const char *library_name;
  const char *library_version;
  const char *valid_extensions;
  bool need_fullpath;
  bool block_extract;
};
unsigned retro_api_version(void);
void retro_get_system_info(struct retro_system_info *info);
}

static void texto(SDL_Renderer *r, float x, float y, const char *s) {
  static char buf[60000];
  int n = stb_easy_font_print(x, y, (char *)s, NULL, buf, sizeof(buf));
  SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
  for (int i = 0; i < n; i++) {
    float *v = (float *)(buf + i * 64);
    SDL_Rect q = {(int)v[0], (int)v[1], (int)(v[8] - v[0]), (int)(v[9] - v[1])};
    SDL_RenderFillRect(r, &q);
  }
}

int main(void) {
  struct retro_system_info info = {0};
  retro_get_system_info(&info);
  unsigned api = retro_api_version();

  char l1[128], l2[128], l3[128];
  snprintf(l1, sizeof(l1), "Nucleo: %s", info.library_name ? info.library_name : "?");
  snprintf(l2, sizeof(l2), "Version: %s", info.library_version ? info.library_version : "?");
  snprintf(l3, sizeof(l3), "API libretro: %u", api);

  SDL_Init(SDL_INIT_VIDEO);
  SDL_Window *w = SDL_CreateWindow("swan", 0, 0, 1920, 1080, 0);
  SDL_Renderer *r = SDL_CreateRenderer(w, -1, 0);
  for (int i = 0; i < 600; i++) {
    SDL_SetRenderDrawColor(r, 0, 0, 80, 255);
    SDL_RenderClear(r);
    SDL_RenderSetScale(r, 4, 4);
    texto(r, 20, 20, l1);
    texto(r, 20, 40, l2);
    texto(r, 20, 60, l3);
    SDL_RenderPresent(r);
    SDL_Delay(16);
  }
  SDL_Quit();
  return 0;
}
