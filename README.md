# Terminator — a custom terminal + shell

Terminator is a terminal emulator **and** shell written in C for Linux/macOS. It draws its own X11 window (Xlib only, no
toolkit) with multiple tabs; each tab is an independent shell that runs commands with pipes, redirection, job control,
history, auto-completion and bash scripts — all interpreted by Terminator itself (it never hands a script to `/bin/bash`).

## Source layout — one file per task
| File | Task | What is inside |
|------|------|----------------|
| `terminator.h` | — | all shared types, constants and function prototypes |
| `util.c` | — | helpers: growable strings `Buf`, vectors `Vec`, parse arena, `mkpipe`, path helpers |
| `task1_gui.c` | 1 | X11 window, tab bar, drawing, text buffer per tab, mouse, `select()` event loop, `main()`, thread/process start-up |
| `engine.c` | 1/2 | per-tab shell engine core: `Tab` state, variables, output routing, `pump()` (the `select()` loop), prompt, main loop |
| `task2_commands.c` | 2 | built-ins (`cd`, `echo`, `pwd`, `export`…), running command lists (`&&`, `\|\|`), exit status `$?` |
| `task3_parser.c` | 3 | lexer + parser: quotes, multi-line / incomplete input, `$( )`, backticks, `if/elif/else/fi` grammar |
| `task3_expand.c` | 3 | expansion: `$VAR`, `$?`, `$(...)`, backticks, quoting rules, `~`, globbing, `$((…))` |
| `task4_5_pipes.c` | 4, 5 | redirection (`<` `>` `>>` `2>` `2>&1`, `dup2`) and pipelines (N commands, N−1 pipes, `fork`+`execvp`) |
| `task6_multiwatch.c` | 6 | `multiWatch`: parallel children, one pipe each, `select()`, re-run timer, Ctrl+C |
| `task7_hotkeys.c` | 7 | key bindings (defaults, `~/.terminatorrc`, `bind`), X11 `KeyPress` mapping, Ctrl+Shift+C/V clipboard |
| `task7_editor.c` | 7 | command-line editor: cursor, kill/delete, key dispatch |
| `task8_jobs.c` | 8 | job table, process groups, waiter threads, Ctrl+C/Ctrl+Z, `jobs` `fg` `bg` `kill` |
| `task9_history.c` | 9 | shared history (shared memory + semaphore + file), `history`, ↑/↓, Ctrl+R search |
| `task10_completion.c` | 10 | Tab completion and numbered pick list |
| `task11_scripts.c` | 11 | script interpreter, `if` execution, `test`/`[ ]`, `source` |

`VIVA.md` is a one-page cheat sheet (task → file → key functions → what to say).

## Dependencies
| OS | Install |
|----|---------|
| Ubuntu/Debian | `sudo apt install build-essential libx11-dev` |
| macOS | `brew install --cask xquartz` (then **log out/in once**, and start XQuartz before running) |

## Build and run
```
make                          # builds ./terminator  (uses -I/opt/X11/include -L/opt/X11/lib -lX11 on macOS)
./terminator                  # thread mode  (default): one thread per tab
./terminator useProcess=True  # process mode: one forked process per tab
```
macOS: if the window does not open, run `open -a XQuartz`, then `echo $DISPLAY` in the terminal you launch from (it must be set).
Both modes behave identically; in process mode killing/crashing one tab's process leaves the other tabs untouched.

## Features
| Task | What you get |
|------|--------------|
| 1 GUI/tabs | Xlib window, per-tab text buffer with scrollback (5000 lines, mouse wheel / Shift+PgUp/PgDn), tab bar `[ 1: ~/dir ] [ + ]`, open/close/switch by keyboard and mouse. The event loop sleeps in `select()` (no busy waiting). |
| 2 Commands | `fork()+execvp()` for externals; built-ins `cd exit history multiWatch jobs fg bg source . export unset pwd echo true false : test [ shift clear bind kill`; `$?`; "command not found" keeps the tab usable. |
| 3 Multiline/quoting | `>` continuation prompt for trailing `\`, open quotes, open `$(`/backticks, trailing `|` `&&` `\|\|`, unfinished `if`; nested/multi-line `$(...)` and backticks; bash quoting rules; `${VAR}`, `$((1+2))`, globbing, `~`. |
| 4 Redirection | `<  >  >>  2>  2>&1  >&2`; missing input file is reported and the command is not run; each job opens its own file descriptor. |
| 5 Pipes | Any length: N commands → N−1 `pipe()`s and N children; combines with redirection; built-ins/scripts work inside pipelines. |
| 6 multiWatch | `multiWatch ["cmd1","cmd2"]` (optional `-n SECONDS`, default 2): one process + one pipe per command, `select()` on all read ends, exact output format from the spec, Ctrl+C kills every child and the shell survives. |
| 7 Hotkeys | ←/→, Ctrl+A/E/U/K/W/L/D, ↑/↓, Ctrl+Shift+C/V (real X11 selections: mouse-select text, copy, paste, also middle-click paste). **Rebindable** (see below). |
| 8 Signals/jobs | Ctrl+C (SIGINT to the job's process group, prompt returns), Ctrl+Z (SIGTSTP + `[1]+ Stopped cmd`), `cmd &`, `jobs`, `fg %n`, `bg %n`, `kill %n`; the GUI never stops. |
| 9 History | Last 10 000 commands in `~/.terminator_history`, shared by every tab (shared memory + semaphore); `history` shows the latest 1 000 (`history N` for fewer); Ctrl+R search exactly as specified (exact match → longest common substring > 2 → "No match…"). |
| 10 Completion | Tab completes the word before the cursor from the tab's directory (also `dir/pre`): unique → complete (dirs get `/`), several → longest common prefix then a numbered list; press the number (several digits + Enter for ≥10). No match → nothing happens. |
| 11 Scripts | `./script.sh`, `bash script.sh [args]`, `bash -c '...'`, `source script.sh [args]`; comments, variables, `$1 $# $? $0 $@`, `if/elif/else/fi` (nested), `[ ]`/`test` (`-eq -ne -gt -ge -lt -le = != -f -d -e -r -w -x -s -z -n`, `! -a -o`), `exit [n]`. A syntax error (e.g. missing `fi`) is reported and nothing runs. |

### Changing the prompt
The prompt comes from the shell variable `PS1` (default `\u@terminator> `). Escapes: `\u` user, `\h` terminator, `\w` current directory (`~` for home), `\W` last part of the directory, `\$` a `$`.
```
PS1='\u@terminator \W> '      # user@terminator mydir>      (type it at the prompt)
PS1=                          # back to the default
TERMINATOR_PS1='\u@terminator \W> ' ./terminator     # set it when starting
```

### Changing the hotkeys (Task 7)
Edit `~/.terminatorrc` (see `tests/sample.terminatorrc`) or use the built-in, which takes effect immediately and is saved:
```
bind                       # list all actions and keys
bind line-start ctrl+b     # rebind
bind line-start default    # back to the default key
bind reset                 # forget all customisations
```
Actions: `cursor-left cursor-right history-prev history-next line-start line-end kill-to-start kill-to-end delete-word
clear-screen eof search-history complete interrupt suspend copy paste new-tab close-tab next-tab prev-tab`.
An invalid key, an unknown action, or a key already used by another action is **reported** and the default key is used.
Tab shortcuts: `Ctrl+Shift+T` or `Ctrl+T` new tab, `Ctrl+Shift+W` close tab (or type `exit` / Ctrl+D on an empty line), `Ctrl+Tab` / `Ctrl+Shift+Tab` next/previous tab, `Ctrl+1..9` or `Alt+1..9` jump to a tab, `Ctrl+PgUp/PgDn` also switch; click a tab label or `[ + ]`, middle-click a tab to close it.
On a Mac keyboard (no PgUp/PgDn, Option is not Alt unless enabled in XQuartz → Preferences → Input) use `Ctrl+T`, `Ctrl+Tab`, `Ctrl+1..9` or the mouse. To see what a key press arrives as: `TERMINATOR_DEBUG_KEYS=1 ./terminator` prints one line per key to the Mac Terminal window.

## Known limitations / design choices
* Output is drawn with core X11 fonts and treated as ASCII (other bytes show as `?`); there is no ANSI colour/cursor-addressing, so full-screen programs (vim, top) are not supported. Commands do not see a TTY (stdout is a pipe), so some programs buffer their output until they exit.
* Typed text is forwarded to a running foreground command line by line (Enter) — e.g. `cat`, `sort`, `./a.out` reading stdin; Ctrl+D closes its stdin.
* `cmd1 && cmd2 &` backgrounds only the last pipeline; `( )` subshells, loops, functions, here-documents and `$(( ))` beyond integer `+ - * / % ( ) comparisons` are not implemented.
* `exit` inside a script given to `source` ends that script (it does not close the tab). Multi-line commands are stored in history with newlines turned into spaces; commands longer than 511 characters are truncated in history.
* multiWatch prints each command's complete output block when that run finishes. Commands run via a forked copy of the interpreter (so they may use built-ins and pipes).
* `useProcess=True` and `useprocess=true` are both accepted.

## Tests
`make test` (needs no X server; it rebuilds all the `.c` files with the GUI's `main` renamed) drives the shell engines through the same key/output pipes the GUI uses and prints the resulting screen
for each script in `tests/` (`sh tests/run_tests.sh process` for process mode). The GUI itself was exercised under Xvfb with synthetic key events.
