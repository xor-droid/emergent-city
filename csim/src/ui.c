/* ui.c — the UI/run loop, written against the gfx.h interface (G->...).
 *
 * Camera transform and frame timing are done here (backend-neutral). All UI
 * chrome (HUD, panels, god mode, legend) is scaled by a global UI scale so it
 * stays readable on hi-dpi/4K displays — see US() and g_uiscale.
 */
#include "gfx.h"
#include "viz.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

const GfxBackend *G = NULL;

#define PANEL_W 320
#define FEED_MAX 60      /* upper bound on event-feed lines (actual count fills to ~mid-screen) */
#define ROW_H 19

/* named colors */
#define COL_WHITE   gfx_rgb(255,255,255)
#define COL_GRAY    gfx_rgb(130,130,130)
#define COL_GOLD    gfx_rgb(212,175,90)
#define COL_PURPLE  gfx_rgb(200,120,220)
#define COL_RED     gfx_rgb(230,90,80)
#define COL_AMBER   gfx_rgb(220,180,90)
#define COL_GREEN   gfx_rgb(120,220,130)
#define COL_PANEL   gfx_rgba(24,22,30,235)

/* Global UI scale: all chrome pixel sizes/offsets and font sizes pass through
 * US(). Default comes from the monitor (G->ui_scale), overridable with CSIM_UI. */
static float g_uiscale = 1.0f;
static int US(int v){ return (int)(v*g_uiscale + 0.5f); }

static unsigned char clampb(int v){ return (unsigned char)(v<0?0:v>255?255:v); }
static GfxColor tile_col(TileType t){ unsigned char r,g,b; tile_rgb(t,&r,&g,&b); return gfx_rgb(r,g,b); }
static GfxColor shade(GfxColor c,int d){ return gfx_rgba(clampb(c.r+d),clampb(c.g+d),clampb(c.b+d),c.a); }

static double now_sec(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return ts.tv_sec+ts.tv_nsec*1e-9; }

static char building_glyph(TileType t){
    switch(t){ case T_HOME:return 'H'; case T_SHOP:return '$'; case T_WORK:return 'O';
               case T_BAR:return 'B'; case T_CHURCH:return '+'; case T_POLICE:return 'P';
               default:return 0; }
}
/* ASCII (Dwarf-Fortress) glyph per tile — CP437-style (UTF-8). The extended
 * codepoints (≈ ♣ ⌂) must be in the font atlas: see gfx_raylib.c LoadFontEx. */
static const char *ascii_glyph(TileType t){
    switch(t){
        case T_GRASS:return ",";       case T_ROAD:return ".";
        case T_WATER:return "≈"; /* ≈ */ case T_PARK:return "♣"; /* ♣ */
        case T_HOME:return "⌂";  /* ⌂ */ case T_SHOP:return "$"; case T_WORK:return "O";
        case T_BAR:return "B"; case T_CHURCH:return "+"; case T_POLICE:return "P";
        default:return ".";
    }
}
/* ---- Optional image tilesets for ASCII/tile mode (--tileset / CSIM_TILESET) ----
 * Assets are NOT bundled (licensing); each name resolves to a file under the
 * tileset dir (CSIM_TILESET_DIR, default "tilesets"). If missing we print where
 * to get it and fall back to font glyphs. A path ending in .png is used directly
 * (treated as a CP437 sheet). Geometry is per-set: cell px (default 16), gap
 * between tiles, and outer margin — override with CSIM_TILESET_CELL / _SPACE /
 * _MARGIN (handles spaced sheets like Kenney's). */
enum { TSK_CP437, TSK_SEMANTIC };
enum { SEM_KENNEY, SEM_CANON };   /* semantic layout: raw Kenney sheet vs our canonical composite */
/* cell = tile px; space = gap between tiles; margin = outer border px. */
typedef struct { const char *name; int kind, sem; const char *file; int cell, space, margin; const char *url; const char *license; } TsEntry;
static const TsEntry TSETS[] = {
  {"camashu",       TSK_CP437,   0,         "camashu.png",          16,0,0, "https://github.com/Camashu/DorfFortressTileSet",                "CC0"},
  {"curses",        TSK_CP437,   0,         "curses.png",           16,0,0, "https://dwarffortresswiki.org/Tileset_repository",              "varies-verify"},
  {"phoebus",       TSK_CP437,   0,         "phoebus.png",          16,0,0, "https://dwarffortresswiki.org/Tileset_repository",              "varies-verify"},
  {"anikki",        TSK_CP437,   0,         "anikki.png",           16,0,0, "https://dwarffortresswiki.org/Tileset_repository",              "varies-verify"},
  {"dawnlike",      TSK_SEMANTIC,SEM_CANON, "dawnlike.png",         16,0,0, "https://opengameart.org/content/dawnlike-16x16-universal-rogue-like-tileset-v181", "CC-BY-4.0"},
  {"kenney",        TSK_SEMANTIC,SEM_KENNEY,"kenney_roguelike.png", 16,1,0, "https://opengameart.org/content/roguelikerpg-pack-1700-tiles",  "CC0"},
  {"kenney-indoor", TSK_SEMANTIC,SEM_KENNEY,"kenney_indoor.png",    16,1,0, "https://opengameart.org/content/roguelike-indoor-pack",         "CC0"},
  {"kenney-caves",  TSK_SEMANTIC,SEM_KENNEY,"kenney_caves.png",     16,1,0, "https://opengameart.org/content/roguelike-caves-dungeons-pack", "CC0"},
  {"kenney-1bit",   TSK_SEMANTIC,SEM_KENNEY,"kenney_1bit.png",      16,1,0, "https://opengameart.org/content/1-bit-pack",                    "CC0"},
};
static const int N_TSETS = (int)(sizeof(TSETS)/sizeof(TSETS[0]));

/* code-page-437 index for a tile (into a 16-wide CP437 sheet) */
static int cp437_for(TileType t){
    switch(t){ case T_GRASS:return ','; case T_ROAD:return '.';
        case T_WATER:return 247 /*≈*/; case T_PARK:return 5 /*♣*/; case T_HOME:return 127 /*⌂*/;
        case T_SHOP:return '$'; case T_WORK:return 'O'; case T_BAR:return 'B';
        case T_CHURCH:return '+'; case T_POLICE:return 'P'; default:return '.'; }
}
/* Semantic sprite cell (col,row). SEM_CANON = our composite atlas, one column
 * per type in a fixed order (built by tools/fetch-tilesets.sh from DawnLike).
 * SEM_KENNEY = best-effort cells in the raw Kenney roguelike sheet (approximate:
 * environment sheets lack per-type/person tiles). col<0 = skip (bg only). */
static void semantic_cell(int sem, TileType t, int *col, int *row){
    if(sem==SEM_CANON){ *row=0;
        switch(t){ case T_HOME:*col=0;break; case T_SHOP:*col=1;break; case T_WORK:*col=2;break;
            case T_BAR:*col=3;break; case T_CHURCH:*col=4;break; case T_POLICE:*col=5;break;
            case T_PARK:*col=6;break; case T_WATER:*col=7;break; case T_ROAD:*col=8;break;
            default:*col=10;break; /* grass tile */ }
        return;
    }
    switch(t){
        case T_HOME:  *col=13; *row=2; break;   /* door */
        case T_SHOP:  *col=20; *row=1; break;
        case T_WORK:  *col=41; *row=1; break;   /* windowed facade */
        case T_BAR:   *col=24; *row=2; break;
        case T_CHURCH:*col=46; *row=1; break;   /* spire */
        case T_POLICE:*col=30; *row=2; break;
        case T_PARK:  *col=13; *row=8; break;   /* tree */
        case T_WATER: *col=3;  *row=0; break;
        case T_ROAD:  *col=2;  *row=2; break;
        default:      *col=-1; *row=0; break;   /* grass -> tinted background only */
    }
}

static const char *faction_name(World *w,int fid){ return (fid<0||fid>=w->n_factions)?"-":w->factions[fid].name; }
static const char *agent_tag(const Agent *a){
    if(a->arrested_ticks>0) return "JAIL";
    if(a->wanted) return "WANTED";
    if(a->is_police) return "police";
    if(a->faction_id!=-1) return "faction";
    return "";
}

/* camera: screen = (world - target)*zoom + offset(=screen centre) */
typedef struct { float tx,ty,ox,oy,zoom; } Cam;
static void w2s(const Cam*c,float wx,float wy,float*sx,float*sy){ *sx=(wx-c->tx)*c->zoom+c->ox; *sy=(wy-c->ty)*c->zoom+c->oy; }
static void s2w(const Cam*c,float sx,float sy,float*wx,float*wy){ *wx=(sx-c->ox)/c->zoom+c->tx; *wy=(sy-c->oy)/c->zoom+c->ty; }

int run_ui(World *w){
    if(!G) return 1;
    if(G->init("Emergent City (C)", 1280, 800)) return 1;

    /* UI scale: monitor-derived default, CSIM_UI overrides. */
    g_uiscale = (G->ui_scale ? G->ui_scale() : 1.0f);
    { const char *e=getenv("CSIM_UI"); if(e){ float v=(float)atof(e); if(v>0) g_uiscale=v; } }
    if(g_uiscale<0.8f) g_uiscale=0.8f; if(g_uiscale>3.0f) g_uiscale=3.0f;

    Cam cam; cam.zoom=1.0f; cam.tx=WORLD_W*TILE_PX*0.5f; cam.ty=WORLD_H*TILE_PX*0.5f;
    { const char *ez=getenv("CSIM_ZOOM"); if(ez){ float z=(float)atof(ez); if(z>0) cam.zoom=z; } }

    int paused=0; float speed=1.0f;
    int god=0, tool=G_SMITE;
    int show_right=1, show_jail=0, show_factions=0, show_legend=1;
    int ascii = getenv("CSIM_ASCII") ? 1 : 0;   /* Dwarf-Fortress ASCII render mode (toggle: a) */
    int selected=-1, list_scroll=0;

    /* optional image tileset (--tileset / CSIM_TILESET); falls back to glyphs */
    void *ts_tex=NULL; int ts_kind=TSK_CP437, ts_sem=SEM_KENNEY, ts_cell=16, ts_space=0, ts_margin=0;
    int ts_cols=16, ts_cw=16, ts_ch=16;   /* cols + cell width/height, derived from the image */
    { const char *tsname=getenv("CSIM_TILESET");
      if(tsname && tsname[0] && G->load_tex){
        char path[600]; const TsEntry *ent=NULL; int direct=0;
        if(strchr(tsname,'/')||strstr(tsname,".png")){ snprintf(path,sizeof(path),"%s",tsname); direct=1; }
        else for(int i=0;i<N_TSETS;i++) if(!strcmp(tsname,TSETS[i].name)){ ent=&TSETS[i]; break; }
        const char *dir=getenv("CSIM_TILESET_DIR"); if(!dir) dir="tilesets";
        if(ent){ snprintf(path,sizeof(path),"%s/%s",dir,ent->file); ts_kind=ent->kind; ts_sem=ent->sem;
                 ts_cell=ent->cell; ts_space=ent->space; ts_margin=ent->margin; }
        { const char *e; if((e=getenv("CSIM_TILESET_CELL"))){ int c=atoi(e); if(c>0) ts_cell=c; }
          if((e=getenv("CSIM_TILESET_SPACE"))){ int c=atoi(e); if(c>=0) ts_space=c; }
          if((e=getenv("CSIM_TILESET_MARGIN"))){ int c=atoi(e); if(c>=0) ts_margin=c; } }
        if(!ent && !direct){
            fprintf(stderr,"unknown tileset '%s'. options:",tsname);
            for(int i=0;i<N_TSETS;i++) fprintf(stderr," %s",TSETS[i].name);
            fprintf(stderr,"  (or a path to a .png)\n");
        } else {
            int tw=0,th=0; ts_tex=G->load_tex(path,&tw,&th);
            if(ts_tex){ ascii=1;
                if(ts_kind==TSK_CP437){
                    /* CP437 is always a 16x16 grid — derive the (possibly non-square) native cell. */
                    ts_cols=16;
                    ts_cw=(tw - 2*ts_margin - 15*ts_space)/16; if(ts_cw<1) ts_cw=1;
                    ts_ch=(th - 2*ts_margin - 15*ts_space)/16; if(ts_ch<1) ts_ch=1;
                } else {
                    ts_cw=ts_ch=ts_cell;
                    int stride=ts_cell+ts_space;
                    ts_cols = stride>0 ? (tw - 2*ts_margin + ts_space)/stride : 16; if(ts_cols<1) ts_cols=1;
                }
                fprintf(stderr,"[tileset] %s  img %dx%d  cell %dx%d  space %d  margin %d  cols %d  (%s)\n",
                        path,tw,th,ts_cw,ts_ch,ts_space,ts_margin,ts_cols, ts_kind==TSK_CP437?"cp437":"semantic");
            } else {
                fprintf(stderr,"[tileset] not found: %s — using font glyphs.\n", path);
                if(ent) fprintf(stderr,"          get it (%s): %s\n", ent->license, ent->url);
            }
        }
      }
    }
    char flash[96]=""; double flash_until=0;
    const char *shot=getenv("CSIM_SHOT"); int frame=0;
    if(getenv("CSIM_DEMO")){ god=1; show_jail=1; show_factions=1; }
    /* benchmark mode: uncapped (backends disable vsync when CSIM_BENCH is set),
     * sim paused, measure FPS over N frames after a 60-frame warmup. */
    int bench=0; { const char *bs=getenv("CSIM_BENCH"); if(bs){ bench=atoi(bs); if(bench<1) bench=1; } }
    long fcount=0; double bench_t0=0;

    static int ids[MAX_AGENTS];
    double prev=now_sec(), facc=0; int frames=0, fps=0;
    char buf[128];
    int running=1;

    while(running && !G->should_close()){
        G->poll();
        if(G->key_pressed(GFX_KEY_Q)) { running=0; }
        int W=G->width(), Hs=G->height();
        cam.ox=W*0.5f; cam.oy=Hs*0.5f;
        int panelW=US(PANEL_W), rowh=US(ROW_H);
        int panelX=W-panelW;
        int hudH=US(24);
        int feedLines=(Hs/2-US(50))/US(15);   /* feed fills down to ~mid-screen */
        if(feedLines<4) feedLines=4; if(feedLines>FEED_MAX) feedLines=FEED_MAX;
        int feedBottom=US(28)+feedLines*US(15)+US(6);
        int listTop=feedBottom+US(34);   /* room for the CITIZENS header above the list */
        int listRows=(Hs-listTop-US(6))/rowh; if(listRows<1) listRows=1;

        int nlist=0;
        for(int i=0;i<w->n_agents;i++) if(w->agents[i].alive) ids[nlist++]=w->agents[i].id;
        int maxscroll=nlist-listRows; if(maxscroll<0) maxscroll=0;
        int selIdx=-1; for(int i=0;i<nlist;i++) if(ids[i]==selected){selIdx=i;break;}

        /* ── input ── */
        if(G->key_pressed(GFX_KEY_SPACE)) paused=!paused;
        if(G->key_pressed(GFX_KEY_G)) god=!god;
        if(G->key_pressed(GFX_KEY_TAB)) show_right=!show_right;
        if(G->key_pressed(GFX_KEY_J)) show_jail=!show_jail;
        if(G->key_pressed(GFX_KEY_F)) show_factions=!show_factions;
        if(G->key_pressed(GFX_KEY_L)) show_legend=!show_legend;
        if(G->key_pressed(GFX_KEY_A)) ascii=!ascii;
        for(int k=0;k<9;k++) if(G->key_pressed(GFX_KEY_1+k)){
            if(god){ if(k<G_NTOOLS) tool=k; }
            else if(k==0) speed=1; else if(k==1) speed=5; else if(k==2) speed=20;
        }
        int navstep=0;
        if(G->key_pressed(GFX_KEY_DOWN)) navstep=1;
        if(G->key_pressed(GFX_KEY_UP)) navstep=-1;
        if(G->key_pressed(GFX_KEY_PGDN)) navstep=listRows;
        if(G->key_pressed(GFX_KEY_PGUP)) navstep=-listRows;
        if(navstep && nlist){
            int ni=(selIdx<0?0:selIdx)+navstep;
            if(ni<0)ni=0; if(ni>=nlist)ni=nlist-1;
            selIdx=ni; selected=ids[ni];
            Agent *a=world_agent_by_id(w,selected);
            if(a){ cam.tx=a->x*TILE_PX; cam.ty=a->y*TILE_PX; }
            if(selIdx<list_scroll) list_scroll=selIdx;
            if(selIdx>=list_scroll+listRows) list_scroll=selIdx-listRows+1;
        }
        if(G->key_pressed(GFX_KEY_ESC)) selected=-1;

        int mx,my; G->mouse(&mx,&my);
        int overPanel = show_right && mx>=panelX;
        float wheel=G->wheel();
        if(wheel!=0){
            if(overPanel && my>=listTop){
                list_scroll-=(int)wheel*3;
                if(list_scroll<0)list_scroll=0; if(list_scroll>maxscroll)list_scroll=maxscroll;
            } else {
                /* zoom toward cursor */
                float bx,by; s2w(&cam,mx,my,&bx,&by);
                cam.zoom+=wheel*0.1f; if(cam.zoom<0.2f)cam.zoom=0.2f; if(cam.zoom>4.0f)cam.zoom=4.0f;
                float ax,ay; s2w(&cam,mx,my,&ax,&ay);
                cam.tx+=bx-ax; cam.ty+=by-ay;
            }
        }
        if(G->mouse_down(GFX_MBTN_RIGHT)){
            static int pmx=0,pmy=0; static int dragging=0;
            if(!dragging){ pmx=mx; pmy=my; dragging=1; }
            cam.tx-=(mx-pmx)/cam.zoom; cam.ty-=(my-pmy)/cam.zoom; pmx=mx; pmy=my;
        }
        if(G->mouse_pressed(GFX_MBTN_LEFT)){
            if(overPanel && my>=listTop){
                int row=list_scroll+(my-listTop)/rowh;
                int trackX=W-US(6), trackY=listTop, trackH=listRows*rowh;
                if(nlist>listRows && mx>=trackX-US(2) && my>=trackY && my<=trackY+trackH){
                    float f=(my-trackY)/(float)trackH; list_scroll=(int)(f*maxscroll);
                    if(list_scroll<0)list_scroll=0; if(list_scroll>maxscroll)list_scroll=maxscroll;
                } else if(row>=0 && row<nlist){
                    selected=ids[row];
                    Agent *a=world_agent_by_id(w,selected);
                    if(a){ cam.tx=a->x*TILE_PX; cam.ty=a->y*TILE_PX; }
                }
            } else if(!overPanel){
                float wx,wy; s2w(&cam,mx,my,&wx,&wy);
                int tx=(int)(wx/TILE_PX), ty=(int)(wy/TILE_PX);
                if(god){ god_apply(w,tool,tx,ty,flash,sizeof(flash)); flash_until=now_sec()+2.5; }
                else { Agent *a=world_agent_at(w,tx,ty,5);
                    if(a){ selected=a->id; cam.tx=a->x*TILE_PX; cam.ty=a->y*TILE_PX; } else selected=-1; }
            }
        }

        /* ── advance sim ── */
        double t=now_sec(), dt=t-prev; prev=t; if(dt>0.1) dt=0.1;
        facc+=dt; frames++; if(facc>=0.5){ fps=(int)(frames/facc); frames=0; facc=0; }
        if(!paused && dt>0 && !bench) world_tick(w, (float)(dt*speed));

        /* ── render world ── */
        G->begin(ascii ? gfx_rgb(0,0,0) : gfx_rgb(18,16,22));
        float tl_wx,tl_wy,br_wx,br_wy;
        s2w(&cam,0,0,&tl_wx,&tl_wy); s2w(&cam,(float)W,(float)Hs,&br_wx,&br_wy);
        int x0=(int)(tl_wx/TILE_PX)-1, x1=(int)(br_wx/TILE_PX)+1;
        int y0=(int)(tl_wy/TILE_PX)-1, y1=(int)(br_wy/TILE_PX)+1;
        if(x0<0)x0=0; if(y0<0)y0=0; if(x1>=WORLD_W)x1=WORLD_W-1; if(y1>=WORLD_H)y1=WORLD_H-1;
        float ts=TILE_PX*cam.zoom; int iw=(int)(ts+1.0f); if(iw<1)iw=1;

        if(ascii){
            /* Dwarf-Fortress look: each cell gets a dark tile-tinted background plus
             * a brighter CP437 glyph; agents are ☺ (☻ for police). Drawn in two
             * passes (all backgrounds, then all glyphs) so each batches — mixing
             * fill_rect and text per cell thrashes the renderer's batch. */
            int fs=(int)(ts*0.95f); if(fs<6)fs=6;
            /* pass 1: cell backgrounds (+ selected-agent highlight) */
            for(int x=x0;x<=x1;x++) for(int y=y0;y<=y1;y++){
                TileType t2=(TileType)w->tile[x][y];
                float sx,sy; w2s(&cam,x*TILE_PX,y*TILE_PX,&sx,&sy);
                GfxColor tc=tile_col(t2);
                GfxColor bg=gfx_rgb((unsigned char)(tc.r*0.32f),(unsigned char)(tc.g*0.32f),(unsigned char)(tc.b*0.32f));
                G->fill_rect((int)sx,(int)sy,iw,iw,bg);
            }
            if(selected>=0){ Agent *a=world_agent_by_id(w,selected);
                if(a&&a->alive){ float sx,sy; w2s(&cam,a->x*TILE_PX,a->y*TILE_PX,&sx,&sy);
                    G->fill_rect((int)sx,(int)sy,iw,iw,gfx_rgba(120,110,40,210)); } }
            /* pass 2: either image-tileset cells or font glyphs */
            if(ts_tex){
                for(int x=x0;x<=x1;x++) for(int y=y0;y<=y1;y++){
                    TileType t2=(TileType)w->tile[x][y];
                    int col,row; GfxColor tint;
                    if(ts_kind==TSK_CP437){ int code=cp437_for(t2); col=code%ts_cols; row=code/ts_cols; tint=shade(tile_col(t2),70); }
                    else { semantic_cell(ts_sem,t2,&col,&row); if(col<0) continue; tint=COL_WHITE; }
                    int srcx=ts_margin+col*(ts_cw+ts_space), srcy=ts_margin+row*(ts_ch+ts_space);
                    float sx,sy; w2s(&cam,x*TILE_PX,y*TILE_PX,&sx,&sy);
                    G->draw_tex(ts_tex, srcx,srcy,ts_cw,ts_ch, (int)sx,(int)sy,iw,iw, tint);
                }
                for(int i=0;i<w->n_agents;i++){ Agent *a=&w->agents[i]; if(!a->alive) continue;
                    int col,row; GfxColor tint;
                    if(ts_kind==TSK_CP437){ int code=a->is_police?2:1; col=code%ts_cols; row=code/ts_cols; tint=gfx_rgb(a->r,a->g,a->b); }
                    else if(ts_sem==SEM_CANON){ col=9; row=0; tint=COL_WHITE; }  /* composite person tile */
                    else { col=23; row=8; tint=COL_WHITE; }  /* Kenney: no person sprite — a small marker */
                    int srcx=ts_margin+col*(ts_cw+ts_space), srcy=ts_margin+row*(ts_ch+ts_space);
                    float sx,sy; w2s(&cam,a->x*TILE_PX,a->y*TILE_PX,&sx,&sy);
                    G->draw_tex(ts_tex, srcx,srcy,ts_cw,ts_ch, (int)sx,(int)sy,iw,iw, tint);
                }
            } else {
                for(int x=x0;x<=x1;x++) for(int y=y0;y<=y1;y++){
                    TileType t2=(TileType)w->tile[x][y];
                    const char *s=ascii_glyph(t2); if(!s||!s[0]) continue;
                    float sx,sy; w2s(&cam,x*TILE_PX,y*TILE_PX,&sx,&sy);
                    int tw=G->text_w(s,fs);
                    G->text(s,(int)sx+(iw-tw)/2,(int)sy+(iw-fs)/2,fs,shade(tile_col(t2),70));
                }
                for(int i=0;i<w->n_agents;i++){ Agent *a=&w->agents[i]; if(!a->alive) continue;
                    const char *s = a->is_police ? "☻" : "☺"; /* ☻ police / ☺ citizen */
                    float sx,sy; w2s(&cam,a->x*TILE_PX,a->y*TILE_PX,&sx,&sy);
                    int tw=G->text_w(s,fs);
                    G->text(s,(int)sx+(iw-tw)/2,(int)sy+(iw-fs)/2,fs,gfx_rgb(a->r,a->g,a->b));
                }
            }
        } else {
            GfxColor outline=gfx_rgb(12,10,16);
            int gap=(int)(ts*0.12f); if(gap<1)gap=1;
            int roofh=(int)(ts*0.25f); if(roofh<1)roofh=1;
            for(int x=x0;x<=x1;x++) for(int y=y0;y<=y1;y++){
                TileType t2=(TileType)w->tile[x][y];
                float sx,sy; w2s(&cam,x*TILE_PX,y*TILE_PX,&sx,&sy);
                int ix=(int)sx, iy=(int)sy;
                if(!tile_is_building(t2)){ G->fill_rect(ix,iy,iw,iw,tile_col(t2)); continue; }
                GfxColor body=shade(tile_col(t2), tile_shade_jitter(x,y,14));
                G->fill_rect(ix,iy,iw,iw,outline);
                if(iw>2*gap){ G->fill_rect(ix+gap,iy+gap,iw-2*gap,iw-2*gap,body);
                              G->fill_rect(ix+gap,iy+gap,iw-2*gap,roofh,shade(body,34)); }
            }
            for(int i=0;i<w->n_agents;i++){ Agent *a=&w->agents[i]; if(!a->alive) continue;
                float sx,sy; w2s(&cam,a->x*TILE_PX+TILE_PX*0.5f,a->y*TILE_PX+TILE_PX*0.5f,&sx,&sy);
                float r=ts*0.34f; if(r<1.5f)r=1.5f;
                G->circle((int)sx,(int)sy,r,gfx_rgba(0,0,0,160));
                G->circle((int)sx,(int)sy,r*0.76f,gfx_rgb(a->r,a->g,a->b));
                if(a->id==selected){ float bx,by; w2s(&cam,a->x*TILE_PX,a->y*TILE_PX,&bx,&by);
                    G->rect_lines((int)bx-2,(int)by-2,iw+4,iw+4,gfx_rgb(255,255,0)); }
            }
            /* zoom-aware building glyphs */
            if(ts>=13.0f){
                int fs=(int)(ts*0.72f); if(fs<8)fs=8;
                for(int x=x0;x<=x1;x++) for(int y=y0;y<=y1;y++){
                    char g=building_glyph((TileType)w->tile[x][y]); if(!g) continue;
                    char s[2]={g,0};
                    float sx,sy; w2s(&cam,x*TILE_PX+TILE_PX*0.5f,y*TILE_PX+TILE_PX*0.5f,&sx,&sy);
                    int tw=G->text_w(s,fs);
                    G->text(s,(int)(sx-tw*0.5f),(int)(sy-fs*0.5f),fs,gfx_rgba(18,14,20,230));
                }
            }
        }

        /* ── HUD ── */
        char hud[256]; hud_string(w,hud,sizeof(hud),speed,paused,fps,G->name);
        G->fill_rect(0,0,W,hudH,gfx_rgba(0,0,0,170));
        G->text(hud,US(8),US(5),US(14),gfx_rgb(230,225,215));

        /* ── right panel: feed + citizens list ── */
        if(show_right){
            G->fill_rect(panelX,hudH,panelW,Hs-hudH,gfx_rgba(24,22,30,225));
            G->text("EVENT FEED [Tab]",panelX+US(10),US(30),US(13),COL_GOLD);
            int yy=US(50);
            for(int i=0;i<feedLines && i<w->ev_count;i++){ const WorldEvent *e=events_recent(w,i); if(!e)break;
                snprintf(buf,sizeof(buf),"%.44s",e->text); G->text(buf,panelX+US(10),yy,US(12),gfx_rgb(220,215,205)); yy+=US(15); }
            G->line(panelX+US(8),feedBottom+US(8),W-US(8),feedBottom+US(8),gfx_rgb(80,76,90));
            snprintf(buf,sizeof(buf),"CITIZENS (%d)",nlist); G->text(buf,panelX+US(10),feedBottom+US(14),US(13),COL_GOLD);
            for(int r=0;r<listRows && (list_scroll+r)<nlist;r++){ int idx=list_scroll+r;
                Agent *a=world_agent_by_id(w,ids[idx]); if(!a) continue;
                int ry=listTop+r*rowh;
                if(a->id==selected) G->fill_rect(panelX+US(2),ry,panelW-US(14),rowh,gfx_rgb(60,56,40));
                G->fill_rect(panelX+US(8),ry+US(6),US(7),US(7),gfx_rgb(a->r,a->g,a->b));
                snprintf(buf,sizeof(buf),"%.22s",a->name); G->text(buf,panelX+US(20),ry+US(3),US(14),gfx_rgb(230,225,215));
                const char *tag=agent_tag(a);
                if(tag[0]){ GfxColor tc=a->wanted?COL_RED:a->arrested_ticks>0?COL_AMBER:gfx_rgb(150,145,135);
                    G->text(tag,W-US(12)-G->text_w(tag,US(11)),ry+US(4),US(11),tc); } }
            if(nlist>listRows){ int trackX=W-US(6), trackY=listTop, trackH=listRows*rowh;
                G->fill_rect(trackX,trackY,US(4),trackH,gfx_rgb(40,38,48));
                int thumbH=trackH*listRows/nlist; if(thumbH<US(10))thumbH=US(10);
                int thumbY=trackY+(trackH-thumbH)*list_scroll/(maxscroll>0?maxscroll:1);
                G->fill_rect(trackX,thumbY,US(4),thumbH,gfx_rgb(150,145,135)); }
        }

        /* ── jail roster ── */
        if(show_jail){
            int pw=US(680), ph=US(40+22+22*22), px=W/2-pw/2, py=US(60);
            G->fill_rect(px,py,pw,ph,COL_PANEL); G->rect_lines(px,py,pw,ph,COL_GOLD);
            snprintf(buf,sizeof(buf),"JAIL ROSTER (%d)  [J]",crime_jailed_count(w)); G->text(buf,px+US(12),py+US(10),US(15),COL_GOLD);
            G->text("NAME",px+US(12),py+US(34),US(10),COL_GRAY); G->text("CRIME",px+US(210),py+US(34),US(10),COL_GRAY);
            G->text("GANG/CULT",px+US(300),py+US(34),US(10),COL_GRAY); G->text("SERVED/SENTENCE",px+US(480),py+US(34),US(10),COL_GRAY);
            int yy=py+US(50), shown=0;
            for(int i=0;i<w->n_agents && shown<22;i++){ Agent *a=&w->agents[i];
                if(!a->alive||a->arrested_ticks<=0) continue;
                int tot=a->sentence_total>0?a->sentence_total:1, served=tot-a->arrested_ticks; if(served<0)served=0;
                snprintf(buf,sizeof(buf),"%.24s",a->name); G->text(buf,px+US(12),yy,US(12),COL_WHITE);
                G->text(a->jailed_for[0]?a->jailed_for:"-",px+US(210),yy,US(12),COL_WHITE);
                snprintf(buf,sizeof(buf),"%.22s",faction_name(w,a->faction_id));
                G->text(buf,px+US(300),yy,US(12),a->faction_id>=0?COL_PURPLE:COL_GRAY);
                snprintf(buf,sizeof(buf),"%d/%d (%d%%)",served,tot,served*100/tot);
                G->text(buf,px+US(480),yy,US(12),served*100/tot>=66?COL_GREEN:COL_WHITE);
                yy+=US(20); shown++; }
            if(!shown) G->text("No one is in jail.",px+US(12),yy,US(13),COL_GRAY);
        }

        /* ── factions menu ── */
        if(show_factions){
            int pw=US(560), ph=US(40+22)+(w->n_factions+1)*US(22), px=W/2-pw/2, py=US(80);
            G->fill_rect(px,py,pw,ph,COL_PANEL); G->rect_lines(px,py,pw,ph,COL_PURPLE);
            G->text("FACTIONS  [F]",px+US(12),py+US(10),US(15),COL_PURPLE);
            G->text("NAME",px+US(12),py+US(34),US(10),COL_GRAY); G->text("KIND",px+US(260),py+US(34),US(10),COL_GRAY);
            G->text("MEMBERS",px+US(360),py+US(34),US(10),COL_GRAY); G->text("LEADER",px+US(450),py+US(34),US(10),COL_GRAY);
            int yy=py+US(50);
            for(int i=0;i<w->n_factions;i++){ Faction *f=&w->factions[i]; if(!f->active) continue;
                Agent *ldr=f->leader_id>=0?world_agent_by_id(w,f->leader_id):NULL;
                G->fill_rect(px+US(12),yy+US(3),US(8),US(8),gfx_rgb(f->r,f->g,f->b));
                snprintf(buf,sizeof(buf),"%.26s",f->name); G->text(buf,px+US(26),yy,US(12),COL_WHITE);
                G->text(f->is_cult?"cult":"gang",px+US(260),yy,US(12),gfx_rgb(200,180,150));
                snprintf(buf,sizeof(buf),"%d",f->members); G->text(buf,px+US(360),yy,US(12),COL_WHITE);
                if(ldr) snprintf(buf,sizeof(buf),"%.16s",ldr->name); else snprintf(buf,sizeof(buf),"-");
                G->text(buf,px+US(450),yy,US(12),COL_GRAY); yy+=US(22); }
        }

        /* ── inspector (left) ── */
        if(selected>=0){
            Agent *a=world_agent_by_id(w,selected);
            if(a && a->alive){
                int pw=US(300); G->fill_rect(0,hudH,pw,US(320),gfx_rgba(24,22,30,230));
                int yy=hudH+US(6);
                snprintf(buf,sizeof(buf),"%s  #%d",a->name,a->id); G->text(buf,US(10),yy,US(15),COL_GOLD); yy+=US(24);
                snprintf(buf,sizeof(buf),"Age %d   Action: %s",a->age,action_name(a->action)); G->text(buf,US(10),yy,US(12),COL_WHITE); yy+=US(18);
                snprintf(buf,sizeof(buf),"Money %.0f   Faction %d",a->needs.money,a->faction_id); G->text(buf,US(10),yy,US(12),COL_WHITE); yy+=US(18);
                if(a->is_police){ G->text("POLICE",US(10),yy,US(12),gfx_rgb(120,180,230)); yy+=US(18); }
                if(a->wanted){ snprintf(buf,sizeof(buf),"WANTED for %s",a->wanted_for); G->text(buf,US(10),yy,US(12),COL_RED); yy+=US(18); }
                if(a->arrested_ticks>0){ snprintf(buf,sizeof(buf),"JAILED for %s",a->jailed_for); G->text(buf,US(10),yy,US(12),COL_AMBER); yy+=US(18); }
                const char *lbl[6]={"hunger","energy","safety","social","meaning","belong"};
                double v[6]={a->needs.hunger,a->needs.energy,a->needs.safety,a->needs.social,a->needs.meaning,a->needs.belonging};
                for(int i=0;i<6;i++){ G->text(lbl[i],US(10),yy,US(11),COL_WHITE);
                    G->fill_rect(US(90),yy,US(180),US(8),gfx_rgb(50,50,60));
                    GfxColor c=v[i]<0.2?COL_RED:v[i]<0.4?COL_AMBER:COL_GREEN;
                    G->fill_rect(US(90),yy,(int)(US(180)*v[i]),US(8),c); yy+=US(16); }
                int friends=0,rivals=0;
                for(int i=0;i<a->rels.n;i++){ if(a->rels.rel[i].affinity>=FRIENDSHIP_AFFINITY) friends++;
                    else if(a->rels.rel[i].affinity<=RIVALRY_AFFINITY) rivals++; }
                snprintf(buf,sizeof(buf),"Friends %d  Rivals %d  Known %d",friends,rivals,a->rels.n);
                G->text(buf,US(10),yy,US(12),COL_WHITE); yy+=US(18);
                G->text("arrows/wheel: browse   esc: close",US(10),yy+US(4),US(10),COL_GRAY);
            } else selected=-1;
        }

        /* ── god toolbar + badge + flash ── */
        if(god){
            int cw=US(92), ch=US(28), g2=US(6), total=G_NTOOLS*cw+(G_NTOOLS-1)*g2;
            int sx=W/2-total/2, yb=Hs-ch-US(12);
            G->fill_rect(sx-US(12),yb-US(8),total+US(24),ch+US(16),gfx_rgba(24,22,30,220));
            for(int i=0;i<G_NTOOLS;i++){ int cx=sx+i*(cw+g2); int sel=(i==tool);
                G->fill_rect(cx,yb,cw,ch, sel?gfx_rgba(212,175,90,230):gfx_rgba(32,30,38,220));
                if(sel) G->rect_lines(cx,yb,cw,ch,COL_WHITE);
                snprintf(buf,sizeof(buf),"%d %s",i+1,GOD_TOOL_NAME[i]);
                G->text(buf,cx+US(8),yb+US(8),US(12),sel?gfx_rgb(0,0,0):COL_WHITE); }
            const char *b="GOD MODE"; G->text(b,W/2-G->text_w(b,US(16))/2,hudH+US(4),US(16),COL_GOLD);
        }
        if(flash[0] && now_sec()<flash_until)
            G->text(flash,W/2-G->text_w(flash,US(16))/2,US(50),US(16),gfx_rgb(230,225,215));

        /* ── legend (bottom-left) ── */
        if(show_legend){
            int rows=(LEGEND_N+1)/2, rh=US(16), lw=US(168), lh=US(20)+rows*rh;
            int lx=US(10), ly=Hs-lh-US(10);
            G->fill_rect(lx,ly,lw,lh,gfx_rgba(24,22,30,225));
            G->rect_lines(lx,ly,lw,lh,gfx_rgb(60,56,70));
            G->text("LEGEND  [L]",lx+US(8),ly+US(5),US(11),COL_GOLD);
            for(int i=0;i<LEGEND_N;i++){ int col=i%2, row=i/2;
                int ex=lx+US(8)+col*US(80), ey=ly+US(22)+row*rh;
                G->fill_rect(ex,ey,US(10),US(10),tile_col(LEGEND[i].type));
                G->rect_lines(ex,ey,US(10),US(10),gfx_rgba(10,8,14,180));
                G->text(LEGEND[i].label,ex+US(14),ey-US(1),US(11),gfx_rgb(225,220,210)); }
        }

        G->present();

        /* Idle down to save power/fans: when unfocused (nobody watching) or paused
         * (just viewing a frozen frame), cap to a low FPS by sleeping the remainder
         * of the frame. Skipped during benchmarks. */
        if(!bench){
            int focused = G->focused ? G->focused() : 1;
            if(!focused || paused){
                double target = focused ? (1.0/20.0) : (1.0/6.0);  /* 20fps paused, 6fps unfocused */
                double spent = now_sec() - t;
                double rem = target - spent;
                if(rem > 0){ struct timespec ts={ (time_t)rem, (long)((rem-(double)(time_t)rem)*1e9) };
                    nanosleep(&ts,NULL); }
            }
        }
        fcount++;
        if(bench){
            if(fcount==60) bench_t0=now_sec();
            if(fcount>=60+bench){ double el=now_sec()-bench_t0;
                printf("[bench] %-7s %d frames / %.3fs = %.1f FPS\n", G->name, bench, el, bench/el);
                fflush(stdout); break; }
        } else if(shot && ++frame==120){ if(G->screenshot) G->screenshot(shot); break; }
    }
    if(ts_tex && G->free_tex) G->free_tex(ts_tex);
    G->shutdown();
    return 0;
}
