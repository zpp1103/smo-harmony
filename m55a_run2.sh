#!/bin/bash
# M55A run2（回退验收）：所有开关归 0（smo_m55a=0 / smo_m45a=0 / smo_m47a=0 = 旧链），
# DM_VK2_SELFTEST=1 让旧链自测写 nvn_vk2_tri_1.ppm，与历史基准逐字节比对：
#   期望 md5 = 4ed86a580bb785e49c4a4990c1f8aed6（M46A/M47A/M48A/M49A/M50A 回退轮相同），max rgb error = 0。
# usage: bash m55a_run2.sh [dur]
set -uo pipefail
DUR="${1:-50}"
HDC="/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc"
TGT="127.0.0.1:5555"; BUNDLE="com.vibecc.smo"
ROOT="/Users/zpp/VibeCoding/switch2hos"
OUT="$ROOT/logs/m55a/m55a_r2_rollback"
FILES="/data/app/el2/100/base/com.vibecc.smo/files"
mkdir -p "$OUT"
log(){ printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*" | tee -a "$OUT/run.log"; }
h(){ "$HDC" -t "$TGT" "$@"; }
knob(){ h shell "echo $2 > $FILES/$1" >/dev/null 2>&1; G=$(h shell "cat $FILES/$1 2>/dev/null" | tr -d '\r\n '); [ "$G" = "$2" ] && log "knob $1=$2 ok" || log "X knob $1=$2 readback=[$G]"; }

: > "$OUT/run.log"
log "=== M55A run2 rollback: all switches 0, old chain selftest"
h shell "echo ok" >/dev/null 2>&1 || { log "X target offline"; exit 1; }
for i in 1 2 3; do P=$(h shell "pidof $BUNDLE" | tr -d '[:space:]'); [ -z "$P" ] && break; log "force-stop pid=$P"; h shell "aa force-stop $BUNDLE" >/dev/null 2>&1; sleep 3; done
knob smo_m55a 0; knob smo_m45a 0; knob smo_m47a 0; knob smo_m47a_save 0
h shell "rm -f $FILES/nvn_vk2_tri_*.ppm" >/dev/null 2>&1
h shell "echo 1 > $FILES/DM_VK2_SELFTEST" >/dev/null 2>&1
log "DM_VK2_SELFTEST=[$(h shell "cat $FILES/DM_VK2_SELFTEST" | tr -d '\r\n ')]"
h shell "power-shell wakeup" >/dev/null 2>&1
h shell "aa start -a EntryAbility -b $BUNDLE" >/dev/null 2>&1
log "app start (selftest=1, old chain)"
P=""; START=$(date +%s)
while [ $(( $(date +%s) - START )) -lt "$DUR" ]; do
  P=$(h shell "pidof $BUNDLE" | tr -d '[:space:]')
  [ -n "$P" ] && break
  sleep 2
done
log "pid=${P:-none} at $(( $(date +%s) - START ))s"
sleep 8
P2=$(h shell "pidof $BUNDLE" | tr -d '[:space:]'); log "pid_after=${P2:-<dead>}"
h shell "ls -l $FILES/nvn_vk2_tri_*.ppm 2>/dev/null" | tr -d '\r' | while read -r l; do log "  $l"; done
h file recv "$FILES/nvn_vk2_tri_1.ppm" "$OUT/nvn_vk2_tri_1.ppm" >/dev/null 2>&1 && log "ppm $(wc -c < "$OUT/nvn_vk2_tri_1.ppm" | tr -d ' ')B md5=$(md5 -q "$OUT/nvn_vk2_tri_1.ppm")"
h shell "echo 0 > $FILES/DM_VK2_SELFTEST" >/dev/null 2>&1
[ -f "$OUT/nvn_vk2_tri_1.ppm" ] && { python3 "$ROOT/tools/m42a_ppm_check.py" "$OUT/nvn_vk2_tri_1.ppm" 2>&1 | tee -a "$OUT/run.log" | tail -6; }
log "done"
