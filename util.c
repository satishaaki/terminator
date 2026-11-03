/*
 * util.c
 * Shared helpers: growable buffers (Buf), string vectors (Vec), parse arena, pipe helpers, path helpers.
 */
#include "terminator.h"

void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) { perror("malloc"); abort(); } return p; }

void *xrealloc(void *q, size_t n) { void *p = realloc(q, n ? n : 1); if (!p) { perror("realloc"); abort(); } return p; }

char *xstrdup(const char *s) { size_t n = strlen(s); char *p = xmalloc(n + 1); memcpy(p, s, n + 1); return p; }

char *xstrndup(const char *s, size_t n) { char *p = xmalloc(n + 1); memcpy(p, s, n); p[n] = 0; return p; }

            /* growable, always NUL terminated */
void b_add(Buf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2 + 16; b->p = xrealloc(b->p, b->cap); }
    if (n) memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

void b_addc(Buf *b, char c) { b_add(b, &c, 1); }

void b_adds(Buf *b, const char *s) { b_add(b, s, strlen(s)); }

const char *b_str(Buf *b) { if (!b->p) b_add(b, "", 0); return b->p; }

void b_free(Buf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

void b_printf(Buf *b, const char *fmt, ...)
{
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    b_adds(b, tmp);
}

               /* vector of owned strings */
void v_push(Vec *v, char *s)
{
    if (v->n + 2 > v->cap) { v->cap = v->cap * 2 + 8; v->v = xrealloc(v->v, v->cap * sizeof(char *)); }
    v->v[v->n++] = s;
    v->v[v->n] = NULL;
}

void v_free(Vec *v) { int i; for (i = 0; i < v->n; i++) free(v->v[i]); free(v->v); v->v = NULL; v->n = v->cap = 0; }

void *ar_alloc(Arena *a, size_t n)
{
    Chunk *c = a->head;
    n = (n + 15) & ~(size_t)15;
    if (!c || c->used + n > c->size) {
        size_t sz = n > 16384 ? n : 16384;
        c = xmalloc(sizeof(Chunk) + sz);
        c->next = a->head; c->used = 0; c->size = sz;
        a->head = c;
    }
    c->used += n;
    return c->data + c->used - n;
}

char *ar_strndup(Arena *a, const char *s, size_t n) { char *p = ar_alloc(a, n + 1); memcpy(p, s, n); p[n] = 0; return p; }

void ar_free(Arena *a) { Chunk *c = a->head, *n; while (c) { n = c->next; free(c); c = n; } a->head = NULL; }

double mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

pthread_mutex_t spawn_lock = PTHREAD_MUTEX_INITIALIZER;

   /* pipe()+fcntl and fork() are not interleaved */

int mkpipe(int p[2])
{
    int r;
    pthread_mutex_lock(&spawn_lock);
    r = pipe(p);
    if (r == 0) {
        fcntl(p[0], F_SETFD, FD_CLOEXEC);
        fcntl(p[1], F_SETFD, FD_CLOEXEC);
    }
    pthread_mutex_unlock(&spawn_lock);
    return r;
}

void set_nonblock(int fd) { int fl = fcntl(fd, F_GETFL); if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK); }

int write_all(int fd, const char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; if (errno == EAGAIN) { usleep(1000); continue; } return -1; }
        p += w; n -= (size_t)w;
    }
    return 0;
}

const char *home_dir(void)
{
    const char *h = getenv("HOME");
    return (h && *h) ? h : "/";
}

void home_path(char *out, size_t sz, const char *name) { snprintf(out, sz, "%s/%s", home_dir(), name); }

void path_norm(char *p)      /* lexical clean-up: // . .. */
{
    char *out = xmalloc(strlen(p) + 2), *seg;
    size_t o = 0;
    char *copy = xstrdup(p), *save = NULL;
    out[0] = 0;
    for (seg = strtok_r(copy, "/", &save); seg; seg = strtok_r(NULL, "/", &save)) {
        if (!strcmp(seg, ".")) continue;
        if (!strcmp(seg, "..")) { while (o > 0 && out[o - 1] != '/') o--; if (o > 0) o--; out[o] = 0; continue; }
        out[o++] = '/';
        strcpy(out + o, seg);
        o += strlen(seg);
    }
    if (!o) strcpy(out, "/");
    strcpy(p, out);
    free(out); free(copy);
}

void path_join(char *out, size_t sz, const char *cwd, const char *p)
{
    if (p[0] == '/') snprintf(out, sz, "%s", p);
    else snprintf(out, sz, "%s/%s", cwd, p);
}

int to_int(const char *s, int *out)
{
    char *e;
    long v;
    if (!*s) return 0;
    v = strtol(s, &e, 10);
    if (*e) return 0;
    *out = (int)v;
    return 1;
}
