/* main.c — entry point: build the world, run the raylib renderer.
 *
 *   ./csim [--backend raylib]
 *
 * The simulation renders in a GPU window (raylib). A stub (below) explains how
 * to enable raylib if it wasn't compiled in.
 */
#include "sim.h"
#include "viz.h"
#include "llm.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Stub if raylib wasn't compiled in. */
#ifndef HAVE_RAYLIB
int run_raylib(World *w) {
    (void)w;
    fprintf(stderr, "raylib backend not built. Reconfigure with raylib available "
                    "(CMake fetches it automatically).\n");
    return 1;
}
#endif

static void usage(const char *argv0) {
    printf("Usage: %s [--backend raylib]\n", argv0);
    printf("  Renders the simulation in a GPU window (raylib).\n");
    printf("  Controls: drag/arrows pan, wheel zoom, click a citizen to inspect,\n");
    printf("            g god mode, j jail, f factions, l legend, Tab feed,\n");
    printf("            Space pause, 1/2/3 speed, Esc quit.\n");
    printf("  Env: CSIM_ZOOM=N initial zoom, CSIM_DEMO=1 open panels,\n");
    printf("       CSIM_SHOT=path dump a screenshot.\n");
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]); return 0;
        } else if ((!strcmp(argv[i], "--backend") || !strcmp(argv[i], "-b")) && i + 1 < argc) {
            if (strcmp(argv[++i], "raylib")) {
                fprintf(stderr, "only the raylib backend is available\n"); return 2;
            }
        } else if (!strncmp(argv[i], "--backend=", 10)) {
            if (strcmp(argv[i] + 10, "raylib")) {
                fprintf(stderr, "only the raylib backend is available\n"); return 2;
            }
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            usage(argv[0]); return 2;
        }
    }

    World w;
    world_init(&w, 1337);
    world_populate(&w, 150);
    dotenv_autoload();   /* pick up the project .env (OPENROUTER_* vars) */
    llm_init();          /* enabled only if OPENROUTER_API_KEY is set */

    int rc = run_raylib(&w);
    llm_shutdown();
    return rc;
}
