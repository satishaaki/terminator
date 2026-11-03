/*
 * task7_hotkeys.c
 * TASK 7 - key bindings: defaults, ~/.terminatorrc, bind command, conflict handling, X11 KeyPress mapping, Ctrl+Shift+C/V X11 clipboard.
 */
#include "terminator.h"

   /* gui: handled by the GUI itself */
static const BindDef bind_defaults[A_COUNT] = {
    { "cursor-left",    "left",         "", 0 }, { "cursor-right",   "right",        "", 0 },
    { "history-prev",   "up",           "", 0 }, { "history-next",   "down",         "", 0 },
    { "line-start",     "ctrl+a",       "", 0 }, { "line-end",       "ctrl+e",       "", 0 },
    { "kill-to-start",  "ctrl+u",       "", 0 }, { "kill-to-end",    "ctrl+k",       "", 0 },
    { "delete-word",    "ctrl+w",       "", 0 }, { "clear-screen",   "ctrl+l",       "", 0 },
    { "eof",            "ctrl+d",       "", 0 }, { "search-history", "ctrl+r",       "", 0 },
    { "complete",       "tab",          "", 0 }, { "interrupt",      "ctrl+c",       "", 0 },
    { "suspend",        "ctrl+z",       "", 0 }, { "copy",           "ctrl+shift+c", "", 1 },
    { "paste",          "ctrl+shift+v", "", 1 }, { "new-tab",        "ctrl+shift+t", "", 1 },
    { "close-tab",      "ctrl+shift+w", "", 1 }, { "next-tab",       "ctrl+pgdn",    "", 1 },
    { "prev-tab",       "ctrl+pgup",    "", 1 },
};

BindDef binds[A_COUNT];

      /* the GUI's active table */

static void binds_reset(BindDef *t)
{
    int i;
    for (i = 0; i < A_COUNT; i++) { t[i] = bind_defaults[i]; snprintf(t[i].cur, sizeof t[i].cur, "%s", t[i].def); }
}

static const char *const special_keys[] = { "left", "right", "up", "down", "home", "end", "tab", "delete",
    "pgup", "pgdn", "space", "escape", "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12", NULL };

/* normalise "Ctrl + Shift+C" -> "ctrl+shift+c"; returns NULL if ok, else the reason it is invalid */
static const char *parse_keyspec(const char *in, char *out, size_t sz)
{
    char tmp[128], key[32] = "";
    int ctrl = 0, shift = 0, alt = 0, i, ok = 0;
    char *tok, *save = NULL;
    size_t n = 0;
    for (; *in == ' ' || *in == '\t'; in++) { }
    for (; in[n] && in[n] != '\n' && in[n] != '#' && n < sizeof tmp - 1; n++) tmp[n] = (char)tolower((unsigned char)in[n]);
    while (n > 0 && (tmp[n - 1] == ' ' || tmp[n - 1] == '\t')) n--;
    tmp[n] = 0;
    if (!n) return "empty key";
    if (n == 1 && tmp[0] == '+') { snprintf(key, sizeof key, "+"); goto done; }
    for (tok = strtok_r(tmp, "+", &save); tok; tok = strtok_r(NULL, "+", &save)) {
        char *s = tok, *e;
        while (*s == ' ') s++;
        e = s + strlen(s);
        while (e > s && e[-1] == ' ') *--e = 0;
        if (!strcmp(s, "ctrl") || !strcmp(s, "control")) ctrl = 1;
        else if (!strcmp(s, "shift")) shift = 1;
        else if (!strcmp(s, "alt") || !strcmp(s, "meta")) alt = 1;
        else if (key[0]) return "unknown modifier or key name";
        else if (!*s) return "empty key";
        else snprintf(key, sizeof key, "%s", s);
    }
done:
    if (!key[0]) return "no key given";
    if (!strcmp(key, "pageup")) snprintf(key, sizeof key, "pgup");
    if (!strcmp(key, "pagedown")) snprintf(key, sizeof key, "pgdn");
    if (!strcmp(key, "esc")) snprintf(key, sizeof key, "escape");
    if (!strcmp(key, "enter") || !strcmp(key, "return") || !strcmp(key, "backspace")) return "that key is reserved";
    if (strlen(key) == 1) { if ((unsigned char)key[0] > 126 || (unsigned char)key[0] < 33) return "unknown key"; ok = 1; }
    for (i = 0; special_keys[i] && !ok; i++) if (!strcmp(key, special_keys[i])) ok = 1;
    if (!ok) return "unknown key name";
    if (strlen(key) == 1 && !ctrl && !alt) return "a plain character key cannot be bound (use ctrl+ or alt+)";
    snprintf(out, sz, "%s%s%s%s", ctrl ? "ctrl+" : "", shift ? "shift+" : "", alt ? "alt+" : "", key);
    return NULL;
}

int find_action(const char *name)
{
    int i;
    for (i = 0; i < A_COUNT; i++) if (!strcmp(bind_defaults[i].name, name)) return i;
    return -1;
}

/* After custom bindings are applied: if two actions share a key, the customised ones revert to their default. */
static void resolve_conflicts(BindDef *t, Buf *msgs)
{
    int again = 1, i, j;
    while (again) {
        again = 0;
        for (i = 0; i < A_COUNT && !again; i++)
            for (j = i + 1; j < A_COUNT && !again; j++)
                if (!strcmp(t[i].cur, t[j].cur)) {
                    int ri = strcmp(t[i].cur, t[i].def) != 0, rj = strcmp(t[j].cur, t[j].def) != 0;
                    if (!ri && !rj) continue;
                    if (msgs) b_printf(msgs, "key %s is bound to both %s and %s; using default key(s) for the custom one(s)\n",
                                       t[i].cur, t[i].name, t[j].name);
                    if (ri) snprintf(t[i].cur, sizeof t[i].cur, "%s", t[i].def);
                    if (rj) snprintf(t[j].cur, sizeof t[j].cur, "%s", t[j].def);
                    again = 1;
                }
    }
}

static void rc_path(char *out, size_t sz) { home_path(out, sz, ".terminatorrc"); }

/* Load ~/.terminatorrc into t (starting from the defaults). Problems are appended to msgs. */
void binds_load(BindDef *t, Buf *msgs)
{
    char path[PATH_MAX], line[256];
    FILE *f;
    int ln = 0;
    binds_reset(t);
    rc_path(path, sizeof path);
    f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *p = line, *act, *key, *eq, canon[32];
        const char *err;
        int a;
        ln++;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#' || *p == '\n') continue;
        if (!strncmp(p, "bind", 4) && (p[4] == ' ' || p[4] == '\t')) p += 4;
        while (*p == ' ' || *p == '\t') p++;
        act = p;
        eq = strpbrk(p, "= \t");
        if (!eq) { if (msgs) b_printf(msgs, "%s:%d: expected \"action = key\"\n", path, ln); continue; }
        key = eq;
        while (*key == ' ' || *key == '\t') key++;
        if (*key == '=') key++;
        *eq = 0;
        a = find_action(act);
        if (a < 0) { if (msgs) b_printf(msgs, "%s:%d: unknown action '%s' (ignored)\n", path, ln, act); continue; }
        err = parse_keyspec(key, canon, sizeof canon);
        if (err) { if (msgs) b_printf(msgs, "%s:%d: invalid binding for %s: %s; using default %s\n", path, ln, act, err, t[a].def); continue; }
        snprintf(t[a].cur, sizeof t[a].cur, "%s", canon);
    }
    fclose(f);
    resolve_conflicts(t, msgs);
}

static int binds_save(const BindDef *t)
{
    char path[PATH_MAX];
    FILE *f;
    int i;
    rc_path(path, sizeof path);
    f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "# Terminator key bindings:  action = key   (e.g.  line-start = ctrl+b)\n");
    for (i = 0; i < A_COUNT; i++)
        if (strcmp(t[i].cur, t[i].def) != 0) fprintf(f, "%s = %s\n", t[i].name, t[i].cur);
    fclose(f);
    return 0;
}

int bi_bind(Tab *t, int argc, char **argv)
{
    BindDef cur[A_COUNT];
    Buf msgs = {0}, out = {0};
    int a, i;
    binds_load(cur, &msgs);
    if (msgs.n) { b_adds(&out, "terminator: bind: problems in ~/.terminatorrc:\n"); b_adds(&out, msgs.p); }
    b_free(&msgs);
    if (argc == 1) {
        for (i = 0; i < A_COUNT; i++)
            b_printf(&out, "%-15s = %-13s%s\n", cur[i].name, cur[i].cur, strcmp(cur[i].cur, cur[i].def) ? "  (custom)" : "");
        emit_fd(t, t->out_fd, b_str(&out), out.n);
        b_free(&out);
        return 0;
    }
    if (!strcmp(argv[1], "reset") || !strcmp(argv[1], "-r")) {
        binds_reset(cur);
        binds_save(cur);
        gui_puts(t, "\033]B\007");
        b_adds(&out, "bind: all key bindings reset to the defaults\n");
    } else if (argc == 3) {
        char canon[32];
        const char *err;
        a = find_action(argv[1]);
        if (a < 0) { errf(t, "terminator: bind: unknown action '%s' (try: bind)", argv[1]); b_free(&out); return 1; }
        if (!strcmp(argv[2], "default")) { snprintf(cur[a].cur, sizeof cur[a].cur, "%s", cur[a].def); err = NULL; canon[0] = 0; }
        else err = parse_keyspec(argv[2], canon, sizeof canon);
        if (err) {
            snprintf(cur[a].cur, sizeof cur[a].cur, "%s", cur[a].def);
            errf(t, "terminator: bind: invalid key '%s' for %s: %s; using default (%s)", argv[2], argv[1], err, cur[a].def);
            binds_save(cur);
            gui_puts(t, "\033]B\007");
            emit_fd(t, t->out_fd, b_str(&out), out.n);
            b_free(&out);
            return 1;
        }
        if (canon[0]) {
            for (i = 0; i < A_COUNT; i++)
                if (i != a && !strcmp(cur[i].cur, canon)) {
                    snprintf(cur[a].cur, sizeof cur[a].cur, "%s", cur[a].def);
                    errf(t, "terminator: bind: %s is already bound to %s; using default (%s) for %s", canon, cur[i].name, cur[a].def, argv[1]);
                    binds_save(cur);
                    gui_puts(t, "\033]B\007");
                    emit_fd(t, t->out_fd, b_str(&out), out.n);
                    b_free(&out);
                    return 1;
                }
            snprintf(cur[a].cur, sizeof cur[a].cur, "%s", canon);
        }
        if (binds_save(cur) < 0) { errf(t, "terminator: bind: cannot write ~/.terminatorrc: %s", strerror(errno)); b_free(&out); return 1; }
        gui_puts(t, "\033]B\007");
        b_printf(&out, "bind: %s = %s\n", cur[a].name, cur[a].cur);
    } else { errf(t, "terminator: bind: usage: bind [ACTION KEY | ACTION default | reset]"); b_free(&out); return 2; }
    emit_fd(t, t->out_fd, b_str(&out), out.n);
    b_free(&out);
    return 0;
}

void sel_range(long *a, int *ac, long *b, int *bc)
{
    if (sel_a_abs < sel_b_abs || (sel_a_abs == sel_b_abs && sel_a_col <= sel_b_col)) { *a = sel_a_abs; *ac = sel_a_col; *b = sel_b_abs; *bc = sel_b_col; }
    else { *a = sel_b_abs; *ac = sel_b_col; *b = sel_a_abs; *bc = sel_a_col; }
}

int pos_from_xy(int x, int y, long *abs, int *col)
{
    GTab *g = gt[cur_tab];
    int r = (y - barh - 2) / fh, c;
    Line *l;
    if (r < 0) r = 0;
    if (nvis == 0) return 0;
    if (r >= nvis) r = nvis - 1;
    l = scr_line(&g->scr, vis[r].line);
    c = vis[r].col0 + (x - 4) / cw;
    if (c < vis[r].col0) c = vis[r].col0;
    if (c > l->len) c = l->len;
    *abs = row_abs(g, vis[r].line);
    *col = c;
    return 1;
}

char *selection_text(void)
{
    GTab *g = sel_tab;
    long a, b, k;
    int ac, bc;
    Buf out = {0};
    if (!g || !sel_on) return NULL;
    sel_range(&a, &ac, &b, &bc);
    for (k = a; k <= b; k++) {
        long idx = k - g->scr.first_abs;
        Line *l;
        int from, to;
        if (idx < 0 || idx >= g->scr.n) continue;
        l = scr_line(&g->scr, (int)idx);
        from = (k == a) ? ac : 0;
        to = (k == b) ? bc : l->len;
        if (to > l->len) to = l->len;
        if (to > from) b_add(&out, l->s + from, (size_t)(to - from));
        if (k != b) { while (out.n && out.p[out.n - 1] == ' ') out.p[--out.n] = 0; b_addc(&out, '\n'); }
    }
    return out.p ? out.p : xstrdup("");
}

static void do_copy(void)
{
    char *t = selection_text();
    if (!t || !*t) { free(t); return; }
    free(sel_text);
    sel_text = t;
    XSetSelectionOwner(dpy, a_clip, win, CurrentTime);
    XSetSelectionOwner(dpy, a_primary, win, CurrentTime);
}

static void paste_text(const char *s, size_t n)
{
    Buf b = {0};
    size_t i;
    if (!ngt) return;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\n') b_addc(&b, '\n');
        else if (c == '\t') b_addc(&b, ' ');
        else if (c >= 32 && c < 127) b_addc(&b, (char)c);
    }
    if (b.n) send_keys(gt[cur_tab], b.p, b.n);
    b_free(&b);
}

void do_paste(Atom which)
{
    if (XGetSelectionOwner(dpy, which) == win && sel_text) { paste_text(sel_text, strlen(sel_text)); return; }
    XConvertSelection(dpy, which, a_utf8, a_prop, win, CurrentTime);
}

void on_selection_request(XSelectionRequestEvent *rq)
{
    XSelectionEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.type = SelectionNotify; ev.display = rq->display; ev.requestor = rq->requestor;
    ev.selection = rq->selection; ev.target = rq->target; ev.time = rq->time; ev.property = None;
    if (rq->target == a_targets) {
        Atom l[3] = { a_targets, a_utf8, XA_STRING };
        XChangeProperty(dpy, rq->requestor, rq->property, XA_ATOM, 32, PropModeReplace, (unsigned char *)l, 3);
        ev.property = rq->property;
    } else if ((rq->target == a_utf8 || rq->target == XA_STRING) && sel_text) {
        XChangeProperty(dpy, rq->requestor, rq->property, rq->target, 8, PropModeReplace, (unsigned char *)sel_text, (int)strlen(sel_text));
        ev.property = rq->property;
    }
    XSendEvent(dpy, rq->requestor, False, 0, (XEvent *)&ev);
}

void on_selection_notify(XSelectionEvent *sn)
{
    Atom type; int fmt; unsigned long n, after; unsigned char *data = NULL;
    if (sn->property == None) return;
    if (XGetWindowProperty(dpy, win, sn->property, 0, 1 << 18, True, AnyPropertyType, &type, &fmt, &n, &after, &data) == Success && data) {
        paste_text((char *)data, n);
        XFree(data);
    }
}

static const char *keyname(KeySym ks, char *one)
{
    if (ks >= XK_A && ks <= XK_Z) ks += 32;
    switch (ks) {
    case XK_Left: case XK_KP_Left: return "left";
    case XK_Right: case XK_KP_Right: return "right";
    case XK_Up: case XK_KP_Up: return "up";
    case XK_Down: case XK_KP_Down: return "down";
    case XK_Home: case XK_KP_Home: return "home";
    case XK_End: case XK_KP_End: return "end";
    case XK_Tab: case XK_ISO_Left_Tab: return "tab";
    case XK_Delete: case XK_KP_Delete: return "delete";
    case XK_Page_Up: case XK_KP_Page_Up: return "pgup";
    case XK_Page_Down: case XK_KP_Page_Down: return "pgdn";
    case XK_space: return "space";
    case XK_Escape: return "escape";
    case XK_Return: case XK_KP_Enter: return "enter";
    case XK_BackSpace: return "backspace";
    case XK_F1: return "f1"; case XK_F2: return "f2"; case XK_F3: return "f3"; case XK_F4: return "f4";
    case XK_F5: return "f5"; case XK_F6: return "f6"; case XK_F7: return "f7"; case XK_F8: return "f8";
    case XK_F9: return "f9"; case XK_F10: return "f10"; case XK_F11: return "f11"; case XK_F12: return "f12";
    default: break;
    }
    if (ks >= 0x21 && ks <= 0x7e) { one[0] = (char)ks; one[1] = 0; return one; }
    return NULL;
}

static void gui_action(int a)
{
    switch (a) {
    case A_COPY: do_copy(); break;
    case A_PASTE: do_paste(a_clip); break;
    case A_NEWTAB: gui_new_tab(); break;
    case A_CLOSETAB: if (ngt) gui_close_tab(cur_tab); break;
    case A_NEXTTAB: switch_tab((cur_tab + 1) % ngt); break;
    case A_PREVTAB: switch_tab((cur_tab + ngt - 1) % ngt); break;
    }
}

void on_key(XKeyEvent *ke)
{
    char buf[64], one[2], spec[64];
    KeySym ks = NoSymbol;
    const char *nm;
    int n, a, ctrl, shift, alt;
    if (!ngt) return;
    n = XLookupString(ke, buf, sizeof buf, &ks, NULL);
    ctrl = (ke->state & ControlMask) != 0; shift = (ke->state & ShiftMask) != 0; alt = (ke->state & Mod1Mask) != 0;
    if (ks == XK_Shift_L || ks == XK_Shift_R || ks == XK_Control_L || ks == XK_Control_R || ks == XK_Alt_L || ks == XK_Alt_R ||
        ks == XK_Meta_L || ks == XK_Meta_R || ks == XK_Caps_Lock || ks == XK_Num_Lock || ks == XK_Super_L || ks == XK_Super_R) return;
    nm = keyname(ks, one);
    if (ks == XK_ISO_Left_Tab) shift = 1;
    if (getenv("TERMINATOR_DEBUG_KEYS"))            /* diagnose keyboards: shows what X11 reports for each key press */
        fprintf(stderr, "key: keysym=0x%lx name=%s modifier-state=0x%x -> %s%s%s%s\n", (unsigned long)ks, nm ? nm : "(none)",
                ke->state, ctrl ? "ctrl+" : "", shift ? "shift+" : "", alt ? "alt+" : "", nm ? nm : "?");
    if (nm) {
        snprintf(spec, sizeof spec, "%s%s%s%s", ctrl ? "ctrl+" : "", shift ? "shift+" : "", alt ? "alt+" : "", nm);
        for (a = 0; a < A_COUNT; a++)
            if (!strcmp(binds[a].cur, spec)) {
                if (bind_defaults[a].gui) gui_action(a);
                else { gt[cur_tab]->scroll = 0; send_act(gt[cur_tab], a); }
                dirty = 1;
                return;
            }
        if (shift && !ctrl && !alt && (!strcmp(nm, "pgup") || !strcmp(nm, "pgdn"))) {      /* scrollback */
            GTab *g = gt[cur_tab];
            g->scroll += !strcmp(nm, "pgup") ? rows / 2 : -rows / 2;
            if (g->scroll < 0) g->scroll = 0;
            dirty = 1;
            return;
        }
        if (alt && !ctrl && nm[0] >= '1' && nm[0] <= '9' && !nm[1]) { switch_tab(nm[0] - '1'); return; }
        /* fixed extra tab shortcuts that also work on keyboards without PgUp/PgDn or a working Alt key (e.g. Mac + XQuartz) */
        if (ctrl && !alt && !shift && nm[0] >= '1' && nm[0] <= '9' && !nm[1]) { switch_tab(nm[0] - '1'); return; }   /* Ctrl+1..9 */
        if (ctrl && !alt && !shift && !strcmp(nm, "t")) { gui_new_tab(); dirty = 1; return; }                       /* Ctrl+T */
        if (ctrl && !alt && !strcmp(nm, "tab")) { switch_tab((cur_tab + (shift ? ngt - 1 : 1)) % ngt); return; }    /* Ctrl+(Shift+)Tab */
        if (!strcmp(nm, "enter") && !ctrl && !alt) { gt[cur_tab]->scroll = 0; send_keys(gt[cur_tab], "\n", 1); dirty = 1; return; }
        if (!strcmp(nm, "backspace") && !ctrl && !alt) { gt[cur_tab]->scroll = 0; send_keys(gt[cur_tab], "\177", 1); dirty = 1; return; }
        if (!strcmp(nm, "delete") && !ctrl && !alt) { send_keys(gt[cur_tab], "\033!", 2); return; }
        if (!strcmp(nm, "escape") && !ctrl && !alt) { send_keys(gt[cur_tab], "\033#", 2); return; }
    }
    if (ctrl || alt) return;
    if (n > 0) {
        int i;
        Buf b = {0};
        for (i = 0; i < n; i++) if ((unsigned char)buf[i] >= 32 && (unsigned char)buf[i] < 127) b_addc(&b, buf[i]);
        if (b.n) { gt[cur_tab]->scroll = 0; send_keys(gt[cur_tab], b.p, b.n); dirty = 1; }
        b_free(&b);
    }
}
