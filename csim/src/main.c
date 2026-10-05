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
#include "record.h"
#include "os_client.h"
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
    printf("  --child-cost N        daily upkeep each dependent child costs its parents\n");
    printf("                        (default 3; 0 = children are free).\n");
    printf("  --occ-pay-spread F    how much occupation sets base pay (socioeconomic class):\n");
    printf("                        0 = flat/legacy (default), 1 = full tier spread (officer/\n");
    printf("                        shopkeep earn more than laborers). Drives wealth inequality.\n");
    printf("  --neighborhoods       give homes a value (downtown/parks) and seat households by\n");
    printf("                        wealth, so rich & poor quarters emerge; adds an AFFLUENCE\n");
    printf("                        map overlay (O). Default off = random residency.\n");
    printf("  --crime-wealth        loot scales with the target's wealth (rob a mansion for a\n");
    printf("                        real score, a tenement for pennies) and offenders pick\n");
    printf("                        targets by expected value. Default off = flat loot.\n");
    printf("  --police-bias MODE    where police concentrate: crime (default, toward crime heat),\n");
    printf("                        money (protect wealthy blocks; crime displaces to poor areas),\n");
    printf("                        or balanced. With --neighborhoods, two very different cities.\n");
    printf("  --weather [--weather-period D]  deterministic temperature/rain/fog (noise-based):\n");
    printf("                        fog/rain reduce perception (crime cover, needs --vision/--hearing)\n");
    printf("                        and cold drives heating costs that hit poor homes hardest.\n");
    printf("                        D = game-days per weather cycle (default 5). Default off.\n");
    printf("  --heat-cost F         scale the cold-weather heating money sink (default 1; 0 = none).\n");
    printf("  --biomes              terrain biomes (hills/floodplain/waterfront/parkland) that shape\n");
    printf("                        land value — rich cluster on hills, poor in the floodplain. Pairs\n");
    printf("                        with --neighborhoods (value) + --noise-worldgen (organic terrain).\n");
    printf("                        BIOMES map overlay (O). Startup-only; default off.\n");
    printf("  --biome-value-weight W  how strongly terrain shapes value (default 1; live in T-panel).\n");
    printf("  --taxation [--tax-rate R] [--welfare W]  tax daily income into a public treasury that\n");
    printf("                        funds welfare relief for the destitute, policing, and schools —\n");
    printf("                        a counterweight to inequality. R=0..1 (default .15). Default off.\n");
    printf("  --wealth-tax-rate R [--wealth-tax-threshold M]  (needs --taxation) a daily levy of R\n");
    printf("                        (0..1) on money held above M× median wealth (default M=4). Taxes the\n");
    printf("                        accumulated STOCK, not just income — the lever that holds Gini flat.\n");
    printf("  --stabilizers [--benefit B]  counter-cyclical shock absorbers: a safety-net benefit\n");
    printf("                        floor for the destitute, eased interest when destitution is high,\n");
    printf("                        and a goods-price damper. Softens boom/bust. Default off.\n");
    printf("  --justice [--corruption C] [--oversight O]  arrests go through trials (convict/acquit),\n");
    printf("                        the wealthy can bribe a corrupt force, innocents are sometimes\n");
    printf("                        wrongfully convicted; oversight (0..1) curbs it. Default off.\n");
    printf("  --vision [--vision-radius N]  agents perceive via field-of-view + line-of-sight\n");
    printf("                        (default off = omniscient): crimes are only witnessed, and\n");
    printf("                        fugitives only spotted, by those who can actually see them.\n");
    printf("  --hearing [--hearing-radius N]  loud acts (murder/assault/arson/riot) carry around\n");
    printf("                        corners and get noticed even out of sight; quiet crimes stay\n");
    printf("                        stealthy, serial killers quieter still. Rebalances --vision.\n");
    printf("  --noise-worldgen      coherent (FastNoiseLite) organic rivers/districts — now the\n");
    printf("                        DEFAULT. Use --no-noise-worldgen for the legacy sine/gradient map.\n");
    printf("  --seed N              worldgen seed (default 1337); same seed+config = same run.\n");
    printf("\n");
    printf("Balance metrics (feeds the live web dashboard in tools/dashboard/):\n");
    printf("  --metrics PATH        append one CSV row of all aggregates per sample to PATH\n");
    printf("                        (live; point it at the dashboard dir so nginx serves it).\n");
    printf("                        Also writes PATH.meta (seed+config) for deterministic re-run.\n");
    printf("  --metrics-every H     sample cadence in game-hours (default 24 = once/day); e.g. 1\n");
    printf("  --metrics-hourly      for hourly, 0.5 for twice an hour. Finer = smoother graphs.\n");
    printf("  --record PATH         record this session (seed+config+live changes) to PATH for\n");
    printf("                        byte-identical replay; forces --fixed-step. Replay headless\n");
    printf("                        with: csim_headless --replay-session PATH.\n");
    printf("  --replay-session F    watch a recorded session play back (F = file or os:<id>);\n");
    printf("                        open T and change a value to FORK a new recorded timeline.\n");
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
    printf("    CSIM_CHILD_COST     per-child daily upkeep (same as --child-cost).\n");
    printf("    CSIM_OCC_PAY_SPREAD occupation pay-tier spread (same as --occ-pay-spread).\n");
    printf("    CSIM_NEIGHBORHOODS=1  home value + residential sorting (same as --neighborhoods).\n");
    printf("    CSIM_CRIME_WEALTH=1   wealth-scaled loot + EV targeting (same as --crime-wealth).\n");
    printf("    CSIM_POLICE_BIAS=MODE crime|money|balanced patrol bias (same as --police-bias).\n");
    printf("    CSIM_WEATHER=1  CSIM_WEATHER_PERIOD=D  CSIM_HEAT_COST=F   weather (as --weather).\n");
    printf("    CSIM_BIOMES=1  CSIM_BIOME_VALUE_WEIGHT=W   terrain biomes (as --biomes).\n");
    printf("    CSIM_TAXATION=1  CSIM_TAX_RATE=R  CSIM_WELFARE=W   fiscal (as --taxation).\n");
    printf("    CSIM_WEALTH_TAX_RATE=R  CSIM_WEALTH_TAX_THRESHOLD=M   top-tail wealth (stock) tax.\n");
    printf("    CSIM_STABILIZERS=1  CSIM_BENEFIT=B   economic stabilizers (as --stabilizers).\n");
    printf("    CSIM_JUSTICE=1  CSIM_CORRUPTION=C  CSIM_OVERSIGHT=O   justice checks (as --justice).\n");
    printf("    CSIM_VISION=1  CSIM_VISION_RADIUS=N   field-of-view perception (as --vision).\n");
    printf("    CSIM_HEARING=1  CSIM_HEARING_RADIUS=N  auditory perception (as --hearing).\n");
    printf("    CSIM_NOISE_WORLDGEN=0   legacy sine/gradient worldgen (noise is the default now).\n");
    printf("    CSIM_SEED=N         worldgen seed (as --seed).\n");
    printf("    CSIM_METRICS=PATH   live balance CSV export (as --metrics).\n");
    printf("    CSIM_METRICS_EVERY=H  sample cadence in game-hours (as --metrics-every).\n");
    printf("    CSIM_RECORD=PATH    record a replayable session (as --record).\n");
    printf("  session archive (OpenSearch via Data Prepper; .env, optional):\n");
    printf("    CSIM_OS_INGEST_URL  Data Prepper /log/ingest URL; ships session+events(+metrics).\n");
    printf("    CSIM_OS_QUERY_URL / CSIM_OS_USER / CSIM_OS_PASS   recall for --replay-session os:<id>.\n");
    printf("  startup state:\n");
    printf("    CSIM_WARMDAYS=N     pre-roll the sim N game-days before the window opens.\n");
    printf("    CSIM_DEMO=1         open with god mode + a criminal selected/followed.\n");
    printf("    CSIM_CITY=1 / CSIM_TUNE=1   open the dashboard / tuning panel at launch.\n");
    printf("    CSIM_OVERLAY=1..6   open a map overlay (1 heat, 2 turf, 3 culture, 4 vision, 5 hearing, 6 affluence).\n");
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
    printf("  %s --metrics tools/dashboard/metrics.csv   live-feed the balance dashboard\n", argv0);
}

/* map a --police-bias value to its mode int (default crime=0) */
static int parse_police_bias(const char *m) {
    if (!m) return 0;
    if (!strcmp(m, "money") || !strcmp(m, "1")) return 1;
    if (!strcmp(m, "balanced") || !strcmp(m, "2")) return 2;
    return 0;   /* "crime" / "0" / anything else */
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
    double childcost = -1.0;       /* per-child daily upkeep; <0 = unset */
    int vision = -1, visradius = -1;   /* field of view; <0 = unset */
    int hearing = -1, hearradius = -1; /* hearing; <0 = unset */
    int noisegen = -1;                 /* noise worldgen; <0 = unset */
    const char *metrics = NULL;        /* balance CSV export path; NULL = off */
    const char *record = NULL;         /* session recording path; NULL = off */
    const char *replay_session = NULL; /* GUI replay of a recorded session; NULL = off */
    double metricsevery = -1.0;        /* sample cadence in game-hours; <0 = unset */
    double occspread = -1.0;           /* occupation pay-tier spread; <0 = unset */
    int neighborhoods = -1;            /* home value + residential sorting; <0 = unset */
    int crimewealth = -1;              /* wealth-scaled loot + EV targeting; <0 = unset */
    int policebias = -1;               /* 0 crime / 1 money / 2 balanced; <0 = unset */
    int weather = -1;                  /* deterministic weather; <0 = unset */
    double weatherperiod = -1.0;       /* days per weather cycle; <0 = unset */
    double heatcost = -1.0;            /* heating-cost scale; <0 = unset */
    int biomes = -1;                   /* terrain biomes; <0 = unset */
    double biomeweight = -1.0;         /* biome value weight; <0 = unset */
    int taxation = -1;                 /* fiscal; <0 = unset */
    double taxrate = -1.0, welfare = -1.0;
    double wtaxr = -1.0, wtaxt = -1.0;
    int stabilizers = -1;              /* economic stabilizers; <0 = unset */
    double benefit = -1.0;
    int justice = -1;                  /* justice checks; <0 = unset */
    double corruption = -1.0, oversight = -1.0;
    long seed = -1;                    /* worldgen seed; <0 = unset (default 1337) */
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
        else if (!strcmp(argv[i], "--child-cost") && i + 1 < argc) { childcost = atof(argv[++i]); }
        else if (!strncmp(argv[i], "--child-cost=", 13)) { childcost = atof(argv[i] + 13); }
        else if (!strcmp(argv[i], "--vision")) { vision = 1; }
        else if (!strcmp(argv[i], "--no-vision")) { vision = 0; }
        else if (!strcmp(argv[i], "--vision-radius") && i + 1 < argc) { visradius = atoi(argv[++i]); vision = (vision < 0) ? 1 : vision; }
        else if (!strncmp(argv[i], "--vision-radius=", 16)) { visradius = atoi(argv[i] + 16); vision = (vision < 0) ? 1 : vision; }
        else if (!strcmp(argv[i], "--hearing")) { hearing = 1; }
        else if (!strcmp(argv[i], "--no-hearing")) { hearing = 0; }
        else if (!strcmp(argv[i], "--hearing-radius") && i + 1 < argc) { hearradius = atoi(argv[++i]); hearing = (hearing < 0) ? 1 : hearing; }
        else if (!strncmp(argv[i], "--hearing-radius=", 17)) { hearradius = atoi(argv[i] + 17); hearing = (hearing < 0) ? 1 : hearing; }
        else if (!strcmp(argv[i], "--noise-worldgen")) { noisegen = 1; }
        else if (!strcmp(argv[i], "--no-noise-worldgen")) { noisegen = 0; }
        else if (!strcmp(argv[i], "--metrics") && i + 1 < argc) { metrics = argv[++i]; }
        else if (!strncmp(argv[i], "--metrics=", 10)) { metrics = argv[i] + 10; }
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) { record = argv[++i]; }
        else if (!strncmp(argv[i], "--record=", 9)) { record = argv[i] + 9; }
        else if (!strcmp(argv[i], "--replay-session") && i + 1 < argc) { replay_session = argv[++i]; }
        else if (!strncmp(argv[i], "--replay-session=", 17)) { replay_session = argv[i] + 17; }
        else if (!strcmp(argv[i], "--metrics-every") && i + 1 < argc) { metricsevery = atof(argv[++i]); }
        else if (!strncmp(argv[i], "--metrics-every=", 16)) { metricsevery = atof(argv[i] + 16); }
        else if (!strcmp(argv[i], "--metrics-hourly")) { metricsevery = 1.0; }
        else if (!strcmp(argv[i], "--occ-pay-spread") && i + 1 < argc) { occspread = atof(argv[++i]); }
        else if (!strncmp(argv[i], "--occ-pay-spread=", 17)) { occspread = atof(argv[i] + 17); }
        else if (!strcmp(argv[i], "--neighborhoods")) { neighborhoods = 1; }
        else if (!strcmp(argv[i], "--no-neighborhoods")) { neighborhoods = 0; }
        else if (!strcmp(argv[i], "--crime-wealth")) { crimewealth = 1; }
        else if (!strcmp(argv[i], "--no-crime-wealth")) { crimewealth = 0; }
        else if (!strcmp(argv[i], "--police-bias") && i + 1 < argc) { policebias = parse_police_bias(argv[++i]); }
        else if (!strncmp(argv[i], "--police-bias=", 14)) { policebias = parse_police_bias(argv[i] + 14); }
        else if (!strcmp(argv[i], "--weather")) { weather = 1; }
        else if (!strcmp(argv[i], "--no-weather")) { weather = 0; }
        else if (!strcmp(argv[i], "--weather-period") && i + 1 < argc) { weatherperiod = atof(argv[++i]); weather = (weather<0)?1:weather; }
        else if (!strncmp(argv[i], "--weather-period=", 17)) { weatherperiod = atof(argv[i] + 17); weather = (weather<0)?1:weather; }
        else if (!strcmp(argv[i], "--heat-cost") && i + 1 < argc) { heatcost = atof(argv[++i]); }
        else if (!strncmp(argv[i], "--heat-cost=", 12)) { heatcost = atof(argv[i] + 12); }
        else if (!strcmp(argv[i], "--biomes")) { biomes = 1; }
        else if (!strcmp(argv[i], "--no-biomes")) { biomes = 0; }
        else if (!strcmp(argv[i], "--biome-value-weight") && i + 1 < argc) { biomeweight = atof(argv[++i]); biomes = (biomes<0)?1:biomes; }
        else if (!strncmp(argv[i], "--biome-value-weight=", 21)) { biomeweight = atof(argv[i] + 21); biomes = (biomes<0)?1:biomes; }
        else if (!strcmp(argv[i], "--taxation")) { taxation = 1; }
        else if (!strcmp(argv[i], "--no-taxation")) { taxation = 0; }
        else if (!strcmp(argv[i], "--tax-rate") && i + 1 < argc) { taxrate = atof(argv[++i]); taxation = (taxation<0)?1:taxation; }
        else if (!strncmp(argv[i], "--tax-rate=", 11)) { taxrate = atof(argv[i] + 11); taxation = (taxation<0)?1:taxation; }
        else if (!strcmp(argv[i], "--welfare") && i + 1 < argc) { welfare = atof(argv[++i]); taxation = (taxation<0)?1:taxation; }
        else if (!strncmp(argv[i], "--welfare=", 10)) { welfare = atof(argv[i] + 10); taxation = (taxation<0)?1:taxation; }
        else if (!strcmp(argv[i], "--wealth-tax-rate") && i + 1 < argc) { wtaxr = atof(argv[++i]); taxation = (taxation<0)?1:taxation; }
        else if (!strncmp(argv[i], "--wealth-tax-rate=", 18)) { wtaxr = atof(argv[i] + 18); taxation = (taxation<0)?1:taxation; }
        else if (!strcmp(argv[i], "--wealth-tax-threshold") && i + 1 < argc) { wtaxt = atof(argv[++i]); }
        else if (!strncmp(argv[i], "--wealth-tax-threshold=", 23)) { wtaxt = atof(argv[i] + 23); }
        else if (!strcmp(argv[i], "--stabilizers")) { stabilizers = 1; }
        else if (!strcmp(argv[i], "--no-stabilizers")) { stabilizers = 0; }
        else if (!strcmp(argv[i], "--benefit") && i + 1 < argc) { benefit = atof(argv[++i]); stabilizers = (stabilizers<0)?1:stabilizers; }
        else if (!strncmp(argv[i], "--benefit=", 10)) { benefit = atof(argv[i] + 10); stabilizers = (stabilizers<0)?1:stabilizers; }
        else if (!strcmp(argv[i], "--justice")) { justice = 1; }
        else if (!strcmp(argv[i], "--no-justice")) { justice = 0; }
        else if (!strcmp(argv[i], "--corruption") && i + 1 < argc) { corruption = atof(argv[++i]); justice = (justice<0)?1:justice; }
        else if (!strncmp(argv[i], "--corruption=", 13)) { corruption = atof(argv[i] + 13); justice = (justice<0)?1:justice; }
        else if (!strcmp(argv[i], "--oversight") && i + 1 < argc) { oversight = atof(argv[++i]); justice = (justice<0)?1:justice; }
        else if (!strncmp(argv[i], "--oversight=", 12)) { oversight = atof(argv[i] + 12); justice = (justice<0)?1:justice; }
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) { seed = atol(argv[++i]); }
        else if (!strncmp(argv[i], "--seed=", 7)) { seed = atol(argv[i] + 7); }
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
    if (childcost < 0.0) { const char *e = getenv("CSIM_CHILD_COST"); if (e) childcost = atof(e); }
    if (childcost >= 0.0) set_child_cost(childcost);
    if (visradius < 0) { const char *e = getenv("CSIM_VISION_RADIUS"); if (e) visradius = atoi(e); }
    if (visradius > 0) set_vision_radius(visradius);
    if (vision < 0) { const char *e = getenv("CSIM_VISION"); if (e) vision = atoi(e); }
    if (vision >= 0) set_vision(vision);
    if (hearradius < 0) { const char *e = getenv("CSIM_HEARING_RADIUS"); if (e) hearradius = atoi(e); }
    if (hearradius > 0) set_hearing_radius(hearradius);
    if (hearing < 0) { const char *e = getenv("CSIM_HEARING"); if (e) hearing = atoi(e); }
    if (hearing >= 0) set_hearing(hearing);
    if (noisegen < 0) { const char *e = getenv("CSIM_NOISE_WORLDGEN"); if (e) noisegen = atoi(e); }
    if (noisegen >= 0) set_noise_worldgen(noisegen);   /* must precede world_init */
    if (!metrics) metrics = getenv("CSIM_METRICS");
    if (metricsevery < 0.0) { const char *e = getenv("CSIM_METRICS_EVERY"); if (e) metricsevery = atof(e); }
    if (metricsevery > 0.0) set_metrics_every(metricsevery);
    if (occspread < 0.0) { const char *e = getenv("CSIM_OCC_PAY_SPREAD"); if (e) occspread = atof(e); }
    if (occspread >= 0.0) set_occ_pay_spread(occspread);
    if (neighborhoods < 0) { const char *e = getenv("CSIM_NEIGHBORHOODS"); if (e) neighborhoods = atoi(e); }
    if (neighborhoods >= 0) set_neighborhoods(neighborhoods);   /* must precede world_populate */
    if (crimewealth < 0) { const char *e = getenv("CSIM_CRIME_WEALTH"); if (e) crimewealth = atoi(e); }
    if (crimewealth >= 0) set_crime_wealth(crimewealth);
    if (policebias < 0) { const char *e = getenv("CSIM_POLICE_BIAS"); if (e) policebias = parse_police_bias(e); }
    if (policebias >= 0) set_police_bias(policebias);
    if (weather < 0) { const char *e = getenv("CSIM_WEATHER"); if (e) weather = atoi(e); }
    if (weather >= 0) set_weather(weather);
    if (weatherperiod < 0) { const char *e = getenv("CSIM_WEATHER_PERIOD"); if (e) weatherperiod = atof(e); }
    if (weatherperiod > 0) set_weather_period(weatherperiod);
    if (heatcost < 0) { const char *e = getenv("CSIM_HEAT_COST"); if (e) heatcost = atof(e); }
    if (heatcost >= 0) set_heat_cost(heatcost);
    if (biomes < 0) { const char *e = getenv("CSIM_BIOMES"); if (e) biomes = atoi(e); }
    if (biomes >= 0) set_biomes(biomes);         /* must precede world_init */
    if (biomeweight < 0) { const char *e = getenv("CSIM_BIOME_VALUE_WEIGHT"); if (e) biomeweight = atof(e); }
    if (biomeweight >= 0) set_biome_value_weight(biomeweight);
    if (taxation < 0) { const char *e = getenv("CSIM_TAXATION"); if (e) taxation = atoi(e); }
    if (taxation >= 0) set_taxation(taxation);
    if (taxrate < 0) { const char *e = getenv("CSIM_TAX_RATE"); if (e) taxrate = atof(e); }
    if (taxrate >= 0) set_tax_rate(taxrate);
    if (welfare < 0) { const char *e = getenv("CSIM_WELFARE"); if (e) welfare = atof(e); }
    if (welfare >= 0) set_welfare(welfare);
    if (wtaxr < 0) { const char *e = getenv("CSIM_WEALTH_TAX_RATE"); if (e) wtaxr = atof(e); }
    if (wtaxr >= 0) set_wealth_tax_rate(wtaxr);
    if (wtaxt < 0) { const char *e = getenv("CSIM_WEALTH_TAX_THRESHOLD"); if (e) wtaxt = atof(e); }
    if (wtaxt >= 0) set_wealth_tax_threshold(wtaxt);
    if (stabilizers < 0) { const char *e = getenv("CSIM_STABILIZERS"); if (e) stabilizers = atoi(e); }
    if (stabilizers >= 0) set_stabilizers(stabilizers);
    if (benefit < 0) { const char *e = getenv("CSIM_BENEFIT"); if (e) benefit = atof(e); }
    if (benefit >= 0) set_benefit(benefit);
    if (justice < 0) { const char *e = getenv("CSIM_JUSTICE"); if (e) justice = atoi(e); }
    if (justice >= 0) set_justice(justice);
    if (corruption < 0) { const char *e = getenv("CSIM_CORRUPTION"); if (e) corruption = atof(e); }
    if (corruption >= 0) set_corruption(corruption);
    if (oversight < 0) { const char *e = getenv("CSIM_OVERSIGHT"); if (e) oversight = atof(e); }
    if (oversight >= 0) set_oversight(oversight);
    if (seed < 0) { const char *e = getenv("CSIM_SEED"); if (e) seed = atol(e); }
    if (seed < 0) seed = 1337;

    G = gfx_raylib();

    /* ── interactive replay mode: watch a recorded session; edits fork a new one ── */
    if (replay_session) {
        char tmp[256];
        if (!strncmp(replay_session, "os:", 3)) {        /* recall from OpenSearch first */
            snprintf(tmp, sizeof tmp, "/tmp/csim-recall-%s.sess", replay_session + 3);
            if (os_fetch_session(replay_session + 3, tmp) != 0) {
                fprintf(stderr, "recall failed for %s (CSIM_OS_QUERY_URL/USER/PASS set?)\n", replay_session + 3);
                return 2;
            }
            replay_session = tmp;
        }
        uint64_t rseed = 1337; int rpop = 150;
        if (replay_load(replay_session, &rseed, &rpop) != 0) {
            fprintf(stderr, "cannot load session %s\n", replay_session); return 2;
        }
        set_fixed_step(1);                    /* deterministic playback (G already set above) */
        World wr;
        world_init(&wr, (unsigned int)rseed);
        world_populate(&wr, rpop);
        /* LLM intentionally NOT initialised in replay → off → byte-identical */
        int rc = run_ui(&wr);
        if (rec_active()) {                   /* a fork was made during replay */
            rec_end(&wr);
            char p[300]; const char *sp = record;
            if (!sp || !*sp) { snprintf(p, sizeof p, "/tmp/%s.sess", rec_session_id()); sp = p; }
            if (rec_save_file(sp)) fprintf(stderr, "forked session -> %s\n", sp);
        }
        return rc;
    }

    if (!record) record = getenv("CSIM_RECORD");
    if (record && *record) set_fixed_step(1);   /* recording needs deterministic stepping to replay */

    World w;
    world_init(&w, (unsigned int)seed);
    world_populate(&w, 150);
    if (metrics && *metrics) { metrics_open(metrics); metrics_write_manifest(metrics, (unsigned int)seed, 0); }
    if (record && *record) rec_begin((unsigned int)seed, 150, "gui");   /* snapshot config before the run */
    dotenv_autoload();   /* pick up the project .env (OPENROUTER_* vars) */
    llm_init();          /* enabled only if OPENROUTER_API_KEY is set */

    int rc = run_ui(&w);
    if (rec_active()) { rec_end(&w); if (rec_save_file(record)) fprintf(stderr, "recorded session -> %s\n", record); }
    llm_shutdown();
    return rc;
}
