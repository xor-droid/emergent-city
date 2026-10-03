/* viz.h — shared rendering helpers + backend entry points.
 *
 * Two interchangeable renderers implement run_*(World*): raylib and SDL2.
 * Which ones are compiled is decided by CMake (HAVE_RAYLIB / HAVE_SDL2); the
 * one that runs is chosen at runtime via --backend. Missing backends get a
 * stub (in main.c) that prints how to enable them.
 */
#ifndef VIZ_H
#define VIZ_H

#include "sim.h"

#define TILE_PX 8
#define FONT_PATH "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"

/* notcurses blitter selection (set from --blit / CSIM_NCBLIT; "default" auto). */
extern const char *g_blit;

/* Backend-neutral tile color (RGB). */
void tile_rgb(TileType t, unsigned char *r, unsigned char *g, unsigned char *b);

/* Compose the HUD status line shared by both backends. */
void hud_string(const World *w, char *buf, int n, float speed, int paused,
                int fps, const char *backend);

/* Renderer entry points. Return 0 on clean exit, nonzero if unavailable. */
int run_raylib(World *w);
int run_sdl2(World *w);
int run_tui(World *w);     /* ncurses ASCII renderer */
int run_notcurses(World *w); /* notcurses high-res terminal renderer */

#endif /* VIZ_H */
