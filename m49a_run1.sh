#!/bin/bash
# M49A / run1：**呈现侧判别实验** —— 同一帧，nx2 passdump 读法 vs 我方 S4-lite present 读法
#   · t≈50 起 trace pass 0（地图层 10-bit RT 的逐 draw 清单）
#   · t≈80 起 smo_m47a_save=6（把 present 读法转出的 RGBA8 逐帧存 PPM）
#   · t≈90 m45a ticker 自动 DM_VK_PASSDUMP=1 → 同帧全 pass dump（nx2 读法）
# 只碰 127.0.0.1:5555；不 install（沿用已装包）；不 commit
set -uo pipefail
TAG="${1:-m49a_r1}"; DUR="${2:-150}"
HDC="/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc"
TGT="127.0.0.1:5555"; BUNDLE="com.vibecc.smo"
ROOT="/Users/zpp/VibeCoding/switch2hos"
OUT="$ROOT/logs/m49a/$TAG"
FILES="/data/app/el2/100/base/com.vibecc.smo/files"
LOG="$FILES/vk_boot.log"
mkdir -p "$OUT/screens"
log(){ printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*" | tee -a "$OUT/run.log"; }
h(){ "$HDC" -t "$TGT" "$@"; }
shot(){ h shell "rm -f /data/local/tmp/${TAG}_$1.jpeg; snapshot_display -f /data/local/tmp/${TAG}_$1.jpeg" >/dev/null 2>&1
        h file recv "/data/local/tmp/${TAG}_$1.jpeg" "$OUT/screens/t$1.jpeg" >/dev/null 2>&1; }
knob(){ h shell "echo $2 > $FILES/$1" >/dev/null 2>&1; G=$(h shell "cat $FILES/$1 2>/dev/null" | tr -d '\r\n '); [ "$G" = "$2" ] && log "knob $1=$2 ok" || log "X knob $1=$2 readback=[$G]"; }

log "=== M49A run1 tag=$TAG dur=${DUR}s out=$OUT"
h shell "echo ok" >/dev/null 2>&1 || { log "X target offline"; exit 1; }
for i in 1 2 3; do P=$(h shell "pidof $BUNDLE" | tr -d '[:space:]'); [ -z "$P" ] && break; log "force-stop pid=$P"; h shell "aa force-stop $BUNDLE" >/dev/null 2>&1; sleep 3; done

# 设备 shell 的 glob 不展开：逐文件删（M48A 踩过）
log "clean dump dir (per-file rm)"
FILES_LIST=$(h shell "ls $FILES/m45a_dump 2>/dev/null" | tr -d '\r')
for f in $FILES_LIST; do
  case "$f" in
    pass_*.ppm|pass_*.f32|m47a_pres_*.ppm) h shell "rm -f $FILES/m45a_dump/$f" >/dev/null 2>&1;;
  esac
done
log "left after clean: $(h shell "ls $FILES/m45a_dump 2>/dev/null" | tr -d '\r' | tr '\n' ' ' | cut -c1-300)"

knob smo_m45a 1; knob smo_m47a 1; knob smo_m47a_save 0; knob smo_m47a_trace 0
knob smo_m48a_passdump 0; knob smo_m48a_rb10 0
h shell ": > $LOG" >/dev/null 2>&1
h shell "hilog -r" >/dev/null 2>&1
CF0=$(h shell "ls /data/log/faultlog/faultlogger/ 2>/dev/null" | tr -d '\r')

h shell "power-shell wakeup" >/dev/null 2>&1
h shell "power-shell timeout -o 1800000" >/dev/null 2>&1
h shell "aa start -a EntryAbility -b $BUNDLE" >/dev/null 2>&1
START=$(date +%s); FIRST=""; DEAD=""; LASTP=""; PEAK=0
DID_TRACE=0; DID_SAVE=0; NEXT_PROG=60; SHOT90=0
while :; do
  EL=$(( $(date +%s) - START ))
  P=$(h shell "pidof $BUNDLE" | tr -d '[:space:]')
  if [ -z "$P" ]; then
    if [ -z "$FIRST" ] && [ "$EL" -lt 60 ]; then sleep 2; continue; fi
    DEAD=$EL; log "t=${EL}s pid=<dead> last=$LASTP"; break
  fi
  [ -z "$FIRST" ] && { FIRST="$P"; log "t=${EL}s pid=$P"; }
  LASTP="$P"
  RSSKB=$(h shell "grep VmRSS /proc/$P/status 2>/dev/null" | tr -dc '0-9'); RSSKB=${RSSKB:-0}
  [ "$RSSKB" -gt "$PEAK" ] && PEAK=$RSSKB
  if [ "$DID_TRACE" = 0 ] && [ "$EL" -ge 50 ]; then DID_TRACE=1; knob smo_m47a_trace 1; fi
  if [ "$DID_SAVE" = 0 ] && [ "$EL" -ge 80 ]; then DID_SAVE=1; knob smo_m47a_save 6; fi
  if [ "$SHOT90" = 0 ] && [ "$EL" -ge 95 ]; then SHOT90=1; log "shot t95"; shot 95; fi
  if [ "$EL" -ge "$NEXT_PROG" ]; then
    NEXT_PROG=$(( NEXT_PROG + 60 ))
    SNAP=$(h shell "tail -c 3000 $LOG 2>/dev/null | grep -a -E '\[m47a\] pres|pass dump|\[m45a\] RT dump' | tail -2" | tr -d '\r' | cut -c1-200)
    log "PROGRESS t=${EL}s rss=$((RSSKB/1024))MiB :: $SNAP"
    shot "$EL"
  fi
  [ "$EL" -ge "$DUR" ] && { log "t=${EL}s reached duration"; break; }
  sleep 3
done
END=$(date +%s); ALIVE=$(( END - START )); [ -n "$DEAD" ] && ALIVE=$DEAD
log "alive=${ALIVE}s dead=$([ -n "$DEAD" ] && echo 1 || echo 0) peak_rss=$((PEAK/1024))MiB"
shot final
NEW=0
for cf in $(h shell "ls /data/log/faultlog/faultlogger/ 2>/dev/null" | tr -d '\r'); do
  grep -qxF "$cf" <<<"$CF0" && continue; NEW=$((NEW+1)); log "!! NEW faultlog $cf"
  h file recv "/data/log/faultlog/faultlogger/$cf" "$OUT/$cf" >/dev/null 2>&1
done
log "new_faultlog_count=$NEW"
h file recv "$LOG" "$OUT/vk_boot.log" >/dev/null 2>&1 && log "vk_boot.log $(wc -c < "$OUT/vk_boot.log")B"
# 拉 pass dump 与 present PPM（逐文件）
NP=0
for f in $(h shell "ls $FILES/m45a_dump 2>/dev/null" | tr -d '\r'); do
  case "$f" in
    pass_*.ppm|m47a_pres_*.ppm)
      case "$f" in pass_*.f32) continue;; esac
      B=$(h shell "stat -c %s $FILES/m45a_dump/$f 2>/dev/null" | tr -dc '0-9'); B=${B:-0}
      [ "$B" -lt 100 ] && continue
      h file recv "$FILES/m45a_dump/$f" "$OUT/$(basename "$f")" >/dev/null 2>&1
      NP=$((NP+1));;
  esac
done
log "dumps pulled=$NP"
{ echo "alive=$ALIVE dead=$([ -n "$DEAD" ] && echo 1 || echo 0) peak_rss=$((PEAK/1024))MiB new_faultlog=$NEW";
  echo "--- [m47a] ---"; grep -a '\[m47a\]' "$OUT/vk_boot.log" 2>/dev/null | tail -20 | tr -d '\r';
  echo "--- pass dump map ---"; grep -a 'pass dump' "$OUT/vk_boot.log" 2>/dev/null | tr -d '\r' | head -40;
} > "$OUT/crash_summary.txt"
log "done"
