#!/bin/sh
# Headless tests: drive each tab's shell engine through the same pipe protocol the GUI uses (no X server needed).
# Usage: sh run_tests.sh [thread|process]      Output of every script is printed for inspection.
MODE=${1:-thread}
export HOME=$(mktemp -d); mkdir -p "$HOME/mydir"; cp check.sh calc.sh bad.sh "$HOME/mydir/"
touch "$HOME/mydir/abc.txt" "$HOME/mydir/abcd.txt" "$HOME/mydir/def.txt" "$HOME/mydir/myprog.c"
cd "$HOME/mydir" || exit 1
T=$OLDPWD
for f in basic quoting_pipes jobs_multiwatch history_hotkeys completion_scripts_bind edge_cases; do
  echo "================ $f ($MODE mode)"; "$T/driver" $MODE < "$T/$f.txt"
done
