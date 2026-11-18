#!/bin/sh
# Rebuild ReXenon (thirdparty/ReXenon, our XenonRecomp fork) and regenerate out/ppc from
# game/default.xex + Title Update #2 (out/tu2/default.xexp).
# First time: py -3 tools/xiso.py extract "<PGR4 disc>.iso" game
#             py -3 tools/stfs.py update "<TU_16L61VP_...>" out/tu2
set -e
cd "$(dirname "$0")/.."
cmd //c "tools\vcenv.cmd ninja -C build/tools rexenon/XenonRecomp/XenonRecomp.exe" | tail -1
rm -rf out/ppc && mkdir -p out/ppc
build/tools/rexenon/XenonRecomp/XenonRecomp.exe config/pgr4.toml thirdparty/ReXenon/XenonUtils/ppc_context.h > out/recomp.log 2>&1
# Its notes (functions split at stranded branch targets, tail calls, the indirect-call table) are information, not errors.
grep -v "^Recompiling" out/recomp.log || true
