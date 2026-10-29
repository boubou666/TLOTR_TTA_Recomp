#!/usr/bin/env bash
# Recompiles TTA's IOP modules to native C++ in generated/iop/ (run with PS2X_IOP_NATIVE=1), with the fork's
# tools/iop_recomp.py. The recompiler works from each module as the IOP loaded it (relocated at its base), so this
# first boots the game long enough for it to load its modules, with PS2X_IOP_DUMP_MODULES. The output is derived
# from the game, like the rest of generated/: never committed. Run it under the bench lock (a game run).
#   scripts/iop_recomp.sh [seconds of boot, default 40] [modules, default: all eight]
source "$(dirname "$0")/common.sh"
cd "$ROOT"
SECONDS_TO_BOOT=${1:-40}
MODULES=${2:-"AUDIOPF LIBSD SIO2MAN SIO2D DS2U_D MCMAN MCSERV DBCMAN"}
BIN="$GAME_BUILD/PS2Recomp/ps2xRuntime/ps2EntryRunner$EXE"
DUMPS="build/iopmods"
rm -rf "$DUMPS" && mkdir -p "$DUMPS" "$GENERATED/iop"
PS2X_IOP_DUMP_MODULES="$ROOT/$DUMPS" timeout "$SECONDS_TO_BOOT" "$BIN" "$ELF" > build/iop_dump.log 2>&1 || true
for module in $MODULES; do
    json=$(ls "$DUMPS" | grep -i "^$module.IRX.*\.json$" | head -1)
    [ -n "$json" ] || { echo "no dump of $module (see build/iop_dump.log)"; exit 1; }
    python PS2Recomp/tools/iop_recomp.py "$DUMPS/$json" --out "$GENERATED/iop"
    python PS2Recomp/tools/iop_recomp.py "$DUMPS/$json" --list > "build/iop_${module}_list.txt"
done
ls "$GENERATED/iop"
