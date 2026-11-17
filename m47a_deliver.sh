#!/bin/bash
# M47A：DMSV bundle 投递（每次 install -r 后必做）（A 路 = 覆盖已存在的占位文件；只碰模拟器 127.0.0.1:5555）
#   usage: bash m47a_deliver.sh [src=/tmp/m42b/full.dmsv]
#   流程：set smo_m45a=1 → 起一次 app 让构造期 mkdir DM_ROOT 并建空 shaders.dmsv 占位 → force-stop
#         → hdc file send 覆盖内容 → **用 ls -l 字节数校验**（el2 下 hdc 不传退出码，M44B 就是被"假成功"骗了）
set -uo pipefail
SRC="${1:-/tmp/m42b/full.dmsv}"
HDC="/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc"
TGT="${HDC_TARGET:-127.0.0.1:5555}"
PKG="com.vibecc.smo"
FILES="/data/app/el2/100/base/com.vibecc.smo/files"
DUMP="$FILES/m45a_dump"
DST="$DUMP/shaders.dmsv"
OUT="$(cd "$(dirname "$0")" && pwd)/logs/m47a"
mkdir -p "$OUT"
LOG="$OUT/deliver.log"
: > "$LOG"
log(){ printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*" | tee -a "$LOG"; }
h(){ "$HDC" -t "$TGT" "$@"; }
sz(){ h shell "stat -c %s $1 2>/dev/null" | tr -dc '0-9'; }

[ -f "$SRC" ] || { log "X src missing: $SRC"; exit 1; }
EXPECT=$(stat -f%z "$SRC"); SRC_MD5=$(md5 -q "$SRC")
log "=== m47a deliver src=$SRC bytes=$EXPECT md5=$SRC_MD5"
h shell "echo ok" >/dev/null 2>&1 || { log "X target offline ($TGT)"; exit 1; }

for i in 1 2 3; do
  P=$(h shell "pidof $PKG" | tr -d '[:space:]'); [ -z "$P" ] && break
  log "pre-stop pid=$P"; h shell "aa force-stop $PKG" >/dev/null 2>&1; sleep 3
done

# 1) 开关 = 1（DM_ROOT 只在这个状态下被构造期设置/建目录）
h shell "echo 1 > $FILES/smo_m45a" >/dev/null 2>&1
log "smo_m45a=$(h shell "cat $FILES/smo_m45a" | tr -d '\r\n ')"

# 2) 起一次，等构造期把 $DUMP + 空 shaders.dmsv 建出来
h shell "power-shell wakeup" >/dev/null 2>&1
h shell "aa start -a EntryAbility -b $PKG" >/dev/null 2>&1
FOUND=0
for i in $(seq 1 30); do
  sleep 2
  if [ "$(h shell "test -f $DST && echo yes" | tr -d '\r\n ')" = "yes" ]; then FOUND=$((i*2)); break; fi
done
PRESZ=$(sz "$DST"); PRESZ=${PRESZ:-none}
log "placeholder: found_after=${FOUND}s size=$PRESZ"
h shell "aa force-stop $PKG" >/dev/null 2>&1; sleep 3
[ "$FOUND" -gt 0 ] || { log "X placeholder never appeared —— 构造期没建出 $DST，A 路不通"; exit 1; }

# 3) 覆盖内容
log "hdc file send -> $DST"
T0=$(date +%s)
h file send "$SRC" "$DST" 2>&1 | tail -3 | while IFS= read -r l; do log "  send: $l"; done
T1=$(date +%s); log "send wall=$((T1-T0))s"

# 4) 校验：只认字节数（+ 设备侧 md5sum，有就追加核对）
GOT=$(sz "$DST"); GOT=${GOT:-0}
DM5=$(h shell "md5sum $DST 2>/dev/null" | tr -d '\r' | awk '{print $1}')
log "verify: size=$GOT expect=$EXPECT match=$([ "$GOT" = "$EXPECT" ] && echo YES || echo NO) dev_md5=${DM5:-na} src_md5=$SRC_MD5 md5_match=$([ "$DM5" = "$SRC_MD5" ] && echo YES || echo NA)"
h shell "ls -l $DST" | tr -d '\r' | while IFS= read -r l; do log "  ls: $l"; done
h shell "df -h /data | tail -1" | tr -d '\r' | while IFS= read -r l; do log "  df: $l"; done

[ "$GOT" = "$EXPECT" ] && { log "RESULT: OK"; exit 0; } || { log "RESULT: FAIL (size mismatch)"; exit 2; }
