/* backend_notcurses.c — high-res terminal renderer. Compiled only when HAVE_NC.
 *
 * Paints the whole world into an RGBA bitmap and blits it with notcurses' best
 * available blitter: true pixel graphics (Sixel/Kitty) if the terminal supports
 * them, otherwise sextants (2x3 sub-cell) — so the terminal shows far more than
 * one glyph per tile. True color throughout.
 * Keys: q quit, space pause, 1/2/3 speed.
 */
#define _GNU_SOURCE   /* wcwidth/wcswidth used by notcurses.h, clock_gettime, nanosleep */
#include <wchar.h>
#include "sim.h"
#include "viz.h"
#include <notcurses/notcurses.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PXT 3   /* source-bitmap pixels per tile */

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int run_notcurses(World *w) {
    struct notcurses_options opts;
    memset(&opts, 0, sizeof(opts));
    opts.flags = NCOPTION_SUPPRESS_BANNERS;
    struct notcurses *nc = notcurses_init(&opts, NULL);
    if (!nc) { fprintf(stderr, "notcurses_init failed\n"); return 1; }
    struct ncplane *std = notcurses_stdplane(nc);

    /* Blitter: default = best available (pixel graphics if the terminal
     * supports them). CSIM_NCBLIT can force a text-cell blitter so it works /
     * is capturable anywhere: sextant (2x3), quad (2x2), half (2x1), braille,
     * ascii (1x1). */
    ncblitter_e blit = NCBLIT_DEFAULT;
    const char *bl = g_blit;   /* set from --blit (falls back to CSIM_NCBLIT) */
    if (bl) {
        if (!strcmp(bl, "sextant")) blit = NCBLIT_3x2;
        else if (!strcmp(bl, "quad")) blit = NCBLIT_2x2;
        else if (!strcmp(bl, "half")) blit = NCBLIT_2x1;
        else if (!strcmp(bl, "braille")) blit = NCBLIT_BRAILLE;
        else if (!strcmp(bl, "ascii")) blit = NCBLIT_1x1;
        else if (!strcmp(bl, "pixel")) blit = NCBLIT_PIXEL;
    }

    const int W = WORLD_W * PXT, H = WORLD_H * PXT;
    unsigned char *buf = (unsigned char *)malloc((size_t)W * H * 4);
    if (!buf) { notcurses_stop(nc); return 1; }

    int paused = 0, running = 1, fps = 0, frames = 0;
    float speed = 1.0f;
    double prev = now_sec(), facc = 0.0;

    while (running) {
        struct ncinput ni;
        uint32_t id;
        while ((id = notcurses_get_nblock(nc, &ni)) != 0) {
            if (ni.evtype == NCTYPE_RELEASE) continue;
            if (id == 'q' || id == 'Q') running = 0;
            else if (id == ' ') paused = !paused;
            else if (id == '1') speed = 1.0f;
            else if (id == '2') speed = 5.0f;
            else if (id == '3') speed = 20.0f;
        }

        double t = now_sec(), dt = t - prev; prev = t;
        facc += dt; frames++;
        if (facc >= 0.5) { fps = (int)(frames / facc); frames = 0; facc = 0.0; }
        if (!paused && dt > 0.0) world_tick(w, dt * speed);

        /* paint tiles */
        for (int x = 0; x < WORLD_W; x++) {
            for (int y = 0; y < WORLD_H; y++) {
                unsigned char r, g, b;
                tile_rgb((TileType)w->tile[x][y], &r, &g, &b);
                for (int py = 0; py < PXT; py++)
                    for (int px = 0; px < PXT; px++) {
                        size_t o = ((size_t)(y * PXT + py) * W + (x * PXT + px)) * 4;
                        buf[o] = r; buf[o + 1] = g; buf[o + 2] = b; buf[o + 3] = 255;
                    }
            }
        }
        /* paint agents on top */
        for (int i = 0; i < w->n_agents; i++) {
            Agent *a = &w->agents[i];
            if (!a->alive) continue;
            int bx = (int)a->x * PXT, by = (int)a->y * PXT;
            for (int py = 0; py < PXT; py++)
                for (int px = 0; px < PXT; px++) {
                    int X = bx + px, Y = by + py;
                    if (X < 0 || X >= W || Y < 0 || Y >= H) continue;
                    size_t o = ((size_t)Y * W + X) * 4;
                    buf[o] = a->r; buf[o + 1] = a->g; buf[o + 2] = a->b; buf[o + 3] = 255;
                }
        }

        struct ncvisual *v = ncvisual_from_rgba(buf, H, W * 4, W);
        if (v) {
            struct ncvisual_options vopts;
            memset(&vopts, 0, sizeof(vopts));
            vopts.n = std;
            vopts.scaling = NCSCALE_SCALE;      /* preserve aspect */
            vopts.blitter = blit;               /* pixel if available, else sextants */
            ncvisual_blit(nc, v, &vopts);
            ncvisual_destroy(v);
        }

        /* HUD on the top row */
        char hud[256];
        hud_string(w, hud, sizeof(hud), speed, paused, fps, "notcurses");
        ncplane_set_fg_rgb8(std, 230, 225, 215);
        ncplane_set_bg_rgb8(std, 0, 0, 0);
        ncplane_putstr_yx(std, 0, 0, hud);

        notcurses_render(nc);
        struct timespec slp = {0, 33 * 1000 * 1000};
        nanosleep(&slp, NULL);
    }

    free(buf);
    notcurses_stop(nc);
    return 0;
}
