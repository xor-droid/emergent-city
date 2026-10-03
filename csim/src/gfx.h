/* gfx.h — tiny immediate-mode drawing + input abstraction.
 *
 * The whole UI (src/ui.c) is written ONCE against this interface. raylib is the
 * shipped backend (gfx_raylib.c); the abstraction stays so another backend
 * (SDL3, GLFW, ...) could be re-added just by providing a GfxBackend vtable.
 * main.c sets the global G and calls run_ui().
 *
 * Coordinates are screen-space pixels, origin top-left, y down. Text is drawn
 * from its top-left at the given pixel height.
 */
#ifndef GFX_H
#define GFX_H

#include "sim.h"

typedef struct { unsigned char r, g, b, a; } GfxColor;
static inline GfxColor gfx_rgb(unsigned char r, unsigned char g, unsigned char b) {
    GfxColor c = { r, g, b, 255 }; return c;
}
static inline GfxColor gfx_rgba(unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
    GfxColor c = { r, g, b, a }; return c;
}

/* Keys the UI uses. GFX_KEY_1..GFX_KEY_9 are contiguous (index with +k). */
enum {
    GFX_KEY_SPACE, GFX_KEY_TAB, GFX_KEY_ESC,
    GFX_KEY_UP, GFX_KEY_DOWN, GFX_KEY_LEFT, GFX_KEY_RIGHT,
    GFX_KEY_PGUP, GFX_KEY_PGDN,
    GFX_KEY_G, GFX_KEY_J, GFX_KEY_F, GFX_KEY_L,
    GFX_KEY_1, GFX_KEY_2, GFX_KEY_3, GFX_KEY_4, GFX_KEY_5,
    GFX_KEY_6, GFX_KEY_7, GFX_KEY_8, GFX_KEY_9,
    GFX_KEY_A, GFX_KEY_Q,
    GFX_KEY__COUNT
};
enum { GFX_MBTN_LEFT = 0, GFX_MBTN_RIGHT = 1 };

typedef struct GfxBackend {
    const char *name;
    int   (*init)(const char *title, int w, int h);
    void  (*shutdown)(void);
    int   (*should_close)(void);
    void  (*poll)(void);                 /* pump events, refresh input snapshot */
    void  (*begin)(GfxColor clear);      /* start frame, clear */
    void  (*present)(void);              /* finish + display frame */
    int   (*width)(void);
    int   (*height)(void);
    /* drawing */
    void  (*fill_rect)(int x, int y, int w, int h, GfxColor c);
    void  (*rect_lines)(int x, int y, int w, int h, GfxColor c);
    void  (*line)(int x1, int y1, int x2, int y2, GfxColor c);
    void  (*circle)(int cx, int cy, float r, GfxColor c);
    void  (*text)(const char *s, int x, int y, int size, GfxColor c);
    int   (*text_w)(const char *s, int size);
    /* input (valid after poll()) */
    int   (*key_pressed)(int key);       /* edge: true the frame it goes down */
    int   (*key_down)(int key);          /* held */
    void  (*mouse)(int *x, int *y);
    int   (*mouse_pressed)(int btn);     /* edge */
    int   (*mouse_down)(int btn);        /* held */
    float (*wheel)(void);                /* this frame's wheel delta */
    void  (*screenshot)(const char *path);
    float (*ui_scale)(void);             /* suggested UI scale from monitor size (may be NULL) */
    int   (*focused)(void);              /* 1 if the window has focus (may be NULL -> assume yes) */
    /* Tileset support (for ASCII/tile render). All may be NULL. */
    void *(*load_tex)(const char *path, int *w, int *h);  /* NULL on failure */
    void  (*draw_tex)(void *tex, int sx,int sy,int sw,int sh,  /* src rect in the sheet */
                      int dx,int dy,int dw,int dh,            /* dest rect on screen */
                      GfxColor tint);
    void  (*free_tex)(void *tex);
} GfxBackend;

/* The active backend, set by main.c; the UI (ui.c) calls through it. */
extern const GfxBackend *G;

/* Backend factory. */
const GfxBackend *gfx_raylib(void);

/* The shared UI/run loop (uses G). Returns 0 on clean exit. */
int run_ui(World *w);

#endif /* GFX_H */
