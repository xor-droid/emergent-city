/* main.c — raylib renderer for the Emergent City C core.
 *
 * Draws the tile grid and agents (colored by mood), with a pannable/zoomable
 * camera and a HUD line. Build via CMake (raylib fetched automatically):
 *   cmake -B build -S . && cmake --build build && ./build/csim
 *
 * Controls: arrows/drag pan, wheel zoom, Space pause, 1/2/3 speed, Esc quit.
 */
#include "sim.h"
#include "raylib.h"
#include <stdio.h>

#define TILE_PX 8

static Color tile_color(TileType t) {
    switch (t) {
        case T_ROAD:   return (Color){48, 44, 52, 255};
        case T_HOME:   return (Color){162, 122, 96, 255};
        case T_SHOP:   return (Color){170, 140, 80, 255};
        case T_WORK:   return (Color){90, 90, 95, 255};
        case T_BAR:    return (Color){150, 110, 170, 255};
        case T_CHURCH: return (Color){200, 180, 150, 255};
        default:       return (Color){52, 78, 58, 255};   /* grass */
    }
}

int main(void) {
    const int screenW = 1280, screenH = 800;
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(screenW, screenH, "Emergent City (C / raylib)");
    SetTargetFPS(60);

    World w;
    world_init(&w, 1337);
    world_populate(&w, 150);

    Camera2D cam = {0};
    cam.zoom = 1.0f;
    cam.offset = (Vector2){screenW * 0.5f, screenH * 0.5f};
    cam.target = (Vector2){WORLD_W * TILE_PX * 0.5f, WORLD_H * TILE_PX * 0.5f};

    int paused = 0;
    float speed = 1.0f;

    while (!WindowShouldClose()) {
        /* input */
        if (IsKeyPressed(KEY_SPACE)) paused = !paused;
        if (IsKeyPressed(KEY_ONE)) speed = 1.0f;
        if (IsKeyPressed(KEY_TWO)) speed = 5.0f;
        if (IsKeyPressed(KEY_THREE)) speed = 20.0f;
        cam.zoom += GetMouseWheelMove() * 0.1f;
        if (cam.zoom < 0.2f) cam.zoom = 0.2f;
        if (cam.zoom > 4.0f) cam.zoom = 4.0f;
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            Vector2 d = GetMouseDelta();
            cam.target.x -= d.x / cam.zoom;
            cam.target.y -= d.y / cam.zoom;
        }

        /* simulate */
        if (!paused) {
            float dt = GetFrameTime() * speed;
            if (dt > 0.0f) world_tick(&w, dt);
        }

        /* render */
        BeginDrawing();
        ClearBackground((Color){18, 16, 22, 255});
        BeginMode2D(cam);
        for (int x = 0; x < WORLD_W; x++)
            for (int y = 0; y < WORLD_H; y++)
                DrawRectangle(x * TILE_PX, y * TILE_PX, TILE_PX, TILE_PX,
                              tile_color((TileType)w.tile[x][y]));
        for (int i = 0; i < w.n_agents; i++) {
            Agent *a = &w.agents[i];
            if (!a->alive) continue;
            DrawRectangle((int)(a->x * TILE_PX) + 1, (int)(a->y * TILE_PX) + 1,
                          TILE_PX - 2, TILE_PX - 2, (Color){a->r, a->g, a->b, 255});
        }
        EndMode2D();

        int alive = 0;
        for (int i = 0; i < w.n_agents; i++) alive += w.agents[i].alive;
        char hud[256];
        snprintf(hud, sizeof(hud),
                 "Day %d  %02d:00  Pop %d  Deaths %d  Crimes %d   [x%.0f%s]  %d FPS",
                 w.day, (int)w.hour, alive, w.deaths, w.crimes, speed,
                 paused ? " PAUSED" : "", GetFPS());
        DrawRectangle(0, 0, GetScreenWidth(), 24, (Color){0, 0, 0, 170});
        DrawText(hud, 8, 5, 14, (Color){230, 225, 215, 255});
        EndDrawing();
    }

    CloseWindow();
    return 0;
}
