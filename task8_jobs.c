/*
 * task8_jobs.c
 * TASK 8 - signals and job control: process groups, waiter threads, Ctrl+C / Ctrl+Z, jobs, fg, bg, kill, background notifications.
 */
#include "terminator.h"

typedef struct { Tab *t; pid_t pid; } WaitArg;

static void *waiter_main(void *arg)
{
    WaitArg *w = arg;
    Tab *t = w->t;
    pid_t pid = w->pid;
    free(w);
    for (;;) {
        int st = 0, kind;
        pid_t r = waitpid(pid, &st, WUNTRACED | WCONTINUED);
        Evt *e;
        if (r < 0) { if (errno == EINTR) continue; kind = 0; st = 0; }
        else if (WIFSTOPPED(st)) kind = 1;
        else if (WIFCONTINUED(st)) kind = 2;
        else kind = 0;
        e = xmalloc(sizeof *e);
        e->pid = pid; e->kind = kind; e->status = st; e->next = NULL;
        pthread_mutex_lock(&t->evl);
        if (t->evt) t->evt->next = e; else t->evh = e;
        t->evt = e;
        pthread_mutex_unlock(&t->evl);
        if (write(t->wake_w, "x", 1) < 0) { }
        if (kind == 0) break;
    }
    return NULL;
}

void start_waiter(Tab *t, pid_t pid)
{
    pthread_t th;
    pthread_attr_t at;
    WaitArg *w = xmalloc(sizeof *w);
    w->t = t; w->pid = pid;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 256 * 1024);
    if (pthread_create(&th, &at, waiter_main, w) != 0) { free(w); }
    pthread_attr_destroy(&at);
}

static Job *job_by_id(Tab *t, int id)
{
    int i;
    for (i = 0; i < MAXJOBS; i++) if (t->jobs[i].used && t->jobs[i].id == id) return &t->jobs[i];
    return NULL;
}

static void job_free(Tab *t, Job *j)
{
    if (j->in_w >= 0) close(j->in_w);
    free(j->p); free(j->cmd);
    if (t->cur_job == j->id) { t->cur_job = t->prev_job; t->prev_job = 0; }
    else if (t->prev_job == j->id) t->prev_job = 0;
    memset(j, 0, sizeof *j);
    if (t->cur_job && !job_by_id(t, t->cur_job)) t->cur_job = 0;
}

Job *job_new(Tab *t, pid_t pgid, int np, const char *cmd, int bg)
{
    int i, id = 1;
    Job *j = NULL;
    for (;; id++) if (!job_by_id(t, id)) break;
    for (i = 0; i < MAXJOBS; i++) if (!t->jobs[i].used) { j = &t->jobs[i]; break; }
    if (!j) return NULL;
    memset(j, 0, sizeof *j);
    j->used = 1; j->id = id; j->pgid = pgid; j->np = np; j->bg = bg; j->state = J_RUN; j->in_w = -1;
    j->p = xmalloc(sizeof(Proc) * (size_t)np);
    memset(j->p, 0, sizeof(Proc) * (size_t)np);
    j->cmd = xstrdup(cmd);
    if (t->cur_job != id) { t->prev_job = t->cur_job; t->cur_job = id; }
    return j;
}

static int job_mark(Tab *t, Job *j) { return j->id == t->cur_job ? '+' : (j->id == t->prev_job ? '-' : ' '); }

static void job_event(Tab *t, pid_t pid, int kind, int st)
{
    int i, k, nrun = 0, nstop = 0, ndone = 0;
    for (i = 0; i < MAXJOBS; i++) {
        Job *j = &t->jobs[i];
        if (!j->used) continue;
        for (k = 0; k < j->np; k++) {
            Proc *p = &j->p[k];
            if (p->pid != pid) continue;
            if (kind == 0) {
                p->state = 2;
                if (WIFEXITED(st)) p->status = WEXITSTATUS(st);
                else if (WIFSIGNALED(st)) { p->sig = WTERMSIG(st); p->status = 128 + p->sig; }
                else p->status = 127;
            } else if (kind == 1) p->state = 1;
            else p->state = 0;
            for (k = 0; k < j->np; k++) { int s = j->p[k].state; if (s == 0) nrun++; else if (s == 1) nstop++; else ndone++; }
            if (ndone == j->np) { j->state = J_DONE; j->status = j->p[j->np - 1].status; if (j->bg) j->notify = 1; }
            else if (nrun == 0) j->state = J_STOP;
            else j->state = J_RUN;
            return;
        }
    }
}

void process_events(Tab *t)
{
    Evt *e;
    pthread_mutex_lock(&t->evl);
    e = t->evh; t->evh = t->evt = NULL;
    pthread_mutex_unlock(&t->evl);
    while (e) {
        Evt *n = e->next;
        job_event(t, e->pid, e->kind, e->status);
        free(e);
        e = n;
    }
    if (t->mode == M_PROMPT && t->prompt_on) {          /* finished background jobs are reported at once */
        int i, any = 0;
        for (i = 0; i < MAXJOBS; i++) if (t->jobs[i].used && t->jobs[i].state == J_DONE && t->jobs[i].notify) any = 1;
        if (any) { gui_puts(t, "\r\033[K"); t->at_bol = 1; show_prompt(t); }
    }
}

static const char *state_name(Job *j) { return j->state == J_STOP ? "Stopped" : (j->state == J_DONE ? "Done" : "Running"); }

/* report (and forget) finished background jobs */
void jobs_notify(Tab *t)
{
    int i;
    for (i = 0; i < MAXJOBS; i++) {
        Job *j = &t->jobs[i];
        char line[PATH_MAX + 128];
        if (!j->used || j->state != J_DONE || !j->notify) continue;
        if (j->status == 0) snprintf(line, sizeof line, "[%d]%c Done %s\n", j->id, job_mark(t, j), j->cmd);
        else snprintf(line, sizeof line, "[%d]%c Exit %d %s\n", j->id, job_mark(t, j), j->status, j->cmd);
        disp(t, line, strlen(line));
        job_free(t, j);
    }
}

void jobs_kill_all(Tab *t)
{
    int i;
    for (i = 0; i < MAXJOBS; i++)
        if (t->jobs[i].used && t->jobs[i].state != J_DONE) { kill(-t->jobs[i].pgid, SIGHUP); kill(-t->jobs[i].pgid, SIGCONT); kill(-t->jobs[i].pgid, SIGKILL); }
}

void child_signals(void)
{
    sigset_t e;
    int s;
    sigemptyset(&e);
    pthread_sigmask(SIG_SETMASK, &e, NULL);
    for (s = 1; s < 32; s++) if (s != SIGKILL && s != SIGSTOP) signal(s, SIG_DFL);
}

static void report_signal(Tab *t, Job *j)
{
    int k;
    for (k = 0; k < j->np; k++) {
        int sg = j->p[k].sig;
        if (sg && sg != SIGINT && sg != SIGPIPE && sg != SIGHUP) {
            char m[128];
            snprintf(m, sizeof m, "%s\n", strsignal(sg));
            if (!t->at_bol) disp(t, "\n", 1);
            disp(t, m, strlen(m));
            return;
        }
    }
}

int wait_fg(Tab *t, Job *j)
{
    int st;
    t->fg = j;
    t->fg_in = j->in_w;
    while (j->state == J_RUN && !t->dead) pump(t, -1);
    t->fg = NULL;
    t->fg_in = -1;
    drain_display(t);
    if (j->state == J_STOP) {
        char m[PATH_MAX + 64];
        j->bg = 1;
        snprintf(m, sizeof m, "[%d]%c Stopped %s\n", j->id, job_mark(t, j), j->cmd);
        if (!t->at_bol) disp(t, "\n", 1);
        disp(t, m, strlen(m));
        return 128 + SIGTSTP;
    }
    if (j->state != J_DONE) return 130;
    st = j->status;
    report_signal(t, j);
    job_free(t, j);
    return st;
}

static Job *job_arg(Tab *t, const char *spec, const char *who)
{
    Job *j = NULL;
    int id;
    if (!spec || !strcmp(spec, "%%") || !strcmp(spec, "%+") || !strcmp(spec, "%")) j = job_by_id(t, t->cur_job);
    else if (!strcmp(spec, "%-")) j = job_by_id(t, t->prev_job);
    else if (spec[0] == '%' && to_int(spec + 1, &id)) j = job_by_id(t, id);
    else if (to_int(spec, &id)) j = job_by_id(t, id);
    if (!j) errf(t, "terminator: %s: %s: no such job", who, spec ? spec : "current");
    return j;
}

int bi_fg(Tab *t, int argc, char **argv)
{
    Job *j = job_arg(t, argc > 1 ? argv[1] : NULL, "fg");
    char m[PATH_MAX + 4];
    if (!j) return 1;
    if (j->state == J_DONE) { errf(t, "terminator: fg: job has terminated"); job_free(t, j); return 1; }
    snprintf(m, sizeof m, "%s\n", j->cmd);
    disp(t, m, strlen(m));
    j->bg = 0; j->notify = 0;
    if (t->cur_job != j->id) { t->prev_job = t->cur_job; t->cur_job = j->id; }
    if (j->state == J_STOP) {
        int k;
        for (k = 0; k < j->np; k++) if (j->p[k].state == 1) j->p[k].state = 0;
        j->state = J_RUN;
        kill(-j->pgid, SIGCONT);
    }
    return wait_fg(t, j);
}

int bi_bg(Tab *t, int argc, char **argv)
{
    Job *j = job_arg(t, argc > 1 ? argv[1] : NULL, "bg");
    char m[PATH_MAX + 32];
    int k;
    if (!j) return 1;
    if (j->state != J_STOP) { errf(t, "terminator: bg: job %d already in background", j->id); return 0; }
    for (k = 0; k < j->np; k++) if (j->p[k].state == 1) j->p[k].state = 0;
    j->state = J_RUN; j->bg = 1;
    kill(-j->pgid, SIGCONT);
    snprintf(m, sizeof m, "[%d]%c %s &\n", j->id, job_mark(t, j), j->cmd);
    emit(t, m);
    return 0;
}

int bi_jobs(Tab *t)
{
    int i;
    Buf b = {0};
    for (i = 0; i < MAXJOBS; i++) {
        Job *j = &t->jobs[i];
        if (!j->used) continue;
        b_printf(&b, "[%d]%c %s %s%s\n", j->id, job_mark(t, j), state_name(j), j->cmd, j->state == J_RUN ? " &" : "");
        if (j->state == J_DONE) j->notify = 1;
    }
    emit_fd(t, t->out_fd, b_str(&b), b.n);
    b_free(&b);
    jobs_notify(t);
    return 0;
}

int bi_kill(Tab *t, int argc, char **argv)
{
    static const struct { const char *n; int s; } sigs[] = { {"HUP", SIGHUP}, {"INT", SIGINT}, {"QUIT", SIGQUIT}, {"KILL", SIGKILL},
        {"TERM", SIGTERM}, {"STOP", SIGSTOP}, {"CONT", SIGCONT}, {"TSTP", SIGTSTP}, {"USR1", SIGUSR1}, {"USR2", SIGUSR2}, {NULL, 0} };
    int sig = SIGTERM, i = 1, rc = 0;
    if (argc > 1 && argv[1][0] == '-' && argv[1][1]) {
        const char *a = argv[1] + 1;
        int k, v;
        if (!strncmp(a, "SIG", 3)) a += 3;
        if (to_int(a, &v)) sig = v;
        else { for (k = 0; sigs[k].n; k++) if (!strcmp(sigs[k].n, a)) break; if (!sigs[k].n) { errf(t, "terminator: kill: %s: invalid signal", argv[1]); return 1; } sig = sigs[k].s; }
        i = 2;
    }
    if (i >= argc) { errf(t, "terminator: kill: usage: kill [-SIGNAL] pid | %%job ..."); return 2; }
    for (; i < argc; i++) {
        int pid;
        if (argv[i][0] == '%') {
            Job *j = job_arg(t, argv[i], "kill");
            if (!j) { rc = 1; continue; }
            kill(-j->pgid, sig);
            if (sig == SIGTERM || sig == SIGHUP) kill(-j->pgid, SIGCONT);
        } else if (to_int(argv[i], &pid)) { if (kill(pid, sig) < 0) { errf(t, "terminator: kill: (%d) - %s", pid, strerror(errno)); rc = 1; } }
        else { errf(t, "terminator: kill: %s: arguments must be process or job IDs", argv[i]); rc = 1; }
    }
    return rc;
}

void run_interrupt(Tab *t)
{
    static __thread int presses;
    if (t->mw_active) { mw_interrupt(t); t->status = 130; disp(t, "^C\n", 3); return; }
    disp(t, "^C\n", 3);
    if (t->fg_in >= 0 && t->fg) { close(t->fg_in); t->fg->in_w = -1; t->fg_in = -1; }
    t->intr = 1;
    presses++;
    if (t->fg) {
        kill(-t->fg->pgid, presses >= 2 ? SIGKILL : SIGINT);
        kill(-t->fg->pgid, SIGCONT);
    }
    if (!t->fg) presses = 0;
}

void run_suspend(Tab *t)
{
    if (t->mw_active || !t->fg || t->fg->state != J_RUN) return;
    disp(t, "^Z\n", 3);
    kill(-t->fg->pgid, SIGTSTP);
}

void run_char(Tab *t, unsigned char c)
{
    if (t->fg_in < 0) return;
    if (c == '\n') {
        b_addc(&t->fgline, '\n');
        if (write_all(t->fg_in, t->fgline.p, t->fgline.n) < 0) { }
        t->fgline.n = 0; t->fgline.p[0] = 0;
        disp(t, "\n", 1);
    } else if (c == 8 || c == 127) {
        if (t->fgline.n > 0) { t->fgline.p[--t->fgline.n] = 0; disp(t, "\b \b", 3); }
    } else if (c >= 32 && c < 127) { b_addc(&t->fgline, (char)c); disp(t, (char *)&c, 1); }
}

void run_eof(Tab *t)
{
    if (t->fg_in < 0) return;
    if (t->fgline.n) write_all(t->fg_in, t->fgline.p, t->fgline.n);
    t->fgline.n = 0; if (t->fgline.p) t->fgline.p[0] = 0;
    close(t->fg_in);
    if (t->fg) t->fg->in_w = -1;
    t->fg_in = -1;
}
