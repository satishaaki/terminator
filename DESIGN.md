# Terminator — design document

## 1. Overview
The program is split into one source file per task (see the table in `README.md`) around a shared header `terminator.h`. Three layers:

```
 X11 GUI (only code that touches Xlib)      per-tab pipes             Shell engine (one per tab)
 window, tab bar, text buffers, key  ──────► keys  (bytes) ───────►   line editor, parser, executor,
 mapping, clipboard, select() loop   ◄────── text (bytes+ESC) ◄──────  jobs, history, completion
                                                                        runs as a THREAD or a forked PROCESS
```
Because the engine only exchanges bytes with the GUI, *thread mode and process mode run exactly the same engine code*; the only
difference is `pthread_create()` vs `fork()` in `gui_new_tab()`. This is what guarantees "both modes behave the same".

## 2. X11 GUI and event loop (Task 1)
* `XOpenDisplay`, `XCreateWindow`, `XLookupString`, `XDrawImageString`; the frame is composed into a Pixmap and copied (no flicker).
* Per tab a **Scr** (screen model): ring buffer of 5000 variable-length lines + cursor column. The GUI interprets a tiny escape
  protocol coming from the engine: text, `\n \r \b \t`, `ESC[K` (erase to end of line), `ESC[nG` (cursor column), `ESC[2J` (clear),
  `ESC]0;titleBEL` (tab label = cwd), `ESC]BBEL` (reload key bindings), `ESC]XBEL` (tab closed). Lines wrap at draw time.
* Loop: drain `XPending/XNextEvent`, redraw if dirty, then block in `select()` on the X connection fd **and** every tab's output pipe —
  no timers, no polling, so it never busy-waits.
* Keys: `KeyPress` → keysym + modifiers (Control/Shift/Mod1) → canonical spec such as `ctrl+shift+c` → lookup in the binding table →
  either a GUI action (copy/paste/tabs) or the two bytes `ESC,'0'+action` sent to the active tab. Plain characters are sent as themselves.
* Tabs: click a label, `[+]`, middle-click to close, keyboard shortcuts. A tab keeps running while hidden (its pipe is still read).
* Clipboard: mouse drag selects text in the buffer; Ctrl+Shift+C owns CLIPBOARD+PRIMARY and answers `SelectionRequest`;
  Ctrl+Shift+V (or middle click) uses `XConvertSelection` and handles `SelectionNotify`.

## 3. Thread mode vs process mode, shared state
| | thread mode | process mode |
|---|---|---|
| tab engine | detached pthread (8 MB stack) | `fork()` of the GUI process (the child closes the X11 socket) |
| tab ↔ GUI | two pipes | two pipes |
| isolation | tabs share the address space; every tab has its own `Tab` struct (cwd, variables, jobs) — no shared mutable globals except history | a crashed/killed tab only closes its own pipes → GUI sees EOF and removes it |

**History** is the only state shared by all tabs: an anonymous `MAP_SHARED` mapping (ring of 10 000 × 512 bytes) guarded by a
process-shared POSIX semaphore (`sem_open` + immediate `sem_unlink`, because unnamed `sem_init` semaphores do not work on macOS).
It is loaded from `~/.terminator_history` at start, appended to (`O_APPEND`) on every command, and compacted to the last 10 000 lines on exit.
Both modes use the identical mechanism, so a command typed in one tab is immediately available (↑, Ctrl+R, `history`) in all others.
`cd`, variables and jobs live in the per-tab `Tab` structure, so they never leak between tabs; children `chdir()` to the tab's cwd after `fork()`.

## 4. Process and job management (Tasks 2, 5, 8)
* **Parsing** (`tokenize` → `parse_list`): words keep their raw text; tokens `| || && ; & newline < > >> >& ( )`; the grammar supports
  and-or lists, pipelines, `!`, and `if/elif/else/fi`. The parser reports *incomplete* (→ `>` prompt) or *syntax error* (→ message, nothing runs).
  Expansion happens at run time, per command: quotes, `$VAR`, `$?`, `$#`, `$1…`, `${…}`, `$(…)`, backticks, `$((…))`, `~`, field splitting, globbing.
* **Pipeline** with N stages: N−1 pipes created with `FD_CLOEXEC`, N× `fork()`; each child `setpgid`s into the pipeline's group, resets signals,
  `dup2`s its stdin/stdout/stderr, `chdir`s, sets `environ` to the tab's variables and calls `execvp`. The parent opens redirection files *before*
  forking (so a missing input file is reported and nothing starts) and closes every end it does not need.
* **Built-ins inside pipelines** (`echo`, `history`, scripts…) run in the engine after the external stages have been started, writing into their pipe.
* **Waiting without SIGCHLD:** every child gets a tiny *waiter thread* that blocks in `waitpid(WUNTRACED|WCONTINUED)` and posts an event to the
  tab (queue + wake-up pipe). The engine's `pump()` therefore waits for *everything* — keys, output, child events — in a single `select()`.
* **Jobs:** a table of `{id, pgid, procs[], state RUN/STOP/DONE, command}`; `fg`, `bg`, `jobs`, `kill %n`, "Done" notifications before the next prompt.
  Foreground jobs get a stdin pipe fed from typed lines.
* **Command substitution** runs the inner text through the same executor with stdout redirected to a capture pipe that `pump()` drains (no deadlock for big output).

## 5. IPC, `dup2`, redirection
Display pipe (per tab): every command's stdout/stderr that is not redirected goes to one pipe whose read end the engine forwards to the GUI.
`<` opens read-only, `>`/`>>` create/truncate/append, `2>`, `2>&1`, `>&2` set the child's fds via `dup2`. Each job has its own descriptor,
so background jobs reading the same file do not share a file offset.

## 6. multiWatch (Task 6)
Each command runs in a forked copy of the interpreter (own process group) whose stdout/stderr is a dedicated pipe. The engine `select()`s on
all read ends plus a timeout for the next re-run (`max(now, start+interval)`), prints a finished run as
`"cmd", <unix_ts>:` / 52 dashes / output / 52 dashes, then schedules the next run. Ctrl+C (an action sent by the GUI) → `kill(-pgid, SIGTERM/SIGKILL)`
for every watcher, `waitpid`, and the prompt returns.

## 7. Signals (Task 8)
No TTY exists, so signals are generated by the shell: Ctrl+C → `kill(-pgid, SIGINT)` (second press: `SIGKILL`); Ctrl+Z → `SIGTSTP`, the waiter sees
the stop, the job becomes *Stopped* and the prompt returns; `fg/bg` send `SIGCONT`. The shell and GUI ignore `SIGPIPE` and never receive
terminal signals, so they cannot be stopped by key presses; children restore all default dispositions before `exec`.

## 8. Data structures: history, completion
* History: ring buffer in shared memory (`start`, `count`, `total` for numbering). Ctrl+R: pass 1 newest-first exact match; pass 2 longest-common-substring
  (O(len·len) DP with two rows) for all entries, max length > 2 selects every entry with that length (newest first, de-duplicated); else "No match…".
* Completion: `opendir/readdir` of the (sub)directory → sorted `Vec` of names; longest common prefix; numbered pick list kept in the tab until a digit arrives.
  Replacement strings are shell-escaped.
* Editor: one growable buffer + cursor per tab; every edit redraws the line with `\r ESC[K prompt text ESC[nG`.

## 9. Hotkeys (Task 7)
Actions have names and default keys (`bind_defaults`). `~/.terminatorrc` is parsed with the same validator the `bind` built-in uses
(`parse_keyspec`, `resolve_conflicts`). Invalid key → reported, default kept; two actions on one key → reported, customised ones fall back to default.
`bind` rewrites the file and sends `ESC]B` so the GUI reloads without a restart; bindings persist across restarts.

## 10. Scripts (Task 11)
`run_script` reads the file, parses it **completely first** (a missing `fi` is reported and nothing runs), then executes. `./x.sh` (with `#!…sh`/`bash`
shebang or `.sh` suffix), `bash x.sh`, `bash -c`, and `source` all use it. `./x.sh` and `bash x.sh` behave like a subshell (variables, cwd, `$@` restored afterwards);
`source` runs in the current shell.

## 11. Other decisions
Prompt `user@terminator> ` (real login name); `$?` of a signal death is `128+n`; `Ctrl+D` on an empty line closes the tab, otherwise deletes a character;
the last tab closing quits the program; closing a tab sends SIGHUP/SIGKILL to its jobs.
