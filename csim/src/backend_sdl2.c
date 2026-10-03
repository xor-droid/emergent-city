/* backend_sdl2.c — SDL2 renderer. Compiled only when HAVE_SDL2. */
#include "sim.h"
#include "viz.h"
#include <SDL.h>
#include <SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>

int run_sdl2(World *w) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    TTF_Init();
    TTF_Font *font = TTF_OpenFont(FONT_PATH, 14);  /* may be NULL; HUD skipped if so */

    int screenW = 1280, screenH = 800;
    SDL_Window *win = SDL_CreateWindow("Emergent City (C / SDL2)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, screenW, screenH,
        SDL_WINDOW_RESIZABLE);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!win || !ren) { fprintf(stderr, "SDL window/renderer: %s\n", SDL_GetError()); return 1; }

    /* Camera: screen = (world - target) * zoom + offset(center) */
    float zoom = 1.0f;
    float tx = WORLD_W * TILE_PX * 0.5f, ty = WORLD_H * TILE_PX * 0.5f;
    int paused = 0;
    float speed = 1.0f;

    Uint64 prev = SDL_GetPerformanceCounter();
    double freq = (double)SDL_GetPerformanceFrequency();
    int running = 1, fps = 0, frames = 0; double facc = 0.0;
    const char *shot = getenv("CSIM_SHOT");   /* save a BMP after warm-up, then exit */
    int shot_frame = 0;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;
            else if (e.type == SDL_KEYDOWN) {
                switch (e.key.keysym.sym) {
                    case SDLK_ESCAPE: running = 0; break;
                    case SDLK_SPACE: paused = !paused; break;
                    case SDLK_1: speed = 1.0f; break;
                    case SDLK_2: speed = 5.0f; break;
                    case SDLK_3: speed = 20.0f; break;
                }
            } else if (e.type == SDL_MOUSEWHEEL) {
                zoom += e.wheel.y * 0.1f;
                if (zoom < 0.2f) zoom = 0.2f;
                if (zoom > 4.0f) zoom = 4.0f;
            } else if (e.type == SDL_MOUSEMOTION && (e.motion.state & SDL_BUTTON_LMASK)) {
                tx -= e.motion.xrel / zoom;
                ty -= e.motion.yrel / zoom;
            } else if (e.type == SDL_WINDOWEVENT &&
                       e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                screenW = e.window.data1; screenH = e.window.data2;
            }
        }

        Uint64 now = SDL_GetPerformanceCounter();
        double dt = (now - prev) / freq; prev = now;
        facc += dt; frames++;
        if (facc >= 0.5) { fps = (int)(frames / facc); frames = 0; facc = 0.0; }

        if (!paused) {
            double sdt = dt * speed;
            if (sdt > 0.0) world_tick(w, sdt);
        }

        SDL_SetRenderDrawColor(ren, 18, 16, 22, 255);
        SDL_RenderClear(ren);

        float offx = screenW * 0.5f, offy = screenH * 0.5f;
        int ts = (int)(TILE_PX * zoom); if (ts < 1) ts = 1;
        for (int x = 0; x < WORLD_W; x++) {
            for (int y = 0; y < WORLD_H; y++) {
                unsigned char r, g, b;
                tile_rgb((TileType)w->tile[x][y], &r, &g, &b);
                SDL_Rect rc = {
                    (int)((x * TILE_PX - tx) * zoom + offx),
                    (int)((y * TILE_PX - ty) * zoom + offy), ts, ts };
                SDL_SetRenderDrawColor(ren, r, g, b, 255);
                SDL_RenderFillRect(ren, &rc);
            }
        }
        for (int i = 0; i < w->n_agents; i++) {
            Agent *a = &w->agents[i];
            if (!a->alive) continue;
            SDL_Rect rc = {
                (int)((a->x * TILE_PX - tx) * zoom + offx) + 1,
                (int)((a->y * TILE_PX - ty) * zoom + offy) + 1,
                ts > 2 ? ts - 2 : 1, ts > 2 ? ts - 2 : 1 };
            SDL_SetRenderDrawColor(ren, a->r, a->g, a->b, 255);
            SDL_RenderFillRect(ren, &rc);
        }

        /* HUD */
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 170);
        SDL_Rect bar = {0, 0, screenW, 24};
        SDL_RenderFillRect(ren, &bar);
        if (font) {
            char hud[256];
            hud_string(w, hud, sizeof(hud), speed, paused, fps, "sdl2");
            SDL_Color col = {230, 225, 215, 255};
            SDL_Surface *s = TTF_RenderUTF8_Blended(font, hud, col);
            if (s) {
                SDL_Texture *t = SDL_CreateTextureFromSurface(ren, s);
                SDL_Rect dst = {8, 4, s->w, s->h};
                SDL_RenderCopy(ren, t, NULL, &dst);
                SDL_DestroyTexture(t);
                SDL_FreeSurface(s);
            }
        }
        SDL_RenderPresent(ren);

        if (shot && ++shot_frame == 120) {
            SDL_Surface *cap = SDL_CreateRGBSurfaceWithFormat(
                0, screenW, screenH, 32, SDL_PIXELFORMAT_ARGB8888);
            if (cap && SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888,
                                            cap->pixels, cap->pitch) == 0) {
                SDL_SaveBMP(cap, shot);
            }
            if (cap) SDL_FreeSurface(cap);
            running = 0;
        }
    }

    if (font) TTF_CloseFont(font);
    TTF_Quit();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
