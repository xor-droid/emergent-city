/* backend_tui.c — ncurses ASCII renderer. Compiled only when HAVE_TUI.
 *
 * Renders the city to the terminal in 256-color: tiles as colored glyphs,
 * agents as '@' tinted by mood. Runs anywhere (incl. over SSH, no display).
 * Keys: q quit, space pause, 1/2/3 speed, arrows pan.
 */
#include "sim.h"
#include "viz.h"
#include <ncurses.h>
#include <time.h>
#include <stdlib.h>

/* nearest xterm-256 cube color for an RGB triple */
static int rgb256(unsigned char r, unsigned char g, unsigned char b) {
    int R = r * 5 / 255, G = g * 5 / 255, B = b * 5 / 255;
    return 16 + 36 * R + 6 * G + B;
}

static char tile_glyph(TileType t) {
    switch (t) {
        case T_ROAD:   return ' ';
        case T_HOME:   return '#';
        case T_SHOP:   return '$';
        case T_WORK:   return '%';
        case T_BAR:    return '&';
        case T_CHURCH: return '+';
        default:       return '.';   /* grass */
    }
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int run_tui(World *w) {
    initscr();
    if (!has_colors()) { endwin(); fprintf(stderr, "terminal has no color support\n"); return 1; }
    start_color();
    use_default_colors();
    /* Map color pair i -> foreground xterm color i on the default background. */
    int npairs = COLOR_PAIRS < 256 ? COLOR_PAIRS : 256;
    for (int i = 1; i < npairs; i++) init_pair((short)i, (short)i, -1);
    cbreak(); noecho(); curs_set(0); keypad(stdscr, TRUE); nodelay(stdscr, TRUE);

    int cam_x = WORLD_W / 2, cam_y = WORLD_H / 2;  /* top-left of viewport */
    int paused = 0;
    float speed = 1.0f;
    double prev = now_sec();
    int running = 1, fps = 0, frames = 0; double facc = 0.0;

    while (running) {
        int ch;
        while ((ch = getch()) != ERR) {
            switch (ch) {
                case 'q': case 'Q': running = 0; break;
                case ' ': paused = !paused; break;
                case '1': speed = 1.0f; break;
                case '2': speed = 5.0f; break;
                case '3': speed = 20.0f; break;
                case KEY_LEFT:  cam_x -= 3; break;
                case KEY_RIGHT: cam_x += 3; break;
                case KEY_UP:    cam_y -= 3; break;
                case KEY_DOWN:  cam_y += 3; break;
            }
        }

        double t = now_sec();
        double dt = t - prev; prev = t;
        facc += dt; frames++;
        if (facc >= 0.5) { fps = (int)(frames / facc); frames = 0; facc = 0.0; }
        if (!paused && dt > 0.0) world_tick(w, dt * speed);

        int rows, cols; getmaxyx(stdscr, rows, cols);
        int view_rows = rows - 1;              /* row 0 = HUD */
        if (cam_x < 0) cam_x = 0; if (cam_x > WORLD_W - cols) cam_x = WORLD_W - cols;
        if (cam_x < 0) cam_x = 0;
        if (cam_y < 0) cam_y = 0; if (cam_y > WORLD_H - view_rows) cam_y = WORLD_H - view_rows;
        if (cam_y < 0) cam_y = 0;

        erase();
        /* tiles */
        for (int sy = 0; sy < view_rows; sy++) {
            int wy = cam_y + sy;
            if (wy >= WORLD_H) break;
            for (int sx = 0; sx < cols; sx++) {
                int wx = cam_x + sx;
                if (wx >= WORLD_W) break;
                TileType tt = (TileType)w->tile[wx][wy];
                unsigned char r, g, b; tile_rgb(tt, &r, &g, &b);
                int c = rgb256(r, g, b);
                attron(COLOR_PAIR(c));
                mvaddch(sy + 1, sx, tile_glyph(tt));
                attroff(COLOR_PAIR(c));
            }
        }
        /* agents on top */
        for (int i = 0; i < w->n_agents; i++) {
            Agent *a = &w->agents[i];
            if (!a->alive) continue;
            int sx = (int)a->x - cam_x, sy = (int)a->y - cam_y;
            if (sx < 0 || sx >= cols || sy < 0 || sy >= view_rows) continue;
            int c = rgb256(a->r, a->g, a->b);
            attron(COLOR_PAIR(c) | A_BOLD);
            mvaddch(sy + 1, sx, '@');
            attroff(COLOR_PAIR(c) | A_BOLD);
        }
        /* HUD */
        char hud[256];
        hud_string(w, hud, sizeof(hud), speed, paused, fps, "tui");
        attron(A_REVERSE);
        mvhline(0, 0, ' ', cols);
        mvaddnstr(0, 0, hud, cols);
        attroff(A_REVERSE);
        refresh();

        napms(33);  /* ~30 fps cap */
    }

    endwin();
    return 0;
}
