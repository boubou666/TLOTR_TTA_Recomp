#!/usr/bin/env bash
# Maps the functions of the stripped ELF with Ghidra (headless, Emotion Engine extension installed) and the fork's
# ExportPS2Functions.java: writes recomp/ghidra/tta_functions.csv (names and boundaries) and a reference TOML with
# the stubs Ghidra classified (build/ghidra/ghidra_export.toml, to merge by hand into recomp/tta_slus21027.toml).
# Usage: GHIDRA_INSTALL_DIR=/path/to/ghidra_12.x_PUBLIC scripts/ghidra.sh     (takes a while: full auto-analysis)
source "$(dirname "$0")/common.sh"
[ -f "$ELF" ] || { echo "missing $ELF, run scripts/extract_iso.sh first"; exit 1; }
[ -d "${GHIDRA_INSTALL_DIR:-}" ] || { echo "set GHIDRA_INSTALL_DIR to a Ghidra install with the EE extension"; exit 1; }

WORK="$ROOT/build/ghidra"
mkdir -p "$WORK/project" "$WORK/scripts" "$(dirname "$GHIDRA_CSV")"
cp "$ROOT/PS2Recomp/ps2xRecomp/tools/ghidra/ExportPS2Functions.java" "$WORK/scripts/"
# Headless askFile() answers come from a .properties file next to the script.
native() { if [ -n "$EXE" ]; then cygpath -m "$1"; else echo "$1"; fi; }
cat > "$WORK/scripts/ExportPS2Functions.properties" <<PROPS
Choose output TOML config file Save = $(native "$WORK/ghidra_export.toml")
Choose output CSV file Save = $(native "$GHIDRA_CSV")
PROPS

args=("$(native "$WORK/project")" TTA -import "$(native "$ELF")" -overwrite -processor r5900:LE:32:default
      -scriptPath "$(native "$WORK/scripts")" -postScript ExportPS2Functions.java
      -analysisTimeoutPerFile 7200 -max-cpu "$JOBS")
if [ -n "$EXE" ]; then
    MSYS2_ARG_CONV_EXCL='*' cmd.exe /c "$(native "$GHIDRA_INSTALL_DIR/support/analyzeHeadless.bat")" "${args[@]}"
else
    "$GHIDRA_INSTALL_DIR/support/analyzeHeadless" "${args[@]}"
fi
[ -f "$GHIDRA_CSV" ] || { echo "Ghidra did not write $GHIDRA_CSV"; exit 1; }
echo "Function map: $GHIDRA_CSV ($(($(wc -l < "$GHIDRA_CSV") - 1)) entries)"
