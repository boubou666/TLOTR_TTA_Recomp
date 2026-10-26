#!/usr/bin/env bash
# Runs ps2_recomp with the committed config and ps2_vu_aot on the ELF's VU1 microcode; output lands in generated/
# (VU1 microcode in generated/vu1/).
# Output goes through build/recomp-staging first and only files whose content changed are
# copied over, so ninja rebuilds just the affected unity batches instead of the whole game.
source "$(dirname "$0")/common.sh"
[ -f "$ELF" ] || { echo "missing $ELF, run scripts/extract_iso.sh first"; exit 1; }
[ -x "$TOOLS_BUILD/ps2xRecomp/ps2_recomp$EXE" ] && [ -x "$TOOLS_BUILD/ps2xRecomp/ps2_vu_aot$EXE" ] || "$ROOT/scripts/build_tools.sh"
cd "$ROOT"

STAGING="$ROOT/build/recomp-staging"
rm -rf "$STAGING"
mkdir -p "$STAGING" "$GENERATED"
sed 's|^output = .*|output = "build/recomp-staging/"|' "$CONFIG" > "$ROOT/build/recomp-staging.toml"
"$TOOLS_BUILD/ps2xRecomp/ps2_recomp$EXE" "build/recomp-staging.toml"
"$TOOLS_BUILD/ps2xRecomp/ps2_vu_aot$EXE" "$ELF" "$STAGING/vu1"

# Copies files whose content changed and drops functions that are no longer generated (rsync -rc --delete takes
# minutes over ~49,000 files on Windows). The manifest keeps the hashes of the last sync, so generated/ is not read back.
changed=$(python3 -I "$ROOT/tools/sync_tree.py" "$STAGING" "$GENERATED" "$ROOT/build/generated.manifest")
echo "Generated $(ls "$GENERATED" | wc -l) files in $GENERATED ($changed changed)"
