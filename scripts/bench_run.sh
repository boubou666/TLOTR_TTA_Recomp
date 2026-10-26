#!/usr/bin/env bash
# Runs one command under the shared bench lock: waits for it, takes it, runs, and releases it on any exit (end,
# error, Ctrl-C, kill), so a stopped run never leaves "tlotr-tta until ..." behind in the file.
#   scripts/bench_run.sh <minutes> "<purpose>" <command> [args...]
source "$(dirname "$0")/common.sh"
minutes="${1:?minutes}"; purpose="${2:?purpose}"; shift 2
[ $# -gt 0 ] || { echo "usage: $0 <minutes> <purpose> <command> [args...]"; exit 1; }

"$ROOT/scripts/bench_lock.sh" take "$minutes" "$purpose" || exit 1
trap '"$ROOT/scripts/bench_lock.sh" release' EXIT
trap 'exit 130' INT TERM HUP
"$@"
