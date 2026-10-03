/* gfx_raylib.c — GfxBackend implemented with raylib. */
#include "gfx.h"
#include "raylib.h"

static Color C(GfxColor c){ return (Color){c.r,c.g,c.b,c.a}; }

static int rl_key(int k){
    switch(k){
        case GFX_KEY_SPACE:return KEY_SPACE; case GFX_KEY_TAB:return KEY_TAB; case GFX_KEY_ESC:return KEY_ESCAPE;
        case GFX_KEY_UP:return KEY_UP; case GFX_KEY_DOWN:return KEY_DOWN;
        case GFX_KEY_LEFT:return KEY_LEFT; case GFX_KEY_RIGHT:return KEY_RIGHT;
        case GFX_KEY_PGUP:return KEY_PAGE_UP; case GFX_KEY_PGDN:return KEY_PAGE_DOWN;
        case GFX_KEY_G:return KEY_G; case GFX_KEY_J:return KEY_J; case GFX_KEY_F:return KEY_F; case GFX_KEY_L:return KEY_L;
        default: if(k>=GFX_KEY_1 && k<=GFX_KEY_9) return KEY_ONE+(k-GFX_KEY_1); return 0;
    }
}

static int  rl_init(const char *title,int w,int h){
    SetConfigFlags(FLAG_WINDOW_RESIZABLE|FLAG_VSYNC_HINT);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(w,h,title); SetTargetFPS(60);
    return IsWindowReady()?0:1;
}
static void rl_shutdown(void){ CloseWindow(); }
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
static void rl_text(const char *s,int x,int y,int size,GfxColor c){ DrawText(s,x,y,size,C(c)); }
static int  rl_text_w(const char *s,int size){ return MeasureText(s,size); }

static int  rl_key_pressed(int k){ return IsKeyPressed(rl_key(k)); }
static int  rl_key_down(int k){ return IsKeyDown(rl_key(k)); }
static void rl_mouse(int *x,int *y){ Vector2 p=GetMousePosition(); *x=(int)p.x; *y=(int)p.y; }
static int  rl_mouse_pressed(int b){ return IsMouseButtonPressed(b==GFX_MBTN_RIGHT?MOUSE_BUTTON_RIGHT:MOUSE_BUTTON_LEFT); }
static int  rl_mouse_down(int b){ return IsMouseButtonDown(b==GFX_MBTN_RIGHT?MOUSE_BUTTON_RIGHT:MOUSE_BUTTON_LEFT); }
static float rl_wheel(void){ return GetMouseWheelMove(); }
static void rl_screenshot(const char *path){ TakeScreenshot(path); }

const GfxBackend *gfx_raylib(void){
    static const GfxBackend b = {
        "raylib", rl_init, rl_shutdown, rl_should_close, rl_poll, rl_begin, rl_present,
        rl_width, rl_height, rl_fill_rect, rl_rect_lines, rl_line, rl_circle, rl_text, rl_text_w,
        rl_key_pressed, rl_key_down, rl_mouse, rl_mouse_pressed, rl_mouse_down, rl_wheel, rl_screenshot,
    };
    return &b;
}
