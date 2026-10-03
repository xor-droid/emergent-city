/* backend_raylib.c — raylib renderer. Compiled only when HAVE_RAYLIB. */
#include "sim.h"
#include "viz.h"
#include "raylib.h"
#include <stdio.h>
#include <stdlib.h>

static Color tile_color(TileType t) {
    unsigned char r, g, b;
    tile_rgb(t, &r, &g, &b);
    return (Color){r, g, b, 255};
}

int run_raylib(World *w) {
    const int screenW = 1280, screenH = 800;
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(screenW, screenH, "Emergent City (C / raylib)");
    SetTargetFPS(60);

    Camera2D cam = {0};
    cam.zoom = 1.0f;
    cam.offset = (Vector2){screenW * 0.5f, screenH * 0.5f};
    cam.target = (Vector2){WORLD_W * TILE_PX * 0.5f, WORLD_H * TILE_PX * 0.5f};

    int paused = 0;
    float speed = 1.0f;

    /* Optional: CSIM_SHOT=path captures a frame after a warm-up then exits. */
    const char *shot = getenv("CSIM_SHOT");
    int frame = 0;

    while (!WindowShouldClose()) {
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

        if (!paused) {
            float dt = GetFrameTime() * speed;
            if (dt > 0.0f) world_tick(w, dt);
        }

        BeginDrawing();
        ClearBackground((Color){18, 16, 22, 255});
        BeginMode2D(cam);
        for (int x = 0; x < WORLD_W; x++)
            for (int y = 0; y < WORLD_H; y++)
                DrawRectangle(x * TILE_PX, y * TILE_PX, TILE_PX, TILE_PX,
                              tile_color((TileType)w->tile[x][y]));
        for (int i = 0; i < w->n_agents; i++) {
            Agent *a = &w->agents[i];
            if (!a->alive) continue;
            DrawRectangle((int)(a->x * TILE_PX) + 1, (int)(a->y * TILE_PX) + 1,
                          TILE_PX - 2, TILE_PX - 2, (Color){a->r, a->g, a->b, 255});
        }
        EndMode2D();

        char hud[256];
        hud_string(w, hud, sizeof(hud), speed, paused, GetFPS(), "raylib");
        DrawRectangle(0, 0, GetScreenWidth(), 24, (Color){0, 0, 0, 170});
        DrawText(hud, 8, 5, 14, (Color){230, 225, 215, 255});
        EndDrawing();

        if (shot && ++frame == 120) { TakeScreenshot(shot); break; }
    }
    CloseWindow();
    return 0;
}
