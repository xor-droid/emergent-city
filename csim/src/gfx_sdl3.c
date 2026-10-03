/* gfx_sdl3.c — GfxBackend implemented with SDL3 (SDL_Renderer + debug text). */
#include "gfx.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "stb_image_write.h"

static SDL_Window   *win = NULL;
static SDL_Renderer *ren = NULL;
static int   down[GFX_KEY__COUNT], pressed[GFX_KEY__COUNT];
static int   mdown[2], mpressed[2];
static float wheel_acc = 0;
static int   quit = 0;

static int sc_to_gfx(SDL_Scancode s){
    switch(s){
        case SDL_SCANCODE_SPACE:return GFX_KEY_SPACE; case SDL_SCANCODE_TAB:return GFX_KEY_TAB;
        case SDL_SCANCODE_ESCAPE:return GFX_KEY_ESC;
        case SDL_SCANCODE_UP:return GFX_KEY_UP; case SDL_SCANCODE_DOWN:return GFX_KEY_DOWN;
        case SDL_SCANCODE_LEFT:return GFX_KEY_LEFT; case SDL_SCANCODE_RIGHT:return GFX_KEY_RIGHT;
        case SDL_SCANCODE_PAGEUP:return GFX_KEY_PGUP; case SDL_SCANCODE_PAGEDOWN:return GFX_KEY_PGDN;
        case SDL_SCANCODE_G:return GFX_KEY_G; case SDL_SCANCODE_J:return GFX_KEY_J;
        case SDL_SCANCODE_F:return GFX_KEY_F; case SDL_SCANCODE_L:return GFX_KEY_L;
        default:
            if(s>=SDL_SCANCODE_1 && s<=SDL_SCANCODE_9) return GFX_KEY_1+(s-SDL_SCANCODE_1);
            return -1;
    }
}

static int sd_init(const char *title,int w,int h){
    if(!SDL_Init(SDL_INIT_VIDEO)){ SDL_Log("SDL_Init: %s", SDL_GetError()); return 1; }
    win = SDL_CreateWindow(title, w, h, SDL_WINDOW_RESIZABLE);
    if(!win){ SDL_Log("CreateWindow: %s", SDL_GetError()); return 1; }
    ren = SDL_CreateRenderer(win, NULL);
    if(!ren){ SDL_Log("CreateRenderer: %s", SDL_GetError()); return 1; }
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    if(!getenv("CSIM_BENCH")) SDL_SetRenderVSync(ren, 1);  /* cap to refresh; uncapped for benchmarks */
    return 0;
}
static void sd_shutdown(void){ if(ren)SDL_DestroyRenderer(ren); if(win)SDL_DestroyWindow(win); SDL_Quit(); }
static int  sd_should_close(void){ return quit; }

static void sd_poll(void){
    memset(pressed,0,sizeof(pressed)); mpressed[0]=mpressed[1]=0; wheel_acc=0;
    SDL_Event e;
    while(SDL_PollEvent(&e)){
        if(e.type==SDL_EVENT_QUIT) quit=1;
        else if(e.type==SDL_EVENT_KEY_DOWN){ int k=sc_to_gfx(e.key.scancode);
            if(k>=0){ if(!e.key.repeat && !down[k]) pressed[k]=1; down[k]=1; } }
        else if(e.type==SDL_EVENT_KEY_UP){ int k=sc_to_gfx(e.key.scancode); if(k>=0) down[k]=0; }
        else if(e.type==SDL_EVENT_MOUSE_BUTTON_DOWN){ int b=(e.button.button==SDL_BUTTON_RIGHT)?1:0; mpressed[b]=1; mdown[b]=1; }
        else if(e.type==SDL_EVENT_MOUSE_BUTTON_UP){ int b=(e.button.button==SDL_BUTTON_RIGHT)?1:0; mdown[b]=0; }
        else if(e.type==SDL_EVENT_MOUSE_WHEEL) wheel_acc+=e.wheel.y;
    }
}

static void setcol(GfxColor c){ SDL_SetRenderDrawColor(ren,c.r,c.g,c.b,c.a); }
static void sd_begin(GfxColor clear){ setcol(clear); SDL_RenderClear(ren); }
static void sd_present(void){ SDL_RenderPresent(ren); }
static int  sd_width(void){ int w,h; SDL_GetCurrentRenderOutputSize(ren,&w,&h); return w; }
static int  sd_height(void){ int w,h; SDL_GetCurrentRenderOutputSize(ren,&w,&h); return h; }

static void sd_fill_rect(int x,int y,int w,int h,GfxColor c){ setcol(c); SDL_FRect r={(float)x,(float)y,(float)w,(float)h}; SDL_RenderFillRect(ren,&r); }
static void sd_rect_lines(int x,int y,int w,int h,GfxColor c){ setcol(c); SDL_FRect r={(float)x,(float)y,(float)w,(float)h}; SDL_RenderRect(ren,&r); }
static void sd_line(int x1,int y1,int x2,int y2,GfxColor c){ setcol(c); SDL_RenderLine(ren,(float)x1,(float)y1,(float)x2,(float)y2); }
static void sd_circle(int cx,int cy,float r,GfxColor c){ setcol(c); int ri=(int)(r+0.5f); if(ri<1)ri=1;
    for(int dy=-ri;dy<=ri;dy++){ int dx=(int)(sqrtf((float)(ri*ri-dy*dy))+0.5f);
        SDL_FRect row={(float)(cx-dx),(float)(cy+dy),(float)(2*dx+1),1.0f}; SDL_RenderFillRect(ren,&row); } }

/* SDL debug font is a fixed 8px cell; scale ~size/16 to roughly match raylib. */
static float tscale(int size){ float s=size/16.0f; if(s<0.4f)s=0.4f; return s; }
static void sd_text(const char *str,int x,int y,int size,GfxColor c){ float s=tscale(size);
    SDL_SetRenderScale(ren,s,s); setcol(c); SDL_RenderDebugText(ren, x/s, y/s, str); SDL_SetRenderScale(ren,1.0f,1.0f); }
static int  sd_text_w(const char *str,int size){ return (int)(strlen(str)*8*tscale(size)); }

static int  sd_key_pressed(int k){ return (k>=0&&k<GFX_KEY__COUNT)?pressed[k]:0; }
static int  sd_key_down(int k){ return (k>=0&&k<GFX_KEY__COUNT)?down[k]:0; }
static void sd_mouse(int *x,int *y){ float fx,fy; SDL_GetMouseState(&fx,&fy); *x=(int)fx; *y=(int)fy; }
static int  sd_mouse_pressed(int b){ return mpressed[b==GFX_MBTN_RIGHT?1:0]; }
static int  sd_mouse_down(int b){ return mdown[b==GFX_MBTN_RIGHT?1:0]; }
static float sd_wheel(void){ return wheel_acc; }

static void sd_screenshot(const char *path){
    SDL_Surface *s = SDL_RenderReadPixels(ren, NULL); if(!s) return;
    SDL_Surface *c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ABGR8888); /* R,G,B,A byte order on LE */
    SDL_DestroySurface(s);
    if(c){ stbi_write_png(path, c->w, c->h, 4, c->pixels, c->pitch); SDL_DestroySurface(c); }
}

const GfxBackend *gfx_sdl3(void){
    static const GfxBackend b = {
        "sdl3", sd_init, sd_shutdown, sd_should_close, sd_poll, sd_begin, sd_present,
        sd_width, sd_height, sd_fill_rect, sd_rect_lines, sd_line, sd_circle, sd_text, sd_text_w,
        sd_key_pressed, sd_key_down, sd_mouse, sd_mouse_pressed, sd_mouse_down, sd_wheel, sd_screenshot,
    };
    return &b;
}
