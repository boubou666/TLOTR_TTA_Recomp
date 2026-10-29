#!/usr/bin/env bash
# Launches the recompiled game. Extra args are passed to ps2EntryRunner.
source "$(dirname "$0")/common.sh"
BIN="$GAME_BUILD/PS2Recomp/ps2xRuntime/ps2EntryRunner$EXE"
[ -x "$BIN" ] || { echo "missing $BIN, run scripts/build_runtime.sh first"; exit 1; }
# Sound and the IOP on the PC's clock (the SPU2 mixes at 48 kHz of real time, the IOP's timers follow the host),
# not on emulated cycles; the runtime reads these before the game override runs. =0 keeps the emulated timing.
export PS2X_SPU2_WALLCLOCK="${PS2X_SPU2_WALLCLOCK:-1}" PS2X_IOP_WALLCLOCK="${PS2X_IOP_WALLCLOCK:-1}"
exec "$BIN" "$ELF" "$@"
