#!/bin/bash
# M58A：隔离树构建（主工作区只改源码，构建一律在 /tmp/m58a_tree）
#   usage: bash m58a_build.sh   (tree=/tmp/m58a_tree, hap -> /tmp/m58a_build)
set -uo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
TREE=/tmp/m58a_tree
DEVECO_APP="/Applications/DevEco-Studio.app"
export NODE_HOME="$DEVECO_APP/Contents/tools/node"
export PATH="$NODE_HOME/bin:$PATH"
export DEVECO_SDK_HOME="$DEVECO_APP/Contents/sdk"
HVIGORW="$DEVECO_APP/Contents/tools/hvigor/bin/hvigorw"
LOG="$ROOT/logs/m58a/build.log"
mkdir -p "$ROOT/logs/m58a" /tmp/m58a_build

SO="$ROOT/mario-odessy-harmony/entry/libs/arm64-v8a/libvk_swiftshader.so"
MARK=$(strings "$SO" 2>/dev/null | grep -c m44c-icache-v1)
echo "== [$(date '+%H:%M:%S')] swiftshader marker m44c-icache-v1 count=$MARK (need >=1)" | tee "$LOG"
[ "$MARK" -ge 1 ] || { echo "X libvk_swiftshader.so lacks m44c marker -> ABORT" | tee -a "$LOG"; exit 1; }
echo "   so md5=$(md5 -q "$SO") bytes=$(stat -f%z "$SO")" | tee -a "$LOG"

echo "== [$(date '+%H:%M:%S')] rsync -> $TREE" | tee -a "$LOG"
/usr/bin/rsync -a --delete \
  --exclude build --exclude .cxx --exclude .hvigor --exclude oh_modules \
  --exclude logs --exclude shots --exclude debug-shots --exclude hap-archive \
  --exclude game-work --exclude infra --exclude third --exclude swiftshader --exclude swiftshader-dl \
  --exclude m15-pack --exclude 'shader-conv/work' --exclude .git --exclude __pycache__ \
  "$ROOT/" "$TREE/" >>"$LOG" 2>&1 || { echo "X rsync failed" | tee -a "$LOG"; exit 1; }
echo "== [$(date '+%H:%M:%S')] rsync done :: $(du -sh "$TREE" | cut -f1)" | tee -a "$LOG"

echo "== [$(date '+%H:%M:%S')] hvigorw assembleHap" | tee -a "$LOG"
cd "$TREE/mario-odessy-harmony" || exit 1
"$HVIGORW" --mode module -p product=default assembleHap >>"$LOG" 2>&1
RC=$?
echo "EXIT=$RC" | tee -a "$LOG"
HAP="$TREE/mario-odessy-harmony/entry/build/default/outputs/default/entry-default-signed.hap"
if [ -f "$HAP" ]; then
  cp -f "$HAP" /tmp/m58a_build/entry.hap
  { echo "bytes=$(stat -f%z /tmp/m58a_build/entry.hap)"; echo "md5=$(md5 -q /tmp/m58a_build/entry.hap)"; } > /tmp/m58a_build/md5.txt
  echo "HAP=$(stat -f%z /tmp/m58a_build/entry.hap) md5=$(md5 -q /tmp/m58a_build/entry.hap)" | tee -a "$LOG"
fi
SO2="$TREE/mario-odessy-harmony/entry/build/default/intermediates/libs/default/arm64-v8a/libgame.so"
[ -f "$SO2" ] && echo "SO=$(stat -f%z "$SO2") md5=$(md5 -q "$SO2") strings_m58a=$(strings "$SO2" | grep -c m58a-al)" | tee -a "$LOG"
exit $RC
