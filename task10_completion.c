/*
 * task10_completion.c
 * TASK 10 - Tab completion of file names: unique match, longest common prefix, numbered pick list.
 */
#include "terminator.h"

static void escape_name(Buf *b, const char *s)
{
    for (; *s; s++) {
        if (strchr(" \t\"'\\$&;|<>()*?[]#`!{}", *s)) b_addc(b, '\\');
        b_addc(b, *s);
    }
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

void do_complete(Tab *t)
{
    int ws = t->ipos, i, isdir;
    Buf word = {0}, dirpart = {0}, repl = {0}, list = {0};
    char *slash, *base, dpath[PATH_MAX * 2];
    Vec names = {0};
    DIR *d;
    struct dirent *de;
    size_t bl;

    while (ws > 0 && !((t->in[ws - 1] == ' ' || t->in[ws - 1] == '\t') && !(ws >= 2 && t->in[ws - 2] == '\\'))) ws--;
    for (i = ws; i < t->ipos; i++) {                          /* un-escape the word */
        if (t->in[i] == '\\' && i + 1 < t->ipos) i++;
        b_addc(&word, t->in[i]);
    }
    b_str(&word);
    slash = strrchr(word.p, '/');
    if (slash) { b_add(&dirpart, word.p, (size_t)(slash - word.p) + 1); base = slash + 1; }
    else base = word.p;
    bl = strlen(base);
    if (dirpart.n == 0) snprintf(dpath, sizeof dpath, "%s", t->cwd);
    else if (!strncmp(dirpart.p, "~/", 2)) snprintf(dpath, sizeof dpath, "%s/%s", home_dir(), dirpart.p + 2);
    else path_join(dpath, sizeof dpath, t->cwd, dirpart.p);
    d = opendir(dpath);
    if (d) {
        while ((de = readdir(d)) != NULL) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            if (de->d_name[0] == '.' && base[0] != '.') continue;
            if (strncmp(de->d_name, base, bl) != 0) continue;
            {
                char full[PATH_MAX * 2 + 8];
                struct stat sb;
                snprintf(full, sizeof full, "%s/%s", dpath, de->d_name);
                isdir = (stat(full, &sb) == 0 && S_ISDIR(sb.st_mode));
            }
            {
                char *nm = xmalloc(strlen(de->d_name) + 2);
                sprintf(nm, "%s%s", de->d_name, isdir ? "/" : "");
                v_push(&names, nm);
            }
        }
        closedir(d);
    }
    if (names.n == 0) goto out;                               /* no match: do nothing */
    qsort(names.v, (size_t)names.n, sizeof(char *), cmp_str);
    if (names.n == 1) {
        b_adds(&repl, dirpart.p ? dirpart.p : "");
        escape_name(&repl, names.v[0]);
        in_delete(t, ws, t->ipos);
        in_insert(t, b_str(&repl), (int)repl.n);
        draw_input(t);
        goto out;
    }
    {   /* several matches: extend to the longest common prefix ... */
        size_t lcp = strlen(names.v[0]);
        for (i = 1; i < names.n; i++) { size_t k = 0; while (k < lcp && names.v[i][k] == names.v[0][k]) k++; lcp = k; }
        if (lcp > bl) {
            char *pre = xstrndup(names.v[0], lcp);
            b_adds(&repl, dirpart.p ? dirpart.p : "");
            escape_name(&repl, pre);
            free(pre);
            in_delete(t, ws, t->ipos);
            in_insert(t, b_str(&repl), (int)repl.n);
        }
    }
    /* ... and, still ambiguous, show a numbered list */
    for (i = 0; i < names.n; i++) {
        Buf r = {0};
        b_adds(&r, dirpart.p ? dirpart.p : "");
        escape_name(&r, names.v[i]);
        v_push(&t->pick, r.p);
        b_printf(&list, "%s%d. %s", i ? " " : "", i + 1, names.v[i]);
    }
    {
        char seq[64];
        int n = snprintf(seq, sizeof seq, "\033[%dG\n", (int)strlen("") + 1);
        (void)n;
        {   /* leave the input line as it is and print the list below it */
            char pr[512];
            cur_prompt(t, pr, sizeof pr);
            n = snprintf(seq, sizeof seq, "\033[%dG\n", (int)strlen(pr) + t->ilen + 1);
            gui_write(t, seq, (size_t)n);
        }
    }
    b_addc(&list, '\n');
    disp(t, list.p, list.n);
    t->pick_ws = ws;
    t->pick_num = 0;
    t->mode = M_PICK;
    t->prompt_on = 1;
    draw_input(t);
out:
    v_free(&names);
    b_free(&word); b_free(&dirpart); b_free(&repl); b_free(&list);
}

void pick_cancel(Tab *t)
{
    v_free(&t->pick);
    t->mode = M_PROMPT;
    t->pick_num = 0;
}

void pick_select(Tab *t)
{
    if (t->pick_num >= 1 && t->pick_num <= t->pick.n) {
        const char *r = t->pick.v[t->pick_num - 1];
        in_delete(t, t->pick_ws, t->ipos);
        in_insert(t, r, (int)strlen(r));
    }
    pick_cancel(t);
    draw_input(t);
}
