/*
 * task9_history.c
 * TASK 9 - shared history (shared memory + semaphore + ~/.terminator_history), history command, Up/Down browsing, Ctrl+R search.
 */
#include "terminator.h"

typedef struct { long total; int start, count; char cmd[HMAX][HLEN]; } Hist;

static Hist *H;

             /* anonymous MAP_SHARED mapping: visible to every tab thread/process */
static sem_t *Hsem;

         /* process-shared semaphore (named, unlinked at once) */
static char hist_path[PATH_MAX];

static void hist_lock(void)   { while (sem_wait(Hsem) < 0 && errno == EINTR) { } }

static void hist_unlock(void) { sem_post(Hsem); }

static void hist_put_locked(const char *line)
{
    int idx;
    if (H->count == HMAX) { idx = H->start; H->start = (H->start + 1) % HMAX; }
    else { idx = (H->start + H->count) % HMAX; H->count++; }
    strncpy(H->cmd[idx], line, HLEN - 1);
    H->cmd[idx][HLEN - 1] = 0;
    H->total++;
}

static void hist_flatten(const char *in, char *out, size_t sz)   /* newlines become spaces: one line per command */
{
    size_t i, j = 0;
    for (i = 0; in[i] && j + 1 < sz; i++) out[j++] = (in[i] == '\n' || in[i] == '\r') ? ' ' : in[i];
    while (j > 0 && out[j - 1] == ' ') j--;
    out[j] = 0;
}

int hist_init(void)
{
    char name[40], line[HLEN * 2];
    FILE *f;
    H = mmap(NULL, sizeof(Hist), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (H == MAP_FAILED) { perror("mmap"); return -1; }

    snprintf(name, sizeof name, "/term_h_%d", (int)getpid());
    sem_unlink(name);
    Hsem = sem_open(name, O_CREAT | O_EXCL, 0600, 1);
    if (Hsem == SEM_FAILED) { perror("sem_open"); return -1; }
    sem_unlink(name);
    home_path(hist_path, sizeof hist_path, ".terminator_history");
    f = fopen(hist_path, "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            size_t n = strlen(line);
            while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
            if (n) hist_put_locked(line);
        }
        fclose(f);
    }
    return 0;
}

void hist_rewrite_file(void)   /* keep only the last HMAX commands in the file */
{
    FILE *f;
    int i;
    hist_lock();
    f = fopen(hist_path, "w");
    if (f) {
        for (i = 0; i < H->count; i++) fprintf(f, "%s\n", H->cmd[(H->start + i) % HMAX]);
        fclose(f);
    }
    hist_unlock();
}

void hist_add(const char *cmd)
{
    char flat[HLEN * 2];
    int fd;
    hist_flatten(cmd, flat, sizeof flat);
    if (!flat[0]) return;
    hist_lock();
    hist_put_locked(flat);
    fd = open(hist_path, O_WRONLY | O_APPEND | O_CREAT, 0600);
    if (fd >= 0) {
        size_t n = strlen(flat);
        char *l = xmalloc(n + 2);
        memcpy(l, flat, n); l[n] = '\n';
        if (write(fd, l, n + 1) < 0) { }
        free(l);
        close(fd);
    }
    hist_unlock();
}

static int hist_count(void) { int n; hist_lock(); n = H->count; hist_unlock(); return n; }

static int hist_get(int i, char *out, size_t sz)   /* i = 0 is the oldest entry */
{
    int ok = 0;
    hist_lock();
    if (i >= 0 && i < H->count) { snprintf(out, sz, "%s", H->cmd[(H->start + i) % HMAX]); ok = 1; }
    hist_unlock();
    return ok;
}

/* longest common substring length of a and b */
static int lcs_len(const char *a, const char *b)
{
    int la = (int)strlen(a), lb = (int)strlen(b), i, j, best = 0;
    int *prev, *cur, *tmp;
    if (!la || !lb) return 0;
    prev = xmalloc((size_t)(lb + 1) * sizeof(int));
    cur = xmalloc((size_t)(lb + 1) * sizeof(int));
    memset(prev, 0, (size_t)(lb + 1) * sizeof(int));
    memset(cur, 0, (size_t)(lb + 1) * sizeof(int));
    for (i = 1; i <= la; i++) {
        for (j = 1; j <= lb; j++) {
            cur[j] = (a[i - 1] == b[j - 1]) ? prev[j - 1] + 1 : 0;
            if (cur[j] > best) best = cur[j];
        }
        tmp = prev; prev = cur; cur = tmp;
    }
    free(prev); free(cur);
    return best;
}

/* Ctrl+R: (a) newest exact match, else (b) command(s) with the longest common substring (> 2 chars) */
static void hist_search(const char *term, Buf *out)
{
    int n, i, best = 0;
    char *e = xmalloc(HLEN);
    Vec seen = {0};
    if (!*term) { b_adds(out, "No match for search term in history\n"); free(e); return; }
    hist_lock();
    n = H->count;
    hist_unlock();
    for (i = n - 1; i >= 0; i--)
        if (hist_get(i, e, HLEN) && strcmp(e, term) == 0) { b_printf(out, "%s\n", e); free(e); return; }
    for (i = n - 1; i >= 0; i--)
        if (hist_get(i, e, HLEN)) { int l = lcs_len(term, e); if (l > best) best = l; }
    if (best > 2) {
        for (i = n - 1; i >= 0; i--) {
            int k, dup = 0;
            if (!hist_get(i, e, HLEN) || lcs_len(term, e) != best) continue;
            for (k = 0; k < seen.n; k++) if (strcmp(seen.v[k], e) == 0) { dup = 1; break; }
            if (dup) continue;
            v_push(&seen, xstrdup(e));
            b_printf(out, "%s\n", e);
        }
    } else b_adds(out, "No match for search term in history\n");
    v_free(&seen);
    free(e);
}

void hist_show(Tab *t, int want)
{
    Buf b = {0};
    int n, i, from;
    long total;
    char *e = xmalloc(HLEN);
    hist_lock(); n = H->count; total = H->total; hist_unlock();
    if (want > 1000 || want <= 0) want = 1000;
    from = n > want ? n - want : 0;
    for (i = from; i < n; i++)
        if (hist_get(i, e, HLEN)) b_printf(&b, "%5ld  %s\n", total - n + i + 1, e);
    emit_fd(t, t->out_fd, b_str(&b), b.n);
    b_free(&b);
    free(e);
}

void hist_nav(Tab *t, int dir)
{
    int n = hist_count(), idx;
    char *e;
    if (dir < 0) {
        int base = t->hidx == -1 ? n : t->hidx;
        if (base <= 0) return;
        idx = base - 1;
        if (t->hidx == -1) { free(t->draft); t->draft = xstrdup(t->in); }
    } else {
        if (t->hidx == -1) return;
        idx = t->hidx + 1;
        if (idx >= n) { in_set(t, t->draft ? t->draft : ""); t->hidx = -1; return; }
    }
    e = xmalloc(HLEN);
    if (hist_get(idx, e, HLEN)) { in_set(t, e); t->hidx = idx; }
    free(e);
}

void search_start(Tab *t)
{
    char pr[160], seq[64];
    int n;
    cur_prompt(t, pr, sizeof pr);
    n = snprintf(seq, sizeof seq, "\033[%dG\n", (int)strlen(pr) + t->ilen + 1);
    gui_write(t, seq, (size_t)n);
    gui_puts(t, "Enter search term: ");
    t->at_bol = 0;
    t->sterm.n = 0; if (t->sterm.p) t->sterm.p[0] = 0;
    t->mode = M_SEARCH;
    t->prompt_on = 0;
}

void search_finish(Tab *t, int run)
{
    gui_puts(t, "\n");
    t->at_bol = 1;
    if (run) {
        Buf out = {0};
        hist_search(b_str(&t->sterm), &out);
        disp(t, out.p, out.n);
        b_free(&out);
    }
    show_prompt(t);
}
