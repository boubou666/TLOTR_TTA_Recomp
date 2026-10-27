#!/usr/bin/env bash
# The bench lock shared by the PS2Recomp ports on one machine (TMB, VP2S, Drakengard, ...): a timed game run or
# benchmark only starts when the lock file is empty and no other port's game is running, and holds the file meanwhile.
# Heavy builds take no lock, but should not start while someone is measuring.
#   scripts/bench_lock.sh wait                         # until the lock is free
#   scripts/bench_lock.sh take <minutes> "<purpose>"   # wait, then write "tlotr-tta until HH:MM (purpose)"
#   scripts/bench_lock.sh release                      # empty it (only if it is ours)
#   scripts/bench_lock.sh status
# BENCH_LOCK overrides the path (default D:/ps2recomp_bench.lock on Windows, /tmp/ps2recomp_bench.lock elsewhere).
source "$(dirname "$0")/common.sh"
if [ -n "$EXE" ]; then LOCK="${BENCH_LOCK:-/d/ps2recomp_bench.lock}"; else LOCK="${BENCH_LOCK:-/tmp/ps2recomp_bench.lock}"; fi
NAME="tlotr-tta"

# An emptied lock may keep a newline (echo > file): only non-whitespace content counts as taken.
held() { [ -f "$LOCK" ] && grep -q '[^[:space:]]' "$LOCK"; }
# Another port's game: every port runs ps2EntryRunner or its own <port>.exe build of it.
game_running() {
    if [ -n "$EXE" ]; then
        tasklist.exe 2>/dev/null | grep -qiE '^(ps2EntryRunner|vp2s|tmb|drakengard)[^ ]*\.exe'
    else
        pgrep -f ps2EntryRunner >/dev/null 2>&1
    fi
}
wait_free() {
    local said=""
    while held || game_running; do
        [ -n "$said" ] || { echo "bench lock busy: $(cat "$LOCK" 2>/dev/null | tr -d '\r')"; said=1; }
        sleep 3
    done
}

case "${1:-status}" in
    wait) wait_free ;;
    take)
        # The fork's shared tool queues waiters in arrival order (PS2Recomp/tools/bench_lock.sh).
        minutes="${2:?minutes}"; purpose="${3:?purpose}"
        export PS2X_BENCH_LOCK="$LOCK"
        PS2X_BENCH_LOCK_ANNOUNCE="${PS2X_BENCH_LOCK_ANNOUNCE:-}"  # the tool reads it unguarded (set -u here)
        . <(tr -d "$(printf '\r')" < "$ROOT/PS2Recomp/tools/bench_lock.sh")  # Windows checkouts may carry CRLF
        bench_lock_take "$NAME" "$minutes" "$purpose"
        echo "took: $(cat "$LOCK")" ;;
    release)
        if grep -q "^$NAME " "$LOCK" 2>/dev/null; then : > "$LOCK"; echo "released"; else echo "not ours: $(cat "$LOCK" 2>/dev/null)"; fi ;;
    status) if held; then cat "$LOCK"; else echo "free"; fi ;;
    *) echo "usage: $0 wait|take <minutes> <purpose>|release|status"; exit 1 ;;
esac
