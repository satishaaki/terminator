/*
 * task7_editor.c
 * TASK 7 - command-line editor inside the shell: cursor movement, kill/delete, key dispatch (actions and characters).
 */
#include "terminator.h"

void cur_prompt(Tab *t, char *buf, size_t sz)
{
    /* Prompt text comes from the shell variable PS1 (bash style); default "\\u@terminator> ".
       Escapes: \\u user  \\h terminator  \\w current dir (~ for home)  \\W last part of the dir  \\$ dollar sign  \\ backslash */
    const char *ps, *h = home_dir(), *base;
    char wd[PATH_MAX + 8];
    size_t hl = strlen(h);
    Buf b = {0};
    if (t->multiline) { snprintf(buf, sz, "> "); return; }
    ps = var_get(t, "PS1");
    if (!ps || !*ps) ps = "\\u@terminator \\W> ";
    if (hl > 1 && !strncmp(t->cwd, h, hl) && (t->cwd[hl] == '/' || !t->cwd[hl])) snprintf(wd, sizeof wd, "~%s", t->cwd + hl);
    else snprintf(wd, sizeof wd, "%s", t->cwd);
    base = strrchr(wd, '/');
    base = (base && base[1]) ? base + 1 : wd;
    for (; *ps; ps++) {
        if (*ps != '\\' || !ps[1]) { b_addc(&b, *ps); continue; }
        ps++;
        switch (*ps) {
        case 'u': b_adds(&b, t->user); break;
        case 'h': b_adds(&b, "terminator"); break;
        case 'w': b_adds(&b, wd); break;
        case 'W': b_adds(&b, base); break;
        case '$': b_addc(&b, '$'); break;
        case '\\': b_addc(&b, '\\'); break;
        default: b_addc(&b, '\\'); b_addc(&b, *ps);
        }
    }
    snprintf(buf, sz, "%s", b_str(&b));
    b_free(&b);
}

void draw_input(Tab *t)             /* redraw "prompt + input" on the current line and place the cursor */
{
    char pr[512], *seq;
    size_t n;
    cur_prompt(t, pr, sizeof pr);
    n = strlen(pr) + (size_t)t->ilen + 64;
    seq = xmalloc(n);
    n = (size_t)snprintf(seq, n, "\r\033[K%s", pr);
    memcpy(seq + n, t->in, (size_t)t->ilen);
    n += (size_t)t->ilen;
    n += (size_t)sprintf(seq + n, "\033[%dG", (int)strlen(pr) + t->ipos + 1);
    gui_write(t, seq, n);
    free(seq);
    t->at_bol = 0;
}

void prompt_refresh(Tab *t) { draw_input(t); }

static void in_reserve(Tab *t, int extra)
{
    if (t->ilen + extra + 2 > t->icap) { t->icap = (t->ilen + extra + 2) * 2; t->in = xrealloc(t->in, (size_t)t->icap); }
}

void in_insert(Tab *t, const char *s, int n)
{
    if (t->ilen + n > 60000) return;
    in_reserve(t, n);
    memmove(t->in + t->ipos + n, t->in + t->ipos, (size_t)(t->ilen - t->ipos));
    memcpy(t->in + t->ipos, s, (size_t)n);
    t->ilen += n; t->ipos += n;
    t->in[t->ilen] = 0;
}

void in_delete(Tab *t, int from, int to)      /* remove [from, to) */
{
    if (from < 0) from = 0;
    if (to > t->ilen) to = t->ilen;
    if (to <= from) return;
    memmove(t->in + from, t->in + to, (size_t)(t->ilen - to));
    t->ilen -= to - from;
    t->in[t->ilen] = 0;
    t->ipos = from;
}

void in_set(Tab *t, const char *s)
{
    int n = (int)strlen(s);
    in_reserve(t, n);
    memcpy(t->in, s, (size_t)n + 1);
    t->ilen = t->ipos = n;
}

static void do_enter(Tab *t)
{
    char pr[512], seq[64];
    int n;
    cur_prompt(t, pr, sizeof pr);
    n = snprintf(seq, sizeof seq, "\033[%dG\n", (int)strlen(pr) + t->ilen + 1);
    gui_write(t, seq, (size_t)n);
    t->at_bol = 1;
    t->prompt_on = 0;
    t->line_ready = 1;
}

static void on_action(Tab *t, int a)
{
    if (t->mode == M_RUN) {
        if (a == A_INTR) run_interrupt(t);
        else if (a == A_SUSP) run_suspend(t);
        else if (a == A_EOF) run_eof(t);
        return;
    }
    if (t->mode == M_SEARCH) { if (a == A_INTR || a == A_EOF || a == '#' - '0') search_finish(t, 0); return; }
    if (t->mode == M_PICK) { pick_cancel(t); if (a == '#' - '0') { draw_input(t); return; } }
    switch (a) {
    case A_LEFT: if (t->ipos > 0) { t->ipos--; draw_input(t); } break;
    case A_RIGHT: if (t->ipos < t->ilen) { t->ipos++; draw_input(t); } break;
    case A_UP: hist_nav(t, -1); draw_input(t); break;
    case A_DOWN: hist_nav(t, 1); draw_input(t); break;
    case A_HOME: t->ipos = 0; draw_input(t); break;
    case A_END: t->ipos = t->ilen; draw_input(t); break;
    case A_KILLBOL: in_delete(t, 0, t->ipos); draw_input(t); break;
    case A_KILLEOL: in_delete(t, t->ipos, t->ilen); draw_input(t); break;
    case A_DELWORD: {
        int p = t->ipos;
        while (p > 0 && (t->in[p - 1] == ' ' || t->in[p - 1] == '\t')) p--;
        while (p > 0 && t->in[p - 1] != ' ' && t->in[p - 1] != '\t') p--;
        in_delete(t, p, t->ipos);
        draw_input(t);
        break;
    }
    case A_CLEAR: gui_puts(t, "\033[2J"); t->at_bol = 1; draw_input(t); break;
    case A_EOF:
        if (t->ilen == 0) { t->closing = 1; t->line_ready = 0; }
        else { in_delete(t, t->ipos, t->ipos + 1); draw_input(t); }
        break;
    case A_SEARCH: search_start(t); break;
    case A_COMPLETE: do_complete(t); break;
    case A_INTR: {
        char pr[512], seq[64];
        int n;
        cur_prompt(t, pr, sizeof pr);
        n = snprintf(seq, sizeof seq, "\033[%dG^C\n", (int)strlen(pr) + t->ilen + 1);
        gui_write(t, seq, (size_t)n);
        t->at_bol = 1;
        t->ilen = t->ipos = 0; t->in[0] = 0; t->hidx = -1;
        t->pend.n = 0; if (t->pend.p) t->pend.p[0] = 0;
        t->multiline = 0;
        t->status = 130;
        show_prompt(t);
        break;
    }
    case KEY_DELETE: in_delete(t, t->ipos, t->ipos + 1); draw_input(t); break;
    default: break;                         /* A_SUSP and GUI-only actions: nothing to do here */
    }
}

static void on_char(Tab *t, unsigned char c)
{
    if (t->mode == M_RUN) { run_char(t, c); return; }
    if (t->mode == M_SEARCH) {
        if (c == '\n') search_finish(t, 1);
        else if (c == 8 || c == 127) { if (t->sterm.n) { t->sterm.p[--t->sterm.n] = 0; gui_puts(t, "\b \b"); } }
        else if (c >= 32 && c < 127) { b_addc(&t->sterm, (char)c); gui_write(t, (char *)&c, 1); }
        return;
    }
    if (t->mode == M_PICK) {
        if (c >= '0' && c <= '9') {
            t->pick_num = t->pick_num * 10 + (c - '0');
            if (t->pick_num > t->pick.n) { pick_cancel(t); draw_input(t); return; }
            if (t->pick.n <= 9 || t->pick_num * 10 > t->pick.n) pick_select(t);
            return;
        }
        if ((c == '\n') && t->pick_num >= 1) { pick_select(t); return; }
        if (c == 8 || c == 127) { t->pick_num /= 10; return; }
        pick_cancel(t);
        if (c == '\n') { do_enter(t); return; }
        /* any other character cancels the list and is typed normally */
    }
    if (c == '\n') { do_enter(t); return; }
    if (c == 8 || c == 127) { if (t->ipos > 0) { in_delete(t, t->ipos - 1, t->ipos); draw_input(t); } return; }
    if (c >= 32 && c < 127) {
        char ch = (char)c;
        in_insert(t, &ch, 1);
        if (t->ipos == t->ilen) gui_write(t, &ch, 1); else draw_input(t);
        t->at_bol = 0;
    }
}

void keys_parse(Tab *t)
{
    int i = 0;
    while (i < t->kbn && !t->line_ready && !t->dead) {
        unsigned char c = t->kb[i];
        if (c == 27) {
            unsigned char a;
            if (i + 1 >= t->kbn) break;
            a = t->kb[i + 1];
            i += 2;
            if (a == '!') on_action(t, KEY_DELETE);
            else if (a >= '0' && a < '0' + 64) on_action(t, a - '0');
        } else { i++; if (c == '\n' || c == 8 || c == 127 || (c >= 32 && c < 127)) on_char(t, c); }
    }
    if (i > 0) { memmove(t->kb, t->kb + i, (size_t)(t->kbn - i)); t->kbn -= i; }
}
