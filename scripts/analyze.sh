#!/usr/bin/env bash
# Regenerates recomp/tta_slus21027.toml from the ELF with ps2_analyzer (SDK functions recognised by the fork's
# signature database, since the retail ELF is stripped) and points it at the Ghidra function map.
# WARNING: this overwrites hand edits in the TOML; diff before committing.
source "$(dirname "$0")/common.sh"
[ -f "$ELF" ] || { echo "missing $ELF, run scripts/extract_iso.sh first"; exit 1; }
cd "$ROOT"
"$TOOLS_BUILD/ps2xAnalyzer/ps2_analyzer$EXE" "game/SLUS_210.27" "recomp/tta_slus21027.toml"
sed -i -e 's|^output = .*|output = "generated/"|' \
       -e 's|^ghidra_output = .*|ghidra_output = "recomp/ghidra/tta_functions.csv"|' "$CONFIG"
