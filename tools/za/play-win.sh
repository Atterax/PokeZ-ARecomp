#!/bin/bash
# Windows: Pokémon Z-A com o teclado (setas = d-pad, Enter = A, Backspace = B, P = Plus, M = Minus; WASD = analógico,
# K/J/L/I = A/B/X/Y). Log em za-tools/play.log, screenshot a cada 120 quadros em build/frame_*.png
S=$(cd "$(dirname "$0")" && pwd)
cd "$S/../LASRecomp" 2>/dev/null || cd "$S/../.." || exit 1  # (from za-tools/ or LASRecomp/tools/za/)
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
rm -f build/frame_*.png
export LARECOMP_SETTINGS=$S/za.ini LARECOMP_SCREENSHOT_EVERY=${SHOT:-120} LARECOMP_WAIT_DUMP=${WAIT:-10} LARECOMP_PROF_SVC=1 LARECOMP_SKIP_WHY=1
./${EXE:-build/lasrecomp.exe} extracted --play > "$S/play.log" 2>&1
echo "rc=$?"
