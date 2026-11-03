/*
 * task3_parser.c
 * TASK 3 - lexer and parser: quotes, multi-line / incomplete input detection, $( ) and backtick scanning, if/elif/else/fi grammar.
 */
#include "terminator.h"

typedef struct { int type, start, end, ionum; char *s; } Tok;

typedef struct { Tok *v; int n, cap; } Toks;

typedef struct { Toks *tk; int p; Arena *ar; const char *src; char err[300]; int inc; } Parser;

static long scan_dq(const char *s, long i);
static List *parse_list(Parser *P, int stops);

long scan_bt(const char *s, long i)
{
    for (;; i++) {
        if (!s[i]) return -1;
        if (s[i] == '\\') { if (!s[i + 1]) return -1; i++; continue; }
        if (s[i] == '`') return i + 1;
    }
}

static long scan_dq(const char *s, long i)
{
    for (;;) {
        if (!s[i]) return -1;
        if (s[i] == '\\') { if (!s[i + 1]) return -1; i += 2; continue; }
        if (s[i] == '"') return i + 1;
        if (s[i] == '$' && s[i + 1] == '(') { i = scan_paren(s, i + 2); if (i < 0) return -1; continue; }
        if (s[i] == '`') { i = scan_bt(s, i + 1); if (i < 0) return -1; continue; }
        i++;
    }
}

long scan_paren(const char *s, long i)       /* i is just after "$(" */
{
    int depth = 1;
    for (;;) {
        char c = s[i];
        if (!c) return -1;
        if (c == '\\') { if (!s[i + 1]) return -1; i += 2; continue; }
        if (c == '\'') { i++; while (s[i] && s[i] != '\'') i++; if (!s[i]) return -1; i++; continue; }
        if (c == '"') { i = scan_dq(s, i + 1); if (i < 0) return -1; continue; }
        if (c == '`') { i = scan_bt(s, i + 1); if (i < 0) return -1; continue; }
        if (c == '(') depth++;
        else if (c == ')') { if (--depth == 0) return i + 1; }
        i++;
    }
}

long scan_brace(const char *s, long i)       /* i is just after "${" */
{
    for (; s[i]; i++) if (s[i] == '}') return i + 1;
    return -1;
}

static void tk_push(Toks *tk, Tok t)
{
    if (tk->n + 1 > tk->cap) { tk->cap = tk->cap * 2 + 32; tk->v = xrealloc(tk->v, sizeof(Tok) * (size_t)tk->cap); }
    tk->v[tk->n++] = t;
}

/* 0 ok, 1 incomplete (needs another line), 2 error */
static int tokenize(Arena *ar, const char *src, Toks *tk, char *err, size_t esz)
{
    long i = 0;
    int bs_nl_end = 0;
    memset(tk, 0, sizeof *tk);
    for (;;) {
        char c = src[i];
        Tok t;
        if (!c) break;
        if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }
        if (c == '\\' && src[i + 1] == '\n') { i += 2; if (!src[i]) bs_nl_end = 1; continue; }
        if (c == '#') { while (src[i] && src[i] != '\n') i++; continue; }
        memset(&t, 0, sizeof t);
        t.ionum = -1;
        t.start = (int)i;
        if (c == '\n') { t.type = T_OP; t.s = "\n"; i++; t.end = (int)i; tk_push(tk, t); continue; }
        if (strchr("|&;<>()", c)) {
            const char *op;
            if (c == '|') op = src[i + 1] == '|' ? "||" : "|";
            else if (c == '&') op = src[i + 1] == '&' ? "&&" : "&";
            else if (c == ';') op = ";";
            else if (c == '<') {
                if (src[i + 1] == '<') { snprintf(err, esz, "here-documents are not supported"); return 2; }
                op = "<";
            } else if (c == '>') op = src[i + 1] == '>' ? ">>" : (src[i + 1] == '&' ? ">&" : ">");
            else op = c == '(' ? "(" : ")";
            t.type = T_OP;
            t.s = (char *)op;
            if ((c == '<' || c == '>') && tk->n > 0) {          /* 2>file : the digits are the descriptor */
                Tok *pv = &tk->v[tk->n - 1];
                if (pv->type == T_WORD && pv->end == (int)i && strlen(pv->s) <= 2 && strspn(pv->s, "0123456789") == strlen(pv->s)) {
                    t.ionum = atoi(pv->s);
                    t.start = pv->start;
                    tk->n--;
                }
            }
            i += (long)strlen(op);
            t.end = (int)i;
            tk_push(tk, t);
            continue;
        }
        /* a word */
        {
            long st = i;
            for (;;) {
                c = src[i];
                if (!c || c == ' ' || c == '\t' || c == '\n' || c == '\r' || strchr("|&;<>()", c)) break;
                if (c == '\\') {
                    if (!src[i + 1]) { return 1; }
                    if (src[i + 1] == '\n' && !src[i + 2]) bs_nl_end = 1;
                    i += 2;
                    continue;
                }
                if (c == '\'') { i++; while (src[i] && src[i] != '\'') i++; if (!src[i]) return 1; i++; continue; }
                if (c == '"') { i = scan_dq(src, i + 1); if (i < 0) return 1; continue; }
                if (c == '`') { i = scan_bt(src, i + 1); if (i < 0) return 1; continue; }
                if (c == '$' && src[i + 1] == '(') { i = scan_paren(src, i + 2); if (i < 0) return 1; continue; }
                if (c == '$' && src[i + 1] == '{') { i = scan_brace(src, i + 2); if (i < 0) return 1; continue; }
                i++;
            }
            t.type = T_WORD;
            t.s = ar_strndup(ar, src + st, (size_t)(i - st));
            t.start = (int)st;
            t.end = (int)i;
            tk_push(tk, t);
        }
    }
    if (bs_nl_end) return 1;
    {
        Tok e;
        memset(&e, 0, sizeof e);
        e.type = T_EOF; e.s = "end of file"; e.start = e.end = (int)strlen(src); e.ionum = -1;
        tk_push(tk, e);
    }
    return 0;
}

static int kw_id(const char *s)
{
    if (!strcmp(s, "if")) return KW_IF;
    if (!strcmp(s, "then")) return KW_THEN;
    if (!strcmp(s, "elif")) return KW_ELIF;
    if (!strcmp(s, "else")) return KW_ELSE;
    if (!strcmp(s, "fi")) return KW_FI;
    return 0;
}

static Tok *peek(Parser *P) { return &P->tk->v[P->p]; }

static int is_op(Tok *t, const char *s) { return t->type == T_OP && !strcmp(t->s, s); }

static int is_kw(Tok *t, int kw) { return t->type == T_WORD && kw_id(t->s) == kw; }

static void *perr(Parser *P, Tok *t)
{
    if (t->type == T_EOF) P->inc = 1;
    else snprintf(P->err, sizeof P->err, "syntax error near unexpected token `%s'", strcmp(t->s, "\n") == 0 ? "newline" : t->s);
    return NULL;
}

static void skip_newlines(Parser *P) { while (is_op(peek(P), "\n")) P->p++; }

static Cmd *parse_simple(Parser *P)
{
    Cmd *c = ar_alloc(P->ar, sizeof *c);
    Redir **tail = &c->rd;
    int cap = 8, first = peek(P)->start;
    memset(c, 0, sizeof *c);
    c->w = ar_alloc(P->ar, sizeof(char *) * (size_t)cap);
    for (;;) {
        Tok *t = peek(P);
        if (t->type == T_WORD) {
            if (c->nw == 0 && kw_id(t->s) && kw_id(t->s) != KW_IF) return perr(P, t);
            if (c->nw + 1 >= cap) {
                char **nw = ar_alloc(P->ar, sizeof(char *) * (size_t)cap * 2);
                memcpy(nw, c->w, sizeof(char *) * (size_t)c->nw);
                c->w = nw; cap *= 2;
            }
            c->w[c->nw++] = t->s;
            P->p++;
        } else if (t->type == T_OP && (!strcmp(t->s, "<") || !strcmp(t->s, ">") || !strcmp(t->s, ">>") || !strcmp(t->s, ">&"))) {
            Redir *r = ar_alloc(P->ar, sizeof *r);
            Tok *tg;
            P->p++;
            tg = peek(P);
            if (tg->type != T_WORD) return perr(P, tg);
            r->kind = !strcmp(t->s, "<") ? 0 : (!strcmp(t->s, ">") ? 1 : (!strcmp(t->s, ">>") ? 2 : 3));
            r->fd = t->ionum >= 0 ? t->ionum : (r->kind == 0 ? 0 : 1);
            r->target = tg->s;
            r->next = NULL;
            *tail = r; tail = &r->next;
            P->p++;
        } else break;
    }
    if (!c->nw && !c->rd) return perr(P, peek(P));
    c->w[c->nw] = NULL;
    c->text = ar_strndup(P->ar, P->src + first, (size_t)(P->tk->v[P->p - 1].end - first));
    return c;
}

static Cmd *parse_if(Parser *P)
{
    Cmd *c = ar_alloc(P->ar, sizeof *c);
    IfNode *f = ar_alloc(P->ar, sizeof *f);
    int cap = 8, first = peek(P)->start;
    memset(c, 0, sizeof *c); memset(f, 0, sizeof *f);
    f->cond = ar_alloc(P->ar, sizeof(List *) * (size_t)cap);
    f->body = ar_alloc(P->ar, sizeof(List *) * (size_t)cap);
    P->p++;                                                      /* if */
    for (;;) {
        List *cd, *bd;
        if (f->n >= cap - 1) return perr(P, peek(P));
        cd = parse_list(P, KW_THEN);
        if (!cd) return NULL;
        if (!is_kw(peek(P), KW_THEN)) return perr(P, peek(P));
        if (!cd->n) return perr(P, peek(P));
        P->p++;
        bd = parse_list(P, KW_ELIF | KW_ELSE | KW_FI);
        if (!bd) return NULL;
        if (!bd->n) return perr(P, peek(P));
        f->cond[f->n] = cd; f->body[f->n] = bd; f->n++;
        if (is_kw(peek(P), KW_ELIF)) { P->p++; continue; }
        break;
    }
    if (is_kw(peek(P), KW_ELSE)) {
        P->p++;
        f->els = parse_list(P, KW_FI);
        if (!f->els) return NULL;
        if (!f->els->n) return perr(P, peek(P));
    }
    if (!is_kw(peek(P), KW_FI)) return perr(P, peek(P));
    P->p++;
    c->ifn = f;
    c->text = ar_strndup(P->ar, P->src + first, (size_t)(P->tk->v[P->p - 1].end - first));
    return c;
}

static Pipe *parse_pipeline(Parser *P)
{
    Pipe *pl = ar_alloc(P->ar, sizeof *pl);
    int cap = 8, start;
    memset(pl, 0, sizeof *pl);
    pl->c = ar_alloc(P->ar, sizeof(Cmd *) * (size_t)cap);
    if (peek(P)->type == T_WORD && !strcmp(peek(P)->s, "!")) { pl->neg = 1; P->p++; }
    start = peek(P)->start;
    for (;;) {
        Cmd *c = is_kw(peek(P), KW_IF) ? parse_if(P) : parse_simple(P);
        if (!c) return NULL;
        if (pl->n + 1 >= cap) {
            Cmd **nc = ar_alloc(P->ar, sizeof(Cmd *) * (size_t)cap * 2);
            memcpy(nc, pl->c, sizeof(Cmd *) * (size_t)pl->n);
            pl->c = nc; cap *= 2;
        }
        pl->c[pl->n++] = c;
        if (is_op(peek(P), "|")) {
            P->p++;
            skip_newlines(P);
            if (peek(P)->type == T_EOF) { P->inc = 1; return NULL; }
            continue;
        }
        break;
    }
    if (pl->n > 1) {
        int i;
        for (i = 0; i < pl->n; i++) if (pl->c[i]->ifn) { snprintf(P->err, sizeof P->err, "`if' cannot be part of a pipeline"); return NULL; }
    }
    pl->text = ar_strndup(P->ar, P->src + start, (size_t)(P->tk->v[P->p - 1].end - start));
    return pl;
}

static AndOr *parse_and_or(Parser *P)
{
    AndOr *ao = ar_alloc(P->ar, sizeof *ao);
    int cap = 8, op = 0;
    ao->p = ar_alloc(P->ar, sizeof(Pipe *) * (size_t)cap);
    ao->op = ar_alloc(P->ar, sizeof(int) * (size_t)cap);
    ao->n = 0;
    for (;;) {
        Pipe *pl = parse_pipeline(P);
        if (!pl) return NULL;
        if (ao->n + 1 >= cap) {
            Pipe **np = ar_alloc(P->ar, sizeof(Pipe *) * (size_t)cap * 2);
            int *no = ar_alloc(P->ar, sizeof(int) * (size_t)cap * 2);
            memcpy(np, ao->p, sizeof(Pipe *) * (size_t)ao->n);
            memcpy(no, ao->op, sizeof(int) * (size_t)ao->n);
            ao->p = np; ao->op = no; cap *= 2;
        }
        ao->p[ao->n] = pl; ao->op[ao->n] = op; ao->n++;
        if (is_op(peek(P), "&&") || is_op(peek(P), "||")) {
            op = is_op(peek(P), "&&") ? 1 : 2;
            P->p++;
            skip_newlines(P);
            if (peek(P)->type == T_EOF) { P->inc = 1; return NULL; }
            continue;
        }
        break;
    }
    return ao;
}

static List *parse_list(Parser *P, int stops)
{
    List *L = ar_alloc(P->ar, sizeof *L);
    memset(L, 0, sizeof *L);
    for (;;) {
        Tok *t = peek(P);
        AndOr *ao;
        while (is_op(t, ";") || is_op(t, "\n")) { P->p++; t = peek(P); }
        if (t->type == T_EOF) break;
        if (t->type == T_WORD) {
            int kw = kw_id(t->s);
            if (kw && (stops & kw)) break;
            if (kw && kw != KW_IF) return perr(P, t);
        }
        ao = parse_and_or(P);
        if (!ao) return NULL;
        if (L->n + 1 > L->cap) {
            int nc = L->cap * 2 + 8;
            AndOr **na = ar_alloc(P->ar, sizeof(AndOr *) * (size_t)nc);
            if (L->n) memcpy(na, L->a, sizeof(AndOr *) * (size_t)L->n);
            L->a = na; L->cap = nc;
        }
        L->a[L->n++] = ao;
        t = peek(P);
        if (is_op(t, "&")) { ao->p[ao->n - 1]->bg = 1; P->p++; }
        else if (is_op(t, ";") || is_op(t, "\n") || t->type == T_EOF) { }
        else return perr(P, t);
    }
    return L;
}

/* Parse a complete text. 0 ok (*out set), 1 incomplete, 2 syntax error (err filled). */
int parse_text(Arena *ar, const char *src, List **out, char *err, size_t esz)
{
    Toks tk;
    Parser P;
    List *L;
    int r;
    r = tokenize(ar, src, &tk, err, esz);
    if (r) { free(tk.v); return r; }
    memset(&P, 0, sizeof P);
    P.tk = &tk; P.ar = ar; P.src = src;
    L = parse_list(&P, 0);
    if (L && peek(&P)->type != T_EOF) { perr(&P, peek(&P)); L = NULL; }
    free(tk.v);
    if (!L) {
        if (P.inc) return 1;
        snprintf(err, esz, "%s", P.err[0] ? P.err : "syntax error");
        return 2;
    }
    *out = L;
    return 0;
}
