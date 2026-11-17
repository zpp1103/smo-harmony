#!/bin/bash
# M55A run1（主线测量）：smo_m55a=1 打开别名判定探针（[m55a-img]/[m55a-pass]/[m55a-dump]/[m55a-stat]/[m55a-skip]），
#   smo_m50a_watch=228600#40 看 pass 50 采样到的到底是哪个 image（handle/pool+off），
#   smo_m45a=1 + smo_m47a=1 让它走 nx2 回放链（探针挂在 exec 的 pass_dump 上）。
# usage: bash m55a_run1.sh [tag] [dur]
set -uo pipefail
TAG="${1:-m55a_r1}"; DUR="${2:-340}"
HDC="/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc"
TGT="127.0.0.1:5555"; BUNDLE="com.vibecc.smo"
ROOT="/Users/zpp/VibeCoding/switch2hos"
OUT="$ROOT/logs/m55a/$TAG"
FILES="/data/app/el2/100/base/com.vibecc.smo/files"
LOG="$FILES/vk_boot.log"
mkdir -p "$OUT/screens"
log(){ printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*" | tee -a "$OUT/run.log"; }
h(){ "$HDC" -t "$TGT" "$@"; }
shot(){ h shell "rm -f /data/local/tmp/${TAG}_$1.jpeg; snapshot_display -f /data/local/tmp/${TAG}_$1.jpeg" >/dev/null 2>&1
        h file recv "/data/local/tmp/${TAG}_$1.jpeg" "$OUT/screens/t$1.jpeg" >/dev/null 2>&1; }
knob(){ h shell "echo $2 > $FILES/$1" >/dev/null 2>&1; G=$(h shell "cat $FILES/$1 2>/dev/null" | tr -d '\r\n '); [ "$G" = "$2" ] && log "knob $1=$2 ok" || log "X knob $1=$2 readback=[$G]"; }

: > "$OUT/run.log"
log "=== M55A run1 tag=$TAG dur=${DUR}s out=$OUT"
h shell "echo ok" >/dev/null 2>&1 || { log "X target offline"; exit 1; }
log "emu_rss_before=$(ps aux | grep 'tools/emulator/Emulator' | grep -v grep | awk '{print int($6/1024)"MiB"}' | head -1)"
for i in 1 2 3; do P=$(h shell "pidof $BUNDLE" | tr -d '[:space:]'); [ -z "$P" ] && break; log "force-stop pid=$P"; h shell "aa force-stop $BUNDLE" >/dev/null 2>&1; sleep 3; done

knob smo_m45a 1; knob smo_m47a 1; knob smo_m47a_save 0; knob smo_m47a_trace 0
knob smo_m48a 0; knob smo_m48a_passdump 0; knob smo_m48a_rb10 0; knob smo_m48a_rbpool 0; knob smo_m48a_fmt10 0; knob smo_m48a_srgb 0
knob smo_m45a_dump_at 99999; knob smo_m45a_dump_frames 1
knob smo_m50a_pdskip 0; knob smo_m50a_trace_pass 0; knob smo_m50a_ubo_pass 0; knob smo_m50a_sh_pass 0; knob smo_m50a_pddepth 0
knob smo_m50a_watch "228600#40"
knob smo_m55a 1

# 清掉旧的 passdump / m55a dump，免得混淆
for f in $(h shell "ls $FILES/m45a_dump/ 2>/dev/null" | tr -d '\r'); do
  case "$f" in pass_*.ppm|pass_*.f32|m55a_*.ppm) h shell "rm -f $FILES/m45a_dump/$f" >/dev/null 2>&1;; esac
done
h shell ": > $LOG" >/dev/null 2>&1
h shell "hilog -r" >/dev/null 2>&1
CF0=$(h shell "ls /data/log/faultlog/faultlogger/ 2>/dev/null" | tr -d '\r')
h shell "power-shell wakeup" >/dev/null 2>&1
h shell "power-shell timeout -o 1800000" >/dev/null 2>&1
h shell "aa start -a EntryAbility -b $BUNDLE" >/dev/null 2>&1
log "app start"
START=$(date +%s); FIRST=""; DEAD=""; LASTP=""; PEAK=0
while :; do
  EL=$(( $(date +%s) - START ))
  P=$(h shell "pidof $BUNDLE" | tr -d '[:space:]')
  if [ -z "$P" ]; then
    if [ -z "$FIRST" ] && [ "$EL" -lt 60 ]; then sleep 2; continue; fi
    DEAD=$EL; log "t=${EL}s pid=<dead> last=$LASTP"; break
  fi
  [ -z "$FIRST" ] && { FIRST=$P; log "t=${EL}s pid=$P"; }
  LASTP="$P"
  RSSKB=$(h shell "grep VmRSS /proc/$P/status 2>/dev/null" | tr -dc '0-9'); RSSKB=${RSSKB:-0}
  [ "$RSSKB" -gt "$PEAK" ] && PEAK=$RSSKB
  case "$EL" in
    45) shot 45;;
    150) shot 150;;
  esac
  [ "$EL" -ge "$DUR" ] && { log "t=${EL}s dur reached, app alive"; break; }
  sleep 5
done
RSSMB=$(( PEAK / 1024 ))
log "peak_app_rss=${RSSMB}MiB first=$FIRST last=$LASTP died_at=${DEAD:-alive}"
shot end

# 证据：vk_boot.log 里的 m55a / watch / skip 行
h shell "grep -c 'm55a-' $LOG 2>/dev/null" | tr -d '\r' | while read -r n; do log "m55a_lines=$n"; done
for k in m55a-skip m55a-pass m55a-dump m55a-stat m55a-img; do
  n=$(h shell "grep -c '$k' $LOG 2>/dev/null" | tr -dc '0-9'); log "count $k = ${n:-0}"
done
h shell "grep -n 'm55a-' $LOG 2>/dev/null | head -400" | tr -d '\r' > "$OUT/m55a_lines.txt"
h shell "grep -n 'watch:' $LOG 2>/dev/null | head -200" | tr -d '\r' > "$OUT/watch.txt"
h shell "grep -n 'stale read' $LOG 2>/dev/null | head -60" | tr -d '\r' > "$OUT/alias.txt"
h shell "grep -n 'skip_prog\|\[m45a\].*exec\|pass dump' $LOG 2>/dev/null | head -60" | tr -d '\r' > "$OUT/exec_head.txt"
h shell "ls -l $FILES/m45a_dump/ 2>/dev/null" | tr -d '\r' > "$OUT/dump_listing.txt"
h shell "cp $LOG /data/local/tmp/${TAG}_vk_boot.log 2>/dev/null; chmod 644 /data/local/tmp/${TAG}_vk_boot.log 2>/dev/null" >/dev/null 2>&1
h file recv "/data/local/tmp/${TAG}_vk_boot.log" "$OUT/vk_boot.log" >/dev/null 2>&1 && log "vk_boot.log $(wc -c < "$OUT/vk_boot.log" | tr -d ' ')B"
for f in $(h shell "ls $FILES/m45a_dump/m55a_*.ppm 2>/dev/null" | tr -d '\r' | head -6); do
  b=$(basename "$f"); h file recv "$f" "$OUT/screens/$b" >/dev/null 2>&1 && log "ppm $b $(wc -c < "$OUT/screens/$b" | tr -d ' ')B"
done
CF1=$(h shell "ls /data/log/faultlog/faultlogger/ 2>/dev/null" | tr -d '\r')
NEW=$(comm -13 <(echo "$CF0" | sort) <(echo "$CF1" | sort) | grep -c . )
log "new_faultlog=$NEW"
log "emu_rss_after=$(ps aux | grep 'tools/emulator/Emulator' | grep -v grep | awk '{print int($6/1024)"MiB"}' | head -1)"
log "done"
