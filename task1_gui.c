/*
 * task1_gui.c
 * TASK 1 - X11 window, tabs, text buffer per tab (screen model), drawing, mouse, select()-based event loop, main(), thread/process mode start-up.
 */
#include "terminator.h"

Line *scr_line(Scr *s, int k) { return &s->ln[(s->base + k) % SCR_MAXLINES]; }

static void scr_clear_all(Scr *s)
{
    int i;
    for (i = 0; i < SCR_MAXLINES; i++) s->ln[i].len = 0;
    s->base = 0; s->n = 1; s->col = 0; s->first_abs = 0;
}

void scr_init(Scr *s)
{
    memset(s, 0, sizeof *s);
    s->ln = xmalloc(sizeof(Line) * SCR_MAXLINES);
    memset(s->ln, 0, sizeof(Line) * SCR_MAXLINES);
    s->n = 1;
    snprintf(s->title, sizeof s->title, "~");
}

static void scr_free(Scr *s)
{
    int i;
    for (i = 0; i < SCR_MAXLINES; i++) free(s->ln[i].s);
    free(s->ln);
    s->ln = NULL;
}

static void scr_newline(Scr *s)
{
    if (s->n == SCR_MAXLINES) { s->base = (s->base + 1) % SCR_MAXLINES; s->first_abs++; scr_line(s, s->n - 1)->len = 0; }
    else { s->n++; scr_line(s, s->n - 1)->len = 0; }
    s->col = 0;
}

static void line_set(Line *l, int col, char c)
{
    if (col >= l->cap) { l->cap = col + 64; l->s = xrealloc(l->s, (size_t)l->cap); }
    while (l->len < col) l->s[l->len++] = ' ';
    if (col == l->len) l->len++;
    l->s[col] = c;
}

static void scr_putc(Scr *s, unsigned char c)
{
    Line *l = scr_line(s, s->n - 1);
    if (c == '\n') { scr_newline(s); return; }
    if (c == '\r') { s->col = 0; return; }
    if (c == '\t') { do line_set(l, s->col++, ' '); while (s->col % 8); return; }
    if (c == '\b') { if (s->col > 0) s->col--; return; }
    if (c < 32 || c == 127) return;
    line_set(l, s->col++, c < 127 ? (char)c : '?');
}

static void scr_csi(Scr *s, char fin)
{
    int n = atoi(s->eb);
    Line *l = scr_line(s, s->n - 1);
    if (fin == 'K') { if (l->len > s->col) l->len = s->col; }
    else if (fin == 'G') { s->col = n > 0 ? n - 1 : 0; }
    else if (fin == 'J' && n == 2) scr_clear_all(s);
}

static void scr_osc(Scr *s)
{
    s->eb[s->en] = 0;
    if (s->eb[0] == '0' && s->eb[1] == ';') snprintf(s->title, sizeof s->title, "%s", s->eb + 2);
    else if (s->eb[0] == 'B') s->want_reload = 1;
    else if (s->eb[0] == 'X') s->want_close = 1;
}

void scr_feed(Scr *s, const unsigned char *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char c = p[i];
        switch (s->st) {
        case 0: if (c == 27) s->st = 1; else scr_putc(s, c); break;
        case 1: if (c == '[') { s->st = 2; s->en = 0; } else if (c == ']') { s->st = 3; s->en = 0; } else s->st = 0; break;
        case 2:
            if ((c >= '0' && c <= '9') || c == ';') { if (s->en < (int)sizeof s->eb - 1) s->eb[s->en++] = (char)c; s->eb[s->en] = 0; }
            else { s->eb[s->en] = 0; scr_csi(s, (char)c); s->st = 0; }
            break;
        default:
            if (c == 7) { scr_osc(s); s->st = 0; }
            else if (s->en < (int)sizeof s->eb - 1) s->eb[s->en++] = (char)c;
            break;
        }
    }
}

static void scr_puts(Scr *s, const char *t) { scr_feed(s, (const unsigned char *)t, strlen(t)); }

GTab *gt[MAXTABS];

int ngt;

int cur_tab;

static int use_process;

static int quit_flag;

int dirty = 1;

static char start_cwd[PATH_MAX];

Display *dpy;

Window win;

static GC gc;

static Pixmap pix;

static XFontStruct *font;

int cw;

int fh;

static int asc;

static int WW = 900;

static int WH = 600;

int barh;

static int cols = 80;

int rows = 24;

static unsigned long c_bg;

static unsigned long c_fg;

static unsigned long c_bar;

static unsigned long c_act;

static unsigned long c_sel;

static unsigned long c_cur;

static unsigned long c_dim;

Atom a_clip;

Atom a_utf8;

Atom a_targets;

Atom a_prop;

static Atom a_wmdel;

Atom a_primary;

RowRef vis[512];

 int nvis;

static int tabx0[MAXTABS + 1];

static int tabx1[MAXTABS + 1];

            static int selecting;

int sel_on;

long sel_a_abs;

long sel_b_abs;

 int sel_a_col;

int sel_b_col;

 GTab *sel_tab;

char *sel_text;

                                         /* what we offer as the X selection */

void send_keys(GTab *g, const char *p, size_t n) { if (write_all(g->kfd, p, n) < 0) { } }

void send_act(GTab *g, int a) { char b[2] = { 27, (char)('0' + a) }; send_keys(g, b, 2); }

static void sigchld_reap(int s) { int e = errno; (void)s; while (waitpid(-1, NULL, WNOHANG) > 0) { } errno = e; }

static void *engine_thread(void *arg)
{
    EngineArgs *ea = arg;
    engine_run(ea);
    free(ea);
    __sync_fetch_and_sub(&g_engines, 1);
    return NULL;
}

GTab *gui_new_tab(void)
{
    int k[2], o[2], i;
    GTab *g;
    EngineArgs *ea;
    if (ngt >= MAXTABS) return NULL;
    if (mkpipe(k) < 0) return NULL;
    if (mkpipe(o) < 0) { close(k[0]); close(k[1]); return NULL; }
    ea = xmalloc(sizeof *ea);
    ea->kfd = k[0]; ea->ofd = o[1];
    snprintf(ea->cwd, sizeof ea->cwd, "%s", start_cwd);
    g = xmalloc(sizeof *g);
    memset(g, 0, sizeof *g);
    scr_init(&g->scr);
    g->kfd = k[1]; g->ofd = o[0];
    if (use_process) {                                        /* process mode: one forked process per tab */
        pid_t pid;
        pthread_mutex_lock(&spawn_lock);
        pid = fork();
        if (pid == 0) {
            signal(SIGCHLD, SIG_DFL);
            if (dpy) close(ConnectionNumber(dpy));             /* only the GUI process talks to X11 */
            for (i = 0; i < ngt; i++) { close(gt[i]->kfd); close(gt[i]->ofd); }
            close(k[1]); close(o[0]);
            pthread_mutex_init(&spawn_lock, NULL);
            engine_run(ea);
            _exit(0);
        }
        pthread_mutex_unlock(&spawn_lock);
        if (pid < 0) { close(k[0]); close(k[1]); close(o[0]); close(o[1]); free(ea); scr_free(&g->scr); free(g); return NULL; }
        g->pid = pid;
        close(k[0]); close(o[1]);
        free(ea);
    } else {                                                   /* thread mode: one thread per tab */
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&at, 8 * 1024 * 1024);
        __sync_fetch_and_add(&g_engines, 1);
        if (pthread_create(&th, &at, engine_thread, ea) != 0) {
            __sync_fetch_and_sub(&g_engines, 1);
            pthread_attr_destroy(&at);
            close(k[0]); close(k[1]); close(o[0]); close(o[1]); free(ea); scr_free(&g->scr); free(g);
            return NULL;
        }
        pthread_attr_destroy(&at);
    }
    set_nonblock(g->ofd);
    gt[ngt++] = g;
    cur_tab = ngt - 1;
    dirty = 1;
    return g;
}

void gui_close_tab(int i)
{
    GTab *g = gt[i];
    if (sel_tab == g) { sel_on = 0; selecting = 0; sel_tab = NULL; }
    close(g->kfd); close(g->ofd);                             /* the engine sees EOF, kills its jobs and ends */
    scr_free(&g->scr);
    free(g);
    memmove(&gt[i], &gt[i + 1], sizeof(GTab *) * (size_t)(ngt - i - 1));
    ngt--;
    if (cur_tab >= ngt) cur_tab = ngt - 1;
    if (cur_tab < 0) cur_tab = 0;
    if (ngt == 0) quit_flag = 1;
    dirty = 1;
}

static void load_colors(void)
{
    Colormap cm = DefaultColormap(dpy, DefaultScreen(dpy));
    XColor c, e;
    unsigned long *dst[] = { &c_bg, &c_fg, &c_bar, &c_act, &c_sel, &c_cur, &c_dim };
    const char *names[] = { "#101418", "#e0e0e0", "#262b31", "#3d4650", "#2f5f9f", "#d0d0d0", "#8a949e" };
    int i;
    for (i = 0; i < 7; i++) {
        if (XAllocNamedColor(dpy, cm, names[i], &c, &e)) *dst[i] = c.pixel;
        else *dst[i] = (i == 0) ? BlackPixel(dpy, DefaultScreen(dpy)) : WhitePixel(dpy, DefaultScreen(dpy));
    }
}

long row_abs(GTab *g, int line) { return g->scr.first_abs + line; }

static void gui_layout(void)
{
    cols = (WW - 8) / cw; if (cols < 10) cols = 10;
    rows = (WH - barh - 4) / fh; if (rows < 2) rows = 2;
    if (rows > 500) rows = 500;
}

static void gui_draw(void)
{
    GTab *g;
    Scr *s;
    int i, k, x, count = 0, C = cols, R = rows, last, scroll;
    RowRef *tmp;
    int need;
    long sa = 0, sb = 0; int sac = 0, sbc = 0, hassel = 0;
    if (!pix || ngt == 0) return;
    g = gt[cur_tab];
    s = &g->scr;
    XSetForeground(dpy, gc, c_bg);
    XFillRectangle(dpy, pix, gc, 0, 0, (unsigned)WW, (unsigned)WH);
    /* tab bar */
    XSetForeground(dpy, gc, c_bar);
    XFillRectangle(dpy, pix, gc, 0, 0, (unsigned)WW, (unsigned)barh);
    x = 4;
    for (i = 0; i <= ngt; i++) {
        char lab[PATH_MAX + 32];
        int w;
        if (i < ngt) snprintf(lab, sizeof lab, "[ %d: %s ]", i + 1, gt[i]->scr.title);
        else snprintf(lab, sizeof lab, "[ + ]");
        if (strlen(lab) > 28) { lab[25] = '.'; lab[26] = '.'; lab[27] = ']'; lab[28] = 0; }
        w = (int)strlen(lab) * cw + 8;
        tabx0[i] = x; tabx1[i] = x + w;
        if (x + w > WW) { tabx1[i] = x + w; }
        if (i == cur_tab && i < ngt) { XSetForeground(dpy, gc, c_act); XFillRectangle(dpy, pix, gc, x, 2, (unsigned)w, (unsigned)(barh - 2)); }
        XSetForeground(dpy, gc, i == cur_tab ? c_fg : c_dim);
        XSetBackground(dpy, gc, i == cur_tab ? c_act : c_bar);
        XDrawImageString(dpy, pix, gc, x + 4, 3 + asc, lab, (int)strlen(lab));
        x += w + 2;
    }
    /* text */
    need = R + g->scroll + 1;
    tmp = xmalloc(sizeof(RowRef) * (size_t)(need + 2));
    last = s->n - 1;
    for (i = last; i >= 0 && count < need; i--) {
        Line *l = scr_line(s, i);
        int len = l->len, nr, r;
        if (i == last) { int m = len > s->col ? len : s->col; nr = m / C + 1; }
        else nr = len == 0 ? 1 : (len + C - 1) / C;
        for (r = nr - 1; r >= 0 && count < need; r--) { tmp[count].line = i; tmp[count].col0 = r * C; count++; }
    }
    scroll = g->scroll;
    if (scroll > count - R) scroll = count - R;
    if (scroll < 0) scroll = 0;
    g->scroll = scroll;
    nvis = 0;
    {
        int top = scroll + R - 1;
        if (top > count - 1) top = count - 1;
        for (k = top; k >= scroll && nvis < 512; k--) vis[nvis++] = tmp[k];
    }
    free(tmp);
    if (sel_on && sel_tab == g) { sel_range(&sa, &sac, &sb, &sbc); hassel = 1; }
    for (i = 0; i < nvis; i++) {
        Line *l = scr_line(s, vis[i].line);
        int c0 = vis[i].col0, c1 = c0 + C, y = barh + 2 + i * fh + asc;
        long ab = row_abs(g, vis[i].line);
        int a = c0, b = l->len < c1 ? l->len : c1;
        int s0 = 0, s1 = 0;
        if (b < a) b = a;
        if (hassel && ab >= sa && ab <= sb) {
            s0 = (ab == sa) ? sac : 0;
            s1 = (ab == sb) ? sbc : (l->len > 0 ? l->len : 0);
            if (s0 < c0) s0 = c0;
            if (s1 > b) s1 = b;
        }
        if (s1 > s0) {
            if (s0 > a) { XSetForeground(dpy, gc, c_fg); XSetBackground(dpy, gc, c_bg); XDrawImageString(dpy, pix, gc, 4 + (a - c0) * cw, y, l->s + a, s0 - a); }
            XSetForeground(dpy, gc, c_fg); XSetBackground(dpy, gc, c_sel);
            XDrawImageString(dpy, pix, gc, 4 + (s0 - c0) * cw, y, l->s + s0, s1 - s0);
            if (b > s1) { XSetForeground(dpy, gc, c_fg); XSetBackground(dpy, gc, c_bg); XDrawImageString(dpy, pix, gc, 4 + (s1 - c0) * cw, y, l->s + s1, b - s1); }
        } else if (b > a) {
            XSetForeground(dpy, gc, c_fg); XSetBackground(dpy, gc, c_bg);
            XDrawImageString(dpy, pix, gc, 4 + 0, y, l->s + a, b - a);
        }
        if (g->scroll == 0 && vis[i].line == last && s->col / C * C == c0) {      /* cursor */
            int cx = 4 + (s->col - c0) * cw;
            char ch = (s->col < l->len) ? l->s[s->col] : ' ';
            XSetForeground(dpy, gc, c_cur);
            XFillRectangle(dpy, pix, gc, cx, barh + 2 + i * fh, (unsigned)cw, (unsigned)fh);
            XSetForeground(dpy, gc, c_bg); XSetBackground(dpy, gc, c_cur);
            XDrawImageString(dpy, pix, gc, cx, y, &ch, 1);
        }
    }
    XCopyArea(dpy, pix, win, gc, 0, 0, (unsigned)WW, (unsigned)WH, 0, 0);
    XFlush(dpy);
}

static void make_pixmap(void)
{
    if (pix) XFreePixmap(dpy, pix);
    pix = XCreatePixmap(dpy, win, (unsigned)WW, (unsigned)WH, (unsigned)DefaultDepth(dpy, DefaultScreen(dpy)));
    gui_layout();
    dirty = 1;
}

void switch_tab(int i) { if (i >= 0 && i < ngt) { cur_tab = i; dirty = 1; } }

static void on_button(XButtonEvent *be, int press)
{
    int i;
    if (!ngt) return;
    if (press && (be->button == Button4 || be->button == Button5)) {
        GTab *g = gt[cur_tab];
        g->scroll += be->button == Button4 ? 3 : -3;
        if (g->scroll < 0) g->scroll = 0;
        dirty = 1;
        return;
    }
    if (be->y < barh) {                                                    /* tab bar */
        if (!press) return;
        for (i = 0; i <= ngt; i++)
            if (be->x >= tabx0[i] && be->x < tabx1[i]) {
                if (i == ngt) { if (be->button == Button1) gui_new_tab(); }
                else if (be->button == Button2) gui_close_tab(i);
                else switch_tab(i);
                dirty = 1;
                return;
            }
        return;
    }
    if (be->button == Button1) {
        long a; int c;
        if (press) {
            if (pos_from_xy(be->x, be->y, &a, &c)) { sel_a_abs = sel_b_abs = a; sel_a_col = sel_b_col = c; selecting = 1; sel_on = 0; sel_tab = gt[cur_tab]; }
        } else if (selecting) {
            selecting = 0;
            if (sel_a_abs == sel_b_abs && sel_a_col == sel_b_col) sel_on = 0;
            else { sel_on = 1; free(sel_text); sel_text = selection_text(); if (sel_text) XSetSelectionOwner(dpy, a_primary, win, CurrentTime); }
            dirty = 1;
        }
    } else if (be->button == Button2 && press) do_paste(a_primary);
}

static void on_motion(XMotionEvent *me)
{
    long a; int c;
    if (!selecting || !ngt) return;
    if (pos_from_xy(me->x, me->y, &a, &c)) { sel_b_abs = a; sel_b_col = c; sel_on = 1; dirty = 1; }
}

static void on_event(XEvent *ev)
{
    switch (ev->type) {
    case Expose: dirty = 1; break;
    case ConfigureNotify:
        if (ev->xconfigure.width != WW || ev->xconfigure.height != WH) { WW = ev->xconfigure.width; WH = ev->xconfigure.height; make_pixmap(); }
        break;
    case KeyPress: on_key(&ev->xkey); break;
    case ButtonPress: on_button(&ev->xbutton, 1); break;
    case ButtonRelease: on_button(&ev->xbutton, 0); break;
    case MotionNotify: on_motion(&ev->xmotion); break;
    case SelectionRequest: on_selection_request(&ev->xselectionrequest); break;
    case SelectionNotify: on_selection_notify(&ev->xselection); break;
    case SelectionClear: if (ev->xselectionclear.selection == a_primary) { sel_on = 0; dirty = 1; } break;
    case ClientMessage: if ((Atom)ev->xclient.data.l[0] == a_wmdel) quit_flag = 1; break;
    default: break;
    }
}

static void shutdown_tabs(void)
{
    int i, waited = 0;
    for (i = 0; i < ngt; i++) { close(gt[i]->kfd); close(gt[i]->ofd); }
    if (use_process) {
        while (waited < 100) { pid_t r = waitpid(-1, NULL, WNOHANG); if (r < 0) break; if (r == 0) { usleep(10000); waited++; } }
    } else {
        while (g_engines > 0 && waited < 100) { usleep(10000); waited++; }          /* engines kill their jobs on EOF */
    }
    hist_rewrite_file();
}

static void read_tab(int i)
{
    GTab *g = gt[i];
    unsigned char tmp[8192];
    for (;;) {
        ssize_t n = read(g->ofd, tmp, sizeof tmp);
        if (n > 0) { scr_feed(&g->scr, tmp, (size_t)n); if (i == cur_tab) { g->scroll = g->scroll; dirty = 1; } if (g->scr.want_close) break; continue; }
        if (n == 0) { g->scr.want_close = 1; }
        break;
    }
    if (g->scr.want_reload) {
        BindDef nb[A_COUNT];
        binds_load(nb, NULL);
        memcpy(binds, nb, sizeof binds);
        g->scr.want_reload = 0;
    }
    if (i != cur_tab) dirty = 1;      /* tab titles may have changed */
    else dirty = 1;
}

static int pick_font(void)
{
    static const char *const names[] = { "9x15", "-misc-fixed-medium-r-normal--15-140-75-75-c-90-iso8859-1",
        "-*-fixed-medium-r-*-*-15-*-*-*-*-*-*-*", "10x20", "fixed", NULL };
    int i;
    for (i = 0; names[i]; i++) { font = XLoadQueryFont(dpy, names[i]); if (font) return 1; }
    return 0;
}

int main(int argc, char **argv)
{
    int i, xfd;
    XEvent ev;
    XSetWindowAttributes swa;
    Buf rcmsg = {0};
    XGCValues gv;

    for (i = 1; i < argc; i++) {
        if (!strcasecmp(argv[i], "useProcess=True") || !strcasecmp(argv[i], "--useProcess=True")) use_process = 1;
        else if (!strcasecmp(argv[i], "useProcess=False")) use_process = 0;
        else { fprintf(stderr, "usage: %s [useProcess=True]\n", argv[0]); return 2; }
    }
    signal(SIGPIPE, SIG_IGN);
    if (!getcwd(start_cwd, sizeof start_cwd)) snprintf(start_cwd, sizeof start_cwd, "%s", home_dir());
    if (hist_init() < 0) return 1;
    binds_load(binds, &rcmsg);
    if (use_process) signal(SIGCHLD, sigchld_reap);

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "terminator: cannot open the X display. On macOS start XQuartz first (and make sure $DISPLAY is set).\n");
        return 1;
    }
    if (!pick_font()) { fprintf(stderr, "terminator: no usable X11 font found\n"); return 1; }
    cw = font->max_bounds.width; asc = font->ascent; fh = font->ascent + font->descent;
    barh = fh + 8;
    load_colors();
    swa.background_pixel = c_bg;
    swa.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | Button1MotionMask | StructureNotifyMask;
    win = XCreateWindow(dpy, DefaultRootWindow(dpy), 0, 0, (unsigned)WW, (unsigned)WH, 0, CopyFromParent, InputOutput,
                        CopyFromParent, CWBackPixel | CWEventMask, &swa);
    XStoreName(dpy, win, "Terminator");
    a_wmdel = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &a_wmdel, 1);
    a_clip = XInternAtom(dpy, "CLIPBOARD", False);
    a_primary = XA_PRIMARY;
    a_utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    a_targets = XInternAtom(dpy, "TARGETS", False);
    a_prop = XInternAtom(dpy, "TERMINATOR_PASTE", False);
    gv.font = font->fid;
    gc = XCreateGC(dpy, win, GCFont, &gv);
    XMapWindow(dpy, win);
    make_pixmap();
    xfd = ConnectionNumber(dpy);

    if (!gui_new_tab()) { fprintf(stderr, "terminator: cannot create the first tab\n"); return 1; }
    if (rcmsg.n) {                                          /* problems found in ~/.terminatorrc */
        char *p = rcmsg.p, *nl;
        char line[600];
        while (*p) {
            nl = strchr(p, '\n');
            snprintf(line, sizeof line, "terminator: %.*s\r\n", nl ? (int)(nl - p) : (int)strlen(p), p);
            scr_puts(&gt[0]->scr, line);
            if (!nl) break;
            p = nl + 1;
        }
    }
    b_free(&rcmsg);

    while (!quit_flag) {                                    /* event loop: sleeps in select(), never busy-waits */
        fd_set rf;
        int mx = xfd;
        while (XPending(dpy)) { XNextEvent(dpy, &ev); on_event(&ev); }
        if (quit_flag) break;
        for (i = 0; i < ngt; i++) if (gt[i]->scr.want_close) { gui_close_tab(i); i--; }
        if (quit_flag) break;
        if (dirty) { gui_draw(); dirty = 0; }
        FD_ZERO(&rf);
        FD_SET(xfd, &rf);
        for (i = 0; i < ngt; i++) { FD_SET(gt[i]->ofd, &rf); if (gt[i]->ofd > mx) mx = gt[i]->ofd; }
        if (select(mx + 1, &rf, NULL, NULL, NULL) < 0) { if (errno == EINTR) continue; break; }
        for (i = 0; i < ngt; i++) if (FD_ISSET(gt[i]->ofd, &rf)) read_tab(i);
    }
    shutdown_tabs();
    XCloseDisplay(dpy);
    return 0;
}
