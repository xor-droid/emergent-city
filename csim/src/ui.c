/* ui.c — the UI/run loop, written against the gfx.h interface (G->...).
 *
 * Camera transform and frame timing are done here (backend-neutral). All UI
 * chrome (HUD, panels, god mode, legend) is scaled by a global UI scale so it
 * stays readable on hi-dpi/4K displays — see US() and g_uiscale.
 */
#include "gfx.h"
#include "viz.h"
#include "record.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <math.h>

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
static GfxColor culture_col(unsigned char c){
    switch(c){ case CUL_HARBOR:  return gfx_rgb(60,180,190);
               case CUL_HILL:    return gfx_rgb(180,140,70);
               case CUL_OLDTOWN: return gfx_rgb(175,110,205);
               default:          return gfx_rgb(235,150,60); }   /* newcomers */
}
static const char *overlay_name(int o){
    switch(o){ case 1:return "CRIME HEAT"; case 2:return "FACTION TURF"; case 3:return "CULTURE"; case 4:return "VISION (selected)"; case 5:return "HEARING (selected)"; case 6:return "AFFLUENCE"; case 7:return "BIOMES"; default:return "off"; }
}

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
    if(a->crime_role==CR_KINGPIN) return "kingpin";
    if(a->crime_role==CR_DEALER)  return "dealer";
    if(a->crime_role==CR_CAREER)  return "crim";   /* killers stay hidden until wanted */
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
    int show_right=1, show_jail=0, show_factions=0, show_legend=1, show_crime=0;
    int show_city=0, overlay=0;   /* city dashboard (E); map overlay cycle (O): heat/turf/culture */
    int show_tune=0;              /* live tuning panel (T): aging pace, pop target, family knobs */
    int show_family=0;           /* family/genealogy panel (K) for the selected citizen */
    { const char *e=getenv("CSIM_OVERLAY"); if(e){ overlay=atoi(e)%8; } }
    if(getenv("CSIM_CITY")) show_city=1;
    if(getenv("CSIM_TUNE")) show_tune=1;
    if(getenv("CSIM_KIN")) show_family=1;
    if(getenv("CSIM_CRIMEWATCH")) show_crime=1;
    int ascii = getenv("CSIM_ASCII") ? 1 : 0;   /* Dwarf-Fortress ASCII render mode (toggle: a) */
    int selected=-1, list_scroll=0, follow=0;    /* follow = keep camera on the selected citizen */
    int crime_scroll=0, jail_scroll=0;           /* scroll offsets for the crime-watch / jail lists */

    /* optional image tileset (--tileset / CSIM_TILESET); falls back to glyphs */
    void *ts_tex=NULL; int ts_kind=TSK_CP437, ts_sem=SEM_KENNEY, ts_cell=16, ts_space=0, ts_margin=0;
    int ts_cols=16, ts_cw=16, ts_ch=16;   /* cols + cell width/height, derived from the image */
    { const char *tsname=getenv("CSIM_TILESET");
      if(tsname && tsname[0] && G->load_tex){
        char path[1200]=""; const TsEntry *ent=NULL; int direct=0;
        if(strchr(tsname,'/')||strstr(tsname,".png")) direct=1;
        else for(int i=0;i<N_TSETS;i++) if(!strcmp(tsname,TSETS[i].name)){ ent=&TSETS[i]; break; }
        if(ent){ ts_kind=ent->kind; ts_sem=ent->sem; ts_cell=ent->cell; ts_space=ent->space; ts_margin=ent->margin; }
        { const char *e; if((e=getenv("CSIM_TILESET_CELL"))){ int c=atoi(e); if(c>0) ts_cell=c; }
          if((e=getenv("CSIM_TILESET_SPACE"))){ int c=atoi(e); if(c>=0) ts_space=c; }
          if((e=getenv("CSIM_TILESET_MARGIN"))){ int c=atoi(e); if(c>=0) ts_margin=c; } }
        if(!ent && !direct){
            fprintf(stderr,"unknown tileset '%s'. options:",tsname);
            for(int i=0;i<N_TSETS;i++) fprintf(stderr," %s",TSETS[i].name);
            fprintf(stderr,"  (or a path to a .png)\n");
        } else {
            /* search cwd- and executable-relative dirs so it works whether launched
             * from csim/ or csim/build/. Existence-check first to avoid loader spam. */
            char cand[8][640]; int nc=0;
            if(direct){ snprintf(cand[nc++],sizeof(cand[0]),"%.600s",tsname); }
            else {
                char exe[400]=""; ssize_t rn=readlink("/proc/self/exe",exe,sizeof(exe)-1);
                if(rn>0){ exe[rn]=0; char*s=strrchr(exe,'/'); if(s)*s=0; } else exe[0]=0;
                const char *envd=getenv("CSIM_TILESET_DIR");
                if(envd) snprintf(cand[nc++],sizeof(cand[0]),"%.400s/%.120s",envd,ent->file);
                snprintf(cand[nc++],sizeof(cand[0]),"tilesets/%.120s",ent->file);
                snprintf(cand[nc++],sizeof(cand[0]),"../tilesets/%.120s",ent->file);
                if(exe[0]){ snprintf(cand[nc++],sizeof(cand[0]),"%.400s/tilesets/%.120s",exe,ent->file);
                            snprintf(cand[nc++],sizeof(cand[0]),"%.400s/../tilesets/%.120s",exe,ent->file); }
            }
            int tw=0,th=0;
            for(int i=0;i<nc && !ts_tex;i++){ FILE*fp=fopen(cand[i],"rb");
                if(fp){ fclose(fp); ts_tex=G->load_tex(cand[i],&tw,&th); if(ts_tex) snprintf(path,sizeof(path),"%.600s",cand[i]); } }
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
                fprintf(stderr,"[tileset] '%s' not found (looked under ./tilesets, ../tilesets, and <exe>/../tilesets) — using font glyphs.\n", tsname);
                if(ent) fprintf(stderr,"          get it (%s): %s   or set CSIM_TILESET_DIR\n", ent->license, ent->url);
            }
        }
      }
    }
    char flash[96]=""; double flash_until=0;
    const char *shot=getenv("CSIM_SHOT"); int frame=0;
    int shot_frames=120; { const char *sf=getenv("CSIM_SHOT_FRAMES"); if(sf){ int n=atoi(sf); if(n>0) shot_frames=n; } }
    /* pre-roll the sim N game-days at headless speed (no render) so the window opens
       on an evolved city — lets screenshots show accumulated crime heat / wars */
    { const char *wd=getenv("CSIM_WARMDAYS"); if(wd){ int days=atoi(wd);
        long ticks=(long)(days*24*SECONDS_PER_HOUR/0.25);
        for(long i=0;i<ticks;i++) world_tick(w,0.25); } }
    if(getenv("CSIM_DEMO")){ god=1; if(w->n_agents>0){ selected=w->agents[0].id;
        for(int i=0;i<w->n_agents;i++) if(w->agents[i].crime_role!=CR_CITIZEN){ selected=w->agents[i].id; break; }
        follow=1; } }
    /* benchmark mode: uncapped (backends disable vsync when CSIM_BENCH is set),
     * sim paused, measure FPS over N frames after a 60-frame warmup. */
    int bench=0; { const char *bs=getenv("CSIM_BENCH"); if(bs){ bench=atoi(bs); if(bench<1) bench=1; } }
    long fcount=0; double bench_t0=0;

    static int ids[MAX_AGENTS];
    double prev=now_sec(), facc=0; int frames=0, fps=0;
    double simacc=0;   /* fixed-timestep accumulator (sim-seconds), when get_fixed_step() */
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

        /* ── crime-watch + jail panel geometry (computed early so scroll input and
           drawing agree; the lists scroll when they'd overflow the window) ── */
        int cwN=0; for(int i=0;i<w->n_agents;i++) if(w->agents[i].alive && w->agents[i].wanted) cwN++;
        int cwActLines=10, cwPW=US(720), cwPX=W/2-cwPW/2, cwPY=hudH+US(6);
        int cwListTop=cwPY+US(40)+US(18)+cwActLines*US(16)+US(10)+US(18)+US(16);
        int cwRows=(Hs-cwListTop-US(14))/US(17); if(cwRows<1) cwRows=1;
        int cwVis=cwN<cwRows?cwN:cwRows, cwMax=cwN-cwRows; if(cwMax<0) cwMax=0;
        int cwPH=(cwListTop-cwPY)+(cwVis+1)*US(17);
        if(crime_scroll>cwMax) crime_scroll=cwMax; if(crime_scroll<0) crime_scroll=0;

        int jlN=crime_jailed_count(w);
        int jlPW=US(680), jlPX=W/2-jlPW/2, jlPY=US(60), jlListTop=jlPY+US(50);
        int jlRows=(Hs-jlListTop-US(12))/US(20); if(jlRows<1) jlRows=1;
        int jlVis=jlN<jlRows?jlN:jlRows, jlMax=jlN-jlRows; if(jlMax<0) jlMax=0;
        int jlPH=(jlListTop-jlPY)+(jlVis>0?jlVis:1)*US(20)+US(10);
        if(jail_scroll>jlMax) jail_scroll=jlMax; if(jail_scroll<0) jail_scroll=0;

        /* ── input ── */
        if(G->key_pressed(GFX_KEY_SPACE)) paused=!paused;
        if(G->key_pressed(GFX_KEY_G)) god=!god;
        if(G->key_pressed(GFX_KEY_TAB)) show_right=!show_right;
        if(G->key_pressed(GFX_KEY_J)) show_jail=!show_jail;
        if(G->key_pressed(GFX_KEY_F)) show_factions=!show_factions;
        if(G->key_pressed(GFX_KEY_C)) show_crime=!show_crime;
        if(G->key_pressed(GFX_KEY_L)) show_legend=!show_legend;
        if(G->key_pressed(GFX_KEY_A)) ascii=!ascii;
        if(G->key_pressed(GFX_KEY_E)) show_city=!show_city;
        if(G->key_pressed(GFX_KEY_O)) overlay=(overlay+1)%8;
        if(G->key_pressed(GFX_KEY_T)) show_tune=!show_tune;
        if(G->key_pressed(GFX_KEY_K)) show_family=!show_family;
        for(int k=0;k<9;k++) if(G->key_pressed(GFX_KEY_1+k)){
            if(god){ if(k<G_NTOOLS) tool=k; }
            else if(k<6) speed=(float)(k+1);   /* keys 1-6 -> 1x..6x */
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
            if(a){ cam.tx=a->x*TILE_PX; cam.ty=a->y*TILE_PX; follow=1; if(cam.zoom<1.6f)cam.zoom=2.2f; }
            if(selIdx<list_scroll) list_scroll=selIdx;
            if(selIdx>=list_scroll+listRows) list_scroll=selIdx-listRows+1;
        }
        if(G->key_pressed(GFX_KEY_ESC)){ selected=-1; follow=0; }

        int mx,my; G->mouse(&mx,&my);
        int overPanel = show_right && mx>=panelX;
        float wheel=G->wheel();
        if(wheel!=0){
            if(show_crime && mx>=cwPX && mx<=cwPX+cwPW && my>=cwPY && my<=cwPY+cwPH){
                crime_scroll-=(int)wheel*3; if(crime_scroll<0)crime_scroll=0; if(crime_scroll>cwMax)crime_scroll=cwMax;
            } else if(show_jail && mx>=jlPX && mx<=jlPX+jlPW && my>=jlPY && my<=jlPY+jlPH){
                jail_scroll-=(int)wheel*3; if(jail_scroll<0)jail_scroll=0; if(jail_scroll>jlMax)jail_scroll=jlMax;
            } else if(overPanel && my>=listTop){
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
            static int pmx=0,pmy=0; static int dragging=0; follow=0;   /* manual pan stops following */
            if(!dragging){ pmx=mx; pmy=my; dragging=1; }
            cam.tx-=(mx-pmx)/cam.zoom; cam.ty-=(my-pmy)/cam.zoom; pmx=mx; pmy=my;
        }
        /* live tuning panel geometry (shared by click-handling + draw) */
        int tuneRows=14, tunePW=US(330), tunePH=US(60)+tuneRows*US(30);
        int tunePX=W/2-tunePW/2, tunePY=hudH+US(30);
        int tuneBW=US(26), tuneBH=US(24);
        int tuneMinusX=tunePX+tunePW-US(122), tunePlusX=tunePX+tunePW-US(40);
        int tuneClick=0;
        if(show_tune && G->mouse_pressed(GFX_MBTN_LEFT) &&
           mx>=tunePX && mx<=tunePX+tunePW && my>=tunePY && my<=tunePY+tunePH){
            tuneClick=1;
            for(int i=0;i<tuneRows;i++){ int ry=tunePY+US(50)+i*US(30);
                if(my<ry || my>ry+tuneBH) continue;
                int dir = (mx>=tuneMinusX && mx<=tuneMinusX+tuneBW) ? -1
                        : (mx>=tunePlusX  && mx<=tunePlusX +tuneBW) ?  1 : 0;
                if(dir){ switch(i){
                    case 0:{ double v=get_years_per_day()+dir*0.5; if(v<0.1)v=0.1; if(v>60)v=60; set_years_per_day(v);} break;
                    case 1:{ int v=get_pop_target()+dir*10; if(v<0)v=0; set_pop_target(v);} break;
                    case 2:{ double v=get_family_share()+dir*0.05; if(v<0)v=0; if(v>1)v=1; set_family_share(v);} break;
                    case 3: set_family_kids(get_family_kids_min()+dir, get_family_kids_max()); break;
                    case 4: set_family_kids(get_family_kids_min(), get_family_kids_max()+dir); break;
                    case 5: set_fixed_step(!get_fixed_step()); break;   /* toggle */
                    case 6:{ double v=get_research_rate()+dir*0.01; if(v<0)v=0; set_research_rate(v);} break;
                    case 7:{ double v=get_craft_bonus()+dir*0.25; if(v<0)v=0; set_craft_bonus(v);} break;
                    case 8: set_vision(!get_vision()); break;   /* toggle */
                    case 9: set_vision_radius(get_vision_radius()+dir); break;
                    case 10: set_hearing(!get_hearing()); break;   /* toggle */
                    case 11: set_hearing_radius(get_hearing_radius()+dir); break;
                    case 12:{ double v=get_child_cost()+dir*1.0; if(v<0)v=0; set_child_cost(v);} break;
                    case 13:{ double v=get_biome_value_weight()+dir*0.25; if(v<0)v=0; set_biome_value_weight(v);} break;
                }
                if(replay_in_progress()) replay_fork(w);   /* editing during replay diverges into a new session */
                if(rec_active()){
                    static const char *TK[14]={"years_per_day","pop_target","family_share","kids_min","kids_max",
                        "fixed_step","research_rate","production","vision","vision_radius","hearing","hearing_radius","child_cost",
                        "biome_value_weight"};
                    if(i>=0 && i<14) rec_tune_now(w, TK[i]);   /* record this live change for replay */
                } }
                break;
            }
        }

        /* family/genealogy panel geometry + relatives (shared by click + draw) */
        int famIds[32]; const char *famRel[32]; int famN=0;
        int famPW=US(306), famPX=W/2-famPW/2, famPY=hudH+US(30), famRowH=US(20), famTop=famPY+US(40);
        if(show_family && selected>=0){
            Agent *sa=world_agent_by_id(w,selected);
            if(sa){
                if(sa->mother_id>=0 && famN<32){ famIds[famN]=sa->mother_id; famRel[famN]="Mother"; famN++; }
                if(sa->father_id>=0 && famN<32){ famIds[famN]=sa->father_id; famRel[famN]="Father"; famN++; }
                if(sa->spouse_id>=0 && famN<32){ famIds[famN]=sa->spouse_id; famRel[famN]="Spouse"; famN++; }
                for(int i=0;i<w->n_agents && famN<32;i++){ Agent *o=&w->agents[i];
                    if(o->mother_id==sa->id || o->father_id==sa->id){ famIds[famN]=o->id; famRel[famN]="Child"; famN++; } }
                for(int i=0;i<w->n_agents && famN<32;i++){ Agent *o=&w->agents[i];
                    if(o->id==sa->id) continue;
                    if((sa->mother_id>=0 && o->mother_id==sa->mother_id) ||
                       (sa->father_id>=0 && o->father_id==sa->father_id)){ famIds[famN]=o->id; famRel[famN]="Sibling"; famN++; } }
            }
        }
        int famPH=US(50)+(famN?famN:1)*famRowH;
        int famClick=0;
        if(show_family && selected>=0 && G->mouse_pressed(GFX_MBTN_LEFT) &&
           mx>=famPX && mx<=famPX+famPW && my>=famPY && my<=famPY+famPH){
            famClick=1;
            int row=(my-famTop)/famRowH;
            if(row>=0 && row<famN){ selected=famIds[row]; follow=1; }   /* jump to the relative */
        }

        /* clicks on the modal crime/jail panels: drag their scrollbar, and don't
           fall through to selecting an agent behind the panel */
        int modalClick=0;
        if(G->mouse_pressed(GFX_MBTN_LEFT) && show_crime &&
           mx>=cwPX && mx<=cwPX+cwPW && my>=cwPY && my<=cwPY+cwPH){
            modalClick=1;
            if(cwMax>0 && mx>=cwPX+cwPW-US(9) && my>=cwListTop){
                float f=(my-cwListTop)/(float)(cwVis*US(17)); crime_scroll=(int)(f*cwMax);
                if(crime_scroll<0)crime_scroll=0; if(crime_scroll>cwMax)crime_scroll=cwMax; }
        }
        if(G->mouse_pressed(GFX_MBTN_LEFT) && show_jail &&
           mx>=jlPX && mx<=jlPX+jlPW && my>=jlPY && my<=jlPY+jlPH){
            modalClick=1;
            if(jlMax>0 && mx>=jlPX+jlPW-US(9) && my>=jlListTop){
                float f=(my-jlListTop)/(float)(jlVis*US(20)); jail_scroll=(int)(f*jlMax);
                if(jail_scroll<0)jail_scroll=0; if(jail_scroll>jlMax)jail_scroll=jlMax; }
        }

        if(G->mouse_pressed(GFX_MBTN_LEFT) && !tuneClick && !famClick && !modalClick){
            if(overPanel && my>=listTop){
                int row=list_scroll+(my-listTop)/rowh;
                int trackX=W-US(6), trackY=listTop, trackH=listRows*rowh;
                if(nlist>listRows && mx>=trackX-US(2) && my>=trackY && my<=trackY+trackH){
                    float f=(my-trackY)/(float)trackH; list_scroll=(int)(f*maxscroll);
                    if(list_scroll<0)list_scroll=0; if(list_scroll>maxscroll)list_scroll=maxscroll;
                } else if(row>=0 && row<nlist){
                    selected=ids[row];
                    Agent *a=world_agent_by_id(w,selected);
                    if(a){ cam.tx=a->x*TILE_PX; cam.ty=a->y*TILE_PX; follow=1; if(cam.zoom<1.6f)cam.zoom=2.2f; }
                }
            } else if(!overPanel){
                float wx,wy; s2w(&cam,mx,my,&wx,&wy);
                int tx=(int)(wx/TILE_PX), ty=(int)(wy/TILE_PX);
                if(god){ if(replay_in_progress()) replay_fork(w);   /* god action during replay forks */
                         god_apply(w,tool,tx,ty,flash,sizeof(flash)); flash_until=now_sec()+2.5;
                         if(rec_active()) rec_god(w,tool,tx,ty); }   /* record for replay */
                else { Agent *a=world_agent_at(w,tx,ty,5);
                    if(a){ selected=a->id; cam.tx=a->x*TILE_PX; cam.ty=a->y*TILE_PX; follow=1; if(cam.zoom<1.6f)cam.zoom=2.2f; }
                    else { selected=-1; follow=0; } }
            }
        }

        /* ── advance sim ── */
        double t=now_sec(), dt=t-prev; prev=t; if(dt>0.1) dt=0.1;
        facc+=dt; frames++; if(facc>=0.5){ fps=(int)(frames/facc); frames=0; facc=0; }
        /* replay: pause the sim while the edit (tune) menu is open and taking input */
        int eff_paused = paused || (replay_is_loaded() && show_tune);
        if(!eff_paused && dt>0 && !bench){
            if(get_fixed_step()){
                /* deterministic: accumulate real time (scaled by speed) and run
                   whole fixed steps — same step sequence as headless */
                simacc += dt*speed;
                double fdt=get_fixed_dt(); if(fdt<=0) fdt=0.25;
                int guard=0;
                while(simacc>=fdt && guard++<100000){
                    if(replay_in_progress() && w->tick>=replay_end_tick()){ simacc=0; break; }  /* hold at end */
                    replay_apply_due(w);              /* apply the recorded session's due events */
                    world_tick(w,(float)fdt); simacc-=fdt;
                }
            } else {
                simacc=0;                     /* variable (real-time) stepping */
                world_tick(w,(float)(dt*speed));
            }
        }

        /* follow the selected citizen (smoothly track them as they move) */
        if(follow && selected>=0){ Agent *fa=world_agent_by_id(w,selected);
            if(fa&&fa->alive){ cam.tx+=(fa->x*TILE_PX-cam.tx)*0.18f; cam.ty+=(fa->y*TILE_PX-cam.ty)*0.18f; }
            else follow=0; }

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

        /* ── map overlays (toggle O): crime heat / faction turf / culture ── */
        if(overlay==1){                                   /* crime heat from the danger_ grid */
            for(int x=x0;x<=x1;x++) for(int y=y0;y<=y1;y++){
                int d=w->danger_[x][y]; if(d<=8) continue;
                float sx,sy; w2s(&cam,x*TILE_PX,y*TILE_PX,&sx,&sy);
                int al=d*150/255; if(al>170)al=170;
                G->fill_rect((int)sx,(int)sy,iw,iw,gfx_rgba(235,45,20,al));
            }
        } else if(overlay==2){                            /* faction turf blobs + war lines */
            for(int i=0;i<w->n_agents;i++){ Agent *a=&w->agents[i];
                if(!a->alive||a->faction_id<0) continue;
                Faction *f=&w->factions[a->faction_id]; if(f->is_cult) continue;
                int tx=(a->turf_x||a->turf_y)?a->turf_x:(int)a->x, ty=(a->turf_x||a->turf_y)?a->turf_y:(int)a->y;
                float sx,sy; w2s(&cam,tx*TILE_PX+TILE_PX*0.5f,ty*TILE_PX+TILE_PX*0.5f,&sx,&sy);
                G->circle((int)sx,(int)sy,TURF_RADIUS*TILE_PX*cam.zoom*0.5f,gfx_rgba(f->r,f->g,f->b,48));
            }
            float wpulse=0.5f+0.5f*sinf((float)(now_sec()*4.5));   /* 0..1 war-line throb */
            int walpha=120+(int)(wpulse*135);                      /* 120..255 */
            int wthick=1+(int)(wpulse*2.5f);                       /* 1..3 px half-width */
            GfxColor wcol=gfx_rgba(255,60,40,walpha);
            for(int i=0;i<w->n_factions;i++){ Faction *f=&w->factions[i];
                if(!f->active||f->war_with<=i) continue;   /* draw each warring pair once */
                Faction *g=&w->factions[f->war_with];
                /* endpoints: each faction's leader, or any surviving member if the leader's gone */
                Agent *la=f->leader_id>=0?world_agent_by_id(w,f->leader_id):NULL;
                Agent *lb=g->leader_id>=0?world_agent_by_id(w,g->leader_id):NULL;
                if(!la||!la->alive){ la=NULL; for(int k=0;k<w->n_agents;k++) if(w->agents[k].alive&&w->agents[k].faction_id==i){la=&w->agents[k];break;} }
                if(!lb||!lb->alive){ lb=NULL; for(int k=0;k<w->n_agents;k++) if(w->agents[k].alive&&w->agents[k].faction_id==f->war_with){lb=&w->agents[k];break;} }
                if(la&&lb){
                    float ax,ay,bx,by;
                    w2s(&cam,la->x*TILE_PX+TILE_PX*0.5f,la->y*TILE_PX+TILE_PX*0.5f,&ax,&ay);
                    w2s(&cam,lb->x*TILE_PX+TILE_PX*0.5f,lb->y*TILE_PX+TILE_PX*0.5f,&bx,&by);
                    for(int o=-wthick;o<=wthick;o++){ G->line((int)ax,(int)ay+o,(int)bx,(int)by+o,wcol);
                                                      G->line((int)ax+o,(int)ay,(int)bx+o,(int)by,wcol); }
                    float er=US(4)+wpulse*US(6);                   /* pulsing endpoint markers */
                    G->circle((int)ax,(int)ay,er,wcol);
                    G->circle((int)bx,(int)by,er,wcol);
                }
            }
        } else if(overlay==3){                            /* recolor citizens by cultural group */
            for(int i=0;i<w->n_agents;i++){ Agent *a=&w->agents[i]; if(!a->alive) continue;
                float sx,sy; w2s(&cam,a->x*TILE_PX+TILE_PX*0.5f,a->y*TILE_PX+TILE_PX*0.5f,&sx,&sy);
                float rr=ts*0.34f; if(rr<1.8f)rr=1.8f;
                G->circle((int)sx,(int)sy,rr,culture_col(a->culture));
            }
        } else if(overlay==4 && selected>=0){             /* the selected agent's field of view */
            Agent *sa=world_agent_by_id(w,selected);
            if(sa&&sa->alive)
                for(int x=x0;x<=x1;x++) for(int y=y0;y<=y1;y++)
                    if(agent_can_see(w,sa,x,y)){
                        float sx,sy; w2s(&cam,x*TILE_PX,y*TILE_PX,&sx,&sy);
                        G->fill_rect((int)sx,(int)sy,iw,iw,gfx_rgba(120,200,255,55));
                    }
        } else if(overlay==5 && selected>=0){             /* the selected agent's hearing range */
            Agent *sa=world_agent_by_id(w,selected);
            if(sa&&sa->alive)
                for(int x=x0;x<=x1;x++) for(int y=y0;y<=y1;y++)
                    if(agent_can_hear(w,sa,x,y,1.0)){      /* at full loudness, wall-muffled */
                        float sx,sy; w2s(&cam,x*TILE_PX,y*TILE_PX,&sx,&sy);
                        G->fill_rect((int)sx,(int)sy,iw,iw,gfx_rgba(150,255,170,50));
                    }
        } else if(overlay==6){                            /* affluence: rich (green) .. poor (red) */
            for(int x=x0;x<=x1;x++) for(int y=y0;y<=y1;y++){
                int a=w->affluence_[x][y]; if(a<=6) continue;
                float sx,sy; w2s(&cam,x*TILE_PX,y*TILE_PX,&sx,&sy);
                int rr=(a<128)?255:(int)(255*(255-a)/127);   /* red high when poor */
                int gg=(a<128)?(int)(255*a/127):255;         /* green high when rich */
                int al=60+a*90/255;                          /* 60..150 */
                G->fill_rect((int)sx,(int)sy,iw,iw,gfx_rgba(rr,gg,60,al));
            }
        } else if(overlay==7){                            /* biomes: terrain character */
            for(int x=x0;x<=x1;x++) for(int y=y0;y<=y1;y++){
                int b=w->biome_[x][y];
                if(w->tile[x][y]==T_WATER) continue;
                int r2,g2,b2;
                switch(b){ case B_HILLS: r2=150;g2=120;b2=70; break;       /* brown */
                           case B_FLOODPLAIN: r2=200;g2=180;b2=120; break; /* tan */
                           case B_WATERFRONT: r2=70;g2=170;b2=190; break;  /* teal */
                           case B_PARKLAND: r2=90;g2=190;b2=90; break;     /* green */
                           default: continue; }                           /* plain = untinted */
                float sx,sy; w2s(&cam,x*TILE_PX,y*TILE_PX,&sx,&sy);
                G->fill_rect((int)sx,(int)sy,iw,iw,gfx_rgba(r2,g2,b2,120));
            }
        }
        if(overlay){ const char *ohint =
                 ((overlay==4||overlay==5)&&selected<0)?"  (select a citizen)"
               : (overlay==4&&!get_vision())?"  (vision off)"
               : (overlay==5&&!get_hearing())?"  (hearing off)"
               : (overlay==6&&!get_neighborhoods())?"  (neighborhoods off)"
               : (overlay==7&&!get_biomes())?"  (biomes off)":"";
            snprintf(buf,sizeof(buf),"OVERLAY [O]: %s%s",overlay_name(overlay),ohint);
            G->text(buf,US(8),hudH+US(6),US(13),gfx_rgb(255,220,120)); }

        /* ── selection marker: a pulsing yellow reticle on the selected citizen ── */
        if(selected>=0){ Agent *sa=world_agent_by_id(w,selected);
            if(sa&&sa->alive){
                float cx,cy; w2s(&cam, sa->x*TILE_PX+TILE_PX*0.5f, sa->y*TILE_PX+TILE_PX*0.5f, &cx,&cy);
                float pulse=0.5f+0.5f*sinf((float)(now_sec()*5.0));
                GfxColor yl=gfx_rgb(255,232,40);
                int tight=(int)(iw*0.6f)+US(3);                       /* inner box ~cell size */
                int ring=tight+US(4)+(int)(pulse*(iw*0.5f+US(7)));     /* pulsing outer ring */
                for(int pass=0;pass<2;pass++){ int h=pass?ring:tight;
                    G->rect_lines((int)cx-h,(int)cy-h,2*h,2*h,yl);
                    G->rect_lines((int)cx-h-1,(int)cy-h-1,2*h+2,2*h+2,yl); }  /* 2px thick */
            }
        }

        /* ── weather ambient tint over the map (rain = cool veil, fog = pale haze) ── */
        if(get_weather() && !ascii){
            int ra=(int)(w->weather.rain*70), fa=(int)(w->weather.fog*95);
            if(ra>4) G->fill_rect(0,hudH,W,Hs-hudH,gfx_rgba(90,110,150,ra));
            if(fa>4) G->fill_rect(0,hudH,W,Hs-hudH,gfx_rgba(200,205,210,fa));
        }

        /* ── HUD ── */
        char hud[256]; hud_string(w,hud,sizeof(hud),speed,paused,fps,G->name);
        G->fill_rect(0,0,W,hudH,gfx_rgba(0,0,0,170));
        G->text(hud,US(8),US(5),US(14),gfx_rgb(230,225,215));
        if(get_weather()){
            const char *sky = w->weather.fog>0.5f?"fog":w->weather.rain>0.4f?"storm":w->weather.rain>0.15f?"rain"
                            : w->weather.temp>0.7f?"hot":w->weather.temp<0.3f?"cold":"clear";
            char wx[96]; snprintf(wx,sizeof wx,"WX %s  t%.0f r%.0f f%.0f",sky,
                w->weather.temp*100,w->weather.rain*100,w->weather.fog*100);
            G->text(wx,W-US(300),US(5),US(13),gfx_rgb(170,200,235));
        }
        if(w->stab.flags){   /* sim-stability warnings (detect-only) */
            char sb[160]; sb[0]=0;
            if(w->stab.flags&STAB_CRIME_SPIRAL) strncat(sb," crime-spiral",sizeof sb-strlen(sb)-1);
            if(w->stab.flags&STAB_DEPOP)        strncat(sb," depopulating",sizeof sb-strlen(sb)-1);
            if(w->stab.flags&STAB_DESTITUTE)    strncat(sb," destitution",sizeof sb-strlen(sb)-1);
            if(w->stab.flags&STAB_INFLATION)    strncat(sb," inflation",sizeof sb-strlen(sb)-1);
            if(w->stab.flags&STAB_BUST)         strncat(sb," bust",sizeof sb-strlen(sb)-1);
            char banner[192]; snprintf(banner,sizeof banner,"\xe2\x9a\xa0 UNSTABLE:%s",sb);
            int bw=US(10)+(int)strlen(banner)*US(7);
            G->fill_rect(W/2-bw/2, hudH+US(2), bw, US(20), gfx_rgba(120,20,20,200));
            G->text(banner, W/2-bw/2+US(6), hudH+US(5), US(13), gfx_rgb(255,210,180));
        }
        if(replay_is_loaded()){
            char rb[160]; unsigned long long et=(unsigned long long)replay_end_tick();
            if(replay_in_progress())
                snprintf(rb,sizeof rb,"REPLAY %s  tick %llu/%llu%s  (T + edit a value to fork)",
                         replay_session_name(),(unsigned long long)w->tick,et,
                         (w->tick>=replay_end_tick())?"  [END]":"");
            else snprintf(rb,sizeof rb,"FORKED from %s — now live & recording",replay_session_name());
            G->text(rb,US(8),hudH+US(6),US(13),gfx_rgb(120,220,255));
        }

        /* ── right panel: feed + citizens list ── */
        if(show_right){
            G->fill_rect(panelX,hudH,panelW,Hs-hudH,gfx_rgba(24,22,30,225));
            G->text("EVENT FEED [Tab]",panelX+US(10),US(30),US(13),COL_GOLD);
            int yy=US(50);
            for(int i=0;i<feedLines && i<w->ev_count;i++){ const WorldEvent *e=events_recent(w,i); if(!e)break;
                GfxColor fc = e->kind==EV_WAR?gfx_rgb(255,120,90)
                            : e->kind==EV_MARRIAGE?gfx_rgb(240,200,120)
                            : e->kind==EV_BIRTH?gfx_rgb(170,220,170)
                            : e->kind==EV_DEATH?gfx_rgb(230,160,160)
                            : gfx_rgb(220,215,205);
                snprintf(buf,sizeof(buf),"%.44s",e->text); G->text(buf,panelX+US(10),yy,US(12),fc); yy+=US(15); }
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

        /* ── jail roster (scrollable) ── */
        if(show_jail){
            int px=jlPX, py=jlPY, pw=jlPW, ph=jlPH;
            G->fill_rect(px,py,pw,ph,COL_PANEL); G->rect_lines(px,py,pw,ph,COL_GOLD);
            snprintf(buf,sizeof(buf),"JAIL ROSTER (%d)  [J]",jlN); G->text(buf,px+US(12),py+US(10),US(15),COL_GOLD);
            G->text("NAME",px+US(12),py+US(34),US(10),COL_GRAY); G->text("CRIME",px+US(210),py+US(34),US(10),COL_GRAY);
            G->text("GANG/CULT",px+US(300),py+US(34),US(10),COL_GRAY); G->text("SERVED/SENTENCE",px+US(480),py+US(34),US(10),COL_GRAY);
            int yy=jlListTop, idx=0, shown=0;
            for(int i=0;i<w->n_agents && shown<jlVis;i++){ Agent *a=&w->agents[i];
                if(!a->alive||a->arrested_ticks<=0) continue;
                if(idx++ < jail_scroll) continue;
                int tot=a->sentence_total>0?a->sentence_total:1, served=tot-a->arrested_ticks; if(served<0)served=0;
                snprintf(buf,sizeof(buf),"%.24s",a->name); G->text(buf,px+US(12),yy,US(12),COL_WHITE);
                G->text(a->jailed_for[0]?a->jailed_for:"-",px+US(210),yy,US(12),COL_WHITE);
                if(a->jail_gang){ snprintf(buf,sizeof(buf),"%.22s",jail_gang_name(a->jail_gang)); G->text(buf,px+US(300),yy,US(12),COL_RED); }
                else { snprintf(buf,sizeof(buf),"%.22s",faction_name(w,a->faction_id)); G->text(buf,px+US(300),yy,US(12),a->faction_id>=0?COL_PURPLE:COL_GRAY); }
                snprintf(buf,sizeof(buf),"%d/%d (%d%%)",served,tot,served*100/tot);
                G->text(buf,px+US(480),yy,US(12),served*100/tot>=66?COL_GREEN:COL_WHITE);
                yy+=US(20); shown++; }
            if(!jlN) G->text("No one is in jail.",px+US(12),jlListTop,US(13),COL_GRAY);
            if(jlMax>0){   /* scrollbar */
                int trackX=px+pw-US(8), trackY=jlListTop, trackH=jlVis*US(20);
                G->fill_rect(trackX,trackY,US(4),trackH,gfx_rgb(40,38,48));
                int thumbH=trackH*jlVis/jlN; if(thumbH<US(10))thumbH=US(10);
                int thumbY=trackY+(trackH-thumbH)*jail_scroll/jlMax;
                G->fill_rect(trackX,thumbY,US(4),thumbH,gfx_rgb(150,145,135)); }
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
                if(f->war_with>=0){ snprintf(buf,sizeof(buf),"%s war",f->is_cult?"cult":"gang");
                    G->text(buf,px+US(260),yy,US(12),COL_RED); }
                else G->text(f->is_cult?"cult":"gang",px+US(260),yy,US(12),gfx_rgb(200,180,150));
                snprintf(buf,sizeof(buf),"%d",f->members); G->text(buf,px+US(360),yy,US(12),COL_WHITE);
                if(f->war_with>=0){ snprintf(buf,sizeof(buf),"vs %.12s  k%d",w->factions[f->war_with].name,f->casualties);
                    G->text(buf,px+US(450),yy,US(12),COL_RED); }
                else { if(ldr) snprintf(buf,sizeof(buf),"%.16s",ldr->name); else snprintf(buf,sizeof(buf),"-");
                    G->text(buf,px+US(450),yy,US(12),COL_GRAY); }
                yy+=US(22); }
        }

        /* ── city dashboard: aggregate economy / culture / governance  [E] ── */
        if(show_city){
            int pw=US(372), ph=US(524), px=W/2-pw/2, py=hudH+US(18);
            G->fill_rect(px,py,pw,ph,gfx_rgba(20,20,28,240)); G->rect_lines(px,py,pw,ph,COL_GOLD);
            int yy=py+US(12), lx=px+US(14);
            G->text("CITY DASHBOARD  [E]",lx,yy,US(15),COL_GOLD); yy+=US(24);
            int alive=0,relig=0,common=0,landl=0,indebt=0,married=0,injured=0,wanted=0;
            int children=0,elders=0; long age_sum=0;
            double money=0,debt=0,edu=0;
            int faith[FAITH_COUNT]={0}, lang[LANG_COUNT]={0};
            for(int i=0;i<w->n_agents;i++){ Agent *a=&w->agents[i]; if(!a->alive) continue;
                alive++; money+=a->needs.money; edu+=a->education; age_sum+=a->age;
                if(life_stage(a)==LS_CHILD||life_stage(a)==LS_YOUTH) children++;
                else if(life_stage(a)==LS_ELDER) elders++;
                faith[a->faith]++; lang[a->language]++;
                if(a->faith!=FAITH_NONE) relig++;
                if(a->language==LANG_COMMON) common++;
                if(a->debt>0.5){ indebt++; debt+=a->debt; }
                if(a->spouse_id>=0) married++;
                if(a->injury>0.05f) injured++;
                if(a->wanted) wanted++;
                if(count_properties(w,a->id)>0) landl++;
            }
            int wars=0,war_cas=0;
            for(int i=0;i<w->n_factions;i++){ if(w->factions[i].war_with>i) wars++; war_cas+=w->factions[i].casualties; }
            GfxColor hd=gfx_rgb(150,200,230), tx=gfx_rgb(214,210,200);
            #define ROW(...) do{ snprintf(buf,sizeof(buf),__VA_ARGS__); G->text(buf,lx,yy,US(12),tx); yy+=US(18);}while(0)
            G->text("POPULATION",lx,yy,US(12),hd); yy+=US(18);
            ROW("Alive %d    Married %d    Injured %d",alive,married,injured);
            ROW("Avg age %ld    Youth %d    Elders %d",alive?age_sum/alive:0,children,elders);
            ROW("Avg wealth %.0f    Wanted %d",alive?money/alive:0,wanted);
            yy+=US(6); G->text("ECONOMY",lx,yy,US(12),hd); yy+=US(18);
            ROW("Goods price %.2fx    Wage %.2fx",w->econ.goods_price,w->econ.wage_mult);
            ROW("Landlords %d    In debt %d (%.0f)",landl,indebt,debt);
            yy+=US(6); G->text("CULTURE",lx,yy,US(12),hd); yy+=US(18);
            ROW("Avg education %.0f%%    Religious %d/%d",alive?edu/alive*100:0,relig,alive);
            ROW("Common tongue %d/%d",common,alive);
            { int bf=1; for(int i=2;i<FAITH_COUNT;i++) if(faith[i]>faith[bf]) bf=i;
              ROW("Top faith: %s (%d)   Cults' Mystic %d",faith_name((unsigned char)bf),faith[bf],faith[FAITH_MYSTIC]); }
            yy+=US(6); G->text("GOVERNANCE",lx,yy,US(12),hd); yy+=US(18);
            ROW("Faction wars %d    War deaths %d",wars,war_cas);
            ROW("Police crackdown: %s",w->crackdown_days>0?"ON":"off");
            yy+=US(6); G->text("KNOWLEDGE",lx,yy,US(12),hd); yy+=US(18);
            { double nextcost=THEORY_BASE*pow(THEORY_GROWTH,w->sci.theories);
              ROW("Theories %d    Research %.0f/%.0f",w->sci.theories,w->sci.research,nextcost); }
            { char tb[72]=""; int tn=0;
              for(int t=0;t<TECH_COUNT;t++) if(w->sci.discovered[t]){ char one[20];
                  snprintf(one,sizeof(one),"%s%.10s",tn?" ":"",tech_name(t)); strncat(tb,one,sizeof(tb)-strlen(tb)-1); tn++; }
              if(!tn) snprintf(tb,sizeof(tb),"(researching...)");
              ROW("Tech: %.44s",tb); }
            #undef ROW
        }

        /* ── live tuning panel [T]: adjust sim knobs while running ── */
        if(show_tune){
            G->fill_rect(tunePX,tunePY,tunePW,tunePH,gfx_rgba(18,20,26,243));
            G->rect_lines(tunePX,tunePY,tunePW,tunePH,gfx_rgb(120,170,120));
            G->text("LIVE TUNING  [T]",tunePX+US(12),tunePY+US(10),US(14),gfx_rgb(150,215,150));
            const char *tlab[14]={"Aging yr/day","Pop target","Family share","Kids min","Kids max","Timestep","Research rate","Production","Vision","Vision range","Hearing","Hearing range","Child cost","Biome value"};
            char tv[14][24];
            snprintf(tv[0],24,"%.1f",get_years_per_day());
            snprintf(tv[1],24,"%d",get_pop_target());
            snprintf(tv[2],24,"%.0f%%",get_family_share()*100);
            snprintf(tv[3],24,"%d",get_family_kids_min());
            snprintf(tv[4],24,"%d",get_family_kids_max());
            snprintf(tv[5],24,"%s",get_fixed_step()?"Fixed":"Variable");
            snprintf(tv[6],24,"%.2f",get_research_rate());
            snprintf(tv[7],24,"%.2f",get_craft_bonus());
            snprintf(tv[8],24,"%s",get_vision()?"On":"Off");
            snprintf(tv[9],24,"%d",get_vision_radius());
            snprintf(tv[10],24,"%s",get_hearing()?"On":"Off");
            snprintf(tv[11],24,"%d",get_hearing_radius());
            snprintf(tv[12],24,"%.0f",get_child_cost());
            snprintf(tv[13],24,"%.2f",get_biome_value_weight());
            for(int i=0;i<tuneRows;i++){ int ry=tunePY+US(50)+i*US(30);
                G->text(tlab[i],tunePX+US(14),ry+US(3),US(12),gfx_rgb(214,210,200));
                G->fill_rect(tuneMinusX,ry,tuneBW,tuneBH,gfx_rgb(58,62,70));
                G->text("-",tuneMinusX+US(9),ry+US(2),US(16),COL_WHITE);
                G->text(tv[i],tuneMinusX+tuneBW+US(9),ry+US(3),US(13),COL_GOLD);
                G->fill_rect(tunePlusX,ry,tuneBW,tuneBH,gfx_rgb(58,62,70));
                G->text("+",tunePlusX+US(8),ry+US(2),US(16),COL_WHITE);
            }
            G->text("click -/+ to adjust (applies live)",tunePX+US(12),tunePY+tunePH-US(18),US(10),COL_GRAY);
        }

        /* ── family / genealogy panel [K]: the selected citizen's kin (clickable) ── */
        if(show_family && selected>=0){
            Agent *sa=world_agent_by_id(w,selected);
            G->fill_rect(famPX,famPY,famPW,famPH,gfx_rgba(20,22,28,243));
            G->rect_lines(famPX,famPY,famPW,famPH,COL_GOLD);
            snprintf(buf,sizeof(buf),"FAMILY — %.20s  [K]", sa?sa->name:"?");
            G->text(buf,famPX+US(12),famPY+US(10),US(14),COL_GOLD);
            if(famN==0) G->text("(no known kin)",famPX+US(14),famTop+US(2),US(12),COL_GRAY);
            const char *prevrel="";
            for(int i=0;i<famN;i++){ int ry=famTop+i*famRowH;
                Agent *o=world_agent_by_id(w,famIds[i]);
                int diff = strcmp(famRel[i],prevrel)!=0; prevrel=famRel[i];
                GfxColor rc = (o&&o->alive)?gfx_rgb(214,210,200):COL_GRAY;
                if(o&&o->alive) snprintf(buf,sizeof(buf),"%-8s %.18s  %d (%s)",diff?famRel[i]:"",o->name,o->age,life_stage_name(o));
                else            snprintf(buf,sizeof(buf),"%-8s %s",diff?famRel[i]:"",o?"(deceased)":"(unknown)");
                G->text(buf,famPX+US(14),ry,US(12),rc);
            }
            G->text("click a name to jump to them",famPX+US(12),famPY+famPH-US(16),US(10),COL_GRAY);
        }

        /* ── crime watch: live crimes + who's on the run (scrollable, with lie-low cooldown) ── */
        if(show_crime){
            int px=cwPX, py=cwPY, pw=cwPW, ph=cwPH;
            G->fill_rect(px,py,pw,ph,COL_PANEL); G->rect_lines(px,py,pw,ph,COL_RED);
            G->text("CRIME WATCH  [c]",px+US(12),py+US(10),US(15),COL_RED);
            int yy=py+US(36);
            G->text("CRIMES IN ACTION",px+US(12),yy,US(11),COL_GOLD); yy+=US(18);
            int shown=0;
            for(int i=0;i<w->ev_count && shown<cwActLines;i++){ const WorldEvent *e=events_recent(w,i); if(!e) break;
                if(e->kind!=EV_CRIME && e->kind!=EV_CRIME_FAILED && e->kind!=EV_ARREST) continue;
                GfxColor c = e->kind==EV_ARREST?COL_GREEN : e->kind==EV_CRIME_FAILED?COL_GRAY : gfx_rgb(232,200,200);
                snprintf(buf,sizeof(buf),"%.92s",e->text); G->text(buf,px+US(16),yy,US(12),c); yy+=US(16); shown++; }
            if(!shown){ G->text("(quiet for now)",px+US(16),yy,US(12),COL_GRAY); yy+=US(16); }
            yy+=US(10);
            snprintf(buf,sizeof(buf),"ON THE RUN (%d)",cwN); G->text(buf,px+US(12),yy,US(11),COL_GOLD); yy+=US(18);
            G->text("NAME",px+US(16),yy,US(10),COL_GRAY); G->text("WANTED FOR",px+US(250),yy,US(10),COL_GRAY);
            G->text("LIE-LOW COOLDOWN",px+US(440),yy,US(10),COL_GRAY); yy+=US(16);
            if(!cwN){ G->text("No one is on the run.",px+US(16),yy,US(12),COL_GRAY); }
            int idx=0, rshown=0;   /* skip `crime_scroll` wanted agents, then draw cwVis */
            for(int i=0;i<w->n_agents && rshown<cwVis;i++){ Agent *a=&w->agents[i];
                if(!a->alive || !a->wanted) continue;
                if(idx++ < crime_scroll) continue;
                snprintf(buf,sizeof(buf),"%.30s",a->name); G->text(buf,px+US(16),yy,US(12),COL_WHITE);
                G->text(a->wanted_for[0]?a->wanted_for:"-",px+US(250),yy,US(12),COL_RED);
                double f=crime_cooldown_frac(a); if(f<0)f=0; if(f>1)f=1;
                int bw=US(200), bx=px+US(440);
                G->fill_rect(bx,yy+US(2),bw,US(9),gfx_rgb(50,46,54));
                G->fill_rect(bx,yy+US(2),(int)(bw*f),US(9),COL_AMBER);
                yy+=US(17); rshown++; }
            if(cwMax>0){   /* scrollbar for the on-the-run list */
                int trackX=px+pw-US(8), trackY=cwListTop, trackH=cwVis*US(17);
                G->fill_rect(trackX,trackY,US(4),trackH,gfx_rgb(40,38,48));
                int thumbH=trackH*cwVis/cwN; if(thumbH<US(10))thumbH=US(10);
                int thumbY=trackY+(trackH-thumbH)*crime_scroll/cwMax;
                G->fill_rect(trackX,thumbY,US(4),thumbH,gfx_rgb(150,145,135)); }
        }

        /* ── inspector (left) ── */
        if(selected>=0){
            Agent *a=world_agent_by_id(w,selected);
            if(a && a->alive){
                int pw=US(348), ph=US(533);
                G->fill_rect(0,hudH,pw,ph,gfx_rgba(22,20,28,236));
                G->rect_lines(0,hudH,pw,ph,gfx_rgb(64,60,76));
                int yy=hudH+US(10);
                snprintf(buf,sizeof(buf),"%.26s  #%d",a->name,a->id); G->text(buf,US(12),yy,US(16),COL_GOLD); yy+=US(25);
                G->line(US(10),yy,pw-US(10),yy,gfx_rgb(72,68,84)); yy+=US(8);
                snprintf(buf,sizeof(buf),"Age %d (%s)   %s",a->age,life_stage_name(a),action_name(a->action)); G->text(buf,US(12),yy,US(13),COL_WHITE); yy+=US(20);
                snprintf(buf,sizeof(buf),"Money %.0f      %.16s",a->needs.money,faction_name(w,a->faction_id)); G->text(buf,US(12),yy,US(13),COL_WHITE); yy+=US(20);
                { int ax=(int)a->x, ay=(int)a->y;
                  TileType tt=(ax>=0&&ax<WORLD_W&&ay>=0&&ay<WORLD_H)?(TileType)w->tile[ax][ay]:T_GRASS;
                  snprintf(buf,sizeof(buf),"On %s  (%d,%d)",tile_name(tt),ax,ay);
                  G->text(buf,US(12),yy,US(13),gfx_rgb(200,196,186)); yy+=US(20); }
                snprintf(buf,sizeof(buf),"%s   rep %+d",status_title(a),(int)(a->reputation*100));
                G->text(buf,US(12),yy,US(13), a->reputation<-0.2f?COL_RED:a->reputation>0.3f?COL_GREEN:gfx_rgb(210,205,195)); yy+=US(20);
                { char sp[40]="single";
                  if(a->spouse_id>=0){ Agent *s=world_agent_by_id(w,a->spouse_id); if(s) snprintf(sp,sizeof(sp),"m. %.20s",s->name); }
                  snprintf(buf,sizeof(buf),"%s  %s  kids %d%s", a->sex?"M":"F", sp, a->n_children,
                           a->pregnant_ticks>0?"  (expecting)":"");
                  G->text(buf,US(12),yy,US(12), a->pregnant_ticks>0?gfx_rgb(230,180,220):gfx_rgb(200,196,186)); yy+=US(19); }
                { int props=count_properties(w,a->id);
                  if(props>0) snprintf(buf,sizeof(buf),"%s  craft %d%%  landlord x%d",occupation_name(a->occupation),(int)(a->craft*100),props);
                  else        snprintf(buf,sizeof(buf),"%s  craft %d%%",occupation_name(a->occupation),(int)(a->craft*100));
                  G->text(buf,US(12),yy,US(12),gfx_rgb(200,196,186)); yy+=US(19); }
                if(a->debt>0.5){ snprintf(buf,sizeof(buf),"Debt %.0f",a->debt);
                  G->text(buf,US(12),yy,US(12), a->debt>DEBT_CEILING*0.75?COL_RED:COL_AMBER); yy+=US(19); }
                { snprintf(buf,sizeof(buf),"%.14s  %.12s  edu %d%%",culture_name(a->culture),language_name(a->language),(int)(a->education*100));
                  G->text(buf,US(12),yy,US(12),gfx_rgb(190,200,210)); yy+=US(19); }
                { snprintf(buf,sizeof(buf),"Faith: %s",faith_name(a->faith));
                  G->text(buf,US(12),yy,US(12), a->faith!=FAITH_NONE?gfx_rgb(220,200,140):COL_GRAY); yy+=US(19); }
                if(a->is_police){ G->text("POLICE",US(12),yy,US(13),gfx_rgb(120,180,230)); yy+=US(20); }
                if(a->wanted){ snprintf(buf,sizeof(buf),"WANTED: %.24s",a->wanted_for); G->text(buf,US(12),yy,US(13),COL_RED); yy+=US(20); }
                if(a->arrested_ticks>0){ snprintf(buf,sizeof(buf),"JAILED: %.24s",a->jailed_for); G->text(buf,US(12),yy,US(13),COL_AMBER); yy+=US(20); }
                if(a->crime_role!=CR_CITIZEN){
                    GfxColor rc = a->crime_role==CR_KILLER?COL_RED : a->crime_role==CR_KINGPIN?gfx_rgb(220,120,220) : COL_AMBER;
                    snprintf(buf,sizeof(buf),"Role: %s",crime_role_name(a->crime_role)); G->text(buf,US(12),yy,US(13),rc); yy+=US(20); }
                if(a->crimes_committed>0){ snprintf(buf,sizeof(buf),"Rap sheet: %d  (skill %d%%)",a->crimes_committed,(int)(a->crime_skill*100)); G->text(buf,US(12),yy,US(12),gfx_rgb(200,196,186)); yy+=US(18); }
                if(a->drug_stock>0){ snprintf(buf,sizeof(buf),"Drug stock: %d units",a->drug_stock); G->text(buf,US(12),yy,US(12),gfx_rgb(200,196,186)); yy+=US(18); }
                if(a->addiction>0.05f){ snprintf(buf,sizeof(buf),"Addiction: %d%%",(int)(a->addiction*100)); G->text(buf,US(12),yy,US(12),gfx_rgb(224,150,120)); yy+=US(18); }
                if(a->injury>0.05f){ snprintf(buf,sizeof(buf),"Injury: %d%%",(int)(a->injury*100)); G->text(buf,US(12),yy,US(12),COL_RED); yy+=US(18); }
                if(a->arrested_ticks>0 && a->jail_gang){ snprintf(buf,sizeof(buf),"Jail gang: %s",jail_gang_name(a->jail_gang)); G->text(buf,US(12),yy,US(12),gfx_rgb(220,120,220)); yy+=US(18); }
                yy+=US(4);
                const char *lbl[6]={"Hunger","Energy","Safety","Social","Meaning","Belong"};
                double v[6]={a->needs.hunger,a->needs.energy,a->needs.safety,a->needs.social,a->needs.meaning,a->needs.belonging};
                int barx=US(96), barw=US(188), barh=US(13);
                for(int i=0;i<6;i++){
                    G->text(lbl[i],US(12),yy+US(1),US(12),gfx_rgb(208,202,192));
                    G->fill_rect(barx,yy,barw,barh,gfx_rgb(46,44,54));
                    GfxColor c=v[i]<0.2?COL_RED:v[i]<0.4?COL_AMBER:COL_GREEN;
                    int fw=(int)(barw*(v[i]<0?0:v[i]>1?1:v[i])); G->fill_rect(barx,yy,fw,barh,c);
                    snprintf(buf,sizeof(buf),"%d%%",(int)(v[i]*100+0.5)); G->text(buf,barx+barw+US(8),yy+US(1),US(11),gfx_rgb(185,180,172));
                    yy+=US(19);
                }
                yy+=US(5);
                int friends=0,rivals=0;
                for(int i=0;i<a->rels.n;i++){ if(a->rels.rel[i].affinity>=FRIENDSHIP_AFFINITY) friends++;
                    else if(a->rels.rel[i].affinity<=RIVALRY_AFFINITY) rivals++; }
                snprintf(buf,sizeof(buf),"Friends %d    Rivals %d    Known %d",friends,rivals,a->rels.n);
                G->text(buf,US(12),yy,US(13),COL_WHITE); yy+=US(20);
                G->text("arrows / wheel: browse      esc: close",US(12),yy,US(11),COL_GRAY);
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
            int rows=(LEGEND_N+1)/2, rh=US(23), colw=US(102), lw=US(16)+2*colw, lh=US(32)+rows*rh;
            int lx=US(10), ly=Hs-lh-US(10);
            G->fill_rect(lx,ly,lw,lh,gfx_rgba(22,20,28,236));
            G->rect_lines(lx,ly,lw,lh,gfx_rgb(64,60,76));
            G->text("LEGEND  [L]",lx+US(10),ly+US(8),US(13),COL_GOLD);
            int sw=US(18);
            for(int i=0;i<LEGEND_N;i++){ int col=i%2, row=i/2;
                int ex=lx+US(10)+col*colw, ey=ly+US(30)+row*rh;
                TileType lt=LEGEND[i].type;
                /* swatch matches the current render: block colors, or in ASCII mode
                 * the tileset tile (if loaded) / the ASCII glyph. */
                if(ascii && ts_tex){
                    int tc,tr; GfxColor tint;
                    if(ts_kind==TSK_CP437){ int code=cp437_for(lt); tc=code%ts_cols; tr=code/ts_cols; tint=shade(tile_col(lt),70); }
                    else { semantic_cell(ts_sem,lt,&tc,&tr); if(tc<0){tc=0;tr=0;} tint=COL_WHITE; }
                    G->fill_rect(ex,ey,sw,sw,gfx_rgb(20,18,24));
                    int srcx=ts_margin+tc*(ts_cw+ts_space), srcy=ts_margin+tr*(ts_ch+ts_space);
                    G->draw_tex(ts_tex, srcx,srcy,ts_cw,ts_ch, ex,ey,sw,sw, tint);
                } else if(ascii){
                    const char *g=ascii_glyph(lt);
                    G->fill_rect(ex,ey,sw,sw,gfx_rgb(20,18,24));
                    if(g&&g[0]){ int gs=US(15), tw=G->text_w(g,gs);
                        G->text(g, ex+(sw-tw)/2, ey+(sw-gs)/2, gs, shade(tile_col(lt),70)); }
                } else {
                    G->fill_rect(ex,ey,sw,sw,tile_col(lt));
                    G->rect_lines(ex,ey,sw,sw,gfx_rgba(10,8,14,180));
                }
                G->text(LEGEND[i].label,ex+sw+US(7),ey+US(3),US(13),gfx_rgb(228,222,212)); }
        }

        /* ── controls hint (bottom, with the legend) ── */
        if(show_legend){
            const char *keys="E city   T tune   K family   O overlay   F factions   C crime   J jail   Tab feed   G god   L legend   Space pause   Q quit";
            int tw=G->text_w(keys,US(11));
            int hx=W/2-tw/2; if(hx<US(8)) hx=US(8);
            G->text(keys,hx,Hs-US(17),US(11),gfx_rgb(158,154,148));
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
        } else if(shot && ++frame==shot_frames){ if(G->screenshot) G->screenshot(shot); break; }
    }
    if(ts_tex && G->free_tex) G->free_tex(ts_tex);
    G->shutdown();
    return 0;
}
