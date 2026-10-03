/* gfx_raylib.c — GfxBackend implemented with raylib. */
#include "gfx.h"
#include "viz.h"      /* FONT_PATH */
#include "raylib.h"
#include <stdlib.h>

static Color C(GfxColor c){ return (Color){c.r,c.g,c.b,c.a}; }

/* UI font: an antialiased TTF (DejaVu Sans) is far more legible for the small
 * HUD/panel text than raylib's built-in pixel font. Loaded at a high base size
 * and bilinear-filtered so it stays crisp when drawn at 10-16px. Falls back to
 * the default font if the TTF can't be loaded. */
static Font g_font;
static int  g_font_ok = 0;
#define TEXT_SPACING 1.0f

static int rl_key(int k){
    switch(k){
        case GFX_KEY_SPACE:return KEY_SPACE; case GFX_KEY_TAB:return KEY_TAB; case GFX_KEY_ESC:return KEY_ESCAPE;
        case GFX_KEY_UP:return KEY_UP; case GFX_KEY_DOWN:return KEY_DOWN;
        case GFX_KEY_LEFT:return KEY_LEFT; case GFX_KEY_RIGHT:return KEY_RIGHT;
        case GFX_KEY_PGUP:return KEY_PAGE_UP; case GFX_KEY_PGDN:return KEY_PAGE_DOWN;
        case GFX_KEY_G:return KEY_G; case GFX_KEY_J:return KEY_J; case GFX_KEY_F:return KEY_F; case GFX_KEY_L:return KEY_L;
        case GFX_KEY_A:return KEY_A; case GFX_KEY_Q:return KEY_Q; case GFX_KEY_C:return KEY_C;
        case GFX_KEY_O:return KEY_O; case GFX_KEY_E:return KEY_E; case GFX_KEY_W:return KEY_W; case GFX_KEY_T:return KEY_T;
        default: if(k>=GFX_KEY_1 && k<=GFX_KEY_9) return KEY_ONE+(k-GFX_KEY_1); return 0;
    }
}

static int  rl_init(const char *title,int w,int h){
    int bench = getenv("CSIM_BENCH") != NULL;
    /* Frame pacing:
     *   default           -> vsync on, no target cap: runs at the monitor refresh
     *                        (60/120/144...). No tearing; never exceeds the display.
     *   CSIM_FPS=N (N>0)   -> hard cap at N, vsync off (can exceed refresh; may tear).
     *   CSIM_FPS=0         -> fully uncapped, vsync off.
     *   CSIM_BENCH         -> uncapped (benchmark). */
    const char *fpsenv = getenv("CSIM_FPS");
    int custom = fpsenv ? atoi(fpsenv) : -1;     /* -1 = default (vsync-paced) */
    unsigned flags = FLAG_WINDOW_RESIZABLE;
    if(!bench && custom < 0) flags |= FLAG_VSYNC_HINT;
    SetConfigFlags(flags);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(w,h,title);
    SetTargetFPS(bench ? 0 : (custom < 0 ? 0 : custom));   /* 0 = no internal cap */
    if(!IsWindowReady()) return 1;
    /* on a hi-dpi monitor, open larger so the (scaled) UI has room */
    if(!bench){
        int mw=GetMonitorWidth(GetCurrentMonitor()), mh=GetMonitorHeight(GetCurrentMonitor());
        if(mh>1600){ int nw=(int)(mw*0.75f), nh=(int)(mh*0.75f);
            SetWindowSize(nw,nh); SetWindowPosition((mw-nw)/2,(mh-nh)/2); }
    }
    /* UI font: 64px atlas (crisp when scaled). Load ASCII plus the CP437-style
     * glyphs the ASCII render mode uses (≈ ♣ ⌂ ☺ ☻). */
    int cps[128], n=0;
    for(int c=32;c<=126;c++) cps[n++]=c;
    cps[n++]=0x2248; cps[n++]=0x2663; cps[n++]=0x2302; cps[n++]=0x263A; cps[n++]=0x263B;
    g_font = LoadFontEx(FONT_PATH, 64, cps, n);
    if(g_font.texture.id != 0){ SetTextureFilter(g_font.texture, TEXTURE_FILTER_BILINEAR); g_font_ok = 1; }
    return 0;
}
static void rl_shutdown(void){ if(g_font_ok) UnloadFont(g_font); CloseWindow(); }
static int  rl_should_close(void){ return WindowShouldClose(); }
static void rl_poll(void){}
static void rl_begin(GfxColor clear){ BeginDrawing(); ClearBackground(C(clear)); }
static void rl_present(void){ EndDrawing(); }
static int  rl_width(void){ return GetScreenWidth(); }
static int  rl_height(void){ return GetScreenHeight(); }

static void rl_fill_rect(int x,int y,int w,int h,GfxColor c){ DrawRectangle(x,y,w,h,C(c)); }
static void rl_rect_lines(int x,int y,int w,int h,GfxColor c){ DrawRectangleLines(x,y,w,h,C(c)); }
static void rl_line(int x1,int y1,int x2,int y2,GfxColor c){ DrawLine(x1,y1,x2,y2,C(c)); }
static void rl_circle(int cx,int cy,float r,GfxColor c){ DrawCircle(cx,cy,r,C(c)); }
static void rl_text(const char *s,int x,int y,int size,GfxColor c){
    if(g_font_ok) DrawTextEx(g_font, s, (Vector2){(float)x,(float)y}, (float)size, TEXT_SPACING, C(c));
    else DrawText(s,x,y,size,C(c));
}
static int  rl_text_w(const char *s,int size){
    if(g_font_ok) return (int)MeasureTextEx(g_font, s, (float)size, TEXT_SPACING).x;
    return MeasureText(s,size);
}

static int  rl_key_pressed(int k){ return IsKeyPressed(rl_key(k)); }
static int  rl_key_down(int k){ return IsKeyDown(rl_key(k)); }
static void rl_mouse(int *x,int *y){ Vector2 p=GetMousePosition(); *x=(int)p.x; *y=(int)p.y; }
static int  rl_mouse_pressed(int b){ return IsMouseButtonPressed(b==GFX_MBTN_RIGHT?MOUSE_BUTTON_RIGHT:MOUSE_BUTTON_LEFT); }
static int  rl_mouse_down(int b){ return IsMouseButtonDown(b==GFX_MBTN_RIGHT?MOUSE_BUTTON_RIGHT:MOUSE_BUTTON_LEFT); }
static float rl_wheel(void){ return GetMouseWheelMove(); }
static void rl_screenshot(const char *path){ TakeScreenshot(path); }
/* Suggested UI scale from the monitor height (1080p -> 1.0, 4K -> 2.0). */
static float rl_ui_scale(void){
    int h = GetMonitorHeight(GetCurrentMonitor());
    if(h<=0) return 1.0f;
    float s = h/1080.0f; if(s<1.0f) s=1.0f; if(s>2.5f) s=2.5f; return s;
}
static int rl_focused(void){ return IsWindowFocused(); }

static void *rl_load_tex(const char *path, int *w, int *h){
    Texture2D t = LoadTexture(path);
    if(t.id == 0) return NULL;
    SetTextureFilter(t, TEXTURE_FILTER_POINT);   /* crisp pixel-art scaling */
    Texture2D *p = malloc(sizeof(Texture2D)); if(!p){ UnloadTexture(t); return NULL; }
    *p = t; if(w)*w=t.width; if(h)*h=t.height; return p;
}
static void rl_draw_tex(void *tex,int sx,int sy,int sw,int sh,int dx,int dy,int dw,int dh,GfxColor c){
    if(!tex) return; Texture2D *p=(Texture2D*)tex;
    Rectangle src={(float)sx,(float)sy,(float)sw,(float)sh}, dst={(float)dx,(float)dy,(float)dw,(float)dh};
    DrawTexturePro(*p, src, dst, (Vector2){0,0}, 0.0f, C(c));
}
static void rl_free_tex(void *tex){ if(tex){ Texture2D *p=(Texture2D*)tex; UnloadTexture(*p); free(p); } }

const GfxBackend *gfx_raylib(void){
    static const GfxBackend b = {
        "raylib", rl_init, rl_shutdown, rl_should_close, rl_poll, rl_begin, rl_present,
        rl_width, rl_height, rl_fill_rect, rl_rect_lines, rl_line, rl_circle, rl_text, rl_text_w,
        rl_key_pressed, rl_key_down, rl_mouse, rl_mouse_pressed, rl_mouse_down, rl_wheel, rl_screenshot,
        rl_ui_scale, rl_focused, rl_load_tex, rl_draw_tex, rl_free_tex,
    };
    return &b;
}
