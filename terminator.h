/* terminator.h - shared declarations for all Terminator source files (see README.md for the task -> file map) */
#ifndef TERMINATOR_H
#define TERMINATOR_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <glob.h>
#include <dirent.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/mman.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
extern char **environ;
#define HMAX 10000          /* commands kept in the history file / shared memory */
#define HLEN 512            /* longest remembered command (longer ones are truncated) */
#define SCR_MAXLINES 5000
#define MAXJOBS 64
#define MW_RULE "----------------------------------------------------"     /* 52 dashes, as in the spec */
#define MAXTABS 32

/* ------------------------------------------------------------------ shared types */
typedef struct { char *p; size_t n, cap; } Buf;
typedef struct { char **v; int n, cap; } Vec;
/* arena for parse trees: everything allocated for one parse is freed at once */
typedef struct Chunk { struct Chunk *next; size_t used, size; char data[1]; } Chunk;
typedef struct { Chunk *head; } Arena;
enum { A_LEFT, A_RIGHT, A_UP, A_DOWN, A_HOME, A_END, A_KILLBOL, A_KILLEOL, A_DELWORD, A_CLEAR,
       A_EOF, A_SEARCH, A_COMPLETE, A_INTR, A_SUSP, A_COPY, A_PASTE, A_NEWTAB, A_CLOSETAB,
       A_NEXTTAB, A_PREVTAB, A_COUNT };
typedef struct { const char *name; const char *def; char cur[32]; int gui; } BindDef;
typedef struct { char *s; int len, cap; } Line;
typedef struct {
    Line *ln;               /* ring buffer of SCR_MAXLINES lines */
    int base, n;            /* index of the oldest line, number of lines in use */
    long first_abs;         /* absolute number of the oldest line (for stable selections) */
    int col;                /* cursor column in the last line */
    int st;                 /* escape parser state: 0 text, 1 ESC, 2 CSI, 3 OSC */
    char eb[300]; int en;
    char title[256];
    int want_reload, want_close;
} Scr;
enum { J_RUN, J_STOP, J_DONE };
enum { M_PROMPT, M_RUN, M_SEARCH, M_PICK };
enum { KEY_DELETE = 100 };
typedef struct { pid_t pid; int state; int status; int sig; } Proc;
          /* state: 0 run, 1 stopped, 2 done */
typedef struct {
    int used, id, state, bg, notify, status, np, in_w;
    pid_t pgid;
    Proc *p;
    char *cmd;
} Job;
typedef struct Evt { pid_t pid; int kind, status; struct Evt *next; } Evt;
    /* kind: 0 ended, 1 stopped, 2 continued */
typedef struct { int rfd, wfd, eof; Buf buf; } Cap;
                           /* command-substitution capture */
typedef struct { char *cmd; pid_t pid; int fd; Buf buf; double started, next; time_t ts; } Watcher;
typedef struct Tab {
    int kfd, ofd;                    /* keys from the GUI / text to the GUI */
    int dead, closing;
    char cwd[PATH_MAX], oldcwd[PATH_MAX], user[64];
    char **env; int nenv, cenv;      /* shell variables == environment of children */
    char **args; int nargs; char *arg0;   /* positional parameters $1.. and $0 */
    int status;                      /* $? */
    int intr;                        /* Ctrl+C seen: abort what is running */
    int mode, line_ready;
    int dpr, dpw;                    /* display pipe: stdout/stderr of commands that are not redirected */
    int out_fd, err_fd, devnull;     /* where commands/builtins of the current context write */
    int wake_r, wake_w;              /* a waiter thread writes here when a child changes state */
    pthread_mutex_t evl; Evt *evh, *evt;
    Cap cap[8]; int ncap;
    unsigned char kb[512]; int kbn;
    /* line editor */
    char *in; int ilen, ipos, icap;
    Buf pend; int multiline;         /* lines of an unfinished command */
    int hidx; char *draft;           /* history browsing */
    Buf sterm;                       /* Ctrl+R search term */
    Vec pick; int pick_ws, pick_num; /* Tab-completion list */
    int at_bol, prompt_on;
    /* jobs */
    Job jobs[MAXJOBS]; int cur_job, prev_job;
    Job *fg; int fg_in; Buf fgline;
    /* scripts */
    int depth, stop_exec, in_sub;
    const char *cmdtext;             /* source text of the simple command being run (for multiWatch) */
    /* multiWatch */
    Watcher *mw; int nmw, mw_active, mw_stop; double mw_interval;
} Tab;
enum { T_WORD, T_OP, T_EOF };
typedef struct Redir { int kind, fd; char *target; struct Redir *next; } Redir;
     /* kind: 0 <  1 >  2 >>  3 >& */
struct IfNode;
typedef struct Cmd { char **w; int nw; Redir *rd; char *text; struct IfNode *ifn; } Cmd;
typedef struct Pipe { Cmd **c; int n, neg, bg; char *text; } Pipe;
typedef struct AndOr { Pipe **p; int *op; int n; } AndOr;
                            /* op[i]: 0 first, 1 &&, 2 || */
typedef struct List { AndOr **a; int n, cap; } List;
typedef struct IfNode { List **cond; List **body; int n; List *els; } IfNode;
enum { KW_IF = 1, KW_THEN = 2, KW_ELIF = 4, KW_ELSE = 8, KW_FI = 16 };
typedef struct { int kfd, ofd; char cwd[PATH_MAX]; } EngineArgs;
typedef struct { int kfd, ofd; pid_t pid; Scr scr; int scroll; } GTab;
typedef struct { int line, col0; } RowRef;

/* ------------------------------------------------------------------ shared global variables */
extern pthread_mutex_t spawn_lock;
extern BindDef binds[A_COUNT];
extern volatile int g_engines;
extern GTab *gt[MAXTABS];
extern int ngt;
extern int cur_tab;
extern int dirty;
extern Display *dpy;
extern Window win;
extern int cw;
extern int fh;
extern int barh;
extern int rows;
extern Atom a_clip;
extern Atom a_utf8;
extern Atom a_targets;
extern Atom a_prop;
extern Atom a_primary;
extern RowRef vis[512];
extern int nvis;
extern int sel_on;
extern long sel_a_abs;
extern long sel_b_abs;
extern int sel_a_col;
extern int sel_b_col;
extern GTab *sel_tab;
extern char *sel_text;

/* ------------------------------------------------------------------ functions shared between files */
/* util.c */
void *xmalloc(size_t n);
void *xrealloc(void *q, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
void b_add(Buf *b, const char *s, size_t n);
void b_addc(Buf *b, char c);
void b_adds(Buf *b, const char *s);
const char *b_str(Buf *b);
void b_free(Buf *b);
void b_printf(Buf *b, const char *fmt, ...);
void v_push(Vec *v, char *s);
void v_free(Vec *v);
void *ar_alloc(Arena *a, size_t n);
char *ar_strndup(Arena *a, const char *s, size_t n);
void ar_free(Arena *a);
double mono_now(void);
int mkpipe(int p[2]);
void set_nonblock(int fd);
int write_all(int fd, const char *p, size_t n);
const char *home_dir(void);
void home_path(char *out, size_t sz, const char *name);
void path_norm(char *p);
void path_join(char *out, size_t sz, const char *cwd, const char *p);
int to_int(const char *s, int *out);
/* task1_gui.c */
Line *scr_line(Scr *s, int k);
void scr_init(Scr *s);
void scr_feed(Scr *s, const unsigned char *p, size_t n);
void send_keys(GTab *g, const char *p, size_t n);
void send_act(GTab *g, int a);
GTab *gui_new_tab(void);
void gui_close_tab(int i);
long row_abs(GTab *g, int line);
void switch_tab(int i);
/* engine.c */
const char *var_get(Tab *t, const char *name);
void var_set(Tab *t, const char *name, const char *value);
void var_unset(Tab *t, const char *name);
int valid_name(const char *s, size_t n);
void gui_write(Tab *t, const char *p, size_t n);
void gui_puts(Tab *t, const char *s);
void disp(Tab *t, const char *p, size_t n);
void emit_fd(Tab *t, int fd, const char *p, size_t n);
void emit(Tab *t, const char *s);
void errf(Tab *t, const char *fmt, ...);
void pump(Tab *t, int timeout_ms);
void drain_display(Tab *t);
void show_prompt(Tab *t);
void engine_run(EngineArgs *ea);
/* task2_commands.c */
int is_builtin(const char *n);
int exec_text(Tab *t, const char *text);
int exec_list(Tab *t, List *L);
int run_builtin(Tab *t, int argc, char **argv);
/* task3_parser.c */
long scan_bt(const char *s, long i);
long scan_paren(const char *s, long i);
long scan_brace(const char *s, long i);
int parse_text(Arena *ar, const char *src, List **out, char *err, size_t esz);
/* task3_expand.c */
void expand_word(Tab *t, const char *raw, Vec *out, int full);
char *expand_one(Tab *t, const char *raw);
/* task4_5_pipes.c */
int exec_pipe(Tab *t, Pipe *pl);
/* task6_multiwatch.c */
void mw_read(Tab *t, Watcher *w);
void mw_tick(Tab *t);
void mw_interrupt(Tab *t);
int bi_multiwatch(Tab *t);
/* task7_hotkeys.c */
int find_action(const char *name);
void binds_load(BindDef *t, Buf *msgs);
int bi_bind(Tab *t, int argc, char **argv);
void sel_range(long *a, int *ac, long *b, int *bc);
int pos_from_xy(int x, int y, long *abs, int *col);
char *selection_text(void);
void do_paste(Atom which);
void on_selection_request(XSelectionRequestEvent *rq);
void on_selection_notify(XSelectionEvent *sn);
void on_key(XKeyEvent *ke);
/* task7_editor.c */
void cur_prompt(Tab *t, char *buf, size_t sz);
void draw_input(Tab *t);
void prompt_refresh(Tab *t);
void in_insert(Tab *t, const char *s, int n);
void in_delete(Tab *t, int from, int to);
void in_set(Tab *t, const char *s);
void keys_parse(Tab *t);
/* task8_jobs.c */
void start_waiter(Tab *t, pid_t pid);
Job *job_new(Tab *t, pid_t pgid, int np, const char *cmd, int bg);
void process_events(Tab *t);
void jobs_notify(Tab *t);
void jobs_kill_all(Tab *t);
void child_signals(void);
int wait_fg(Tab *t, Job *j);
int bi_fg(Tab *t, int argc, char **argv);
int bi_bg(Tab *t, int argc, char **argv);
int bi_jobs(Tab *t);
int bi_kill(Tab *t, int argc, char **argv);
void run_interrupt(Tab *t);
void run_suspend(Tab *t);
void run_char(Tab *t, unsigned char c);
void run_eof(Tab *t);
/* task9_history.c */
int hist_init(void);
void hist_rewrite_file(void);
void hist_add(const char *cmd);
void hist_show(Tab *t, int want);
void hist_nav(Tab *t, int dir);
void search_start(Tab *t);
void search_finish(Tab *t, int run);
/* task10_completion.c */
void do_complete(Tab *t);
void pick_cancel(Tab *t);
void pick_select(Tab *t);
/* task11_scripts.c */
int file_is_script(const char *full);
int exec_if(Tab *t, IfNode *f);
int run_script(Tab *t, const char *path, char **args, int nargs, int subshell, const char *cstr);
int bi_test(Tab *t, int argc, char **argv);
int bi_source(Tab *t, int argc, char **argv);

#endif
