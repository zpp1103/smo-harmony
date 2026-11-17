#!/bin/bash
# M44C：隔离树构建（主工作区只改源码，构建一律在 /tmp/m44c_tree）
#   usage: bash m44c_build.sh [extra_define ...]   例：M44C_X=1
# 只有隔离树的 CMakeLists 会加 -DM44C_X=1；主工作区源码不带这个编译开关。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
TREE=/tmp/m44c_tree
DEVECO_APP="/Applications/DevEco-Studio.app"
export NODE_HOME="$DEVECO_APP/Contents/tools/node"
export PATH="$NODE_HOME/bin:$PATH"
export DEVECO_SDK_HOME="$DEVECO_APP/Contents/sdk"
HVIGORW="$DEVECO_APP/Contents/tools/hvigor/bin/hvigorw"
LOG="$ROOT/logs/m44c/build.log"
mkdir -p "$ROOT/logs/m44c"

echo "== [$(date '+%H:%M:%S')] rsync -> $TREE" | tee "$LOG"
/usr/bin/rsync -a --delete \
  --exclude build --exclude .cxx --exclude .hvigor --exclude oh_modules \
  --exclude logs --exclude shots --exclude debug-shots --exclude hap-archive \
  --exclude swiftshader/build-ohos --exclude swiftshader/dist \
  --exclude .git \
  "$ROOT/" "$TREE/" >>"$LOG" 2>&1 || { echo "X rsync failed" | tee -a "$LOG"; exit 1; }

DEFS=""
for d in "$@"; do DEFS="$DEFS $d"; done
if [ -n "$DEFS" ]; then
  CM="$TREE/mario-odessy-harmony/entry/src/main/cpp/CMakeLists.txt"
  # 隔离树专用的一行：把额外宏塞进 target_compile_definitions（只在这一行生效）
  /usr/bin/sed -i '' "s|target_compile_definitions(\${NODE_MODULE} PRIVATE HOST_ARCH=1)|target_compile_definitions(\${NODE_MODULE} PRIVATE HOST_ARCH=1$DEFS)|" "$CM"
  grep -n "HOST_ARCH=1" "$CM" | head -2 | tee -a "$LOG"
fi

echo "== [$(date '+%H:%M:%S')] hvigorw assembleHap" | tee -a "$LOG"
cd "$TREE/mario-odessy-harmony" || exit 1
"$HVIGORW" --mode module -p product=default assembleHap >>"$LOG" 2>&1
RC=$?
echo "EXIT=$RC" | tee -a "$LOG"
HAP="$TREE/mario-odessy-harmony/entry/build/default/outputs/default/entry-default-signed.hap"
[ -f "$HAP" ] && { echo "HAP=$(stat -f%z "$HAP") md5=$(md5 -q "$HAP" | cut -c1-12)" | tee -a "$LOG"; cp "$HAP" "$ROOT/logs/m44c/entry-m44c-signed.hap"; }
exit $RC
