#include "../terminator.h"
#undef main
static void *engine_thread(void *arg) { engine_run((EngineArgs *)arg); return NULL; }
/* headless driver: script on stdin; argv[1] = "thread" | "process" */
static int kw, orr; static Scr S;
static void pull(int idle_ms, int max_ms)
{
    double start = mono_now(), last = start;
    for (;;) {
        fd_set rf; struct timeval tv = {0, 50000}; unsigned char b[4096];
        FD_ZERO(&rf); FD_SET(orr, &rf);
        if (select(orr + 1, &rf, 0, 0, &tv) > 0) {
            ssize_t n = read(orr, b, sizeof b);
            if (n > 0) { scr_feed(&S, b, (size_t)n); last = mono_now(); }
            else if (n == 0) return;
        }
        if ((mono_now() - last) * 1000 > idle_ms) return;
        if ((mono_now() - start) * 1000 > max_ms) return;
    }
}
static void sendk(const char *s) { write_all(kw, s, strlen(s)); }
static void dump(void)
{
    int i;
    for (i = 0; i < S.n; i++) { Line *l = scr_line(&S, i); printf("|%.*s\n", l->len, l->s ? l->s : ""); }
    printf("+-- cursor col %d title=%s\n", S.col, S.title);
}
int main(int argc, char **argv)
{
    char line[2048]; int k[2], o[2]; EngineArgs *ea = xmalloc(sizeof *ea);
    int proc = argc > 1 && !strcmp(argv[1], "process");
    signal(SIGPIPE, SIG_IGN);
    hist_init(); binds_load(binds, NULL);
    scr_init(&S);
    mkpipe(k); mkpipe(o);
    ea->kfd = k[0]; ea->ofd = o[1]; getcwd(ea->cwd, sizeof ea->cwd);
    if (proc) { if (fork() == 0) { close(k[1]); close(o[0]); engine_run(ea); _exit(0); } close(k[0]); close(o[1]); }
    else { pthread_t th; pthread_create(&th, 0, engine_thread, ea); }
    kw = k[1]; orr = o[0];
    pull(300, 3000);
    while (fgets(line, sizeof line, stdin)) {
        size_t n = strlen(line); while (n && line[n-1] == '\n') line[--n] = 0;
        if (!strncmp(line, "> ", 2)) { sendk(line + 2); sendk("\n"); pull(350, 8000); }
        else if (!strncmp(line, "raw ", 4)) { sendk(line + 4); pull(250, 3000); }
        else if (!strncmp(line, "k ", 2)) { int a = find_action(line + 2); char b[2] = {27, (char)('0' + a)}; if (a < 0) { printf("bad action\n"); continue; } write_all(kw, b, 2); pull(300, 3000); }
        else if (!strcmp(line, "enter")) { sendk("\n"); pull(350, 5000); }
        else if (!strncmp(line, "esc", 3)) { write_all(kw, "\033#", 2); pull(250, 3000); }
        else if (!strncmp(line, "w ", 2)) pull(atoi(line + 2), atoi(line + 2) + 500);
        else if (!strcmp(line, "dump")) dump();
        else if (!strncmp(line, "#", 1)) { printf("\n##### %s\n", line + 1); }
    }
    close(kw); pull(500, 2000);
    return 0;
}
