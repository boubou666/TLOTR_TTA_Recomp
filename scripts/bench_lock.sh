#!/usr/bin/env bash
# The bench lock shared by the PS2Recomp ports on one machine (TMB, VP2S, FFOP, SCTPT2, ...), through the fork's
# PS2Recomp/tools/bench_lock.sh: two slots, a game run takes one, a build or a timed run takes both ("alone").
#   scripts/bench_lock.sh take <minutes> "<purpose>" [alone]   # queue, then write "tlotr-tta until HH:MM (purpose)"
#   scripts/bench_lock.sh release                              # empty the slots that are ours
#   scripts/bench_lock.sh status
# BENCH_LOCK overrides the first slot's path (default D:/ps2recomp_bench.lock on Windows, /tmp/... elsewhere).
source "$(dirname "$0")/common.sh"
if [ -n "$EXE" ]; then LOCK="${BENCH_LOCK:-D:/ps2recomp_bench.lock}"; else LOCK="${BENCH_LOCK:-/tmp/ps2recomp_bench.lock}"; fi
NAME="tlotr-tta"
export PS2X_BENCH_LOCK="$LOCK"
PS2X_BENCH_LOCK_ANNOUNCE="${PS2X_BENCH_LOCK_ANNOUNCE:-}"  # the tool reads it unguarded (set -u here)
. <(tr -d "$(printf '\r')" < "$ROOT/PS2Recomp/tools/bench_lock.sh")  # Windows checkouts may carry CRLF

case "${1:-status}" in
    take)
        set +e # the shared tool is not written for set -e (a missing .ask file ends the take half-way)
        bench_lock_take "$NAME" "${2:?minutes}" "${3:?purpose}" "${4:-}"
        set -e
        echo "took: ${BENCH_LOCK_FILES[*]}" ;;
    release)
        # take and release run in different shells, so release finds our slots by their content.
        for file in "$(bench_lock_file)" "$(bench_lock_file2)"; do
            if grep -q "^$NAME until" "$file" 2> /dev/null; then : > "$file"; echo "released $file"; fi
        done ;;
    status)
        for file in "$(bench_lock_file)" "$(bench_lock_file2)"; do
            echo "$file: $(tr -d '\r' < "$file" 2> /dev/null)"
        done ;;
    *) echo "usage: $0 take <minutes> <purpose> [alone]|release|status"; exit 1 ;;
esac
