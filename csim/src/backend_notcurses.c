/* backend_notcurses.c — high-res terminal renderer with full God Mode + panels.
 *
 * Feature parity with the raylib backend: God Mode (8 tools), event feed, jail
 * roster, agent inspector, HUD. Terminal controls (no mouse needed):
 *   g god mode   arrows move the cursor   enter apply tool   1-8 pick tool
 *   i inspect agent at cursor   Tab feed   j jail   space pause   1/2/3 speed
 *   esc close inspector   q quit
 */
#define _GNU_SOURCE
#include <wchar.h>
#include "sim.h"
#include "viz.h"
#include "llm.h"
#include <notcurses/notcurses.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#define PXT 3

static double now_sec(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void put(struct ncplane *n, int y, int x, uint32_t fg, uint32_t bg, const char *s) {
    ncplane_set_fg_rgb(n, fg);
    ncplane_set_bg_rgb(n, bg);
    ncplane_putstr_yx(n, y, x, s);
}
static void fillrow(struct ncplane *n, int y, int x, int w, uint32_t bg) {
    char sp[256]; if (w > 255) w = 255;
    for (int i = 0; i < w; i++) sp[i] = ' '; sp[w] = '\0';
    ncplane_set_bg_rgb(n, bg); ncplane_set_fg_rgb(n, 0xbbbbbb);
    ncplane_putstr_yx(n, y, x, sp);
}

int run_notcurses(World *w) {
    struct notcurses_options opts; memset(&opts, 0, sizeof(opts));
    opts.flags = NCOPTION_SUPPRESS_BANNERS;
    struct notcurses *nc = notcurses_init(&opts, NULL);
    if (!nc) { fprintf(stderr, "notcurses_init failed\n"); return 1; }
    struct ncplane *std = notcurses_stdplane(nc);

    ncblitter_e blit = NCBLIT_DEFAULT;
    if (g_blit) {
        if (!strcmp(g_blit, "sextant")) blit = NCBLIT_3x2;
        else if (!strcmp(g_blit, "quad")) blit = NCBLIT_2x2;
        else if (!strcmp(g_blit, "half")) blit = NCBLIT_2x1;
        else if (!strcmp(g_blit, "braille")) blit = NCBLIT_BRAILLE;
        else if (!strcmp(g_blit, "ascii")) blit = NCBLIT_1x1;
        else if (!strcmp(g_blit, "pixel")) blit = NCBLIT_PIXEL;
    }

    const int W = WORLD_W * PXT, H = WORLD_H * PXT;
    unsigned char *buf = malloc((size_t)W * H * 4);
    if (!buf) { notcurses_stop(nc); return 1; }

    int paused = 0, running = 1, fps = 0, frames = 0;
    float speed = 1.0f;
    int god = 0, tool = G_SMITE, show_feed = 0, show_jail = 0, selected = -1;
    int cur_x = WORLD_W / 2, cur_y = WORLD_H / 2;
    char flash[96] = ""; double flash_until = 0;
    double prev = now_sec(), facc = 0;
    if (getenv("CSIM_DEMO")) { god = 1; show_feed = 1; show_jail = 1; }

    while (running) {
        struct ncinput ni; uint32_t id;
        while ((id = notcurses_get_nblock(nc, &ni)) != 0) {
            if (ni.evtype == NCTYPE_RELEASE) continue;
            if (id == 'q' || id == 'Q') running = 0;
            else if (id == ' ') paused = !paused;
            else if (id == 'g' || id == 'G') god = !god;
            else if (id == NCKEY_TAB) show_feed = !show_feed;
            else if (id == 'j' || id == 'J') show_jail = !show_jail;
            else if (id == 'i' || id == 'I') { Agent *a = world_agent_at(w, cur_x, cur_y, 5); selected = a ? a->id : -1; }
            else if (id == NCKEY_ESC) selected = -1;
            else if (id == NCKEY_ENTER && god) { god_apply(w, tool, cur_x, cur_y, flash, sizeof(flash)); flash_until = now_sec() + 2.5; }
            else if (id == NCKEY_LEFT)  cur_x = cur_x > 1 ? cur_x - 2 : 0;
            else if (id == NCKEY_RIGHT) cur_x = cur_x < WORLD_W - 2 ? cur_x + 2 : WORLD_W - 1;
            else if (id == NCKEY_UP)    cur_y = cur_y > 1 ? cur_y - 2 : 0;
            else if (id == NCKEY_DOWN)  cur_y = cur_y < WORLD_H - 2 ? cur_y + 2 : WORLD_H - 1;
            else if (id >= '1' && id <= '8') {
                if (god) tool = id - '1';
                else if (id == '1') speed = 1; else if (id == '2') speed = 5; else if (id == '3') speed = 20;
            }
        }

        double t = now_sec(), dt = t - prev; prev = t;
        facc += dt; frames++;
        if (facc >= 0.5) { fps = (int)(frames / facc); frames = 0; facc = 0; }
        if (!paused && dt > 0) world_tick(w, dt * speed);

        /* paint bitmap */
        for (int x = 0; x < WORLD_W; x++)
            for (int y = 0; y < WORLD_H; y++) {
                unsigned char r, g, b; tile_rgb((TileType)w->tile[x][y], &r, &g, &b);
                for (int py = 0; py < PXT; py++)
                    for (int px = 0; px < PXT; px++) {
                        size_t o = ((size_t)(y*PXT+py)*W + (x*PXT+px)) * 4;
                        buf[o]=r; buf[o+1]=g; buf[o+2]=b; buf[o+3]=255;
                    }
            }
        for (int i = 0; i < w->n_agents; i++) {
            Agent *a = &w->agents[i];
            if (!a->alive) continue;
            int bx = (int)a->x*PXT, by = (int)a->y*PXT;
            for (int py = 0; py < PXT; py++) for (int px = 0; px < PXT; px++) {
                int X = bx+px, Y = by+py; if (X<0||X>=W||Y<0||Y>=H) continue;
                size_t o = ((size_t)Y*W+X)*4; buf[o]=a->r; buf[o+1]=a->g; buf[o+2]=a->b; buf[o+3]=255;
            }
        }
        if (god) {  /* cursor crosshair */
            int bx = cur_x*PXT, by = cur_y*PXT;
            for (int d = -PXT; d <= 2*PXT; d++) {
                int X=bx+d, Y=by+PXT/2; if(X>=0&&X<W&&Y>=0&&Y<H){size_t o=((size_t)Y*W+X)*4;buf[o]=255;buf[o+1]=0;buf[o+2]=255;buf[o+3]=255;}
                X=bx+PXT/2; Y=by+d; if(X>=0&&X<W&&Y>=0&&Y<H){size_t o=((size_t)Y*W+X)*4;buf[o]=255;buf[o+1]=0;buf[o+2]=255;buf[o+3]=255;}
            }
        }

        struct ncvisual *v = ncvisual_from_rgba(buf, H, W*4, W);
        if (v) {
            struct ncvisual_options vopts; memset(&vopts, 0, sizeof(vopts));
            vopts.n = std; vopts.scaling = NCSCALE_SCALE; vopts.blitter = blit;
            ncvisual_blit(nc, v, &vopts);
            ncvisual_destroy(v);
        }

        unsigned rows, cols; ncplane_dim_yx(std, &rows, &cols);

        /* HUD */
        char hud[256]; hud_string(w, hud, sizeof(hud), speed, paused, fps, "notcurses");
        fillrow(std, 0, 0, (int)cols, 0x000000);
        put(std, 0, 0, 0xe6e1d7, 0x000000, hud);

        /* god toolbar (bottom) */
        if (god) {
            fillrow(std, rows-1, 0, (int)cols, 0x181620);
            int x = 0;
            put(std, rows-1, x, 0xd4af5a, 0x181620, "GOD "); x += 4;
            for (int i = 0; i < G_NTOOLS; i++) {
                char seg[24]; snprintf(seg, sizeof(seg), "%d%s ", i+1, GOD_TOOL_NAME[i]);
                put(std, rows-1, x, i==tool?0x141216:0xe6e1d7, i==tool?0xd4af5a:0x181620, seg);
                x += (int)strlen(seg);
            }
            put(std, 1, 2, 0xd4af5a, 0x000000, "GOD MODE  (arrows move cursor, enter apply, i inspect)");
        }
        if (flash[0] && now_sec() < flash_until)
            put(std, 2, 2, 0xe6e1d7, 0x000000, flash);

        /* event feed (right) */
        if (show_feed) {
            int pw = 36, px = (int)cols - pw; if (px < 0) px = 0;
            for (int r = 1; r < (int)rows-1; r++) fillrow(std, r, px, pw, 0x18161e);
            put(std, 1, px+1, 0xd4af5a, 0x18161e, "EVENT FEED [Tab]");
            int yy = 3;
            for (int i = 0; i < w->ev_count && yy < (int)rows-2; i++) {
                const WorldEvent *e = events_recent(w, i); if (!e) break;
                char line[40]; snprintf(line, sizeof(line), "%.34s", e->text);
                put(std, yy++, px+1, 0xd0cbc0, 0x18161e, line);
            }
        }

        /* jail roster (center) */
        if (show_jail) {
            int pw = 54, ph = 4 + 18, px = (int)cols/2 - pw/2, py = 3;
            if (px < 0) px = 0;
            for (int r = 0; r < ph && py+r < (int)rows-1; r++) fillrow(std, py+r, px, pw, 0x18161e);
            char title[64]; snprintf(title, sizeof(title), "JAIL ROSTER (%d) [j]", crime_jailed_count(w));
            put(std, py, px+1, 0xd4af5a, 0x18161e, title);
            put(std, py+1, px+1, 0x8a8780, 0x18161e, "NAME              CRIME      SERVED/SENT");
            int yy = py+2, shown = 0;
            for (int i = 0; i < w->n_agents && shown < 15 && yy < (int)rows-2; i++) {
                Agent *a = &w->agents[i];
                if (!a->alive || a->arrested_ticks <= 0) continue;
                int tot = a->sentence_total>0?a->sentence_total:1, served = tot-a->arrested_ticks;
                if (served<0) served=0;
                char line[64]; snprintf(line, sizeof(line), "%-16.16s  %-9.9s  %d/%d",
                    a->name, a->jailed_for[0]?a->jailed_for:"-", served, tot);
                put(std, yy++, px+1, 0xe6e1d7, 0x18161e, line); shown++;
            }
            if (!shown) put(std, yy, px+1, 0x8a8780, 0x18161e, "No one is in jail.");
        }

        /* inspector (left) */
        if (selected >= 0) {
            Agent *a = world_agent_by_id(w, selected);
            if (a && a->alive) {
                int pw = 30, ph = 12, px = 0, py = 2;
                for (int r = 0; r < ph; r++) fillrow(std, py+r, px, pw, 0x18161e);
                char l[48];
                snprintf(l, sizeof(l), "%.20s #%d", a->name, a->id); put(std, py, px+1, 0xd4af5a, 0x18161e, l);
                snprintf(l, sizeof(l), "age %d  %s", a->age, action_name(a->action)); put(std, py+1, px+1, 0xe6e1d7, 0x18161e, l);
                snprintf(l, sizeof(l), "money %.0f  fac %d", a->needs.money, a->faction_id); put(std, py+2, px+1, 0xe6e1d7, 0x18161e, l);
                if (a->wanted) { snprintf(l, sizeof(l), "WANTED: %s", a->wanted_for); put(std, py+3, px+1, 0xe65a50, 0x18161e, l); }
                const char *lb[6]={"hun","ene","saf","soc","mea","bel"};
                double vv[6]={a->needs.hunger,a->needs.energy,a->needs.safety,a->needs.social,a->needs.meaning,a->needs.belonging};
                for (int i=0;i<6;i++){ char bar[24]; int fill=(int)(vv[i]*10);
                    char b2[11]; for(int k=0;k<10;k++) b2[k]=k<fill?'#':'.'; b2[10]='\0';
                    snprintf(bar,sizeof(bar),"%s %s", lb[i], b2); put(std, py+4+i, px+1, 0xd0cbc0, 0x18161e, bar); }
                put(std, py+11, px+1, 0x8a8780, 0x18161e, "esc close");
            } else selected = -1;
        }

        notcurses_render(nc);
        struct timespec slp = {0, 33*1000*1000}; nanosleep(&slp, NULL);
    }

    free(buf);
    notcurses_stop(nc);
    return 0;
}
