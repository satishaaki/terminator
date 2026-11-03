/*
 * engine.c
 * Shell engine core (one per tab): Tab state, shell variables, output routing to the GUI, the select() event pump, prompt and main loop.
 */
#include "terminator.h"

static Tab *tab_new(const char *cwd)
{
    Tab *t = xmalloc(sizeof *t);
    char **e;
    int p[2];
    memset(t, 0, sizeof *t);
    t->kfd = t->ofd = -1;
    snprintf(t->cwd, sizeof t->cwd, "%s", cwd);
    snprintf(t->oldcwd, sizeof t->oldcwd, "%s", cwd);
    t->cenv = 64;
    t->env = xmalloc(sizeof(char *) * (size_t)t->cenv);
    for (e = environ; e && *e; e++) {
        if (t->nenv + 2 > t->cenv) { t->cenv *= 2; t->env = xrealloc(t->env, sizeof(char *) * (size_t)t->cenv); }
        t->env[t->nenv++] = xstrdup(*e);
    }
    t->env[t->nenv] = NULL;
    t->arg0 = xstrdup("terminator");
    t->icap = 256;
    t->in = xmalloc((size_t)t->icap);
    t->in[0] = 0;
    t->hidx = -1;
    t->cur_job = t->prev_job = 0;
    t->fg_in = -1;
    t->at_bol = 1;
    t->mw_interval = 2.0;
    pthread_mutex_init(&t->evl, NULL);
    if (mkpipe(p) == 0) { t->wake_r = p[0]; t->wake_w = p[1]; set_nonblock(p[0]); set_nonblock(p[1]); } else t->wake_r = t->wake_w = -1;
    if (mkpipe(p) == 0) { t->dpr = p[0]; t->dpw = p[1]; set_nonblock(p[0]); } else t->dpr = t->dpw = -1;
    t->devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
    t->out_fd = t->err_fd = t->dpw;
    {
        const char *u = getenv("USER");
        snprintf(t->user, sizeof t->user, "%s", (u && *u) ? u : "user");
    }
    return t;
}

const char *var_get(Tab *t, const char *name)
{
    size_t n = strlen(name);
    int i;
    for (i = 0; i < t->nenv; i++)
        if (!strncmp(t->env[i], name, n) && t->env[i][n] == '=') return t->env[i] + n + 1;
    return NULL;
}

void var_set(Tab *t, const char *name, const char *value)
{
    size_t n = strlen(name), vl = strlen(value);
    char *s = xmalloc(n + vl + 2);
    int i;
    memcpy(s, name, n); s[n] = '='; memcpy(s + n + 1, value, vl + 1);
    for (i = 0; i < t->nenv; i++)
        if (!strncmp(t->env[i], name, n) && t->env[i][n] == '=') { free(t->env[i]); t->env[i] = s; return; }
    if (t->nenv + 2 > t->cenv) { t->cenv *= 2; t->env = xrealloc(t->env, sizeof(char *) * (size_t)t->cenv); }
    t->env[t->nenv++] = s;
    t->env[t->nenv] = NULL;
}

void var_unset(Tab *t, const char *name)
{
    size_t n = strlen(name);
    int i;
    for (i = 0; i < t->nenv; i++)
        if (!strncmp(t->env[i], name, n) && t->env[i][n] == '=') {
            free(t->env[i]);
            memmove(&t->env[i], &t->env[i + 1], sizeof(char *) * (size_t)(t->nenv - i));
            t->nenv--;
            return;
        }
}

int valid_name(const char *s, size_t n)
{
    size_t i;
    if (!n || !(isalpha((unsigned char)s[0]) || s[0] == '_')) return 0;
    for (i = 1; i < n; i++) if (!(isalnum((unsigned char)s[i]) || s[i] == '_')) return 0;
    return 1;
}

void gui_write(Tab *t, const char *p, size_t n)
{
    if (t->ofd < 0 || !n) return;
    if (write_all(t->ofd, p, n) < 0) t->dead = 1;
}

void gui_puts(Tab *t, const char *s) { gui_write(t, s, strlen(s)); }

void disp(Tab *t, const char *p, size_t n)       /* text the user should see */
{
    if (!n) return;
    gui_write(t, p, n);
    t->at_bol = (p[n - 1] == '\n');
}

void emit_fd(Tab *t, int fd, const char *p, size_t n)
{
    int i;
    if (!n) return;
    if (fd == t->dpw || fd < 0) { disp(t, p, n); return; }
    for (i = 0; i < t->ncap; i++)
        if (t->cap[i].wfd == fd) { b_add(&t->cap[i].buf, p, n); return; }
    if (write_all(fd, p, n) < 0) { }
}

void emit(Tab *t, const char *s) { emit_fd(t, t->out_fd, s, strlen(s)); }

void errf(Tab *t, const char *fmt, ...)
{
    char tmp[PATH_MAX + 256];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n > sizeof tmp - 3) n = (int)sizeof tmp - 3;
    tmp[n++] = '\n';
    emit_fd(t, t->err_fd, tmp, (size_t)n);
}

void pump(Tab *t, int timeout_ms)
{
    fd_set rf;
    int mx = -1, i, r, k;
    double wait = -1;
    struct timeval tv, *tp = NULL;
    char tmp[4096];

    if (t->kbn > 0 && !t->line_ready) keys_parse(t);
    if (t->line_ready || t->dead) return;
    FD_ZERO(&rf);
#define ADDFD(f) do { if ((f) >= 0) { FD_SET((f), &rf); if ((f) > mx) mx = (f); } } while (0)
    ADDFD(t->kfd);
    ADDFD(t->dpr);
    ADDFD(t->wake_r);
    for (i = 0; i < t->ncap; i++) if (!t->cap[i].eof) ADDFD(t->cap[i].rfd);
    if (t->mw_active)
        for (i = 0; i < t->nmw; i++) ADDFD(t->mw[i].fd);
    if (timeout_ms >= 0) wait = timeout_ms / 1000.0;
    if (t->mw_active) {
        double now = mono_now();
        for (i = 0; i < t->nmw; i++)
            if (t->mw[i].pid == 0) {
                double d = t->mw[i].next - now;
                if (d < 0) d = 0;
                if (wait < 0 || d < wait) wait = d;
            }
    }
    if (wait >= 0) { long us = (long)(wait * 1e6) + 500; tv.tv_sec = us / 1000000; tv.tv_usec = us % 1000000; tp = &tv; }
    r = select(mx + 1, &rf, NULL, NULL, tp);
    if (r < 0) { if (errno != EINTR) t->dead = 1; return; }

    if (t->kfd >= 0 && FD_ISSET(t->kfd, &rf)) {
        ssize_t n = read(t->kfd, t->kb + t->kbn, sizeof t->kb - (size_t)t->kbn);
        if (n == 0) { t->dead = 1; t->kfd = -1; }
        else if (n > 0) { t->kbn += (int)n; keys_parse(t); }
    }
    if (t->dpr >= 0 && FD_ISSET(t->dpr, &rf)) drain_display(t);
    if (t->wake_r >= 0 && FD_ISSET(t->wake_r, &rf)) {
        while (read(t->wake_r, tmp, sizeof tmp) > 0) { }
        process_events(t);
    }
    for (i = 0; i < t->ncap; i++)
        if (!t->cap[i].eof && FD_ISSET(t->cap[i].rfd, &rf)) {
            ssize_t n = read(t->cap[i].rfd, tmp, sizeof tmp);
            if (n > 0) b_add(&t->cap[i].buf, tmp, (size_t)n);
            else if (n == 0) t->cap[i].eof = 1;
        }
    if (t->mw_active)
        for (k = 0; k < t->nmw; k++)
            if (t->mw[k].fd >= 0 && FD_ISSET(t->mw[k].fd, &rf)) mw_read(t, &t->mw[k]);
    if (t->mw_active) mw_tick(t);
}

void drain_display(Tab *t)
{
    char tmp[8192];
    for (;;) {
        ssize_t n = read(t->dpr, tmp, sizeof tmp);
        if (n <= 0) break;
        if (t->mode == M_PROMPT && t->prompt_on) {      /* background output while the user is typing */
            gui_puts(t, "\r\033[K");
            t->at_bol = 1;
            disp(t, tmp, (size_t)n);
            if (!t->at_bol) gui_puts(t, "\n");
            t->at_bol = 1;
            prompt_refresh(t);
        } else disp(t, tmp, (size_t)n);
    }
}

static void send_title(Tab *t)
{
    static __thread char last[PATH_MAX];       /* per engine thread */
    char title[PATH_MAX + 32];
    const char *h = home_dir();
    size_t hl = strlen(h);
    if (hl > 1 && !strncmp(t->cwd, h, hl) && (t->cwd[hl] == '/' || !t->cwd[hl])) snprintf(title, sizeof title, "~%s", t->cwd + hl);
    else snprintf(title, sizeof title, "%s", t->cwd);
    if (!strcmp(last, title) && t->kbn >= 0 && last[0]) return;
    snprintf(last, sizeof last, "%s", title);
    {
        char seq[PATH_MAX + 64];
        int n = snprintf(seq, sizeof seq, "\033]0;%s\007", title);
        gui_write(t, seq, (size_t)n);
    }
}

void show_prompt(Tab *t)
{
    jobs_notify(t);
    send_title(t);
    if (!t->at_bol) gui_puts(t, "\n");
    t->mode = M_PROMPT;
    t->prompt_on = 1;
    draw_input(t);
}

static void handle_line(Tab *t)
{
    Arena ar = {0};
    List *L = NULL;
    char err[300], *cmd;
    int r;
    size_t n;
    int blank = 1, i;
    for (i = 0; i < t->ilen; i++) if (t->in[i] != ' ' && t->in[i] != '\t') blank = 0;
    if (blank && !t->multiline) { t->ilen = t->ipos = 0; t->in[0] = 0; return; }
    b_add(&t->pend, t->in, (size_t)t->ilen);
    b_addc(&t->pend, '\n');
    t->ilen = t->ipos = 0; t->in[0] = 0; t->hidx = -1;
    r = parse_text(&ar, b_str(&t->pend), &L, err, sizeof err);
    if (r == 1) { t->multiline = 1; ar_free(&ar); return; }        /* show the > prompt and keep reading */
    t->multiline = 0;
    n = t->pend.n;
    while (n > 0 && t->pend.p[n - 1] == '\n') n--;
    cmd = xstrndup(t->pend.p, n);
    t->pend.n = 0; t->pend.p[0] = 0;
    {
        int nb = 1;
        for (i = 0; cmd[i]; i++) if (cmd[i] != ' ' && cmd[i] != '\t' && cmd[i] != '\n') nb = 0;
        if (!nb) hist_add(cmd);
    }
    t->mode = M_RUN;
    t->intr = 0; t->stop_exec = 0;
    if (r == 2) { errf(t, "terminator: %s", err); t->status = 2; }
    else {
        t->out_fd = t->err_fd = t->dpw;
        t->status = exec_list(t, L);
    }
    t->stop_exec = 0;
    drain_display(t);
    t->mode = M_PROMPT;
    ar_free(&ar);
    free(cmd);
}

volatile int g_engines;

void engine_run(EngineArgs *ea)
{
    Tab *t = tab_new(ea->cwd);
    t->kfd = ea->kfd;
    t->ofd = ea->ofd;
    var_set(t, "PWD", t->cwd);
    if (!var_get(t, "TERM") || !strcmp(var_get(t, "TERM"), "")) var_set(t, "TERM", "dumb");
    var_set(t, "SHELL", "terminator");
    var_unset(t, "PS1");                                  /* ignore the PS1 of the launching shell (zsh/bash syntax differs) */
    if (getenv("TERMINATOR_PS1")) var_set(t, "PS1", getenv("TERMINATOR_PS1"));
    while (!t->dead && !t->closing) {
        show_prompt(t);
        while (!t->line_ready && !t->dead && !t->closing) pump(t, -1);
        if (t->line_ready) {
            t->line_ready = 0;
            handle_line(t);
        }
    }
    jobs_kill_all(t);
    if (!t->dead) gui_puts(t, "\033]X\007");
    close(t->ofd);
    close(t->kfd);
}
