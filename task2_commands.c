/*
 * task2_commands.c
 * TASK 2 - built-in command dispatch (cd, echo, export, pwd, ...), running a parsed list / and-or list, exit status.
 */
#include "terminator.h"

static const char *const builtin_names[] = { "cd", "exit", "history", "multiWatch", "jobs", "fg", "bg", "source", ".",
    "export", "unset", "pwd", "echo", "true", "false", ":", "test", "[", "shift", "clear", "bind", "kill", NULL };

int is_builtin(const char *n)
{
    int i;
    for (i = 0; builtin_names[i]; i++) if (!strcmp(builtin_names[i], n)) return 1;
    return 0;
}

int exec_text(Tab *t, const char *text)
{
    Arena ar = {0};
    List *L = NULL;
    char err[300];
    int r = parse_text(&ar, text, &L, err, sizeof err), st;
    if (r == 1) { errf(t, "terminator: syntax error: unexpected end of file"); st = 2; }
    else if (r == 2) { errf(t, "terminator: %s", err); st = 2; }
    else st = exec_list(t, L);
    ar_free(&ar);
    t->status = st;
    return st;
}

static int exec_andor(Tab *t, AndOr *ao)
{
    int st = 0, i;
    for (i = 0; i < ao->n; i++) {
        if (i > 0) {
            if (ao->op[i] == 1 && st != 0) continue;
            if (ao->op[i] == 2 && st == 0) continue;
        }
        st = exec_pipe(t, ao->p[i]);
        t->status = st;
        if (t->stop_exec || t->intr || t->dead) break;
    }
    return st;
}

int exec_list(Tab *t, List *L)
{
    int st = t->status, i;
    for (i = 0; i < L->n; i++) {
        if (t->intr || t->stop_exec || t->dead || t->closing) break;
        st = exec_andor(t, L->a[i]);
        t->status = st;
    }
    return st;
}

static int bi_cd(Tab *t, int argc, char **argv)
{
    char target[PATH_MAX * 2], norm[PATH_MAX * 2];
    struct stat sb;
    const char *arg;
    int print = 0;
    if (argc > 2) { errf(t, "terminator: cd: too many arguments"); return 1; }
    if (argc == 1 || !strcmp(argv[1], "~")) arg = home_dir();
    else if (!strcmp(argv[1], "-")) { arg = t->oldcwd; print = 1; }
    else if (argv[1][0] == '~' && argv[1][1] == '/') { snprintf(target, sizeof target, "%s%s", home_dir(), argv[1] + 1); arg = target; }
    else arg = argv[1];
    path_join(norm, sizeof norm, t->cwd, arg);
    path_norm(norm);
    if (stat(norm, &sb) < 0) { errf(t, "terminator: cd: %s: %s", argc > 1 ? argv[1] : arg, strerror(errno)); return 1; }
    if (!S_ISDIR(sb.st_mode)) { errf(t, "terminator: cd: %s: Not a directory", argv[1]); return 1; }
    if (access(norm, X_OK) < 0) { errf(t, "terminator: cd: %s: %s", argv[1], strerror(errno)); return 1; }
    snprintf(t->oldcwd, sizeof t->oldcwd, "%s", t->cwd);
    snprintf(t->cwd, sizeof t->cwd, "%s", norm);
    var_set(t, "OLDPWD", t->oldcwd);
    var_set(t, "PWD", t->cwd);
    if (print) { char m[PATH_MAX + 2]; snprintf(m, sizeof m, "%s\n", t->cwd); emit(t, m); }
    return 0;
}

static int bi_echo(Tab *t, int argc, char **argv)
{
    int i = 1, nl = 1, esc = 0;
    Buf b = {0};
    for (; i < argc && argv[i][0] == '-' && argv[i][1] && strspn(argv[i] + 1, "neE") == strlen(argv[i] + 1); i++) {
        if (strchr(argv[i], 'n')) nl = 0;
        if (strchr(argv[i], 'e')) esc = 1;
        if (strchr(argv[i], 'E')) esc = 0;
    }
    for (; i < argc; i++) {
        const char *s = argv[i];
        if (!esc) b_adds(&b, s);
        else for (; *s; s++) {
            if (*s != '\\' || !s[1]) { b_addc(&b, *s); continue; }
            s++;
            switch (*s) {
            case 'n': b_addc(&b, '\n'); break;
            case 't': b_addc(&b, '\t'); break;
            case 'r': b_addc(&b, '\r'); break;
            case 'a': b_addc(&b, '\a'); break;
            case 'b': b_addc(&b, '\b'); break;
            case 'e': b_addc(&b, 27); break;
            case '\\': b_addc(&b, '\\'); break;
            case 'c': nl = 0; goto done;
            default: b_addc(&b, '\\'); b_addc(&b, *s);
            }
        }
        if (i + 1 < argc) b_addc(&b, ' ');
    }
done:
    if (nl) b_addc(&b, '\n');
    emit_fd(t, t->out_fd, b_str(&b), b.n);
    b_free(&b);
    return 0;
}

int run_builtin(Tab *t, int argc, char **argv)
{
    const char *n = argv[0];
    int i, v;
    if (!strcmp(n, "cd")) return bi_cd(t, argc, argv);
    if (!strcmp(n, "exit")) {
        int code = t->status;
        if (argc > 1) { if (!to_int(argv[1], &code)) { errf(t, "terminator: exit: %s: numeric argument required", argv[1]); code = 2; } }
        t->status = code & 255;
        if (t->depth > 0) t->stop_exec = 1; else t->closing = 1;
        return code & 255;
    }
    if (!strcmp(n, "history")) {
        v = 1000;
        if (argc > 1) {
            if (!strcmp(argv[1], "-c")) { errf(t, "terminator: history: -c is not supported (history is shared by all tabs)"); return 1; }
            if (!to_int(argv[1], &v) || v <= 0) { errf(t, "terminator: history: %s: numeric argument required", argv[1]); return 1; }
        }
        hist_show(t, v);
        return 0;
    }
    if (!strcmp(n, "multiWatch")) return bi_multiwatch(t);
    if (!strcmp(n, "jobs")) return bi_jobs(t);
    if (!strcmp(n, "fg")) return bi_fg(t, argc, argv);
    if (!strcmp(n, "bg")) return bi_bg(t, argc, argv);
    if (!strcmp(n, "source") || !strcmp(n, ".")) return bi_source(t, argc, argv);
    if (!strcmp(n, "export")) {
        for (i = 1; i < argc; i++) {
            char *eq = strchr(argv[i], '=');
            if (eq) { *eq = 0; if (valid_name(argv[i], strlen(argv[i]))) var_set(t, argv[i], eq + 1); else errf(t, "terminator: export: `%s': not a valid identifier", argv[i]); *eq = '='; }
        }
        if (argc == 1) {
            Buf b = {0};
            for (i = 0; i < t->nenv; i++) b_printf(&b, "export %s\n", t->env[i]);
            emit_fd(t, t->out_fd, b_str(&b), b.n);
            b_free(&b);
        }
        return 0;
    }
    if (!strcmp(n, "unset")) { for (i = 1; i < argc; i++) var_unset(t, argv[i]); return 0; }
    if (!strcmp(n, "pwd")) { char m[PATH_MAX + 2]; snprintf(m, sizeof m, "%s\n", t->cwd); emit(t, m); return 0; }
    if (!strcmp(n, "echo")) return bi_echo(t, argc, argv);
    if (!strcmp(n, "true") || !strcmp(n, ":")) return 0;
    if (!strcmp(n, "false")) return 1;
    if (!strcmp(n, "test") || !strcmp(n, "[")) return bi_test(t, argc, argv);
    if (!strcmp(n, "shift")) {
        int k = 1;
        if (argc > 1 && !to_int(argv[1], &k)) { errf(t, "terminator: shift: %s: numeric argument required", argv[1]); return 1; }
        if (k < 0 || k > t->nargs) return 1;
        t->args += k; t->nargs -= k;
        return 0;
    }
    if (!strcmp(n, "clear")) { if (t->out_fd == t->dpw) { gui_puts(t, "\033[2J"); t->at_bol = 1; } return 0; }
    if (!strcmp(n, "bind")) return bi_bind(t, argc, argv);
    if (!strcmp(n, "kill")) return bi_kill(t, argc, argv);
    return 127;
}
