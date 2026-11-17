#!/bin/bash
# M50A / run3 抢救驱动（run3 主循环因 bash 3.2 不支持 declare -A 而退出，但 app 仍在跑）
# 以「进程启动时刻 T0」为基准，补齐 W1/W2/收尾各步。bash 3.2 兼容。
set -uo pipefail
TAG=m50a_r3
T0="${1:?T0 epoch}"
HDC="/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc"
TGT=127.0.0.1:5555
BUNDLE=com.vibecc.smo
OUT="/Users/zpp/VibeCoding/switch2hos/logs/m50a/$TAG"
FILES="/data/app/el2/100/base/com.vibecc.smo/files"
mkdir -p "$OUT/screens"
log(){ printf '[%s t=%s] %s\n' "$(date '+%H:%M:%S')" "$(( $(date +%s) - T0 ))" "$*" | tee -a "$OUT/driver.log"; }
h(){ "$HDC" -t "$TGT" "$@"; }
shot(){ h shell "rm -f /data/local/tmp/${TAG}_$1.jpeg; snapshot_display -f /data/local/tmp/${TAG}_$1.jpeg" >/dev/null 2>&1
        h file recv "/data/local/tmp/${TAG}_$1.jpeg" "$OUT/screens/t$1.jpeg" >/dev/null 2>&1; }
knob(){ h shell "echo $2 > $FILES/$1" >/dev/null 2>&1; G=$(h shell "cat $FILES/$1 2>/dev/null" | tr -d '\r\n '); [ "$G" = "$2" ] && log "knob $1=$2 ok" || log "X knob $1=$2 readback=[$G]"; }
at(){ local target=$1; shift; local now=$(( $(date +%s) - T0 ))
     [ "$now" -lt "$target" ] && sleep $(( target - now )); "$@"; }
at_t(){ local target=$1; local now=$(( $(date +%s) - T0 )); [ "$now" -lt "$target" ] && sleep $(( target - now )); log "t=$(( $(date +%s) - T0 )) mark"; }

log "driver start (T0=$T0)"
at_t 84
knob smo_m50a_ubo_pass 1; knob smo_m47a_save 8; shot 85
at_t 88; knob smo_m48a_rbpool f:0x87; shot 89
at_t 93; knob smo_m48a_rbpool f:0x49; shot 94
at_t 99; knob smo_m48a_rbpool 0; shot 100
at_t 106; knob smo_m50a_ubo_pass 0; knob smo_m47a_save 0; shot 106
at_t 176; knob smo_m50a_ubo_pass 1; knob smo_m47a_save 8; shot 178
at_t 183; shot 183
at_t 187; shot 187
at_t 208; knob smo_m50a_ubo_pass 0; knob smo_m47a_save 0; shot 208
at_t 216; knob smo_m50a_trace_pass 0; shot 216
at_t 254; shot 255
at_t 260; log "dump window done (dump_at=252)"; shot 260
at_t 330; shot 330
log "driver done (app still running; 收尾由调用方做)"
