/* main.c — entry point: parse --backend, build the world, dispatch to a renderer.
 *
 *   ./csim [--backend raylib|sdl2] [-b raylib|sdl2]
 *
 * Both backends share the same sim core; the only difference is the renderer.
 * Backends not compiled in (see CMake HAVE_RAYLIB/HAVE_SDL2) fall back to a stub
 * that explains how to enable them.
 */
#include "sim.h"
#include "viz.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Stubs for backends that weren't compiled in. */
#ifndef HAVE_RAYLIB
int run_raylib(World *w) {
    (void)w;
    fprintf(stderr, "raylib backend not built. Reconfigure with raylib available "
                    "(CMake fetches it automatically).\n");
    return 1;
}
#endif
#ifndef HAVE_NC
int run_notcurses(World *w) {
    (void)w;
    fprintf(stderr, "notcurses backend not built. Install it and rebuild:\n"
                    "  sudo apt install libnotcurses-dev\n");
    return 1;
}
#endif

static void usage(const char *argv0) {
    printf("Usage: %s [--backend raylib|notcurses] [--blit MODE]\n", argv0);
    printf("  --blit (notcurses only): default|pixel|sextant|quad|half|braille|ascii\n"
           "         pixel = true terminal pixel graphics (Kitty/Sixel/iTerm2);\n"
           "         sextant = 2x3 sub-cell blocks (works everywhere, incl. tmux).\n");
#if defined(HAVE_RAYLIB)
    printf("  raylib backend: built\n");
#else
    printf("  raylib backend: NOT built\n");
#endif
#if defined(HAVE_NC)
    printf("  notcurses:      built\n");
#else
    printf("  notcurses:      NOT built (sudo apt install libnotcurses-dev)\n");
#endif
}

int main(int argc, char **argv) {
#if defined(HAVE_RAYLIB)
    const char *backend = "raylib";
#elif defined(HAVE_NC)
    const char *backend = "notcurses";
#else
    const char *backend = "none";
#endif

    /* --blit default: the CLI flag wins, else CSIM_NCBLIT env, else "default". */
    const char *env_blit = getenv("CSIM_NCBLIT");
    if (env_blit) g_blit = env_blit;

    for (int i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "--backend") || !strcmp(argv[i], "-b")) && i + 1 < argc)
            backend = argv[++i];
        else if (!strncmp(argv[i], "--backend=", 10))
            backend = argv[i] + 10;
        else if (!strcmp(argv[i], "--blit") && i + 1 < argc)
            g_blit = argv[++i];
        else if (!strncmp(argv[i], "--blit=", 7))
            g_blit = argv[i] + 7;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]); return 0;
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            usage(argv[0]); return 2;
        }
    }

    World w;
    world_init(&w, 1337);
    world_populate(&w, 150);

    if (!strcmp(backend, "raylib")) return run_raylib(&w);
    if (!strcmp(backend, "notcurses") || !strcmp(backend, "nc")) return run_notcurses(&w);
    fprintf(stderr, "unknown backend '%s' (use raylib or notcurses)\n", backend);
    usage(argv[0]);
    return 2;
}
