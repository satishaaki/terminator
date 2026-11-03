/*
 * task11_scripts.c
 * TASK 11 - bash script interpreter: ./script.sh, bash script.sh, source; if/elif/else/fi execution; test / [ ] builtin; $1 $# $?; exit.
 */
#include "terminator.h"

typedef struct { Tab *t; char **a; int n, p, err; const char *who; } TestP;

static int test_or(TestP *T);

static void env_snapshot(Tab *t, Vec *v) { int i; for (i = 0; i < t->nenv; i++) v_push(v, xstrdup(t->env[i])); }

static void env_restore(Tab *t, Vec *v)
{
    int i;
    for (i = 0; i < t->nenv; i++) free(t->env[i]);
    t->nenv = 0;
    for (i = 0; i < v->n; i++) {
        if (t->nenv + 2 > t->cenv) { t->cenv *= 2; t->env = xrealloc(t->env, sizeof(char *) * (size_t)t->cenv); }
        t->env[t->nenv++] = xstrdup(v->v[i]);
    }
    t->env[t->nenv] = NULL;
}

int file_is_script(const char *full)
{
    char head[160];
    ssize_t n;
    struct stat sb;
    int fd;
    size_t l = strlen(full);
    if (stat(full, &sb) < 0 || !S_ISREG(sb.st_mode)) return 0;
    fd = open(full, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    n = read(fd, head, sizeof head - 1);
    close(fd);
    if (n < 2) return 0;
    head[n] = 0;
    if (head[0] == '#' && head[1] == '!') {
        char *nl = strchr(head, '\n');
        if (nl) *nl = 0;
        return strstr(head, "bash") != NULL || strstr(head, "/sh") != NULL || strstr(head, " sh") != NULL;
    }
    if ((unsigned char)head[0] == 0x7f || (unsigned char)head[0] == 0xcf || (unsigned char)head[0] == 0xca || (unsigned char)head[0] == 0xfe)
        return 0;                                             /* ELF / Mach-O */
    return l > 3 && !strcmp(full + l - 3, ".sh");
}

int exec_if(Tab *t, IfNode *f)
{
    int i;
    for (i = 0; i < f->n; i++) {
        int st = exec_list(t, f->cond[i]);
        if (t->intr || t->stop_exec || t->dead) return st;
        if (st == 0) return exec_list(t, f->body[i]);
    }
    if (f->els) return exec_list(t, f->els);
    return 0;
}

int run_script(Tab *t, const char *path, char **args, int nargs, int subshell, const char *cstr)
{
    char *src = NULL, err[300], **sa = NULL, **oargs = t->args, *oarg0 = t->arg0;
    int onargs = t->nargs, st, i, r;
    Arena ar = {0};
    List *L = NULL;
    Vec snap = {0};
    char ocwd[PATH_MAX], oold[PATH_MAX];
    if (t->depth > 40) { errf(t, "terminator: %s: too many nested scripts", path); return 1; }
    if (cstr) src = xstrdup(cstr);
    else {
        FILE *f = fopen(path, "rb");
        size_t cap = 4096, n = 0;
        if (!f) { errf(t, "terminator: %s: %s", path, strerror(errno)); return 127; }
        src = xmalloc(cap);
        while (!feof(f)) {
            size_t got;
            if (n + 4096 + 1 > cap) { cap *= 2; src = xrealloc(src, cap); }
            got = fread(src + n, 1, cap - n - 1, f);
            n += got;
            if (!got) break;
        }
        src[n] = 0;
        fclose(f);
        if (memchr(src, 0, n)) { errf(t, "terminator: %s: cannot execute binary file", path); free(src); return 126; }
    }
    r = parse_text(&ar, src, &L, err, sizeof err);
    if (r == 1) { errf(t, "terminator: %s: syntax error: unexpected end of file (missing `fi' or closing quote?)", path); ar_free(&ar); free(src); return 2; }
    if (r == 2) { errf(t, "terminator: %s: %s", path, err); ar_free(&ar); free(src); return 2; }
    if (subshell) { env_snapshot(t, &snap); snprintf(ocwd, sizeof ocwd, "%s", t->cwd); snprintf(oold, sizeof oold, "%s", t->oldcwd); }
    sa = xmalloc(sizeof(char *) * (size_t)(nargs + 1));
    for (i = 0; i < nargs; i++) sa[i] = xstrdup(args[i]);
    if (subshell || nargs > 0) { t->args = sa; t->nargs = nargs; }
    if (subshell) t->arg0 = xstrdup(cstr ? "bash" : path);
    t->depth++;
    st = exec_list(t, L);
    t->depth--;
    if (t->stop_exec) { t->stop_exec = 0; st = t->status; }
    if (subshell) {
        free(t->arg0);
        t->arg0 = oarg0;
        env_restore(t, &snap);
        snprintf(t->cwd, sizeof t->cwd, "%s", ocwd);
        snprintf(t->oldcwd, sizeof t->oldcwd, "%s", oold);
        v_free(&snap);
    }
    if (subshell || nargs > 0) { t->args = oargs; t->nargs = onargs; for (i = 0; i < nargs; i++) free(sa[i]); free(sa); }
    else { for (i = 0; i < nargs; i++) free(sa[i]); free(sa); }
    ar_free(&ar);
    free(src);
    return st;
}

/* ---- test / [ */
static int file_test(Tab *t, const char *op, const char *arg)
{
    char full[PATH_MAX * 2];
    struct stat sb;
    int lst = (!strcmp(op, "-L") || !strcmp(op, "-h"));
    path_join(full, sizeof full, t->cwd, arg);
    if ((lst ? lstat(full, &sb) : stat(full, &sb)) < 0) return 0;
    if (!strcmp(op, "-e")) return 1;
    if (!strcmp(op, "-f")) return S_ISREG(sb.st_mode);
    if (!strcmp(op, "-d")) return S_ISDIR(sb.st_mode);
    if (lst) return S_ISLNK(sb.st_mode);
    if (!strcmp(op, "-s")) return sb.st_size > 0;
    if (!strcmp(op, "-r")) return access(full, R_OK) == 0;
    if (!strcmp(op, "-w")) return access(full, W_OK) == 0;
    if (!strcmp(op, "-x")) return access(full, X_OK) == 0;
    return 0;
}

static int is_unary(const char *s) { return strlen(s) == 2 && s[0] == '-' && strchr("fdersw xLhzn", s[1]) && s[1] != ' '; }

static int is_binary(const char *s)
{
    static const char *const b[] = { "=", "==", "!=", "-eq", "-ne", "-gt", "-ge", "-lt", "-le", NULL };
    int i;
    for (i = 0; b[i]; i++) if (!strcmp(s, b[i])) return 1;
    return 0;
}

static int test_primary(TestP *T)
{
    int left = T->n - T->p;
    char **a = T->a + T->p;
    if (left <= 0) { T->err = 1; return 0; }
    if (left >= 3 && is_binary(a[1])) {
        const char *x = a[0], *op = a[1], *y = a[2];
        T->p += 3;
        if (!strcmp(op, "=") || !strcmp(op, "==")) return strcmp(x, y) == 0;
        if (!strcmp(op, "!=")) return strcmp(x, y) != 0;
        {
            int xi, yi;
            if (!to_int(x, &xi)) { errf(T->t, "terminator: %s: %s: integer expression expected", T->who, x); T->err = 2; return 0; }
            if (!to_int(y, &yi)) { errf(T->t, "terminator: %s: %s: integer expression expected", T->who, y); T->err = 2; return 0; }
            if (!strcmp(op, "-eq")) return xi == yi;
            if (!strcmp(op, "-ne")) return xi != yi;
            if (!strcmp(op, "-gt")) return xi > yi;
            if (!strcmp(op, "-ge")) return xi >= yi;
            if (!strcmp(op, "-lt")) return xi < yi;
            return xi <= yi;
        }
    }
    if (!strcmp(a[0], "(") && left >= 3) {
        int r;
        T->p++;
        r = test_or(T);
        if (T->p < T->n && !strcmp(T->a[T->p], ")")) T->p++; else T->err = 1;
        return r;
    }
    if (left >= 2 && is_unary(a[0])) {
        T->p += 2;
        if (!strcmp(a[0], "-z")) return a[1][0] == 0;
        if (!strcmp(a[0], "-n")) return a[1][0] != 0;
        return file_test(T->t, a[0], a[1]);
    }
    T->p++;
    return a[0][0] != 0;
}

static int test_not(TestP *T)
{
    if (T->p < T->n && !strcmp(T->a[T->p], "!") && !(T->n - T->p >= 3 && is_binary(T->a[T->p + 1]))) { T->p++; return !test_not(T); }
    return test_primary(T);
}

static int test_and(TestP *T)
{
    int r = test_not(T);
    while (T->p < T->n && !strcmp(T->a[T->p], "-a")) { int q; T->p++; q = test_not(T); r = r && q; }
    return r;
}

static int test_or(TestP *T)
{
    int r = test_and(T);
    while (T->p < T->n && !strcmp(T->a[T->p], "-o")) { int q; T->p++; q = test_and(T); r = r || q; }
    return r;
}

int bi_test(Tab *t, int argc, char **argv)
{
    TestP T;
    int r;
    memset(&T, 0, sizeof T);
    T.t = t; T.a = argv + 1; T.n = argc - 1; T.who = argv[0];
    if (!strcmp(argv[0], "[")) {
        if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) { errf(t, "terminator: [: missing `]'"); return 2; }
        T.n--;
    }
    if (T.n == 0) return 1;
    r = test_or(&T);
    if (T.err == 2) return 2;
    if (T.err || T.p < T.n) { errf(t, "terminator: %s: syntax error: unexpected argument `%s'", argv[0], T.p < T.n ? T.a[T.p] : ""); return 2; }
    return r ? 0 : 1;
}

int bi_source(Tab *t, int argc, char **argv)
{
    char full[PATH_MAX * 2];
    struct stat sb;
    if (argc < 2) { errf(t, "terminator: %s: filename argument required", argv[0]); return 2; }
    path_join(full, sizeof full, t->cwd, argv[1]);
    if (stat(full, &sb) < 0 && !strchr(argv[1], '/')) {          /* look in PATH like bash does */
        const char *pa = var_get(t, "PATH");
        char *copy = xstrdup(pa ? pa : ""), *d, *save = NULL;
        for (d = strtok_r(copy, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
            char c2[PATH_MAX * 2];
            snprintf(c2, sizeof c2, "%s/%s", d, argv[1]);
            if (stat(c2, &sb) == 0) { snprintf(full, sizeof full, "%s", c2); break; }
        }
        free(copy);
    }
    return run_script(t, full, argv + 2, argc - 2, 0, NULL);
}
