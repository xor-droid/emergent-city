/* ui.c — the shared UI/run loop for all windowed backends.
 *
 * Written once against the gfx.h interface (G->...), so raylib, SDL3 and
 * GLFW+OpenGL render the identical city, HUD, panels, god mode, legend and
 * glyphs. Camera transform and frame timing are done here (backend-neutral).
 */
#include "gfx.h"
#include "viz.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

const GfxBackend *G = NULL;

#define PANEL_W 320
#define FEED_LINES 10
#define ROW_H 16

/* named colors */
#define COL_WHITE   gfx_rgb(255,255,255)
#define COL_GRAY    gfx_rgb(130,130,130)
#define COL_GOLD    gfx_rgb(212,175,90)
#define COL_PURPLE  gfx_rgb(200,120,220)
#define COL_RED     gfx_rgb(230,90,80)
#define COL_AMBER   gfx_rgb(220,180,90)
#define COL_GREEN   gfx_rgb(120,220,130)
#define COL_PANEL   gfx_rgba(24,22,30,235)

static unsigned char clampb(int v){ return (unsigned char)(v<0?0:v>255?255:v); }
static GfxColor tile_col(TileType t){ unsigned char r,g,b; tile_rgb(t,&r,&g,&b); return gfx_rgb(r,g,b); }
static GfxColor shade(GfxColor c,int d){ return gfx_rgba(clampb(c.r+d),clampb(c.g+d),clampb(c.b+d),c.a); }

static double now_sec(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return ts.tv_sec+ts.tv_nsec*1e-9; }

static char building_glyph(TileType t){
    switch(t){ case T_HOME:return 'H'; case T_SHOP:return '$'; case T_WORK:return 'O';
               case T_BAR:return 'B'; case T_CHURCH:return '+'; case T_POLICE:return 'P';
               default:return 0; }
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

    Cam cam; cam.zoom=1.0f; cam.tx=WORLD_W*TILE_PX*0.5f; cam.ty=WORLD_H*TILE_PX*0.5f;
    { const char *ez=getenv("CSIM_ZOOM"); if(ez){ float z=(float)atof(ez); if(z>0) cam.zoom=z; } }

    int paused=0; float speed=1.0f;
    int god=0, tool=G_SMITE;
    int show_right=1, show_jail=0, show_factions=0, show_legend=1;
    int selected=-1, list_scroll=0;
    char flash[96]=""; double flash_until=0;
    const char *shot=getenv("CSIM_SHOT"); int frame=0;
    if(getenv("CSIM_DEMO")){ god=1; show_jail=1; show_factions=1; }

    static int ids[MAX_AGENTS];
    double prev=now_sec(), facc=0; int frames=0, fps=0;
    char buf[128];

    while(!G->should_close()){
        G->poll();
        int W=G->width(), Hs=G->height();
        cam.ox=W*0.5f; cam.oy=Hs*0.5f;
        int panelX=W-PANEL_W;
        int feedBottom=28+FEED_LINES*15+6;
        int listTop=feedBottom+18;
        int listRows=(Hs-listTop-6)/ROW_H; if(listRows<1) listRows=1;

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
                int row=list_scroll+(my-listTop)/ROW_H;
                int trackX=W-6, trackY=listTop, trackH=listRows*ROW_H;
                if(nlist>listRows && mx>=trackX-2 && my>=trackY && my<=trackY+trackH){
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
        if(!paused && dt>0) world_tick(w, (float)(dt*speed));

        /* ── render world ── */
        G->begin(gfx_rgb(18,16,22));
        float tl_wx,tl_wy,br_wx,br_wy;
        s2w(&cam,0,0,&tl_wx,&tl_wy); s2w(&cam,(float)W,(float)Hs,&br_wx,&br_wy);
        int x0=(int)(tl_wx/TILE_PX)-1, x1=(int)(br_wx/TILE_PX)+1;
        int y0=(int)(tl_wy/TILE_PX)-1, y1=(int)(br_wy/TILE_PX)+1;
        if(x0<0)x0=0; if(y0<0)y0=0; if(x1>=WORLD_W)x1=WORLD_W-1; if(y1>=WORLD_H)y1=WORLD_H-1;
        GfxColor outline=gfx_rgb(12,10,16);
        float ts=TILE_PX*cam.zoom; int iw=(int)(ts+1.0f); if(iw<1)iw=1;
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

        /* ── zoom-aware building glyphs ── */
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

        /* ── HUD ── */
        char hud[256]; hud_string(w,hud,sizeof(hud),speed,paused,fps,G->name);
        G->fill_rect(0,0,W,24,gfx_rgba(0,0,0,170));
        G->text(hud,8,5,14,gfx_rgb(230,225,215));

        /* ── right panel: feed + citizens list ── */
        if(show_right){
            G->fill_rect(panelX,24,PANEL_W,Hs-24,gfx_rgba(24,22,30,225));
            G->text("EVENT FEED [Tab]",panelX+10,30,13,COL_GOLD);
            int yy=50;
            for(int i=0;i<FEED_LINES && i<w->ev_count;i++){ const WorldEvent *e=events_recent(w,i); if(!e)break;
                snprintf(buf,sizeof(buf),"%.44s",e->text); G->text(buf,panelX+10,yy,11,gfx_rgb(220,215,205)); yy+=15; }
            G->line(panelX+8,feedBottom+8,W-8,feedBottom+8,gfx_rgb(80,76,90));
            snprintf(buf,sizeof(buf),"CITIZENS (%d)",nlist); G->text(buf,panelX+10,feedBottom+12,13,COL_GOLD);
            for(int r=0;r<listRows && (list_scroll+r)<nlist;r++){ int idx=list_scroll+r;
                Agent *a=world_agent_by_id(w,ids[idx]); if(!a) continue;
                int ry=listTop+r*ROW_H;
                if(a->id==selected) G->fill_rect(panelX+2,ry,PANEL_W-14,ROW_H,gfx_rgb(60,56,40));
                G->fill_rect(panelX+8,ry+5,6,6,gfx_rgb(a->r,a->g,a->b));
                snprintf(buf,sizeof(buf),"%.22s",a->name); G->text(buf,panelX+18,ry+2,11,gfx_rgb(230,225,215));
                const char *tag=agent_tag(a);
                if(tag[0]){ GfxColor tc=a->wanted?COL_RED:a->arrested_ticks>0?COL_AMBER:gfx_rgb(150,145,135);
                    G->text(tag,W-12-G->text_w(tag,10),ry+3,10,tc); } }
            if(nlist>listRows){ int trackX=W-6, trackY=listTop, trackH=listRows*ROW_H;
                G->fill_rect(trackX,trackY,4,trackH,gfx_rgb(40,38,48));
                int thumbH=trackH*listRows/nlist; if(thumbH<10)thumbH=10;
                int thumbY=trackY+(trackH-thumbH)*list_scroll/(maxscroll>0?maxscroll:1);
                G->fill_rect(trackX,thumbY,4,thumbH,gfx_rgb(150,145,135)); }
        }

        /* ── jail roster ── */
        if(show_jail){
            int pw=680, ph=40+22+22*22, px=W/2-pw/2, py=60;
            G->fill_rect(px,py,pw,ph,COL_PANEL); G->rect_lines(px,py,pw,ph,COL_GOLD);
            snprintf(buf,sizeof(buf),"JAIL ROSTER (%d)  [J]",crime_jailed_count(w)); G->text(buf,px+12,py+10,15,COL_GOLD);
            G->text("NAME",px+12,py+34,10,COL_GRAY); G->text("CRIME",px+210,py+34,10,COL_GRAY);
            G->text("GANG/CULT",px+300,py+34,10,COL_GRAY); G->text("SERVED/SENTENCE",px+480,py+34,10,COL_GRAY);
            int yy=py+50, shown=0;
            for(int i=0;i<w->n_agents && shown<22;i++){ Agent *a=&w->agents[i];
                if(!a->alive||a->arrested_ticks<=0) continue;
                int tot=a->sentence_total>0?a->sentence_total:1, served=tot-a->arrested_ticks; if(served<0)served=0;
                snprintf(buf,sizeof(buf),"%.24s",a->name); G->text(buf,px+12,yy,12,COL_WHITE);
                G->text(a->jailed_for[0]?a->jailed_for:"-",px+210,yy,12,COL_WHITE);
                snprintf(buf,sizeof(buf),"%.22s",faction_name(w,a->faction_id));
                G->text(buf,px+300,yy,12,a->faction_id>=0?COL_PURPLE:COL_GRAY);
                snprintf(buf,sizeof(buf),"%d/%d (%d%%)",served,tot,served*100/tot);
                G->text(buf,px+480,yy,12,served*100/tot>=66?COL_GREEN:COL_WHITE);
                yy+=20; shown++; }
            if(!shown) G->text("No one is in jail.",px+12,yy,13,COL_GRAY);
        }

        /* ── factions menu ── */
        if(show_factions){
            int pw=560, ph=40+22+(w->n_factions+1)*22, px=W/2-pw/2, py=80;
            G->fill_rect(px,py,pw,ph,COL_PANEL); G->rect_lines(px,py,pw,ph,COL_PURPLE);
            G->text("FACTIONS  [F]",px+12,py+10,15,COL_PURPLE);
            G->text("NAME",px+12,py+34,10,COL_GRAY); G->text("KIND",px+260,py+34,10,COL_GRAY);
            G->text("MEMBERS",px+360,py+34,10,COL_GRAY); G->text("LEADER",px+450,py+34,10,COL_GRAY);
            int yy=py+50;
            for(int i=0;i<w->n_factions;i++){ Faction *f=&w->factions[i]; if(!f->active) continue;
                Agent *ldr=f->leader_id>=0?world_agent_by_id(w,f->leader_id):NULL;
                G->fill_rect(px+12,yy+3,8,8,gfx_rgb(f->r,f->g,f->b));
                snprintf(buf,sizeof(buf),"%.26s",f->name); G->text(buf,px+26,yy,12,COL_WHITE);
                G->text(f->is_cult?"cult":"gang",px+260,yy,12,gfx_rgb(200,180,150));
                snprintf(buf,sizeof(buf),"%d",f->members); G->text(buf,px+360,yy,12,COL_WHITE);
                if(ldr) snprintf(buf,sizeof(buf),"%.16s",ldr->name); else snprintf(buf,sizeof(buf),"-");
                G->text(buf,px+450,yy,12,COL_GRAY); yy+=22; }
        }

        /* ── inspector (left) ── */
        if(selected>=0){
            Agent *a=world_agent_by_id(w,selected);
            if(a && a->alive){
                int pw=300; G->fill_rect(0,24,pw,320,gfx_rgba(24,22,30,230));
                int yy=30;
                snprintf(buf,sizeof(buf),"%s  #%d",a->name,a->id); G->text(buf,10,yy,15,COL_GOLD); yy+=24;
                snprintf(buf,sizeof(buf),"Age %d   Action: %s",a->age,action_name(a->action)); G->text(buf,10,yy,12,COL_WHITE); yy+=18;
                snprintf(buf,sizeof(buf),"Money %.0f   Faction %d",a->needs.money,a->faction_id); G->text(buf,10,yy,12,COL_WHITE); yy+=18;
                if(a->is_police){ G->text("POLICE",10,yy,12,gfx_rgb(120,180,230)); yy+=18; }
                if(a->wanted){ snprintf(buf,sizeof(buf),"WANTED for %s",a->wanted_for); G->text(buf,10,yy,12,COL_RED); yy+=18; }
                if(a->arrested_ticks>0){ snprintf(buf,sizeof(buf),"JAILED for %s",a->jailed_for); G->text(buf,10,yy,12,COL_AMBER); yy+=18; }
                const char *lbl[6]={"hunger","energy","safety","social","meaning","belong"};
                double v[6]={a->needs.hunger,a->needs.energy,a->needs.safety,a->needs.social,a->needs.meaning,a->needs.belonging};
                for(int i=0;i<6;i++){ G->text(lbl[i],10,yy,11,COL_WHITE);
                    G->fill_rect(90,yy,180,8,gfx_rgb(50,50,60));
                    GfxColor c=v[i]<0.2?COL_RED:v[i]<0.4?COL_AMBER:COL_GREEN;
                    G->fill_rect(90,yy,(int)(180*v[i]),8,c); yy+=16; }
                int friends=0,rivals=0;
                for(int i=0;i<a->rels.n;i++){ if(a->rels.rel[i].affinity>=FRIENDSHIP_AFFINITY) friends++;
                    else if(a->rels.rel[i].affinity<=RIVALRY_AFFINITY) rivals++; }
                snprintf(buf,sizeof(buf),"Friends %d  Rivals %d  Known %d",friends,rivals,a->rels.n);
                G->text(buf,10,yy,12,COL_WHITE); yy+=18;
                G->text("arrows/wheel: browse   esc: close",10,yy+4,10,COL_GRAY);
            } else selected=-1;
        }

        /* ── god toolbar + badge + flash ── */
        if(god){
            int cw=92, ch=28, g2=6, total=G_NTOOLS*cw+(G_NTOOLS-1)*g2;
            int sx=W/2-total/2, yb=Hs-ch-12;
            G->fill_rect(sx-12,yb-8,total+24,ch+16,gfx_rgba(24,22,30,220));
            for(int i=0;i<G_NTOOLS;i++){ int cx=sx+i*(cw+g2); int sel=(i==tool);
                G->fill_rect(cx,yb,cw,ch, sel?gfx_rgba(212,175,90,230):gfx_rgba(32,30,38,220));
                if(sel) G->rect_lines(cx,yb,cw,ch,COL_WHITE);
                snprintf(buf,sizeof(buf),"%d %s",i+1,GOD_TOOL_NAME[i]);
                G->text(buf,cx+8,yb+8,12,sel?gfx_rgb(0,0,0):COL_WHITE); }
            const char *b="GOD MODE"; G->text(b,W/2-G->text_w(b,16)/2,28,16,COL_GOLD);
        }
        if(flash[0] && now_sec()<flash_until)
            G->text(flash,W/2-G->text_w(flash,16)/2,50,16,gfx_rgb(230,225,215));

        /* ── legend (bottom-left) ── */
        if(show_legend){
            int rows=(LEGEND_N+1)/2, rh=16, lw=168, lh=20+rows*rh;
            int lx=10, ly=Hs-lh-10;
            G->fill_rect(lx,ly,lw,lh,gfx_rgba(24,22,30,225));
            G->rect_lines(lx,ly,lw,lh,gfx_rgb(60,56,70));
            G->text("LEGEND  [L]",lx+8,ly+5,11,COL_GOLD);
            for(int i=0;i<LEGEND_N;i++){ int col=i%2, row=i/2;
                int ex=lx+8+col*80, ey=ly+22+row*rh;
                G->fill_rect(ex,ey,10,10,tile_col(LEGEND[i].type));
                G->rect_lines(ex,ey,10,10,gfx_rgba(10,8,14,180));
                G->text(LEGEND[i].label,ex+14,ey-1,11,gfx_rgb(225,220,210)); }
        }

        G->present();
        if(shot && ++frame==120){ if(G->screenshot) G->screenshot(shot); break; }
    }
    G->shutdown();
    return 0;
}
