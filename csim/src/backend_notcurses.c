/* backend_notcurses.c — terminal renderer with full parity: God Mode, feed,
 * scrollable client list, jail roster, factions menu, inspector.
 *
 * Keyboard-driven (no mouse needed): the client list IS the selector/cursor.
 *   arrows / PgUp PgDn / Home End : browse the client list (selects + highlights)
 *   i inspect (same as selecting)   g god mode   1-8 pick tool
 *   enter : apply god tool at the selected citizen   Tab feed   j jail   f factions
 *   space pause   1/2/3 speed (outside god mode)   esc deselect   q quit
 */
#define _GNU_SOURCE
#include <wchar.h>
#include "sim.h"
#include "viz.h"
#include "llm.h"
#include <notcurses/notcurses.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#define PXT 3

static double now_sec(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return ts.tv_sec+ts.tv_nsec*1e-9; }
static void put(struct ncplane *n,int y,int x,uint32_t fg,uint32_t bg,const char*s){
    ncplane_set_fg_rgb(n,fg); ncplane_set_bg_rgb(n,bg); ncplane_putstr_yx(n,y,x,s);
}
static void fillrow(struct ncplane *n,int y,int x,int wdt,uint32_t bg){
    char sp[300]; if(wdt>299)wdt=299; for(int i=0;i<wdt;i++)sp[i]=' '; sp[wdt]='\0';
    ncplane_set_bg_rgb(n,bg); ncplane_set_fg_rgb(n,0xbbbbbb); ncplane_putstr_yx(n,y,x,sp);
}
static const char *fac_name(World*w,int fid){ return (fid<0||fid>=w->n_factions)?"-":w->factions[fid].name; }

/* Optional diagnostic log (CSIM_NCLOG=/path). No-op unless the env var is set. */
static FILE *g_nclog = NULL;
#define NCLOG(...) do{ if(g_nclog){ fprintf(g_nclog,__VA_ARGS__); fflush(g_nclog);} }while(0)

/* notcurses_init() blocks forever on its terminal-capability handshake (it waits
 * on a condvar for the terminal's DA1 reply, with no timeout — see
 * inputlayer_get_responses in notcurses' in.c). Some terminals (bare ptys, and
 * some WSL/Windows Terminal setups) never complete that reply, freezing the app
 * before it can render. We run init on a worker thread and give it a deadline;
 * if it stalls we restore the terminal and bail with guidance instead. */
struct nc_init_ctx { struct notcurses_options *opts; struct notcurses *nc;
                     int done; pthread_mutex_t m; pthread_cond_t c; };
static void *nc_init_worker(void *arg){
    struct nc_init_ctx *c = arg;
    struct notcurses *nc = notcurses_init(c->opts, NULL);
    pthread_mutex_lock(&c->m); c->nc = nc; c->done = 1;
    pthread_cond_signal(&c->c); pthread_mutex_unlock(&c->m);
    return NULL;
}

int run_notcurses(World *w) {
    const char *logpath = getenv("CSIM_NCLOG");
    if (logpath) { g_nclog = fopen(logpath,"w");
        NCLOG("[nclog] start  TERM=%s  COLORTERM=%s\n",
              getenv("TERM")?getenv("TERM"):"(unset)",
              getenv("COLORTERM")?getenv("COLORTERM"):"(unset)"); }

    struct notcurses_options opts; memset(&opts,0,sizeof(opts));
    opts.flags = NCOPTION_SUPPRESS_BANNERS;

    /* Watchdog-protected init (see nc_init_ctx comment). */
    double init_timeout = 4.0;
    { const char *e = getenv("CSIM_NC_INIT_TIMEOUT"); if (e){ double v=atof(e); if(v>0) init_timeout=v; } }
    struct nc_init_ctx ic; memset(&ic,0,sizeof(ic)); ic.opts=&opts;
    pthread_mutex_init(&ic.m,NULL); pthread_cond_init(&ic.c,NULL);
    pthread_t ith; pthread_create(&ith,NULL,nc_init_worker,&ic);
    struct timespec dl; clock_gettime(CLOCK_REALTIME,&dl); dl.tv_sec += (time_t)init_timeout;
    pthread_mutex_lock(&ic.m);
    int werr=0; while(!ic.done && werr==0) werr=pthread_cond_timedwait(&ic.c,&ic.m,&dl);
    struct notcurses *nc = ic.nc; int timed_out = !ic.done;
    pthread_mutex_unlock(&ic.m);
    if (timed_out) {
        pthread_detach(ith);  /* worker is wedged in notcurses' DA1 wait; it dies with the process */
        NCLOG("[nclog] notcurses_init TIMED OUT after %.1fs (terminal never answered the DA1 handshake)\n", init_timeout);
        const char *restore = "\033[?1049l\033[?25h\033[0m\r\n";  /* leave alt-screen, show cursor, reset attrs */
        ssize_t wn = write(STDERR_FILENO, restore, strlen(restore)); (void)wn;
        fprintf(stderr,
            "notcurses: your terminal did not complete the startup handshake within %.0fs.\n"
            "  TERM=%s is not answering notcurses' capability query (common on some WSL/\n"
            "  Windows Terminal and bare-pty setups). The notcurses backend can't run here.\n"
            "  Use the GPU backend instead:  ./build/csim --backend raylib\n"
            "  (If this terminal looks garbled now, run:  reset )\n",
            init_timeout, getenv("TERM")?getenv("TERM"):"(unset)");
        if (g_nclog) fclose(g_nclog);
        return 2;
    }
    pthread_join(ith,NULL);
    if (!nc) { fprintf(stderr, "notcurses_init failed\n"); NCLOG("[nclog] notcurses_init FAILED (returned NULL)\n"); if(g_nclog)fclose(g_nclog); return 1; }
    struct ncplane *std = notcurses_stdplane(nc);
    { unsigned ir,icol; ncplane_dim_yx(std,&ir,&icol);
      NCLOG("[nclog] init ok  stdplane=%ux%u (rows x cols)  isatty(stdout)=%d\n", ir, icol, isatty(1)); }

    /* Default to sextants: high-res AND composes with the HUD/panel text on the
     * std plane. NCBLIT_DEFAULT can auto-pick PIXEL graphics, which abort when
     * drawn on the same plane as text — so pixel is opt-in via --blit pixel. */
    ncblitter_e blit = NCBLIT_3x2;
    if (g_blit) {
        if(!strcmp(g_blit,"sextant"))blit=NCBLIT_3x2; else if(!strcmp(g_blit,"quad"))blit=NCBLIT_2x2;
        else if(!strcmp(g_blit,"half"))blit=NCBLIT_2x1; else if(!strcmp(g_blit,"braille"))blit=NCBLIT_BRAILLE;
        else if(!strcmp(g_blit,"ascii"))blit=NCBLIT_1x1; else if(!strcmp(g_blit,"pixel"))blit=NCBLIT_PIXEL;
        else if(!strcmp(g_blit,"auto"))blit=NCBLIT_DEFAULT;
    }

    const int W = WORLD_W*PXT, H = WORLD_H*PXT;
    unsigned char *buf = malloc((size_t)W*H*4);
    if (!buf) { notcurses_stop(nc); return 1; }

    int paused=0, running=1, fps=0, frames=0;
    float speed=1.0f;
    int god=0, tool=G_SMITE, show_right=1, show_jail=0, show_fac=0;
    int selected=-1, list_scroll=0;
    char flash[96]=""; double flash_until=0;
    double prev=now_sec(), facc=0;
    if (getenv("CSIM_DEMO")) { god=1; show_jail=1; }
    static int ids[MAX_AGENTS];

    while (running) {
        unsigned rows, cols; ncplane_dim_yx(std, &rows, &cols);
        int panelX = (int)cols - 34; if (panelX < 0) panelX = 0;
        int listTop = 10, listRows = (int)rows - listTop - 1; if (listRows < 1) listRows = 1;

        int nlist = 0;
        for (int i=0;i<w->n_agents;i++) if(w->agents[i].alive) ids[nlist++]=w->agents[i].id;
        int maxscroll = nlist-listRows; if(maxscroll<0) maxscroll=0;
        int selIdx=-1; for(int i=0;i<nlist;i++) if(ids[i]==selected){selIdx=i;break;}

        struct ncinput ni; uint32_t id;
        while ((id = notcurses_get_nblock(nc, &ni)) != 0) {
            NCLOG("[nclog] input id=%u (0x%x) evtype=%d\n", id, id, ni.evtype);
            if (ni.evtype == NCTYPE_RELEASE) continue;
            int nav = 0;
            if (id=='q'||id=='Q') running=0;
            else if (id==' ') paused=!paused;
            else if (id=='g'||id=='G') god=!god;
            else if (id==NCKEY_TAB) show_right=!show_right;
            else if (id=='j'||id=='J') show_jail=!show_jail;
            else if (id=='f'||id=='F') show_fac=!show_fac;
            else if (id=='i'||id=='I') { /* selection already is inspection */ }
            else if (id==NCKEY_ESC) selected=-1;
            else if (id==NCKEY_DOWN) nav=1;
            else if (id==NCKEY_UP) nav=-1;
            else if (id==NCKEY_PGDOWN) nav=listRows;
            else if (id==NCKEY_PGUP) nav=-listRows;
            else if (id==NCKEY_HOME) nav=-1000000;
            else if (id==NCKEY_END) nav=1000000;
            else if (id==NCKEY_ENTER && god && selected>=0) {
                Agent *a=world_agent_by_id(w,selected);
                if(a){ god_apply(w,tool,(int)a->x,(int)a->y,flash,sizeof(flash)); flash_until=now_sec()+2.5; }
            }
            else if (id>='1'&&id<='8') { if(god) tool=id-'1';
                else if(id=='1')speed=1; else if(id=='2')speed=5; else if(id=='3')speed=20; }
            if (nav && nlist) {
                int base = selIdx<0?0:selIdx; int ni2 = base+nav;
                if (ni2<0) ni2=0; if (ni2>=nlist) ni2=nlist-1;
                selIdx=ni2; selected=ids[ni2];
                if (selIdx<list_scroll) list_scroll=selIdx;
                if (selIdx>=list_scroll+listRows) list_scroll=selIdx-listRows+1;
            }
        }
        if (list_scroll>maxscroll) list_scroll=maxscroll; if(list_scroll<0) list_scroll=0;

        double t=now_sec(), dt=t-prev; prev=t; facc+=dt; frames++;
        if (facc>=0.5){ fps=(int)(frames/facc); frames=0; facc=0; }
        if (!paused && dt>0) world_tick(w, dt*speed);

        /* paint bitmap */
        for (int x=0;x<WORLD_W;x++) for(int y=0;y<WORLD_H;y++){
            unsigned char r,g,b; tile_rgb((TileType)w->tile[x][y],&r,&g,&b);
            for(int py=0;py<PXT;py++) for(int px=0;px<PXT;px++){
                size_t o=((size_t)(y*PXT+py)*W+(x*PXT+px))*4; buf[o]=r;buf[o+1]=g;buf[o+2]=b;buf[o+3]=255; } }
        for (int i=0;i<w->n_agents;i++){ Agent*a=&w->agents[i]; if(!a->alive)continue;
            int bx=(int)a->x*PXT, by=(int)a->y*PXT;
            for(int py=0;py<PXT;py++) for(int px=0;px<PXT;px++){ int X=bx+px,Y=by+py;
                if(X<0||X>=W||Y<0||Y>=H)continue; size_t o=((size_t)Y*W+X)*4; buf[o]=a->r;buf[o+1]=a->g;buf[o+2]=a->b;buf[o+3]=255; } }
        /* selected-agent crosshair */
        if (selected>=0) { Agent*a=world_agent_by_id(w,selected);
            if(a&&a->alive){ int bx=(int)a->x*PXT,by=(int)a->y*PXT;
                for(int d=-PXT;d<=2*PXT;d++){ int X=bx+d,Y=by+PXT/2; if(X>=0&&X<W&&Y>=0&&Y<H){size_t o=((size_t)Y*W+X)*4;buf[o]=255;buf[o+1]=255;buf[o+2]=0;buf[o+3]=255;}
                    X=bx+PXT/2;Y=by+d; if(X>=0&&X<W&&Y>=0&&Y<H){size_t o=((size_t)Y*W+X)*4;buf[o]=255;buf[o+1]=255;buf[o+2]=0;buf[o+3]=255;} } } }

        struct ncvisual *v = ncvisual_from_rgba(buf, H, W*4, W);
        struct ncplane *blitp = NULL;
        if (v) { struct ncvisual_options vo; memset(&vo,0,sizeof(vo));
            vo.n=std; vo.scaling=NCSCALE_SCALE; vo.blitter=blit; blitp = ncvisual_blit(nc,v,&vo); ncvisual_destroy(v); }
        if (frames<=3) NCLOG("[nclog] frame %d dims=%ux%u from_rgba=%s blit=%s blitter=%d\n",
                             frames, rows, cols, v?"ok":"NULL", blitp?"ok":"NULL", (int)blit);

        /* HUD */
        char hud[256]; hud_string(w,hud,sizeof(hud),speed,paused,fps,"notcurses");
        fillrow(std,0,0,(int)cols,0x000000); put(std,0,0,0xe6e1d7,0x000000,hud);

        /* right column: feed (top) + client list (bottom) */
        if (show_right) {
            for (int r=1;r<(int)rows;r++) fillrow(std,r,panelX,34,0x18161e);
            put(std,1,panelX+1,0xd4af5a,0x18161e,"EVENT FEED [Tab]");
            for (int i=0;i<7 && i<w->ev_count;i++){ const WorldEvent*e=events_recent(w,i); if(!e)break;
                char ln[36]; snprintf(ln,sizeof(ln),"%.32s",e->text); put(std,2+i,panelX+1,0xd0cbc0,0x18161e,ln); }
            char ch[36]; snprintf(ch,sizeof(ch),"CITIZENS (%d)",nlist);
            put(std,9,panelX+1,0xd4af5a,0x18161e,ch);
            for (int r=0;r<listRows && list_scroll+r<nlist;r++){ int idx=list_scroll+r;
                Agent*a=world_agent_by_id(w,ids[idx]); if(!a)continue; int ry=listTop+r;
                uint32_t bg = (a->id==selected)?0x3c3828:0x18161e;
                fillrow(std,ry,panelX,34,bg);
                const char*tag = a->arrested_ticks>0?"JAIL":a->wanted?"WANT":a->is_police?"pol":a->faction_id>=0?"fac":"";
                char ln[40]; snprintf(ln,sizeof(ln),"%-24.24s %s",a->name,tag);
                put(std,ry,panelX+1,(a->id==selected)?0xffffff:0xe6e1d7,bg,ln); }
            /* scroll indicator */
            if (nlist>listRows){ char si[40]; snprintf(si,sizeof(si),"%d-%d/%d",list_scroll+1,list_scroll+listRows,nlist);
                put(std,9,(int)cols-(int)strlen(si)-1,0x8a8780,0x18161e,si); }
        }

        /* god toolbar */
        if (god) {
            fillrow(std,rows-1,0,(int)cols,0x181620);
            int x=0; put(std,rows-1,x,0xd4af5a,0x181620,"GOD "); x+=4;
            for(int i=0;i<G_NTOOLS;i++){ char seg[20]; snprintf(seg,sizeof(seg),"%d%s ",i+1,GOD_TOOL_NAME[i]);
                put(std,rows-1,x,i==tool?0x141216:0xe6e1d7,i==tool?0xd4af5a:0x181620,seg); x+=(int)strlen(seg); }
            put(std,1,2,0xd4af5a,0x000000,"GOD MODE  (browse list, enter applies to selected)");
        }
        if (flash[0] && now_sec()<flash_until) put(std,2,2,0xe6e1d7,0x000000,flash);

        /* jail roster (center) */
        if (show_jail) {
            int pw=58, ph=4+16, px=(int)cols/2-pw/2, py=3; if(px<0)px=0;
            for(int r=0;r<ph && py+r<(int)rows-1;r++) fillrow(std,py+r,px,pw,0x18161e);
            char ti[64]; snprintf(ti,sizeof(ti),"JAIL ROSTER (%d) [j]",crime_jailed_count(w));
            put(std,py,px+1,0xd4af5a,0x18161e,ti);
            put(std,py+1,px+1,0x8a8780,0x18161e,"NAME            CRIME     GANG/CULT    SERV/SENT");
            int yy=py+2, shown=0;
            for(int i=0;i<w->n_agents && shown<14 && yy<(int)rows-2;i++){ Agent*a=&w->agents[i];
                if(!a->alive||a->arrested_ticks<=0)continue;
                int tot=a->sentence_total>0?a->sentence_total:1, srv=tot-a->arrested_ticks; if(srv<0)srv=0;
                char ln[72]; snprintf(ln,sizeof(ln),"%-14.14s  %-8.8s  %-10.10s  %d/%d",
                    a->name,a->jailed_for[0]?a->jailed_for:"-",fac_name(w,a->faction_id),srv,tot);
                put(std,yy++,px+1,0xe6e1d7,0x18161e,ln); shown++; }
            if(!shown) put(std,py+2,px+1,0x8a8780,0x18161e,"No one is in jail.");
        }

        /* factions menu (center) */
        if (show_fac) {
            int pw=52, ph=3+w->n_factions, px=(int)cols/2-pw/2, py=4; if(px<0)px=0;
            for(int r=0;r<ph && py+r<(int)rows-1;r++) fillrow(std,py+r,px,pw,0x18161e);
            put(std,py,px+1,0xc878dc,0x18161e,"FACTIONS [f]");
            put(std,py+1,px+1,0x8a8780,0x18161e,"NAME                 KIND  MEMBERS");
            int yy=py+2;
            for(int i=0;i<w->n_factions && yy<(int)rows-2;i++){ Faction*f=&w->factions[i]; if(!f->active)continue;
                char ln[64]; snprintf(ln,sizeof(ln),"%-20.20s %-5s %d",f->name,f->is_cult?"cult":"gang",f->members);
                put(std,yy++,px+1,0xe6e1d7,0x18161e,ln); }
        }

        /* inspector (left) */
        if (selected>=0) { Agent*a=world_agent_by_id(w,selected);
            if(a&&a->alive){ int pw=30,ph=12,px=0,py=2;
                for(int r=0;r<ph;r++) fillrow(std,py+r,px,pw,0x18161e);
                char l[48];
                snprintf(l,sizeof(l),"%.20s #%d",a->name,a->id); put(std,py,px+1,0xd4af5a,0x18161e,l);
                snprintf(l,sizeof(l),"age %d  %s",a->age,action_name(a->action)); put(std,py+1,px+1,0xe6e1d7,0x18161e,l);
                snprintf(l,sizeof(l),"money %.0f  %s",a->needs.money,fac_name(w,a->faction_id)); put(std,py+2,px+1,0xe6e1d7,0x18161e,l);
                if(a->wanted){snprintf(l,sizeof(l),"WANTED: %s",a->wanted_for); put(std,py+3,px+1,0xe65a50,0x18161e,l);}
                else if(a->arrested_ticks>0){snprintf(l,sizeof(l),"JAILED: %s",a->jailed_for); put(std,py+3,px+1,0xdcb45a,0x18161e,l);}
                const char*lb[6]={"hun","ene","saf","soc","mea","bel"};
                double vv[6]={a->needs.hunger,a->needs.energy,a->needs.safety,a->needs.social,a->needs.meaning,a->needs.belonging};
                for(int i=0;i<6;i++){ char b2[11]; int fl=(int)(vv[i]*10); for(int k=0;k<10;k++)b2[k]=k<fl?'#':'.'; b2[10]='\0';
                    char bar[24]; snprintf(bar,sizeof(bar),"%s %s",lb[i],b2); put(std,py+4+i,px+1,0xd0cbc0,0x18161e,bar); }
                put(std,py+11,px+1,0x8a8780,0x18161e,"esc close");
            } else selected=-1;
        }

        int rr = notcurses_render(nc);
        if (frames<=3) NCLOG("[nclog] frame %d notcurses_render=%d\n", frames, rr);
        struct timespec slp={0,33*1000*1000}; nanosleep(&slp,NULL);
    }
    free(buf);
    NCLOG("[nclog] clean exit (q)\n"); if(g_nclog) fclose(g_nclog);
    notcurses_stop(nc);
    return 0;
}
