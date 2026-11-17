#!/usr/bin/env bash
# Mac 侧构建 mario-odessy-harmony HAP（对齐 Windows 侧: hvigorw --mode module -p product=default assembleHap）
# 用法: ./build_hap_mac.sh [hvigorw参数...]   例如 ./build_hap_mac.sh clean 再 ./build_hap_mac.sh
set -euo pipefail

PROJECT_DIR="/Users/zpp/VibeCoding/switch2hos/mario-odessy-harmony"
DEVECO_APP="/Applications/DevEco-Studio.app"
NODE_HOME="$DEVECO_APP/Contents/tools/node"
export NODE_HOME
export PATH="$NODE_HOME/bin:$PATH"
export DEVECO_SDK_HOME="$DEVECO_APP/Contents/sdk"
HVIGORW="$DEVECO_APP/Contents/tools/hvigor/bin/hvigorw"

cd "$PROJECT_DIR"
echo "== node: $(node --version)  DEVECO_SDK_HOME=$DEVECO_SDK_HOME"
echo "== hvigorw: $HVIGORW $*"
exec "$HVIGORW" --mode module -p product=default assembleHap "$@"
