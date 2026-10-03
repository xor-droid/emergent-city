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
    printf("Usage: %s [--backend raylib] [--tileset NAME|path.png]\n", argv0);
    printf("  Renders the simulation in a GPU window (raylib).\n");
    printf("  --tileset: image tileset for ASCII/tile mode (asset must be present under\n");
    printf("     the tileset dir; if missing, prints where to get it and uses font glyphs):\n");
    printf("       CC0 CP437 grid: camashu\n");
    printf("       DF-wiki CP437 (verify license): curses, phoebus, anikki\n");
    printf("       CC-BY per-type sprites: dawnlike\n");
    printf("       CC0 sprites (approx mapping): kenney, kenney-indoor, kenney-caves, kenney-1bit\n");
    printf("     dir=CSIM_TILESET_DIR (default ./tilesets); cell px=CSIM_TILESET_CELL (default 16).\n");
    printf("  Controls: drag/wheel pan+zoom, click a citizen to inspect, g god mode,\n");
    printf("            j jail, f factions, c crime-watch, l legend, a ASCII mode,\n");
    printf("            e city dashboard, o overlay (heat/turf/culture),\n");
    printf("            Tab feed, Space pause, 1/2/3 speed, q/Esc quit.\n");
    printf("  Env: CSIM_UI=N ui scale (default: from monitor), CSIM_ZOOM=N zoom,\n");
    printf("       CSIM_ASCII=1 start in ASCII mode, CSIM_DEMO=1 panels,\n");
    printf("       CSIM_CITY=1 dashboard, CSIM_OVERLAY=1..3 overlay,\n");
    printf("  --years-per-day N  aging pace (life-years per game-day; default 2 = ~8h/life @1x).\n");
    printf("  --family-share F   fraction of immigrants that are young families (0..1, default 0.4).\n");
    printf("  --family-kids-min N / --family-kids-max N  kids per immigrant family (default 1..3).\n");
    printf("  Env: CSIM_YEARS_PER_DAY, CSIM_FAMILY_SHARE, CSIM_FAMILY_KIDS_MIN/MAX (same as flags),\n");
    printf("       CSIM_WARMDAYS=N pre-roll sim N days before the window opens,\n");
    printf("       CSIM_SHOT=name shot (to cwd), CSIM_SHOT_FRAMES=N warm-up, CSIM_BENCH=N fps.\n");
}

int main(int argc, char **argv) {
    double ypd = -1.0;             /* aging pace (life-years per game-day); <0 = unset */
    double fshare = -1.0;          /* immigrant family share; <0 = unset */
    int fkmin = -1, fkmax = -1;    /* kids per immigrant family; <0 = unset */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(argv[0]); return 0; }
        else if (!strcmp(argv[i], "--years-per-day") && i + 1 < argc) { ypd = atof(argv[++i]); }
        else if (!strncmp(argv[i], "--years-per-day=", 16)) { ypd = atof(argv[i] + 16); }
        else if (!strcmp(argv[i], "--family-share") && i + 1 < argc) { fshare = atof(argv[++i]); }
        else if (!strncmp(argv[i], "--family-share=", 15)) { fshare = atof(argv[i] + 15); }
        else if (!strcmp(argv[i], "--family-kids-min") && i + 1 < argc) { fkmin = atoi(argv[++i]); }
        else if (!strncmp(argv[i], "--family-kids-min=", 18)) { fkmin = atoi(argv[i] + 18); }
        else if (!strcmp(argv[i], "--family-kids-max") && i + 1 < argc) { fkmax = atoi(argv[++i]); }
        else if (!strncmp(argv[i], "--family-kids-max=", 18)) { fkmax = atoi(argv[i] + 18); }
        else if ((!strcmp(argv[i], "--backend") || !strcmp(argv[i], "-b")) && i + 1 < argc) {
            if (strcmp(argv[++i], "raylib")) { fprintf(stderr, "only the raylib backend is available\n"); return 2; }
        } else if (!strncmp(argv[i], "--backend=", 10)) {
            if (strcmp(argv[i] + 10, "raylib")) { fprintf(stderr, "only the raylib backend is available\n"); return 2; }
        } else if (!strcmp(argv[i], "--tileset") && i + 1 < argc) {
            setenv("CSIM_TILESET", argv[++i], 1);
        } else if (!strncmp(argv[i], "--tileset=", 10)) {
            setenv("CSIM_TILESET", argv[i] + 10, 1);
        } else { fprintf(stderr, "unknown argument: %s\n", argv[i]); usage(argv[0]); return 2; }
    }

    if (ypd <= 0.0) { const char *e = getenv("CSIM_YEARS_PER_DAY"); if (e) ypd = atof(e); }
    if (ypd > 0.0) set_years_per_day(ypd);
    if (fshare < 0.0) { const char *e = getenv("CSIM_FAMILY_SHARE"); if (e) fshare = atof(e); }
    if (fshare >= 0.0) set_family_share(fshare);
    if (fkmin < 0) { const char *e = getenv("CSIM_FAMILY_KIDS_MIN"); if (e) fkmin = atoi(e); }
    if (fkmax < 0) { const char *e = getenv("CSIM_FAMILY_KIDS_MAX"); if (e) fkmax = atoi(e); }
    if (fkmin >= 0 || fkmax >= 0)
        set_family_kids(fkmin >= 0 ? fkmin : get_family_kids_min(),
                        fkmax >= 0 ? fkmax : get_family_kids_max());

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
