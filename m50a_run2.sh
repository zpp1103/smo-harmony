#!/bin/bash
# M50A / run2（目标 2）：世界地图层红源 —— pass 0 两帧（红帧 vs 金色帧）UBO/顶点常量 diff
#   · smo_m50a_trace_pass=1   全程 trace 引擎 pass 0（逐 draw：纹理身份/混合/stream0 顶点前 4 个 word）
#   · smo_m50a_ubo_pass=1     只在两个窗口打开（W1 红帧 / W2 金帧），每次 600 行 UBO 逐 word float
#   · smo_m47a_save=8         两个窗口各存 8 帧 present PPM（做屏幕级数值 before/after）
#   · smo_m48a_rbpool         W1 里做纹理级 A/B：f:0x87 → f:0x49（每次 8 s，截图）
# usage: bash m50a_run2.sh [tag] [dur]
set -uo pipefail
TAG="${1:-m50a_r2}"; DUR="${2:-230}"
HDC="/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc"
TGT="127.0.0.1:5555"; BUNDLE="com.vibecc.smo"
ROOT="/Users/zpp/VibeCoding/switch2hos"
OUT="$ROOT/logs/m50a/$TAG"
FILES="/data/app/el2/100/base/com.vibecc.smo/files"
LOG="$FILES/vk_boot.log"
mkdir -p "$OUT/screens"
log(){ printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*" | tee -a "$OUT/run.log"; }
h(){ "$HDC" -t "$TGT" "$@"; }
shot(){ h shell "rm -f /data/local/tmp/${TAG}_$1.jpeg; snapshot_display -f /data/local/tmp/${TAG}_$1.jpeg" >/dev/null 2>&1
        h file recv "/data/local/tmp/${TAG}_$1.jpeg" "$OUT/screens/t$1.jpeg" >/dev/null 2>&1; }
knob(){ h shell "echo $2 > $FILES/$1" >/dev/null 2>&1; G=$(h shell "cat $FILES/$1 2>/dev/null" | tr -d '\r\n '); [ "$G" = "$2" ] && log "knob $1=$2 ok" || log "X knob $1=$2 readback=[$G]"; }

log "=== M50A run2 tag=$TAG dur=${DUR}s out=$OUT"
h shell "echo ok" >/dev/null 2>&1 || { log "X target offline"; exit 1; }
for i in 1 2 3; do P=$(h shell "pidof $BUNDLE" | tr -d '[:space:]'); [ -z "$P" ] && break; log "force-stop pid=$P"; h shell "aa force-stop $BUNDLE" >/dev/null 2>&1; sleep 3; done
log "clean dump dir (per-file rm)"
for f in $(h shell "ls $FILES/m45a_dump 2>/dev/null" | tr -d '\r'); do
  case "$f" in pass_*.ppm|pass_*.f32|m47a_pres_*.ppm) h shell "rm -f $FILES/m45a_dump/$f" >/dev/null 2>&1;; esac
done
log "left after clean: $(h shell "ls $FILES/m45a_dump 2>/dev/null" | tr -d '\r' | tr '\n' ' ' | cut -c1-200)"

knob smo_m45a 1; knob smo_m47a 1; knob smo_m47a_save 0; knob smo_m47a_trace 0
knob smo_m48a_passdump 0; knob smo_m48a_rb10 0; knob smo_m48a_rbpool 0
knob smo_m45a_dump_at 9999; knob smo_m45a_dump_frames 1
knob smo_m50a_pdskip 0; knob smo_m50a_trace_pass 1; knob smo_m50a_ubo_pass 0
h shell ": > $LOG" >/dev/null 2>&1
h shell "hilog -r" >/dev/null 2>&1
CF0=$(h shell "ls /data/log/faultlog/faultlogger/ 2>/dev/null" | tr -d '\r')

h shell "power-shell wakeup" >/dev/null 2>&1
h shell "power-shell timeout -o 1800000" >/dev/null 2>&1
h shell "aa start -a EntryAbility -b $BUNDLE" >/dev/null 2>&1
START=$(date +%s); FIRST=""; DEAD=""; LASTP=""; PEAK=0
NEXT_PROG=60
DID=" "
once(){ case "$DID" in *" $1 "*) return 0;; esac; DID="$DID$1 "; "$@"; }
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
  # W1 红帧窗口 (72..102 s)
  if [ "$EL" -ge 72 ] && [ -z "${W1O:-}" ]; then W1O=1; log "W1 open"; knob smo_m50a_ubo_pass 1; knob smo_m47a_save 8; fi
  [ "$EL" -ge 74 ] && once shot 74
  [ "$EL" -ge 78 ] && once shot 78
  if [ "$EL" -ge 80 ] && [ -z "${RB87:-}" ]; then RB87=1; knob smo_m48a_rbpool f:0x87; fi
  [ "$EL" -ge 84 ] && once shot 84
  if [ "$EL" -ge 90 ] && [ -z "${RB49:-}" ]; then RB49=1; knob smo_m48a_rbpool f:0x49; fi
  [ "$EL" -ge 94 ] && once shot 94
  if [ "$EL" -ge 100 ] && [ -z "${RB0:-}" ]; then RB0=1; knob smo_m48a_rbpool 0; fi
  if [ "$EL" -ge 104 ] && [ -z "${W1C:-}" ]; then W1C=1; log "W1 close"; knob smo_m50a_ubo_pass 0; knob smo_m47a_save 0; fi
  # W2 金帧窗口 (176..208 s)
  if [ "$EL" -ge 176 ] && [ -z "${W2O:-}" ]; then W2O=1; log "W2 open"; knob smo_m50a_ubo_pass 1; knob smo_m47a_save 8; fi
  [ "$EL" -ge 178 ] && once shot 178
  [ "$EL" -ge 182 ] && once shot 182
  [ "$EL" -ge 186 ] && once shot 186
  if [ "$EL" -ge 208 ] && [ -z "${W2C:-}" ]; then W2C=1; log "W2 close"; knob smo_m50a_ubo_pass 0; knob smo_m47a_save 0; fi
  if [ "$EL" -ge "$NEXT_PROG" ]; then
    NEXT_PROG=$(( NEXT_PROG + 60 ))
    SNAP=$(h shell "tail -c 3000 $LOG 2>/dev/null | grep -a -E '\[m50a\]|traced pass|ubo:' | tail -3" | tr -d '\r' | cut -c1-200)
    log "PROGRESS t=${EL}s rss=$((RSSKB/1024))MiB :: $SNAP"
    shot "$EL"
  fi
  [ "$EL" -ge "$DUR" ] && { log "t=${EL}s reached duration"; break; }
  sleep 2
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
  echo "--- [m50a] ---"; grep -a '\[m50a\]' "$OUT/vk_boot.log" 2>/dev/null | tail -20 | tr -d '\r';
  echo "--- traced pass 0 ---"; grep -a 'traced pass' "$OUT/vk_boot.log" 2>/dev/null | tr -d '\r' | head -30;
} > "$OUT/crash_summary.txt"
log "done"
