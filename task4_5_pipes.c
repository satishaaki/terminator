/*
 * task4_5_pipes.c
 * TASKS 4+5 - I/O redirection (< > >> 2> 2>&1 with dup2) and pipelines of any length (N commands, N-1 pipes, fork + execvp).
 */
#include "terminator.h"

typedef struct {
    Vec av, asg;                     /* expanded words / NAME=value prefixes */
    int in_fd, out_fd, err_fd;       /* redirection files, opened by the shell (-1: none) */
    int err_to_out, out_to_err;
    int kind;                        /* 0 external, 1 builtin, 2 script, 3 assignments only */
    char *script, *cstr;             /* script path (kind 2) or `bash -c` string */
    int sarg0;                       /* index in av of the first script argument */
    const char *text;
} Stage;

static char **build_env(Tab *t, Vec *asg)
{
    char **e = xmalloc(sizeof(char *) * (size_t)(t->nenv + asg->n + 2));
    int n = 0, i, k;
    for (i = 0; i < t->nenv; i++) {
        int over = 0;
        for (k = 0; k < asg->n; k++) {
            size_t nl = (size_t)(strchr(asg->v[k], '=') - asg->v[k]);
            if (!strncmp(t->env[i], asg->v[k], nl) && t->env[i][nl] == '=') over = 1;
        }
        if (!over) e[n++] = t->env[i];
    }
    for (i = 0; i < asg->n; i++) e[n++] = asg->v[i];
    e[n] = NULL;
    return e;
}

static pid_t spawn_child(Tab *t, Stage *s, int cin, int cout, int cerr, pid_t pgid, char **envp)
{
    char nf[PATH_MAX + 80], pd[PATH_MAX + 80];
    char *a0 = s->av.v[0];
    pid_t pid;
    if (strchr(a0, '/')) snprintf(nf, sizeof nf, "terminator: %s: No such file or directory\n", a0);
    else snprintf(nf, sizeof nf, "terminator: %s: command not found\n", a0);
    snprintf(pd, sizeof pd, "terminator: %s: Permission denied\n", a0);
    pthread_mutex_lock(&spawn_lock);
    pid = fork();
    if (pid == 0) {
        int e;
        child_signals();
        if (!t->in_sub) setpgid(0, pgid ? pgid : 0);
        if (cin != 0) dup2(cin, 0);
        if (cout != 1) dup2(cout, 1);
        if (cerr != 2) dup2(cerr, 2);
        if (chdir(t->cwd) < 0) { }
        environ = envp;
        execvp(a0, s->av.v);
        e = errno;
        if (e == ENOENT) { if (write(2, nf, strlen(nf)) < 0) { } _exit(127); }
        if (write(2, pd, strlen(pd)) < 0) { }
        _exit(126);
    }
    pthread_mutex_unlock(&spawn_lock);
    if (pid > 0) {
        if (!t->in_sub) setpgid(pid, pgid ? pgid : pid);
        start_waiter(t, pid);
    }
    return pid;
}

static void stage_close(Stage *s)
{
    if (s->in_fd >= 0) close(s->in_fd);
    if (s->out_fd >= 0) close(s->out_fd);
    if (s->err_fd >= 0) close(s->err_fd);
    s->in_fd = s->out_fd = s->err_fd = -1;
}

static int apply_redirs(Tab *t, Cmd *c, Stage *s)
{
    Redir *r;
    for (r = c->rd; r; r = r->next) {
        char *path = expand_one(t, r->target), full[PATH_MAX * 2];
        int fd, *slot;
        if (r->kind == 3) {                                   /* >&N */
            if (r->fd == 2 && !strcmp(path, "1")) s->err_to_out = 1;
            else if (r->fd == 1 && !strcmp(path, "2")) s->out_to_err = 1;
            else if (strspn(path, "0123456789") != strlen(path) || !*path) { errf(t, "terminator: %s: ambiguous redirect", path); free(path); return 1; }
            free(path);
            continue;
        }
        path_join(full, sizeof full, t->cwd, path);
        if (r->kind == 0) fd = open(full, O_RDONLY | O_CLOEXEC);
        else fd = open(full, O_WRONLY | O_CREAT | O_CLOEXEC | (r->kind == 1 ? O_TRUNC : O_APPEND), 0644);
        if (fd < 0) { errf(t, "terminator: %s: %s", path, strerror(errno)); free(path); return 1; }
        free(path);
        slot = r->fd == 0 ? &s->in_fd : (r->fd == 2 ? &s->err_fd : &s->out_fd);
        if (*slot >= 0) close(*slot);
        *slot = fd;
        if (r->fd == 2) s->err_to_out = 0;
        if (r->fd == 1) s->out_to_err = 0;
    }
    return 0;
}

static int prepare_stage(Tab *t, Cmd *c, Stage *s)
{
    int k = 0, rc;
    Vec rest = {0};
    memset(s, 0, sizeof *s);
    s->in_fd = s->out_fd = s->err_fd = -1;
    s->text = c->text;
    s->sarg0 = 1;
    while (k < c->nw) {                                      /* NAME=value prefixes */
        const char *w = c->w[k], *eq = strchr(w, '=');
        char *val, *asg;
        if (!eq || !valid_name(w, (size_t)(eq - w))) break;
        val = expand_one(t, eq + 1);
        asg = xmalloc(strlen(w) + strlen(val) + 2);
        sprintf(asg, "%.*s=%s", (int)(eq - w), w, val);
        free(val);
        v_push(&s->asg, asg);
        k++;
    }
    for (; k < c->nw; k++) expand_word(t, c->w[k], &s->av, 1);
    (void)rest;
    rc = apply_redirs(t, c, s);
    if (rc) { stage_close(s); v_free(&s->av); v_free(&s->asg); return rc; }
    if (s->av.n == 0) { s->kind = 3; return 0; }
    {
        char *a0 = s->av.v[0];
        const char *base = strrchr(a0, '/') ? strrchr(a0, '/') + 1 : a0;
        char full[PATH_MAX * 2];
        if (is_builtin(a0)) { s->kind = 1; return 0; }
        if (!strcmp(base, "bash") || !strcmp(base, "sh")) {
            int i = 1;
            for (; i < s->av.n; i++) {
                char *a = s->av.v[i];
                if (!strcmp(a, "-c") && i + 1 < s->av.n) { s->cstr = s->av.v[i + 1]; s->sarg0 = i + 2; s->kind = 2; return 0; }
                if (!strcmp(a, "--")) { i++; break; }
                if (a[0] == '-' && a[1]) continue;
                break;
            }
            if (i < s->av.n) {
                path_join(full, sizeof full, t->cwd, s->av.v[i]);
                s->script = xstrdup(full);
                s->sarg0 = i + 1;
                s->kind = 2;
                return 0;
            }
            errf(t, "terminator: %s: reading commands from the keyboard is not supported (give a script file or -c)", base);
            stage_close(s); v_free(&s->av); v_free(&s->asg);
            return 2;
        }
        if (strchr(a0, '/')) {
            path_join(full, sizeof full, t->cwd, a0);
            if (file_is_script(full)) { s->script = xstrdup(full); s->sarg0 = 1; s->kind = 2; return 0; }
        }
        s->kind = 0;
    }
    return 0;
}

static void stage_free(Stage *s) { stage_close(s); v_free(&s->av); v_free(&s->asg); free(s->script); }

/* run a builtin / script / assignment stage inside the engine */
static int run_inengine(Tab *t, Stage *s)
{
    int st = 0, i;
    t->cmdtext = s->text;
    switch (s->kind) {
    case 3:
        for (i = 0; i < s->asg.n; i++) {
            char *eq = strchr(s->asg.v[i], '=');
            *eq = 0;
            var_set(t, s->asg.v[i], eq + 1);
            *eq = '=';
        }
        return 0;
    case 1: return run_builtin(t, s->av.n, s->av.v);
    case 2:
        if (s->cstr) return run_script(t, s->cstr, s->av.v + (s->sarg0 < s->av.n ? s->sarg0 : s->av.n), s->av.n > s->sarg0 ? s->av.n - s->sarg0 : 0, 1, s->cstr);
        return run_script(t, s->script, s->av.v + (s->sarg0 < s->av.n ? s->sarg0 : s->av.n), s->av.n > s->sarg0 ? s->av.n - s->sarg0 : 0, 1, NULL);
    }
    return st;
}

int exec_pipe(Tab *t, Pipe *pl)
{
    int n = pl->n, i, st = 0, bg = pl->bg, next = 0, last_inengine_status = 0, last_is_ext;
    Stage *S;
    int (*pp)[2] = NULL;
    pid_t pgid = 0, *pids;
    Job *job = NULL;
    int stdin_r = -1, stdin_w = -1;

    if (n == 1 && pl->c[0]->ifn) {
        st = exec_if(t, pl->c[0]->ifn);
        return pl->neg ? !st : st;
    }
    S = xmalloc(sizeof(Stage) * (size_t)n);
    memset(S, 0, sizeof(Stage) * (size_t)n);
    for (i = 0; i < n; i++) {
        int rc = prepare_stage(t, pl->c[i], &S[i]);
        if (rc) {
            int k;
            for (k = 0; k < i; k++) stage_free(&S[k]);
            free(S);
            return rc;
        }
    }
        if (n == 1 && S[0].kind != 0) {
        int so = t->out_fd, se = t->err_fd;
        if (S[0].out_fd >= 0) t->out_fd = S[0].out_fd;
        if (S[0].err_fd >= 0) t->err_fd = S[0].err_fd;
        if (S[0].err_to_out) t->err_fd = t->out_fd;
        if (S[0].out_to_err) t->out_fd = t->err_fd;
        st = run_inengine(t, &S[0]);
        t->out_fd = so; t->err_fd = se;
        stage_free(&S[0]);
        free(S);
        return pl->neg ? !st : st;
    }
        pids = xmalloc(sizeof(pid_t) * (size_t)n);
    for (i = 0; i < n; i++) pids[i] = -1;
    if (n > 1) {
        pp = xmalloc(sizeof(int[2]) * (size_t)(n - 1));
        for (i = 0; i < n - 1; i++) if (mkpipe(pp[i]) < 0) { pp[i][0] = pp[i][1] = -1; errf(t, "terminator: pipe: %s", strerror(errno)); }
    }
    if (S[0].kind == 0 && S[0].in_fd < 0) {
        if (bg || t->in_sub) stdin_r = t->devnull;
        else { int p[2]; if (mkpipe(p) == 0) { stdin_r = p[0]; stdin_w = p[1]; } else stdin_r = t->devnull; }
    }
    for (i = 0; i < n; i++) {
        int cin, cout, cerr;
        char **envp;
        if (S[i].kind != 0) continue;
        if (S[i].in_fd >= 0) cin = S[i].in_fd;
        else if (i > 0) cin = pp[i - 1][0];
        else cin = stdin_r >= 0 ? stdin_r : t->devnull;
        if (S[i].out_fd >= 0) cout = S[i].out_fd; else cout = i < n - 1 ? pp[i][1] : (t->out_fd == t->dpw ? t->dpw : t->out_fd);
        cerr = S[i].err_fd >= 0 ? S[i].err_fd : t->err_fd;
        if (S[i].err_to_out) cerr = cout;
        if (S[i].out_to_err) cout = cerr;
        envp = build_env(t, &S[i].asg);
        pids[i] = spawn_child(t, &S[i], cin, cout, cerr, pgid, envp);
        free(envp);
        if (pids[i] < 0) { errf(t, "terminator: fork: %s", strerror(errno)); continue; }
        if (!pgid) pgid = pids[i];
        next++;
    }
    /* the shell keeps only the ends it still needs */
    if (stdin_r >= 0 && stdin_r != t->devnull) close(stdin_r);
    for (i = 0; i < n - 1; i++) {
        if (S[i].kind == 0 && pp[i][1] >= 0) { close(pp[i][1]); pp[i][1] = -1; }
        if (S[i + 1].kind == 0 && pp[i][0] >= 0) { close(pp[i][0]); pp[i][0] = -1; }
    }
    if (next > 0) {
        char *txt = pl->text;
        int k = 0;
        job = job_new(t, pgid, next, txt, bg);
        if (job) {
            for (i = 0; i < n; i++) if (pids[i] > 0) job->p[k++].pid = pids[i];
            job->np = k;
            job->in_w = bg ? -1 : stdin_w;
            if (bg) close(stdin_w < 0 ? -1 : stdin_w);
        }
    } else if (stdin_w >= 0) close(stdin_w);
    if (job) { t->fg = bg ? NULL : job; t->fg_in = bg ? -1 : job->in_w; }
    /* in-engine stages of a pipeline run now, writing into their pipe */
    last_is_ext = S[n - 1].kind == 0;
    for (i = 0; i < n; i++) {
        int so, se, rc;
        if (S[i].kind == 0) continue;
        so = t->out_fd; se = t->err_fd;
        if (S[i].out_fd >= 0) t->out_fd = S[i].out_fd;
        else if (i < n - 1) t->out_fd = pp[i][1];
        if (S[i].err_fd >= 0) t->err_fd = S[i].err_fd;
        if (S[i].err_to_out) t->err_fd = t->out_fd;
        if (S[i].out_to_err) t->out_fd = t->err_fd;
        rc = run_inengine(t, &S[i]);
        t->out_fd = so; t->err_fd = se;
        if (i == n - 1) last_inengine_status = rc;
        if (i < n - 1 && pp[i][1] >= 0) { close(pp[i][1]); pp[i][1] = -1; }
        if (i > 0 && pp[i - 1][0] >= 0) { close(pp[i - 1][0]); pp[i - 1][0] = -1; }
    }
    for (i = 0; i < n - 1; i++) { if (pp[i][0] >= 0) close(pp[i][0]); if (pp[i][1] >= 0) close(pp[i][1]); }
    if (job && bg) {
        char m[PATH_MAX + 64];
        snprintf(m, sizeof m, "[%d] %d\n", job->id, (int)pgid);
        disp(t, m, strlen(m));
        st = 0;
    } else if (job) st = wait_fg(t, job);
    else st = last_inengine_status;
    if (job && !last_is_ext && !bg) st = last_inengine_status;
    for (i = 0; i < n; i++) stage_free(&S[i]);
    free(S); free(pp); free(pids);
    return pl->neg ? !st : st;
}
