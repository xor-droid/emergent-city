/* main.c — entry point: build the world, pick a windowed backend, run the UI.
 *
 *   ./csim [--backend raylib|sdl3|glfw]
 *
 * All three backends render the same UI (src/ui.c) through the gfx.h interface.
 * Only the backends compiled in (CMake HAVE_RAYLIB/HAVE_SDL3/HAVE_GLFW) are
 * selectable; raylib is the default.
 */
#include "sim.h"
#include "gfx.h"
#include "llm.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const GfxBackend *pick(const char *name) {
#ifdef HAVE_RAYLIB
    if (!strcmp(name, "raylib")) return gfx_raylib();
#endif
#ifdef HAVE_SDL3
    if (!strcmp(name, "sdl3"))   return gfx_sdl3();
#endif
#ifdef HAVE_GLFW
    if (!strcmp(name, "glfw"))   return gfx_glfw();
#endif
    return NULL;
}

static const GfxBackend *first_available(void) {
#if defined(HAVE_RAYLIB)
    return gfx_raylib();
#elif defined(HAVE_SDL3)
    return gfx_sdl3();
#elif defined(HAVE_GLFW)
    return gfx_glfw();
#else
    return NULL;
#endif
}

static void usage(const char *argv0) {
    printf("Usage: %s [--backend NAME]\n", argv0);
    printf("  NAME is one of the compiled-in windowed backends:");
#ifdef HAVE_RAYLIB
    printf(" raylib");
#endif
#ifdef HAVE_SDL3
    printf(" sdl3");
#endif
#ifdef HAVE_GLFW
    printf(" glfw");
#endif
    printf("   (default: raylib)\n");
    printf("  Controls: drag/wheel pan+zoom, click a citizen to inspect, g god mode,\n");
    printf("            j jail, f factions, l legend, Tab feed, Space pause, 1/2/3 speed.\n");
    printf("  Env: CSIM_ZOOM=N initial zoom, CSIM_DEMO=1 open panels, CSIM_SHOT=path screenshot.\n");
}

int main(int argc, char **argv) {
    const char *backend = "raylib";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(argv[0]); return 0; }
        else if ((!strcmp(argv[i], "--backend") || !strcmp(argv[i], "-b")) && i + 1 < argc) backend = argv[++i];
        else if (!strncmp(argv[i], "--backend=", 10)) backend = argv[i] + 10;
        else { fprintf(stderr, "unknown argument: %s\n", argv[i]); usage(argv[0]); return 2; }
    }

    const GfxBackend *gb = pick(backend);
    if (!gb) {
        fprintf(stderr, "backend '%s' not available; ", backend);
        gb = first_available();
        if (!gb) { fprintf(stderr, "no windowed backend compiled in.\n"); return 2; }
        fprintf(stderr, "using '%s' instead.\n", gb->name);
    }
    G = gb;

    World w;
    world_init(&w, 1337);
    world_populate(&w, 150);
    dotenv_autoload();   /* pick up the project .env (OPENROUTER_* vars) */
    llm_init();          /* enabled only if OPENROUTER_API_KEY is set */

    int rc = run_ui(&w);
    llm_shutdown();
    return rc;
}
