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

/* Stubs for backends that weren't compiled in. */
#ifndef HAVE_RAYLIB
int run_raylib(World *w) {
    (void)w;
    fprintf(stderr, "raylib backend not built. Reconfigure with raylib available "
                    "(CMake fetches it automatically).\n");
    return 1;
}
#endif
#ifndef HAVE_SDL2
int run_sdl2(World *w) {
    (void)w;
    fprintf(stderr, "sdl2 backend not built. Install it and rebuild:\n"
                    "  sudo apt install libsdl2-dev libsdl2-ttf-dev\n");
    return 1;
}
#endif
#ifndef HAVE_TUI
int run_tui(World *w) {
    (void)w;
    fprintf(stderr, "tui backend not built. Install it and rebuild:\n"
                    "  sudo apt install libncurses-dev\n");
    return 1;
}
#endif

static void usage(const char *argv0) {
    printf("Usage: %s [--backend raylib|sdl2|tui]\n", argv0);
#if defined(HAVE_RAYLIB)
    printf("  raylib backend: built\n");
#else
    printf("  raylib backend: NOT built\n");
#endif
#if defined(HAVE_SDL2)
    printf("  sdl2 backend:   built\n");
#else
    printf("  sdl2 backend:   NOT built (sudo apt install libsdl2-dev libsdl2-ttf-dev)\n");
#endif
#if defined(HAVE_TUI)
    printf("  tui backend:    built\n");
#else
    printf("  tui backend:    NOT built (sudo apt install libncurses-dev)\n");
#endif
}

int main(int argc, char **argv) {
#if defined(HAVE_RAYLIB)
    const char *backend = "raylib";
#elif defined(HAVE_SDL2)
    const char *backend = "sdl2";
#else
    const char *backend = "none";
#endif

    for (int i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "--backend") || !strcmp(argv[i], "-b")) && i + 1 < argc)
            backend = argv[++i];
        else if (!strncmp(argv[i], "--backend=", 10))
            backend = argv[i] + 10;
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
    if (!strcmp(backend, "sdl2"))   return run_sdl2(&w);
    if (!strcmp(backend, "tui") || !strcmp(backend, "ascii")) return run_tui(&w);
    fprintf(stderr, "unknown backend '%s' (use raylib, sdl2 or tui)\n", backend);
    usage(argv[0]);
    return 2;
}
