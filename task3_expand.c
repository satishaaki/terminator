/*
 * task3_expand.c
 * TASK 3 - word expansion: quoting rules, $VAR, $?, $#, $(...) and backticks (command substitution), $((..)), ~, globbing.
 */
#include "terminator.h"

        /* defined with the executor */

typedef struct { Vec *out; Buf cur, q; int have, full; Tab *t; } XS;

/* $(( ... )) integer arithmetic: + - * / % ( ) unary -, comparisons, variables */
typedef struct { Tab *t; const char *p; } Ar;

static char *run_subst(Tab *t, const char *cmd);
static long ar_cmp(Ar *a);

static void xs_add(XS *x, char c, int quoted) { b_addc(&x->cur, c); b_addc(&x->q, quoted ? 1 : 0); x->have = 1; }

static void glob_escape(Buf *b, const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) { if (strchr("*?[]\\", s[i])) b_addc(b, '\\'); b_addc(b, s[i]); }
}

static void xs_flush(XS *x)
{
    int i, wild = 0;
    if (!x->have) return;
    if (x->full)
        for (i = 0; i < (int)x->cur.n; i++) if (!x->q.p[i] && strchr("*?[", x->cur.p[i])) { wild = 1; break; }
    if (wild) {
        Buf pat = {0};
        glob_t g;
        size_t strip = 0;
        int abs = (x->cur.p[0] == '/' && !x->q.p[0]) || x->cur.p[0] == '/';
        if (!abs) { glob_escape(&pat, x->t->cwd, strlen(x->t->cwd)); b_addc(&pat, '/'); strip = strlen(x->t->cwd) + 1; }
        for (i = 0; i < (int)x->cur.n; i++) {
            if (x->q.p[i] && strchr("*?[]\\", x->cur.p[i])) b_addc(&pat, '\\');
            b_addc(&pat, x->cur.p[i]);
        }
        memset(&g, 0, sizeof g);
        if (glob(b_str(&pat), 0, NULL, &g) == 0 && g.gl_pathc > 0) {
            size_t k;
            for (k = 0; k < g.gl_pathc; k++) {
                const char *m = g.gl_pathv[k];
                if (strip && strlen(m) > strip) m += strip;
                v_push(x->out, xstrdup(m));
            }
        } else v_push(x->out, xstrdup(b_str(&x->cur)));
        globfree(&g);
        b_free(&pat);
    } else v_push(x->out, xstrdup(b_str(&x->cur)));
    x->cur.n = 0; if (x->cur.p) x->cur.p[0] = 0;
    x->q.n = 0; if (x->q.p) x->q.p[0] = 0;
    x->have = 0;
}

static void xs_text(XS *x, const char *s, int quoted)
{
    if (quoted || !x->full) {
        x->have = 1;
        for (; *s; s++) xs_add(x, *s, 1);
        return;
    }
    for (; *s; s++) {
        if (*s == ' ' || *s == '\t' || *s == '\n') xs_flush(x);
        else xs_add(x, *s, 0);
    }
}

static char *unescape_bt(const char *s, size_t n)
{
    Buf b = {0};
    size_t i;
    for (i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n && strchr("`$\\", s[i + 1])) i++;
        b_addc(&b, s[i]);
    }
    return b.p ? b.p : xstrdup("");
}

static void subst_into(XS *x, const char *cmd, int quoted)
{
    char *r = run_subst(x->t, cmd);
    xs_text(x, r, quoted);
    free(r);
}

static void positional_all(XS *x, int quoted, int at)
{
    Tab *t = x->t;
    int i;
    if (quoted && at) {
        for (i = 0; i < t->nargs; i++) { xs_text(x, t->args[i], 1); if (i + 1 < t->nargs) xs_flush(x); }
    } else {
        Buf b = {0};
        for (i = 0; i < t->nargs; i++) { if (i) b_addc(&b, ' '); b_adds(&b, t->args[i]); }
        xs_text(x, b_str(&b), quoted);
        b_free(&b);
    }
}

static void ar_ws(Ar *a) { while (*a->p == ' ' || *a->p == '\t' || *a->p == '\n') a->p++; }

static long ar_atom(Ar *a)
{
    long v = 0;
    ar_ws(a);
    if (*a->p == '(') { a->p++; v = ar_cmp(a); ar_ws(a); if (*a->p == ')') a->p++; return v; }
    if (*a->p == '-') { a->p++; return -ar_atom(a); }
    if (*a->p == '+') { a->p++; return ar_atom(a); }
    if (*a->p == '!') { a->p++; return !ar_atom(a); }
    if (isdigit((unsigned char)*a->p)) { char *e; v = strtol(a->p, &e, 0); a->p = e; return v; }
    if (*a->p == '$') a->p++;
    if (isalpha((unsigned char)*a->p) || *a->p == '_') {
        char nm[128]; size_t n = 0; const char *val;
        while ((isalnum((unsigned char)*a->p) || *a->p == '_') && n < sizeof nm - 1) nm[n++] = *a->p++;
        nm[n] = 0;
        val = var_get(a->t, nm);
        return val ? strtol(val, NULL, 0) : 0;
    }
    return 0;
}

static long ar_mul(Ar *a)
{
    long v = ar_atom(a);
    for (;;) {
        ar_ws(a);
        if (*a->p == '*') { a->p++; v *= ar_atom(a); }
        else if (*a->p == '/') { long d; a->p++; d = ar_atom(a); v = d ? v / d : 0; }
        else if (*a->p == '%') { long d; a->p++; d = ar_atom(a); v = d ? v % d : 0; }
        else return v;
    }
}

static long ar_add(Ar *a)
{
    long v = ar_mul(a);
    for (;;) {
        ar_ws(a);
        if (*a->p == '+') { a->p++; v += ar_mul(a); }
        else if (*a->p == '-') { a->p++; v -= ar_mul(a); }
        else return v;
    }
}

static long ar_cmp(Ar *a)
{
    long v = ar_add(a);
    for (;;) {
        ar_ws(a);
        if (!strncmp(a->p, "==", 2)) { a->p += 2; v = v == ar_add(a); }
        else if (!strncmp(a->p, "!=", 2)) { a->p += 2; v = v != ar_add(a); }
        else if (!strncmp(a->p, "<=", 2)) { a->p += 2; v = v <= ar_add(a); }
        else if (!strncmp(a->p, ">=", 2)) { a->p += 2; v = v >= ar_add(a); }
        else if (*a->p == '<') { a->p++; v = v < ar_add(a); }
        else if (*a->p == '>') { a->p++; v = v > ar_add(a); }
        else return v;
    }
}

static void expand_dollar(XS *x, const char *raw, long *ip, int quoted)
{
    Tab *t = x->t;
    long i = *ip;                       /* raw[i] == '$' */
    char c = raw[i + 1], name[256];
    const char *v;
    char num[32];
    if (c == '(' && raw[i + 2] == '(') {                           /* $(( expr )) */
        long j = scan_paren(raw, i + 2);
        if (j > i + 5 && raw[j - 2] == ')') {
            char *ex = xstrndup(raw + i + 3, (size_t)(j - 2 - (i + 3)));
            Ar ar; ar.t = t; ar.p = ex;
            snprintf(num, sizeof num, "%ld", ar_cmp(&ar));
            free(ex);
            xs_text(x, num, quoted);
            *ip = j;
            return;
        }
    }
    if (c == '(') {
        long j = scan_paren(raw, i + 2);
        char *inner;
        if (j < 0) { xs_add(x, '$', 0); *ip = i + 1; return; }
        inner = xstrndup(raw + i + 2, (size_t)(j - 1 - (i + 2)));
        subst_into(x, inner, quoted);
        free(inner);
        *ip = j;
    } else if (c == '{') {
        long j = scan_brace(raw, i + 2);
        char *inner, *dflt = NULL;
        const char *val;
        if (j < 0) { xs_add(x, '$', 0); *ip = i + 1; return; }
        inner = xstrndup(raw + i + 2, (size_t)(j - 1 - (i + 2)));
        if ((dflt = strstr(inner, ":-")) != NULL) { *dflt = 0; dflt += 2; }
        if (inner[0] == '#' && inner[1]) { val = var_get(t, inner + 1); snprintf(num, sizeof num, "%d", val ? (int)strlen(val) : 0); val = num; }
        else if (inner[0] && strspn(inner, "0123456789") == strlen(inner)) {
            int d = atoi(inner);
            val = d == 0 ? t->arg0 : (d <= t->nargs ? t->args[d - 1] : NULL);
        } else val = var_get(t, inner);
        if ((!val || !*val) && dflt) val = dflt;
        xs_text(x, val ? val : "", quoted);
        free(inner);
        *ip = j;
    } else if (c == '?') { snprintf(num, sizeof num, "%d", t->status); xs_text(x, num, quoted); *ip = i + 2; }
    else if (c == '#') { snprintf(num, sizeof num, "%d", t->nargs); xs_text(x, num, quoted); *ip = i + 2; }
    else if (c == '$') { snprintf(num, sizeof num, "%d", (int)getpid()); xs_text(x, num, quoted); *ip = i + 2; }
    else if (c == '@' || c == '*') { positional_all(x, quoted, c == '@'); *ip = i + 2; }
    else if (isdigit((unsigned char)c)) {
        int d = c - '0';
        v = d == 0 ? t->arg0 : (d <= t->nargs ? t->args[d - 1] : "");
        xs_text(x, v, quoted);
        *ip = i + 2;
    } else if (isalpha((unsigned char)c) || c == '_') {
        long j = i + 1;
        size_t n = 0;
        while (isalnum((unsigned char)raw[j]) || raw[j] == '_') { if (n < sizeof name - 1) name[n++] = raw[j]; j++; }
        name[n] = 0;
        v = var_get(t, name);
        xs_text(x, v ? v : "", quoted);
        *ip = j;
    } else { xs_add(x, '$', quoted); *ip = i + 1; }
}

/* full == 1: field splitting + globbing (command words); full == 0: one string (file names, assignments) */
void expand_word(Tab *t, const char *raw, Vec *out, int full)
{
    XS x;
    long i = 0;
    memset(&x, 0, sizeof x);
    x.out = out; x.t = t; x.full = full;
    if (raw[0] == '~' && (raw[1] == '/' || !raw[1])) { xs_text(&x, home_dir(), 1); i = 1; }
    while (raw[i]) {
        char c = raw[i];
        if (c == '\\') {
            if (raw[i + 1] == '\n') { i += 2; continue; }
            if (!raw[i + 1]) { xs_add(&x, '\\', 1); i++; continue; }
            xs_add(&x, raw[i + 1], 1);
            i += 2;
        } else if (c == '\'') {
            long j = i + 1;
            x.have = 1;
            while (raw[j] && raw[j] != '\'') xs_add(&x, raw[j++], 1);
            i = raw[j] ? j + 1 : j;
        } else if (c == '"') {
            long j = i + 1;
            x.have = 1;
            while (raw[j] && raw[j] != '"') {
                if (raw[j] == '\\' && raw[j + 1]) {
                    if (raw[j + 1] == '\n') { xs_add(&x, '\n', 1); j += 2; continue; }     /* spec: newline is kept inside "..." */
                    if (strchr("$`\"\\", raw[j + 1])) { xs_add(&x, raw[j + 1], 1); j += 2; continue; }
                    xs_add(&x, '\\', 1); j++;
                } else if (raw[j] == '$') expand_dollar(&x, raw, &j, 1);
                else if (raw[j] == '`') {
                    long e = scan_bt(raw, j + 1);
                    char *inner;
                    if (e < 0) { xs_add(&x, '`', 1); j++; continue; }
                    inner = unescape_bt(raw + j + 1, (size_t)(e - 1 - (j + 1)));
                    subst_into(&x, inner, 1);
                    free(inner);
                    j = e;
                } else xs_add(&x, raw[j++], 1);
            }
            i = raw[j] ? j + 1 : j;
        } else if (c == '`') {
            long e = scan_bt(raw, i + 1);
            char *inner;
            if (e < 0) { xs_add(&x, '`', 0); i++; continue; }
            inner = unescape_bt(raw + i + 1, (size_t)(e - 1 - (i + 1)));
            subst_into(&x, inner, 0);
            free(inner);
            i = e;
        } else if (c == '$') expand_dollar(&x, raw, &i, 0);
        else { xs_add(&x, c, 0); i++; }
    }
    xs_flush(&x);
    b_free(&x.cur); b_free(&x.q);
}

char *expand_one(Tab *t, const char *raw)       /* no splitting / globbing */
{
    Vec v = {0};
    char *r;
    expand_word(t, raw, &v, 0);
    r = v.n ? xstrdup(v.v[0]) : xstrdup("");
    v_free(&v);
    return r;
}

static char *run_subst(Tab *t, const char *cmd)
{
    int p[2], sfd = t->out_fd, ds = t->depth, i;
    Cap *c;
    char *res;
    if (t->ncap >= 8 || mkpipe(p) < 0) return xstrdup("");
    c = &t->cap[t->ncap++];
    memset(c, 0, sizeof *c);
    c->rfd = p[0]; c->wfd = p[1];
    set_nonblock(p[0]);
    t->out_fd = p[1];
    t->depth++;                                  /* `exit` inside $( ) only ends the substitution */
    exec_text(t, cmd);
    t->depth = ds;
    if (t->stop_exec) { t->stop_exec = 0; t->status = 0; }
    t->out_fd = sfd;
    close(c->wfd); c->wfd = -1;
    while (!c->eof && !t->dead && !t->intr) pump(t, -1);
    close(c->rfd);
    res = c->buf.p ? c->buf.p : xstrdup("");
    t->ncap--;
    i = (int)strlen(res);
    while (i > 0 && res[i - 1] == '\n') res[--i] = 0;
    return res;
}
