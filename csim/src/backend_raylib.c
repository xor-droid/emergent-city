/* backend_raylib.c — raylib renderer with God Mode + panels. HAVE_RAYLIB only. */
#include "sim.h"
#include "viz.h"
#include "raylib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TILE_PX 8

static Color tile_color(TileType t) {
    unsigned char r, g, b; tile_rgb(t, &r, &g, &b);
    return (Color){r, g, b, 255};
}
static Color rgb(unsigned char r, unsigned char g, unsigned char b) { return (Color){r,g,b,255}; }

int run_raylib(World *w) {
    const int screenW = 1280, screenH = 800;
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(screenW, screenH, "Emergent City (C / raylib)");
    SetTargetFPS(60);

    Camera2D cam = {0};
    cam.zoom = 1.0f;
    cam.offset = (Vector2){screenW * 0.5f, screenH * 0.5f};
    cam.target = (Vector2){WORLD_W * TILE_PX * 0.5f, WORLD_H * TILE_PX * 0.5f};

    int paused = 0; float speed = 1.0f;
    int god = 0, tool = G_SMITE;
    int show_feed = 0, show_jail = 0;
    int selected = -1;                 /* inspector agent id */
    char flash[96] = ""; double flash_until = 0;

    const char *shot = getenv("CSIM_SHOT"); int frame = 0;
    if (getenv("CSIM_DEMO")) { god = 1; show_feed = 1; show_jail = 1; }  /* for screenshots */

    while (!WindowShouldClose()) {
        /* ── input ── */
        if (IsKeyPressed(KEY_SPACE)) paused = !paused;
        if (IsKeyPressed(KEY_G)) god = !god;
        if (IsKeyPressed(KEY_TAB)) show_feed = !show_feed;
        if (IsKeyPressed(KEY_J)) show_jail = !show_jail;
        for (int k = 0; k < 9; k++) {
            if (IsKeyPressed(KEY_ONE + k)) {
                if (god) { if (k < G_NTOOLS) tool = k; }
                else if (k == 0) speed = 1.0f; else if (k == 1) speed = 5.0f; else if (k == 2) speed = 20.0f;
            }
        }
        cam.zoom += GetMouseWheelMove() * 0.1f;
        if (cam.zoom < 0.2f) cam.zoom = 0.2f; if (cam.zoom > 4.0f) cam.zoom = 4.0f;
        if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {           /* right-drag pans */
            Vector2 d = GetMouseDelta();
            cam.target.x -= d.x / cam.zoom; cam.target.y -= d.y / cam.zoom;
        }
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            Vector2 wp = GetScreenToWorld2D(GetMousePosition(), cam);
            int tx = (int)(wp.x / TILE_PX), ty = (int)(wp.y / TILE_PX);
            if (!god) {
                Agent *a = world_agent_at(w, tx, ty, 5);
                selected = a ? a->id : -1;
            } else {
                god_apply(w, tool, tx, ty, flash, sizeof(flash));
                flash_until = GetTime() + 2.5;
            }
        }

        if (!paused) { float dt = GetFrameTime() * speed; if (dt > 0) world_tick(w, dt); }

        /* ── render world ── */
        BeginDrawing();
        ClearBackground((Color){18, 16, 22, 255});
        BeginMode2D(cam);
        for (int x = 0; x < WORLD_W; x++)
            for (int y = 0; y < WORLD_H; y++)
                DrawRectangle(x*TILE_PX, y*TILE_PX, TILE_PX, TILE_PX, tile_color((TileType)w->tile[x][y]));
        for (int i = 0; i < w->n_agents; i++) {
            Agent *a = &w->agents[i];
            if (!a->alive) continue;
            DrawRectangle((int)(a->x*TILE_PX)+1, (int)(a->y*TILE_PX)+1, TILE_PX-2, TILE_PX-2, rgb(a->r,a->g,a->b));
            if (a->id == selected)
                DrawRectangleLines((int)(a->x*TILE_PX)-1, (int)(a->y*TILE_PX)-1, TILE_PX+2, TILE_PX+2, YELLOW);
        }
        EndMode2D();

        /* ── HUD ── */
        char hud[256];
        hud_string(w, hud, sizeof(hud), speed, paused, GetFPS(), "raylib");
        DrawRectangle(0, 0, GetScreenWidth(), 24, (Color){0,0,0,170});
        DrawText(hud, 8, 5, 14, (Color){230,225,215,255});

        /* ── event feed (right) ── */
        if (show_feed) {
            int pw = 330, px = GetScreenWidth()-pw;
            DrawRectangle(px, 24, pw, GetScreenHeight()-24, (Color){24,22,30,220});
            DrawText("EVENT FEED [Tab]", px+10, 30, 13, (Color){212,175,90,255});
            int yy = 52;
            for (int i = 0; i < w->ev_count && yy < GetScreenHeight()-14; i++) {
                const WorldEvent *e = events_recent(w, i);
                if (!e) break;
                DrawText(TextFormat("%.44s", e->text), px+10, yy, 11, (Color){220,215,205,255});
                yy += 15;
            }
        }

        /* ── jail roster (center) ── */
        if (show_jail) {
            int pw = 560, ph = 40 + 22 + 22*22;
            int px = GetScreenWidth()/2 - pw/2, py = 60;
            DrawRectangle(px, py, pw, ph, (Color){24,22,30,235});
            DrawRectangleLines(px, py, pw, ph, (Color){212,175,90,255});
            DrawText(TextFormat("JAIL ROSTER (%d)  [J]", crime_jailed_count(w)), px+12, py+10, 15, (Color){212,175,90,255});
            DrawText("NAME", px+12, py+36, 11, GRAY);
            DrawText("CRIME", px+210, py+36, 11, GRAY);
            DrawText("SERVED/SENTENCE", px+330, py+36, 11, GRAY);
            int yy = py+56, shown=0;
            for (int i = 0; i < w->n_agents && shown < 22; i++) {
                Agent *a = &w->agents[i];
                if (!a->alive || a->arrested_ticks <= 0) continue;
                int tot = a->sentence_total > 0 ? a->sentence_total : 1;
                int served = tot - a->arrested_ticks; if (served < 0) served = 0;
                DrawText(TextFormat("%.26s", a->name), px+12, yy, 13, WHITE);
                DrawText(a->jailed_for[0]?a->jailed_for:"-", px+210, yy, 13, WHITE);
                DrawText(TextFormat("%d / %d (%d%%)", served, tot, served*100/tot), px+330, yy, 13,
                         served*100/tot>=66?(Color){120,220,130,255}:(Color){230,225,215,255});
                yy += 20; shown++;
            }
            if (shown == 0) DrawText("No one is in jail.", px+12, yy, 13, GRAY);
        }

        /* ── inspector (left) ── */
        if (selected >= 0) {
            Agent *a = world_agent_by_id(w, selected);
            if (a && a->alive) {
                int pw = 300; DrawRectangle(0, 24, pw, 300, (Color){24,22,30,230});
                int yy = 30;
                DrawText(TextFormat("%s  #%d", a->name, a->id), 10, yy, 15, (Color){212,175,90,255}); yy+=24;
                DrawText(TextFormat("Age %d   Action: %s", a->age, action_name(a->action)), 10, yy, 12, WHITE); yy+=18;
                DrawText(TextFormat("Money %.0f   Faction %d", a->needs.money, a->faction_id), 10, yy, 12, WHITE); yy+=18;
                if (a->is_police) { DrawText("POLICE", 10, yy, 12, (Color){120,180,230,255}); yy+=18; }
                if (a->wanted) { DrawText(TextFormat("WANTED for %s", a->wanted_for), 10, yy, 12, (Color){230,90,80,255}); yy+=18; }
                const char *lbl[6]={"hunger","energy","safety","social","meaning","belong"};
                double v[6]={a->needs.hunger,a->needs.energy,a->needs.safety,a->needs.social,a->needs.meaning,a->needs.belonging};
                for (int i=0;i<6;i++){ DrawText(lbl[i],10,yy,11,WHITE);
                    DrawRectangle(90,yy,180,8,(Color){50,50,60,255});
                    Color c = v[i]<0.2?(Color){230,90,80,255}:v[i]<0.4?(Color){220,180,90,255}:(Color){120,220,130,255};
                    DrawRectangle(90,yy,(int)(180*v[i]),8,c); yy+=16; }
                int friends=0; for(int i=0;i<a->rels.n;i++) if(a->rels.rel[i].affinity>=FRIENDSHIP_AFFINITY) friends++;
                DrawText(TextFormat("Friends: %d   Known: %d", friends, a->rels.n), 10, yy, 12, WHITE); yy+=18;
                DrawText("click elsewhere / ESC to close", 10, yy+4, 10, GRAY);
            } else selected = -1;
        }
        if (IsKeyPressed(KEY_ESCAPE)) selected = -1;

        /* ── god toolbar + badge + flash ── */
        if (god) {
            int cw=92, ch=28, gap=6, total=G_NTOOLS*cw+(G_NTOOLS-1)*gap;
            int sx=GetScreenWidth()/2-total/2, yb=GetScreenHeight()-ch-12;
            DrawRectangle(sx-12, yb-8, total+24, ch+16, (Color){24,22,30,220});
            for (int i=0;i<G_NTOOLS;i++){ int cx=sx+i*(cw+gap); int sel=(i==tool);
                DrawRectangle(cx,yb,cw,ch, sel?(Color){212,175,90,230}:(Color){32,30,38,220});
                if(sel) DrawRectangleLines(cx,yb,cw,ch,(Color){255,255,255,255});
                DrawText(TextFormat("%d %s", i+1, GOD_TOOL_NAME[i]), cx+8, yb+8, 12, sel?BLACK:WHITE); }
            const char *b = "GOD MODE";
            DrawText(b, GetScreenWidth()/2 - MeasureText(b,16)/2, 28, 16, (Color){212,175,90,255});
        }
        if (flash[0] && GetTime() < flash_until)
            DrawText(flash, GetScreenWidth()/2 - MeasureText(flash,16)/2, 50, 16, (Color){230,225,215,255});

        EndDrawing();
        if (shot && ++frame == 120) { TakeScreenshot(shot); break; }
    }
    CloseWindow();
    return 0;
}
