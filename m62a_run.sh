#!/bin/bash
# M62A 真机单次启动：按 M61A 同口径（无开关基线 + smo_m62a=0/1 A/B）
#   usage: bash m62a_run.sh <runId: r1|r2> <m62a: 0|1>
set -uo pipefail
ID="${1:?runId}"; M62A="${2:?m62a}"
HDC="/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc"
T=192.168.0.109:41901; PKG=com.vibecc.smo
F=/data/app/el2/100/base/com.vibecc.smo/files
LOG="$F/vk_boot.log"
OUT=/Users/zpp/VibeCoding/switch2hos/logs/m62a
RL="$OUT/run_${ID}.log"; mkdir -p "$OUT" /tmp/m62a_shots
: > "$RL"
h(){ "$HDC" -t "$T" "$@"; }
log(){ printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*" | tee -a "$RL"; }
knob(){ h shell "echo $2 > $F/$1" >/dev/null 2>&1; G=$(h shell "cat $F/$1 2>/dev/null" | tr -d '\r\n '); [ "$G" = "$2" ] && log "knob $1=$2 ok" || log "X knob $1=$2 readback=[$G]"; }
shot(){ # $1=tag
  TS=$(date +%H:%M:%S); REM=/data/local/tmp/m62a_${ID}_$1.jpeg
  h shell "snapshot_display -f $REM" >/dev/null 2>&1
  h file recv "$REM" "$OUT/m62a_${ID}_$1.jpeg" >/dev/null 2>&1
  h shell "rm -f $REM" >/dev/null 2>&1
  [ -s "$OUT/m62a_${ID}_$1.jpeg" ] || { log "X shot $1 failed"; return; }
  python3 -c "
from PIL import Image
im=Image.open('$OUT/m62a_${ID}_$1.jpeg').convert('RGB'); im.save('/tmp/m62a_shots/${ID}_$1.ppm')" 2>/dev/null
  log "--- shot $1 ($(stat -f%z "$OUT/m62a_${ID}_$1.jpeg")B)"
  python3 /Users/zpp/VibeCoding/switch2hos/logs/m58c/m58c_stats.py "/tmp/m62a_shots/${ID}_$1.ppm" 2>&1 | tee -a "$RL"
}

log "=== M62A run $ID : smo_m62a=$M62A (baseline = 全部 smo_*/DM_* 关)"
h shell "echo ok" >/dev/null 2>&1 || { log "X device offline"; exit 1; }
for i in 1 2 3; do P=$(h shell "pidof $PKG" | tr -d '[:space:]'); [ -z "$P" ] && break; log "pre-stop pid=$P"; h shell "aa force-stop $PKG" >/dev/null 2>&1; sleep 3; done
# 开关：与 M61A 基线一致（全 0），只把 smo_m62a 设成本次值
for k in smo_m45a smo_m47a smo_m48a smo_m50a_trace_pass smo_m55a smo_m56a smo_m57a smo_m59a smo_m14b; do knob $k 0; done
knob smo_m62a "$M62A"
h shell "rm -f $F/m45a_dump/pass_* 2>/dev/null" >/dev/null 2>&1
h shell ": > $LOG" >/dev/null 2>&1
h shell "hilog -r" >/dev/null 2>&1
h shell "power-shell wakeup" >/dev/null 2>&1
h shell "power-shell timeout -o 1800000" >/dev/null 2>&1
h shell "aa start -a EntryAbility -b $PKG" >/dev/null 2>&1
S=$(date +%s); log "app start (epoch=$S)"
P0=""; for i in $(seq 1 20); do P0=$(h shell "pidof $PKG" | tr -d '[:space:]'); [ -n "$P0" ] && break; sleep 1; done
log "pid=$P0"
DID_SHOT60=0; DID_SHOT135=0; DID_EN1=0; DID_CL1=0; DID_SH470=0; DID_EN2=0; DID_S508=0; DID_S517=0; LASTPID=0
while :; do
  EL=$(( $(date +%s) - S ))
  if [ "$EL" -le 6 ] && [ "$DID_SHOT60" = 0 ] && [ "$DID_EN1" = 0 ] && [ "$EL" -ge 5 ]; then shot s05; DID_SHOT60=2; fi
  if [ "$DID_SHOT60" = 2 ] && [ "$EL" -ge 60 ]; then shot s60; DID_SHOT60=1; fi
  if [ "$DID_EN1" = 0 ] && [ "$EL" -ge 74 ]; then
    h shell "uitest uiInput keyEvent 2054" 2>&1 | tr -d '\r' | sed 's/^/  in: /' | tee -a "$RL"; DID_EN1=$EL; fi
  if [ "$DID_EN1" != 0 ] && [ "$DID_CL1" = 0 ] && [ $(( EL - DID_EN1 )) -ge 8 ]; then shot s82; DID_CL1=1; fi
  if [ "$DID_CL1" = 1 ] && [ "$DID_SHOT135" = 0 ] && [ "$EL" -ge 133 ]; then
    h shell "uitest uiInput click 1424 660" 2>&1 | tr -d '\r' | sed 's/^/  in: /' | tee -a "$RL"; DID_SHOT135=$EL; fi
  if [ "$DID_SHOT135" != 0 ] && [ "$DID_SH470" = 0 ] && [ $(( EL - DID_SHOT135 )) -ge 8 ]; then shot s141; DID_SH470=1; fi
  if [ "$DID_SH470" = 1 ] && [ "$DID_EN2" = 0 ] && [ "$EL" -ge 480 ]; then shot s480; DID_EN2=1; fi
  if [ "$DID_EN2" = 1 ] && [ "$DID_S508" = 0 ] && [ "$EL" -ge 500 ]; then
    h shell "uitest uiInput keyEvent 2054" 2>&1 | tr -d '\r' | sed 's/^/  in: /' | tee -a "$RL"; DID_S508=$EL; fi
  if [ "$DID_S508" != 0 ] && [ "$DID_S517" = 0 ] && [ $(( EL - DID_S508 )) -ge 8 ]; then shot s508; DID_S517=1; fi
  if [ "$DID_S517" = 1 ] && [ $(( EL - DID_S508 )) -ge 17 ]; then shot s517; DID_S517=2; fi
  if [ $(( EL % 30 )) -eq 0 ] && [ "$EL" -ne "$LASTPID" ]; then
    P=$(h shell "pidof $PKG" | tr -d '[:space:]'); log "t=${EL}s pid=${P:-<dead>}"; LASTPID=$EL; fi
  [ "$EL" -ge 540 ] && { shot s540; break; }
  sleep 2
done
ALIVE=$(( $(date +%s) - S )); log "alive=${ALIVE}s"
# 三条未验证项 + m62a 取证行
h shell "grep -a 'm62a\]' $LOG | head -8" | tr -d '\r' | sed 's/^/  m62a| /' | tee -a "$RL"
h shell "grep -a -c 'graphics pipeline failed' $LOG 2>/dev/null" | tr -d '\r' | sed 's/^/  LOG pipeline_failed_count=/' | tee -a "$RL"
h shell "grep -a 'graphics pipeline failed' $LOG | tail -3" | tr -d '\r' | sed 's/^/  LOG pipe| /' | tee -a "$RL"
h shell "grep -a 'spin\] kill timeout' $LOG | tail -3" | tr -d '\r' | sed 's/^/  LOG spin| /' | tee -a "$RL"
h shell "grep -a 'm47a\] pres' $LOG | tail -3" | tr -d '\r' | sed 's/^/  LOG pres| /' | tee -a "$RL"
h shell "grep -a 'proc nvnDepthStencilStateSetDefaults\|proc nvnPolygonStateSetDefaults\|proc nvnChannelMaskStateSetDefaults\|proc nvnColorStateSetLogicOp' $LOG | head -6" | tr -d '\r' | sed 's/^/  proc| /' | tee -a "$RL"
h shell "grep -a -c 'stub#' $LOG 2>/dev/null" | tr -d '\r' | sed 's/^/  stub_lines=/' | tee -a "$RL"
h shell "tail -c 4000 $LOG | grep -a -E 'nvnStub|distinct|pres|beat' | tail -6" | tr -d '\r' | sed 's/^/  tail| /' | tee -a "$RL"
h shell "hilog" 2>/dev/null | tr -d '\r' | grep -a -E "graphics pipeline failed|kill timeout|SigBlk|\[m47a\] pres" | tail -40 > "$OUT/hilog_${ID}.txt"
log "hilog grep lines=$(wc -l < "$OUT/hilog_${ID}.txt")"
h shell "cp $LOG /data/local/tmp/m62a_${ID}_boot.log; chmod 644 /data/local/tmp/m62a_${ID}_boot.log" >/dev/null 2>&1
h file recv "/data/local/tmp/m62a_${ID}_boot.log" "$OUT/vk_boot_${ID}.log" >/dev/null 2>&1
log "vk_boot_${ID}.log bytes=$(stat -f%z "$OUT/vk_boot_${ID}.log" 2>/dev/null)"
h shell "aa force-stop $PKG" >/dev/null 2>&1
log "force-stop done; knob m62a readback=$(h shell "cat $F/smo_m62a" | tr -d '\r\n ')"
log "=== run $ID done"
