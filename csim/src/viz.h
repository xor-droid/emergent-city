/* viz.h — shared rendering helpers (tile colors, legend, HUD, God Mode).
 *
 * The windowed UI lives in ui.c behind the gfx.h interface; these helpers are
 * backend-neutral and used by it.
 */
#ifndef VIZ_H
#define VIZ_H

#include "sim.h"

#define TILE_PX 8
#define FONT_PATH "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"

/* ── God Mode ────────────────────────────────────────────────────────────── */
enum { G_SMITE, G_BLESS, G_STARVE, G_INCITE, G_SPAWN, G_GANG, G_CULT, G_RIOT, G_NTOOLS };
extern const char *GOD_TOOL_NAME[G_NTOOLS];
/* Apply the selected tool at world tile (tx,ty); writes a short status line. */
void god_apply(World *w, int tool, int tx, int ty, char *flash, int flashn);

/* Backend-neutral tile color (RGB). */
void tile_rgb(TileType t, unsigned char *r, unsigned char *g, unsigned char *b);

/* True for tile types that are structures (drawn as raised blocks), false for
 * ground (grass/road/park/water). Shared so both backends draw them alike. */
int tile_is_building(TileType t);

/* Shared map legend (swatch color + label), used by both backends. */
typedef struct { TileType type; const char *label; } LegendEntry;
extern const LegendEntry LEGEND[];
extern const int LEGEND_N;

/* Deterministic per-lot shade jitter in [-range,+range] from tile coords, so
 * clusters of same-type buildings don't read as one flat blob. */
int tile_shade_jitter(int x, int y, int range);

/* Compose the HUD status line shared by both backends. */
void hud_string(const World *w, char *buf, int n, float speed, int paused,
                int fps, const char *backend);

#endif /* VIZ_H */
