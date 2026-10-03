/* backend_raylib.c — raylib renderer: God Mode, panels, scrollable client list. */
#include "sim.h"
#include "viz.h"
#include "raylib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TILE_PX 8
#define PANEL_W 320
#define FEED_LINES 10
#define ROW_H 16

static Color tile_color(TileType t) { unsigned char r,g,b; tile_rgb(t,&r,&g,&b); return (Color){r,g,b,255}; }
static Color rgb(unsigned char r, unsigned char g, unsigned char b) { return (Color){r,g,b,255}; }
static unsigned char clampb(int v){ return (unsigned char)(v<0?0:v>255?255:v); }
static Color shade(Color c, int d) { return (Color){clampb(c.r+d),clampb(c.g+d),clampb(c.b+d),c.a}; }

/* Single-char type marker drawn on buildings when zoomed in. */
static char building_glyph(TileType t) {
    switch (t) { case T_HOME: return 'H'; case T_SHOP: return '$'; case T_WORK: return 'O';
                 case T_BAR: return 'B'; case T_CHURCH: return '+'; case T_POLICE: return 'P';
                 default: return 0; }
}

static const char *faction_name(World *w, int fid) {
    if (fid < 0 || fid >= w->n_factions) return "-";
    return w->factions[fid].name;
}

static const char *agent_tag(const Agent *a) {
    if (a->arrested_ticks > 0) return "JAIL";
    if (a->wanted) return "WANTED";
    if (a->is_police) return "police";
    if (a->faction_id != -1) return "faction";
    return "";
}

int run_raylib(World *w) {
    const int screenW = 1280, screenH = 800;
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(screenW, screenH, "Emergent City (C / raylib)");
    SetTargetFPS(60);

    Camera2D cam = {0};
    cam.zoom = 1.0f;
    { const char *ez = getenv("CSIM_ZOOM"); if (ez) { float z = (float)atof(ez); if (z > 0) cam.zoom = z; } }
    cam.offset = (Vector2){screenW * 0.5f, screenH * 0.5f};
    cam.target = (Vector2){WORLD_W * TILE_PX * 0.5f, WORLD_H * TILE_PX * 0.5f};

    int paused = 0; float speed = 1.0f;
    int god = 0, tool = G_SMITE;
    int show_right = 1, show_jail = 0, show_factions = 0, show_legend = 1;
    int selected = -1, list_scroll = 0;
    char flash[96] = ""; double flash_until = 0;
    const char *shot = getenv("CSIM_SHOT"); int frame = 0;
    if (getenv("CSIM_DEMO")) { god = 1; show_jail = 1; show_factions = 1; }

    /* reusable alive-agent id list */
    static int ids[MAX_AGENTS];

    while (!WindowShouldClose()) {
        int W = GetScreenWidth(), Hs = GetScreenHeight();
        int panelX = W - PANEL_W;
        int feedBottom = 28 + FEED_LINES * 15 + 6;
        int listTop = feedBottom + 18;
        int listRows = (Hs - listTop - 6) / ROW_H;
        if (listRows < 1) listRows = 1;

        /* ── gather alive agents ── */
        int nlist = 0;
        for (int i = 0; i < w->n_agents; i++) if (w->agents[i].alive) ids[nlist++] = w->agents[i].id;
        int maxscroll = nlist - listRows; if (maxscroll < 0) maxscroll = 0;

        /* current selection index within the list */
        int selIdx = -1;
        for (int i = 0; i < nlist; i++) if (ids[i] == selected) { selIdx = i; break; }

        /* ── input ── */
        if (IsKeyPressed(KEY_SPACE)) paused = !paused;
        if (IsKeyPressed(KEY_G)) god = !god;
        if (IsKeyPressed(KEY_TAB)) show_right = !show_right;
        if (IsKeyPressed(KEY_J)) show_jail = !show_jail;
        if (IsKeyPressed(KEY_F)) show_factions = !show_factions;
        if (IsKeyPressed(KEY_L)) show_legend = !show_legend;
        for (int k = 0; k < 9; k++) if (IsKeyPressed(KEY_ONE + k)) {
            if (god) { if (k < G_NTOOLS) tool = k; }
            else if (k == 0) speed = 1; else if (k == 1) speed = 5; else if (k == 2) speed = 20;
        }

        /* list selection via keyboard */
        int navstep = 0;
        if (IsKeyPressed(KEY_DOWN)) navstep = 1;
        if (IsKeyPressed(KEY_UP)) navstep = -1;
        if (IsKeyPressed(KEY_PAGE_DOWN)) navstep = listRows;
        if (IsKeyPressed(KEY_PAGE_UP)) navstep = -listRows;
        if (navstep && nlist) {
            int ni = (selIdx < 0 ? 0 : selIdx) + navstep;
            if (ni < 0) ni = 0; if (ni >= nlist) ni = nlist - 1;
            selIdx = ni; selected = ids[ni];
            Agent *a = world_agent_by_id(w, selected);
            if (a) cam.target = (Vector2){a->x * TILE_PX, a->y * TILE_PX};
            if (selIdx < list_scroll) list_scroll = selIdx;
            if (selIdx >= list_scroll + listRows) list_scroll = selIdx - listRows + 1;
        }
        if (IsKeyPressed(KEY_ESCAPE)) selected = -1;

        Vector2 mp = GetMousePosition();
        int overPanel = show_right && mp.x >= panelX;
        float wheel = GetMouseWheelMove();
        if (wheel != 0) {
            if (overPanel && mp.y >= listTop) {
                list_scroll -= (int)wheel * 3;
                if (list_scroll < 0) list_scroll = 0; if (list_scroll > maxscroll) list_scroll = maxscroll;
            } else { cam.zoom += wheel * 0.1f; if (cam.zoom < 0.2f) cam.zoom = 0.2f; if (cam.zoom > 4.0f) cam.zoom = 4.0f; }
        }
        if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) { Vector2 d = GetMouseDelta();
            cam.target.x -= d.x / cam.zoom; cam.target.y -= d.y / cam.zoom; }

        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            if (overPanel && mp.y >= listTop) {
                int row = list_scroll + (int)((mp.y - listTop) / ROW_H);
                if (row >= 0 && row < nlist) {
                    selected = ids[row];
                    Agent *a = world_agent_by_id(w, selected);
                    if (a) cam.target = (Vector2){a->x * TILE_PX, a->y * TILE_PX};  /* auto-pan */
                }
            } else if (!overPanel) {
                Vector2 wp = GetScreenToWorld2D(mp, cam);
                int tx = (int)(wp.x / TILE_PX), ty = (int)(wp.y / TILE_PX);
                if (god) { god_apply(w, tool, tx, ty, flash, sizeof(flash)); flash_until = GetTime() + 2.5; }
                else { Agent *a = world_agent_at(w, tx, ty, 5);
                    if (a) { selected = a->id; cam.target = (Vector2){a->x*TILE_PX, a->y*TILE_PX}; } else selected = -1; }
            }
        }

        if (!paused) { float dt = GetFrameTime() * speed; if (dt > 0) world_tick(w, dt); }

        /* ── render world ── */
        BeginDrawing();
        ClearBackground((Color){18,16,22,255});
        BeginMode2D(cam);
        /* only draw tiles inside the camera viewport (huge win when zoomed in) */
        Vector2 tl = GetScreenToWorld2D((Vector2){0,0}, cam);
        Vector2 br = GetScreenToWorld2D((Vector2){(float)W,(float)Hs}, cam);
        int x0=(int)(tl.x/TILE_PX)-1, x1=(int)(br.x/TILE_PX)+1;
        int y0=(int)(tl.y/TILE_PX)-1, y1=(int)(br.y/TILE_PX)+1;
        if (x0<0)x0=0; if (y0<0)y0=0; if (x1>=WORLD_W)x1=WORLD_W-1; if (y1>=WORLD_H)y1=WORLD_H-1;
        Color outline = (Color){12,10,16,255};
        for (int x = x0; x <= x1; x++)
            for (int y = y0; y <= y1; y++) {
                TileType t = (TileType)w->tile[x][y];
                int px = x*TILE_PX, py = y*TILE_PX;
                if (!tile_is_building(t)) {             /* flat ground / road / water / park */
                    DrawRectangle(px, py, TILE_PX, TILE_PX, tile_color(t));
                    continue;
                }
                /* raised block in 3 rects: dark backing (=gap+outline) + body + lit roof */
                Color body = shade(tile_color(t), tile_shade_jitter(x, y, 14));
                DrawRectangle(px, py, TILE_PX, TILE_PX, outline);
                DrawRectangle(px+1, py+1, TILE_PX-2, TILE_PX-2, body);
                DrawRectangle(px+1, py+1, TILE_PX-2, 2, shade(body, 34));  /* roof highlight */
            }
        for (int i = 0; i < w->n_agents; i++) {
            Agent *a = &w->agents[i];
            if (!a->alive) continue;
            int cxp = (int)(a->x*TILE_PX) + TILE_PX/2, cyp = (int)(a->y*TILE_PX) + TILE_PX/2;
            DrawCircle(cxp, cyp, TILE_PX*0.34f, (Color){0,0,0,160});     /* dark halo for contrast */
            DrawCircle(cxp, cyp, TILE_PX*0.26f, rgb(a->r,a->g,a->b));    /* agent */
            if (a->id == selected)
                DrawRectangleLines((int)(a->x*TILE_PX)-2, (int)(a->y*TILE_PX)-2, TILE_PX+4, TILE_PX+4, YELLOW);
        }
        EndMode2D();

        /* ── zoom-aware building type glyphs (screen space, crisp) ── */
        float tpx = TILE_PX * cam.zoom;              /* on-screen tile size */
        if (tpx >= 13.0f) {
            int fs = (int)(tpx * 0.72f); if (fs < 8) fs = 8;
            for (int x = x0; x <= x1; x++)
                for (int y = y0; y <= y1; y++) {
                    char g = building_glyph((TileType)w->tile[x][y]);
                    if (!g) continue;
                    char s[2] = { g, 0 };
                    Vector2 sp = GetWorldToScreen2D(
                        (Vector2){ x*TILE_PX + TILE_PX*0.5f, y*TILE_PX + TILE_PX*0.5f }, cam);
                    int tw = MeasureText(s, fs);
                    DrawText(s, (int)(sp.x - tw*0.5f), (int)(sp.y - fs*0.5f), fs, (Color){18,14,20,230});
                }
        }

        /* ── HUD ── */
        char hud[256]; hud_string(w, hud, sizeof(hud), speed, paused, GetFPS(), "raylib");
        DrawRectangle(0, 0, W, 24, (Color){0,0,0,170});
        DrawText(hud, 8, 5, 14, (Color){230,225,215,255});

        /* ── right panel: feed (top) + client list (bottom) ── */
        if (show_right) {
            DrawRectangle(panelX, 24, PANEL_W, Hs-24, (Color){24,22,30,225});
            DrawText("EVENT FEED [Tab]", panelX+10, 30, 13, (Color){212,175,90,255});
            int yy = 50;
            for (int i = 0; i < FEED_LINES && i < w->ev_count; i++) {
                const WorldEvent *e = events_recent(w, i); if (!e) break;
                DrawText(TextFormat("%.44s", e->text), panelX+10, yy, 11, (Color){220,215,205,255}); yy += 15;
            }
            DrawLine(panelX+8, feedBottom+8, W-8, feedBottom+8, (Color){80,76,90,255});
            DrawText(TextFormat("CITIZENS (%d)", nlist), panelX+10, feedBottom+12, 13, (Color){212,175,90,255});
            /* list rows */
            for (int r = 0; r < listRows && (list_scroll+r) < nlist; r++) {
                int idx = list_scroll + r;
                Agent *a = world_agent_by_id(w, ids[idx]);
                if (!a) continue;
                int ry = listTop + r * ROW_H;
                if (a->id == selected) DrawRectangle(panelX+2, ry, PANEL_W-14, ROW_H, (Color){60,56,40,255});
                DrawRectangle(panelX+8, ry+5, 6, 6, rgb(a->r,a->g,a->b));
                DrawText(TextFormat("%.22s", a->name), panelX+18, ry+2, 11, (Color){230,225,215,255});
                const char *tag = agent_tag(a);
                if (tag[0]) {
                    Color tc = a->wanted?(Color){230,90,80,255}:a->arrested_ticks>0?(Color){220,180,90,255}:(Color){150,145,135,255};
                    DrawText(tag, W-12-MeasureText(tag,10), ry+3, 10, tc);
                }
            }
            /* scrollbar */
            if (nlist > listRows) {
                int trackX = W-6, trackY = listTop, trackH = listRows*ROW_H;
                DrawRectangle(trackX, trackY, 4, trackH, (Color){40,38,48,255});
                int thumbH = trackH * listRows / nlist; if (thumbH < 10) thumbH = 10;
                int thumbY = trackY + (trackH - thumbH) * list_scroll / (maxscroll>0?maxscroll:1);
                DrawRectangle(trackX, thumbY, 4, thumbH, (Color){150,145,135,255});
                /* click track to jump */
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && mp.x >= trackX-2 && mp.y >= trackY && mp.y <= trackY+trackH) {
                    float f = (mp.y - trackY) / (float)trackH; list_scroll = (int)(f * maxscroll);
                    if (list_scroll < 0) list_scroll = 0; if (list_scroll > maxscroll) list_scroll = maxscroll;
                }
            }
        }

        /* ── jail roster (name / crime / gang / sentence) ── */
        if (show_jail) {
            int pw = 680, ph = 40 + 22 + 22*22, px = W/2 - pw/2, py = 60;
            DrawRectangle(px, py, pw, ph, (Color){24,22,30,235});
            DrawRectangleLines(px, py, pw, ph, (Color){212,175,90,255});
            DrawText(TextFormat("JAIL ROSTER (%d)  [J]", crime_jailed_count(w)), px+12, py+10, 15, (Color){212,175,90,255});
            DrawText("NAME", px+12, py+34, 10, GRAY); DrawText("CRIME", px+210, py+34, 10, GRAY);
            DrawText("GANG/CULT", px+300, py+34, 10, GRAY); DrawText("SERVED/SENTENCE", px+480, py+34, 10, GRAY);
            int yy = py+50, shown = 0;
            for (int i = 0; i < w->n_agents && shown < 22; i++) {
                Agent *a = &w->agents[i];
                if (!a->alive || a->arrested_ticks <= 0) continue;
                int tot = a->sentence_total>0?a->sentence_total:1, served = tot-a->arrested_ticks; if (served<0) served=0;
                DrawText(TextFormat("%.24s", a->name), px+12, yy, 12, WHITE);
                DrawText(a->jailed_for[0]?a->jailed_for:"-", px+210, yy, 12, WHITE);
                DrawText(TextFormat("%.22s", faction_name(w, a->faction_id)), px+300, yy, 12,
                         a->faction_id>=0?(Color){200,120,220,255}:GRAY);
                DrawText(TextFormat("%d/%d (%d%%)", served, tot, served*100/tot), px+480, yy, 12,
                         served*100/tot>=66?(Color){120,220,130,255}:WHITE);
                yy += 20; shown++;
            }
            if (!shown) DrawText("No one is in jail.", px+12, yy, 13, GRAY);
        }

        /* ── factions menu (name / kind / members / leader) ── */
        if (show_factions) {
            int pw = 560, ph = 40 + 22 + (w->n_factions+1)*22, px = W/2 - pw/2, py = 80;
            DrawRectangle(px, py, pw, ph, (Color){24,22,30,235});
            DrawRectangleLines(px, py, pw, ph, (Color){200,120,220,255});
            DrawText("FACTIONS  [F]", px+12, py+10, 15, (Color){200,120,220,255});
            DrawText("NAME", px+12, py+34, 10, GRAY); DrawText("KIND", px+260, py+34, 10, GRAY);
            DrawText("MEMBERS", px+360, py+34, 10, GRAY); DrawText("LEADER", px+450, py+34, 10, GRAY);
            int yy = py+50;
            for (int i = 0; i < w->n_factions; i++) {
                Faction *f = &w->factions[i];
                if (!f->active) continue;
                Agent *ldr = f->leader_id >= 0 ? world_agent_by_id(w, f->leader_id) : NULL;
                DrawRectangle(px+12, yy+3, 8, 8, rgb(f->r, f->g, f->b));
                DrawText(TextFormat("%.26s", f->name), px+26, yy, 12, WHITE);
                DrawText(f->is_cult?"cult":"gang", px+260, yy, 12, (Color){200,180,150,255});
                DrawText(TextFormat("%d", f->members), px+360, yy, 12, WHITE);
                DrawText(ldr?TextFormat("%.16s", ldr->name):"-", px+450, yy, 12, GRAY);
                yy += 22;
            }
        }

        /* ── inspector (left) ── */
        if (selected >= 0) {
            Agent *a = world_agent_by_id(w, selected);
            if (a && a->alive) {
                int pw = 300; DrawRectangle(0, 24, pw, 320, (Color){24,22,30,230});
                int yy = 30;
                DrawText(TextFormat("%s  #%d", a->name, a->id), 10, yy, 15, (Color){212,175,90,255}); yy+=24;
                DrawText(TextFormat("Age %d   Action: %s", a->age, action_name(a->action)), 10, yy, 12, WHITE); yy+=18;
                DrawText(TextFormat("Money %.0f   Faction %d", a->needs.money, a->faction_id), 10, yy, 12, WHITE); yy+=18;
                if (a->is_police) { DrawText("POLICE", 10, yy, 12, (Color){120,180,230,255}); yy+=18; }
                if (a->wanted) { DrawText(TextFormat("WANTED for %s", a->wanted_for), 10, yy, 12, (Color){230,90,80,255}); yy+=18; }
                if (a->arrested_ticks>0) { DrawText(TextFormat("JAILED for %s", a->jailed_for), 10, yy, 12, (Color){220,180,90,255}); yy+=18; }
                const char *lbl[6]={"hunger","energy","safety","social","meaning","belong"};
                double v[6]={a->needs.hunger,a->needs.energy,a->needs.safety,a->needs.social,a->needs.meaning,a->needs.belonging};
                for (int i=0;i<6;i++){ DrawText(lbl[i],10,yy,11,WHITE);
                    DrawRectangle(90,yy,180,8,(Color){50,50,60,255});
                    Color c = v[i]<0.2?(Color){230,90,80,255}:v[i]<0.4?(Color){220,180,90,255}:(Color){120,220,130,255};
                    DrawRectangle(90,yy,(int)(180*v[i]),8,c); yy+=16; }
                int friends=0, rivals=0;
                for(int i=0;i<a->rels.n;i++){ if(a->rels.rel[i].affinity>=FRIENDSHIP_AFFINITY) friends++;
                    else if(a->rels.rel[i].affinity<=RIVALRY_AFFINITY) rivals++; }
                DrawText(TextFormat("Friends %d  Rivals %d  Known %d", friends, rivals, a->rels.n), 10, yy, 12, WHITE); yy+=18;
                DrawText("arrows/wheel: browse   esc: close", 10, yy+4, 10, GRAY);
            } else selected = -1;
        }

        /* ── god toolbar + badge + flash ── */
        if (god) {
            int cw=92, ch=28, gap=6, total=G_NTOOLS*cw+(G_NTOOLS-1)*gap;
            int sx=W/2-total/2, yb=Hs-ch-12;
            DrawRectangle(sx-12, yb-8, total+24, ch+16, (Color){24,22,30,220});
            for (int i=0;i<G_NTOOLS;i++){ int cx=sx+i*(cw+gap); int sel=(i==tool);
                DrawRectangle(cx,yb,cw,ch, sel?(Color){212,175,90,230}:(Color){32,30,38,220});
                if(sel) DrawRectangleLines(cx,yb,cw,ch,(Color){255,255,255,255});
                DrawText(TextFormat("%d %s", i+1, GOD_TOOL_NAME[i]), cx+8, yb+8, 12, sel?BLACK:WHITE); }
            const char *b = "GOD MODE";
            DrawText(b, W/2 - MeasureText(b,16)/2, 28, 16, (Color){212,175,90,255});
        }
        if (flash[0] && GetTime() < flash_until)
            DrawText(flash, W/2 - MeasureText(flash,16)/2, 50, 16, (Color){230,225,215,255});

        /* ── legend (bottom-left) ── */
        if (show_legend) {
            int rows = (LEGEND_N + 1) / 2, rh = 16, lw = 168, lh = 20 + rows*rh;
            int lx = 10, ly = Hs - lh - 10;
            DrawRectangle(lx, ly, lw, lh, (Color){24,22,30,225});
            DrawRectangleLines(lx, ly, lw, lh, (Color){60,56,70,255});
            DrawText("LEGEND  [L]", lx+8, ly+5, 11, (Color){212,175,90,255});
            for (int i = 0; i < LEGEND_N; i++) {
                int col = i % 2, row = i / 2;
                int ex = lx + 8 + col*80, ey = ly + 22 + row*rh;
                Color sc = tile_color(LEGEND[i].type);
                DrawRectangle(ex, ey, 10, 10, sc);
                DrawRectangleLines(ex, ey, 10, 10, (Color){10,8,14,180});
                DrawText(LEGEND[i].label, ex+14, ey-1, 11, (Color){225,220,210,255});
            }
        }

        EndDrawing();
        if (shot && ++frame == 120) { TakeScreenshot(shot); break; }
    }
    CloseWindow();
    return 0;
}
