#!/bin/bash
# M45A / S2b-2：隔离树构建（主工作区只改源码，构建一律在 /tmp/m46a_tree）
#   usage: bash m46a_build.sh
# 构建树 = rsync 副本（排除 build/.cxx/logs/game-work/infra/third/swiftshader 等大件与 vendor 源）
set -uo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
TREE=/tmp/m46a_tree
DEVECO_APP="/Applications/DevEco-Studio.app"
export NODE_HOME="$DEVECO_APP/Contents/tools/node"
export PATH="$NODE_HOME/bin:$PATH"
export DEVECO_SDK_HOME="$DEVECO_APP/Contents/sdk"
HVIGORW="$DEVECO_APP/Contents/tools/hvigor/bin/hvigorw"
LOG="$ROOT/logs/m46a/build.log"
mkdir -p "$ROOT/logs/m46a"

echo "== [$(date '+%H:%M:%S')] rsync -> $TREE" | tee "$LOG"
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
[ -f "$HAP" ] && echo "HAP=$(stat -f%z "$HAP") md5=$(md5 -q "$HAP")" | tee -a "$LOG"
SO="$TREE/mario-odessy-harmony/entry/build/default/intermediates/libs/default/arm64-v8a/libgame.so"
[ -f "$SO" ] && echo "SO=$(stat -f%z "$SO") md5=$(md5 -q "$SO")" | tee -a "$LOG"
exit $RC
