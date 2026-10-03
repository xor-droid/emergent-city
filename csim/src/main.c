/* main.c — entry point: build the world, run the UI (raylib backend).
 *
 *   ./csim [--backend raylib]
 *
 * The UI (src/ui.c) renders through the gfx.h interface; raylib is the shipped
 * backend. --backend raylib is accepted (for habit); anything else errors.
 */
#include "sim.h"
#include "gfx.h"
#include "llm.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void usage(const char *argv0) {
    printf("Usage: %s [--backend raylib]\n", argv0);
    printf("  Renders the simulation in a GPU window (raylib).\n");
    printf("  Controls: drag/wheel pan+zoom, click a citizen to inspect, g god mode,\n");
    printf("            j jail, f factions, l legend, a ASCII(Dwarf-Fortress) mode,\n");
    printf("            Tab feed, Space pause, 1/2/3 speed, q/Esc quit.\n");
    printf("  Env: CSIM_UI=N ui scale (default: from monitor), CSIM_ZOOM=N zoom,\n");
    printf("       CSIM_ASCII=1 start in ASCII mode, CSIM_DEMO=1 panels,\n");
    printf("       CSIM_SHOT=path shot, CSIM_BENCH=N fps.\n");
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *name = NULL;
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(argv[0]); return 0; }
        else if ((!strcmp(argv[i], "--backend") || !strcmp(argv[i], "-b")) && i + 1 < argc) name = argv[++i];
        else if (!strncmp(argv[i], "--backend=", 10)) name = argv[i] + 10;
        else { fprintf(stderr, "unknown argument: %s\n", argv[i]); usage(argv[0]); return 2; }
        if (name && strcmp(name, "raylib")) { fprintf(stderr, "only the raylib backend is available\n"); return 2; }
    }

    G = gfx_raylib();

    World w;
    world_init(&w, 1337);
    world_populate(&w, 150);
    dotenv_autoload();   /* pick up the project .env (OPENROUTER_* vars) */
    llm_init();          /* enabled only if OPENROUTER_API_KEY is set */

    int rc = run_ui(&w);
    llm_shutdown();
    return rc;
}
