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
    printf("Emergent City — a living-city simulation rendered in a GPU window (raylib).\n");
    printf("\n");
    printf("Usage: %s [options]\n", argv0);
    printf("\n");
    printf("Simulation options (also live-adjustable in-window via the T tuning panel):\n");
    printf("  --years-per-day N     aging pace: life-years per game-day (default 2 = ~8h/life @1x).\n");
    printf("                        higher = faster life cycle; 4-6 is good for quick testing.\n");
    printf("  --pop-target N        living-population the city immigrates toward (default 150).\n");
    printf("                        an attractor, not a cap: deaths lower it, arrivals refill it.\n");
    printf("  --family-share F      fraction of immigrant arrivals that are families (0..1, def 0.4).\n");
    printf("                        0 = adults only (adult-skewed); higher = more children.\n");
    printf("  --family-kids-min N   min kids per immigrant family (default 1).\n");
    printf("  --family-kids-max N   max kids per immigrant family (default 3).\n");
    printf("  --research-rate R     pace of knowledge/technology discovery (default 0.05).\n");
    printf("  --production F        how much craft skill + schooling lift a worker's pay\n");
    printf("                        (default 1.0; 0 = flat wages, higher = skill matters more).\n");
    printf("  --vision [--vision-radius N]  agents perceive via field-of-view + line-of-sight\n");
    printf("                        (default off = omniscient): crimes are only witnessed, and\n");
    printf("                        fugitives only spotted, by those who can actually see them.\n");
    printf("\n");
    printf("Timing / determinism:\n");
    printf("  --fixed-step          GUI steps a fixed timestep (deterministic, frame-rate-\n");
    printf("                        independent) instead of the default variable wall-clock dt.\n");
    printf("                        A run is then bit-reproducible from the seed (with LLM off).\n");
    printf("  --variable-step       force the default (real-time) stepping.\n");
    printf("  --fixed-dt N          fixed step size in sim-seconds (default 0.25; implies --fixed-step).\n");
    printf("\n");
    printf("Rendering:\n");
    printf("  --backend raylib      the only/default backend (accepted for habit).\n");
    printf("  --tileset NAME|FILE   image tileset for ASCII/tile mode (asset must be present under\n");
    printf("                        the tileset dir; if missing, prints where to get it + uses font):\n");
    printf("                          CC0 CP437 grid: camashu\n");
    printf("                          DF-wiki CP437 (verify license): curses, phoebus, anikki\n");
    printf("                          CC-BY per-type sprites: dawnlike\n");
    printf("                          CC0 sprites (approx): kenney, kenney-indoor/-caves/-1bit\n");
    printf("  -h, --help            show this help.\n");
    printf("\n");
    printf("Controls (in-window):\n");
    printf("  drag/wheel  pan + zoom          click a citizen  inspect\n");
    printf("  1-6  speed 1x-6x                Space  pause          q/Esc  quit\n");
    printf("  g god mode (1-8 tools)          T  tuning panel       E  city dashboard\n");
    printf("  K family tree (selected)        O overlay (heat/turf/culture)\n");
    printf("  F factions   J jail   C crime-watch\n");
    printf("  Tab feed   L legend   a ASCII mode\n");
    printf("\n");
    printf("Environment variables:\n");
    printf("  sim (same as the flags above):\n");
    printf("    CSIM_YEARS_PER_DAY  CSIM_POP_TARGET  CSIM_FAMILY_SHARE\n");
    printf("    CSIM_FAMILY_KIDS_MIN  CSIM_FAMILY_KIDS_MAX  CSIM_FIXED_STEP=1  CSIM_FIXED_DT=N\n");
    printf("    CSIM_RESEARCH_RATE  knowledge/tech discovery pace (same as --research-rate).\n");
    printf("    CSIM_PRODUCTION     craft/output pay weight (same as --production).\n");
    printf("    CSIM_VISION=1  CSIM_VISION_RADIUS=N   field-of-view perception (as --vision).\n");
    printf("  startup state:\n");
    printf("    CSIM_WARMDAYS=N     pre-roll the sim N game-days before the window opens.\n");
    printf("    CSIM_DEMO=1         open with god mode + a criminal selected/followed.\n");
    printf("    CSIM_CITY=1 / CSIM_TUNE=1   open the dashboard / tuning panel at launch.\n");
    printf("    CSIM_OVERLAY=1..3   open a map overlay (1 heat, 2 turf, 3 culture).\n");
    printf("    CSIM_ASCII=1        start in Dwarf-Fortress ASCII render mode.\n");
    printf("  display / render:\n");
    printf("    CSIM_UI=N           UI scale (default: derived from monitor height).\n");
    printf("    CSIM_ZOOM=N         initial map zoom.\n");
    printf("    CSIM_FPS=N          frame cap (vsync off); 0 = uncapped; unset = vsync.\n");
    printf("    CSIM_TILESET=NAME   same as --tileset.\n");
    printf("    CSIM_TILESET_DIR    tileset search dir (default ./tilesets).\n");
    printf("    CSIM_TILESET_CELL / _SPACE / _MARGIN   override cell px / spacing / margin.\n");
    printf("  screenshots / benchmark:\n");
    printf("    CSIM_SHOT=NAME      render a frame then exit; raylib writes NAME to the CWD.\n");
    printf("    CSIM_SHOT_FRAMES=N  warm-up frames before the shot (default 120).\n");
    printf("    CSIM_BENCH=N        uncapped N-frame FPS benchmark (sim paused).\n");
    printf("  LLM (optional Qwen consults; makes runs NON-deterministic):\n");
    printf("    OPENROUTER_API_KEY / OPENROUTER_BASE_URL / OPENROUTER_MODEL   enable + target it.\n");
    printf("    CSIM_ENV            path to a .env file to load these from (default: ./.env, ../.env).\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s                                    watch the city with defaults\n", argv0);
    printf("  %s --pop-target 250 --years-per-day 4   bigger city, faster life cycle\n", argv0);
    printf("  %s --fixed-step                       deterministic (reproducible) run\n", argv0);
    printf("  CSIM_WARMDAYS=12 %s --pop-target 250    open on an already-grown city\n", argv0);
    printf("  CSIM_SHOT=out.png %s                  render ./out.png and exit\n", argv0);
    printf("  %s --tileset dawnlike                 run with the DawnLike sprite tileset\n", argv0);
}

int main(int argc, char **argv) {
    double ypd = -1.0;             /* aging pace (life-years per game-day); <0 = unset */
    double fshare = -1.0;          /* immigrant family share; <0 = unset */
    int fkmin = -1, fkmax = -1;    /* kids per immigrant family; <0 = unset */
    int ptarget = -1;              /* living-population target; <0 = unset */
    int fixedstep = -1;            /* GUI fixed-timestep: -1 unset, 0 off, 1 on */
    double fixeddt = -1.0;         /* fixed step size; <0 = unset */
    double rrate = -1.0;           /* research rate; <0 = unset */
    double craftb = -1.0;          /* production/craft bonus; <0 = unset */
    int vision = -1, visradius = -1;   /* field of view; <0 = unset */
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
        else if (!strcmp(argv[i], "--pop-target") && i + 1 < argc) { ptarget = atoi(argv[++i]); }
        else if (!strncmp(argv[i], "--pop-target=", 13)) { ptarget = atoi(argv[i] + 13); }
        else if (!strcmp(argv[i], "--fixed-step")) { fixedstep = 1; }
        else if (!strcmp(argv[i], "--variable-step")) { fixedstep = 0; }
        else if (!strcmp(argv[i], "--fixed-dt") && i + 1 < argc) { fixeddt = atof(argv[++i]); fixedstep = (fixedstep < 0) ? 1 : fixedstep; }
        else if (!strncmp(argv[i], "--fixed-dt=", 11)) { fixeddt = atof(argv[i] + 11); fixedstep = (fixedstep < 0) ? 1 : fixedstep; }
        else if (!strcmp(argv[i], "--research-rate") && i + 1 < argc) { rrate = atof(argv[++i]); }
        else if (!strncmp(argv[i], "--research-rate=", 16)) { rrate = atof(argv[i] + 16); }
        else if (!strcmp(argv[i], "--production") && i + 1 < argc) { craftb = atof(argv[++i]); }
        else if (!strncmp(argv[i], "--production=", 13)) { craftb = atof(argv[i] + 13); }
        else if (!strcmp(argv[i], "--vision")) { vision = 1; }
        else if (!strcmp(argv[i], "--no-vision")) { vision = 0; }
        else if (!strcmp(argv[i], "--vision-radius") && i + 1 < argc) { visradius = atoi(argv[++i]); vision = (vision < 0) ? 1 : vision; }
        else if (!strncmp(argv[i], "--vision-radius=", 16)) { visradius = atoi(argv[i] + 16); vision = (vision < 0) ? 1 : vision; }
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
    if (ptarget < 0) { const char *e = getenv("CSIM_POP_TARGET"); if (e) ptarget = atoi(e); }
    if (ptarget >= 0) set_pop_target(ptarget);
    if (fixeddt < 0.0) { const char *e = getenv("CSIM_FIXED_DT"); if (e) fixeddt = atof(e); }
    if (fixeddt > 0.0) set_fixed_dt(fixeddt);
    if (fixedstep < 0) { const char *e = getenv("CSIM_FIXED_STEP"); if (e) fixedstep = atoi(e); }
    if (fixedstep >= 0) set_fixed_step(fixedstep);
    if (rrate < 0.0) { const char *e = getenv("CSIM_RESEARCH_RATE"); if (e) rrate = atof(e); }
    if (rrate >= 0.0) set_research_rate(rrate);
    if (craftb < 0.0) { const char *e = getenv("CSIM_PRODUCTION"); if (e) craftb = atof(e); }
    if (craftb >= 0.0) set_craft_bonus(craftb);
    if (visradius < 0) { const char *e = getenv("CSIM_VISION_RADIUS"); if (e) visradius = atoi(e); }
    if (visradius > 0) set_vision_radius(visradius);
    if (vision < 0) { const char *e = getenv("CSIM_VISION"); if (e) vision = atoi(e); }
    if (vision >= 0) set_vision(vision);

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
