#!/usr/bin/env bash
# 工作流构建门禁 v2：防 PATH / 环境自证
set -u
echo "[wf_build] bash=$0 argv0=$(readlink -f "$0" 2>/dev/null || echo '?') pwd=$(pwd)"
export DEVECO_SDK_HOME="C:/Program Files/Huawei/DevEco Studio/sdk"
NODE_BIN="$(command -v node 2>/dev/null || true)"
if [ -z "$NODE_BIN" ]; then
  for c in "/c/Program Files/Huawei/DevEco Studio/tools/node/node.exe" "/c/Program Files/nodejs/node.exe" "$HOME/AppData/Local/Programs/nodejs/node.exe" "/c/nvm4w/nodejs/node.exe"; do
    [ -x "$c" ] && NODE_BIN="$c" && break
  done
fi
echo "[wf_build] node=${NODE_BIN:-NOT_FOUND}"
[ -z "$NODE_BIN" ] && exit 127
cd "C:/Users/macvi/VibeCoding/switch2hos/mario-odessy-harmony" || { echo "[wf_build] cd failed"; exit 9; }
echo "[wf_build] building..."
"$NODE_BIN" "/c/Program Files/Huawei/DevEco Studio/tools/hvigor/bin/hvigorw.js" assembleHap -p product=default -p buildMode=debug --no-daemon
rc=$?
echo "[wf_build] exit=$rc hap_mtime=$(ls -l --time-style=long-iso entry/build/default/outputs/default/entry-default-signed.hap 2>/dev/null | awk '{print $6,$7}')"
exit $rc
