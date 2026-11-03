/*
 * task6_multiwatch.c
 * TASK 6 - multiWatch: parallel commands, one pipe per command, select() on all, timed re-run, Ctrl+C kills every child.
 */
#include "terminator.h"

/* a forked copy of the interpreter, running `cmd` with stdout/stderr on a pipe (used by multiWatch) */
static pid_t fork_subshell(Tab *t, const char *cmd, int *rfd)
{
    int p[2];
    pid_t pid;
    if (mkpipe(p) < 0) return -1;
    pthread_mutex_lock(&spawn_lock);
    pid = fork();
    if (pid == 0) {
        int w[2];
        pthread_mutex_unlock(&spawn_lock);
        child_signals();
        setpgid(0, 0);
        close(p[0]);
        pthread_mutex_init(&t->evl, NULL);
        t->evh = t->evt = NULL;
        if (mkpipe(w) == 0) { t->wake_r = w[0]; t->wake_w = w[1]; set_nonblock(w[0]); set_nonblock(w[1]); }
        t->kfd = -1; t->dpr = -1; t->dpw = -2; t->ofd = -1;
        t->ncap = 0; t->nmw = 0; t->mw_active = 0; t->mw = NULL;
        memset(t->jobs, 0, sizeof t->jobs);
        t->fg = NULL; t->fg_in = -1; t->cur_job = t->prev_job = 0; t->kbn = 0;
        t->in_sub = 1; t->mode = M_RUN; t->intr = 0; t->closing = 0; t->dead = 0;
        t->out_fd = t->err_fd = p[1];
        _exit(exec_text(t, cmd) & 255);
    }
    pthread_mutex_unlock(&spawn_lock);
    close(p[1]);
    if (pid < 0) { close(p[0]); return -1; }
    setpgid(pid, pid);
    *rfd = p[0];
    return pid;
}

static const char *mw_usage = "terminator: multiWatch: usage: multiWatch [-n SECONDS] [\"cmd1\", \"cmd2\", ...]";

static int mw_parse(Tab *t, const char *text, Vec *cmds, double *interval)
{
    const char *p = strstr(text, "multiWatch");
    *interval = 2.0;
    if (!p) { errf(t, "%s", mw_usage); return 1; }
    p += 10;
    while (*p == ' ' || *p == '\t') p++;
    if (p[0] == '-' && p[1] == 'n') {
        char *e;
        double v;
        p += 2;
        v = strtod(p, &e);
        if (e == p || v <= 0) { errf(t, "%s", mw_usage); return 1; }
        *interval = v;
        p = e;
        while (*p == ' ' || *p == '\t') p++;
    }
    if (*p != '[') { errf(t, "%s", mw_usage); return 1; }
    p++;
    for (;;) {
        Buf b = {0};
        char q;
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (*p == ']' && cmds->n == 0) { errf(t, "%s", mw_usage); return 1; }
        if (*p != '"' && *p != '\'') { errf(t, "terminator: multiWatch: each command must be quoted"); return 1; }
        q = *p++;
        while (*p && *p != q) {
            if (q == '"' && *p == '\\' && (p[1] == '"' || p[1] == '\\')) p++;
            b_addc(&b, *p++);
        }
        if (*p != q) { errf(t, "%s", mw_usage); b_free(&b); return 1; }
        p++;
        {
            const char *s = b_str(&b);
            while (*s == ' ') s++;
            if (!*s) { errf(t, "terminator: multiWatch: empty command in list"); b_free(&b); return 1; }
        }
        v_push(cmds, b.p ? b.p : xstrdup(""));
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (*p == ',') { p++; continue; }
        if (*p == ']') break;
        errf(t, "%s", mw_usage);
        return 1;
    }
    return 0;
}

static void mw_print_block(Tab *t, Watcher *w)
{
    Buf b = {0};
    b_printf(&b, "\"%s\", %ld:\n%s\n", w->cmd, (long)w->ts, MW_RULE);
    if (w->buf.n) { b_add(&b, w->buf.p, w->buf.n); if (w->buf.p[w->buf.n - 1] != '\n') b_addc(&b, '\n'); }
    b_printf(&b, "%s\n", MW_RULE);
    if (!t->at_bol) disp(t, "\n", 1);
    disp(t, b_str(&b), b.n);
    b_free(&b);
}

void mw_read(Tab *t, Watcher *w)
{
    char tmp[4096];
    ssize_t n = read(w->fd, tmp, sizeof tmp);
    if (n > 0) { b_add(&w->buf, tmp, (size_t)n); return; }
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) return;
    close(w->fd);
    w->fd = -1;
    if (w->pid > 0) { int st; waitpid(w->pid, &st, 0); }
    w->pid = 0;
    mw_print_block(t, w);
    {
        double now = mono_now();
        w->next = w->started + t->mw_interval;
        if (w->next < now) w->next = now;
    }
}

void mw_tick(Tab *t)
{
    int i;
    double now = mono_now();
    for (i = 0; i < t->nmw; i++) {
        Watcher *w = &t->mw[i];
        if (w->pid != 0 || w->next > now) continue;
        w->buf.n = 0; if (w->buf.p) w->buf.p[0] = 0;
        w->ts = time(NULL);
        w->started = now;
        w->pid = fork_subshell(t, w->cmd, &w->fd);
        if (w->pid < 0) { w->pid = 0; w->next = now + t->mw_interval; }
    }
}

void mw_interrupt(Tab *t)                  /* Ctrl+C: stop every watcher child, the shell keeps running */
{
    int i;
    for (i = 0; i < t->nmw; i++) {
        Watcher *w = &t->mw[i];
        if (w->pid > 0) { int st; kill(-w->pid, SIGTERM); kill(-w->pid, SIGKILL); kill(w->pid, SIGKILL); waitpid(w->pid, &st, 0); w->pid = 0; }
        if (w->fd >= 0) { close(w->fd); w->fd = -1; }
    }
    t->mw_stop = 1;
}

int bi_multiwatch(Tab *t)
{
    Vec cmds = {0};
    double iv;
    int i;
    if (t->in_sub) { errf(t, "terminator: multiWatch: not available here"); return 1; }
    if (mw_parse(t, t->cmdtext ? t->cmdtext : "", &cmds, &iv)) { v_free(&cmds); return 2; }
    t->mw = xmalloc(sizeof(Watcher) * (size_t)cmds.n);
    memset(t->mw, 0, sizeof(Watcher) * (size_t)cmds.n);
    t->nmw = cmds.n;
    t->mw_interval = iv;
    for (i = 0; i < cmds.n; i++) { t->mw[i].cmd = cmds.v[i]; t->mw[i].fd = -1; t->mw[i].next = mono_now(); }
    free(cmds.v);
    t->mw_stop = 0; t->mw_active = 1; t->intr = 0;
    mw_tick(t);
    while (!t->mw_stop && !t->dead) pump(t, -1);
    if (t->dead) mw_interrupt(t);
    t->mw_active = 0;
    for (i = 0; i < t->nmw; i++) { free(t->mw[i].cmd); b_free(&t->mw[i].buf); }
    free(t->mw);
    t->mw = NULL; t->nmw = 0;
    return 130;
}
