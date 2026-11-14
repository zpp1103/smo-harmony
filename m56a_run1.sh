#!/bin/bash
# M56A run1（before 基线 + 两个廉价确认）：smo_m56a=2（只开探针，不开重定向）
#   → [m56a-dpass]（被采样深度图那个 pass 的 loadOp/clearDepth/clearStencil/draws）
#   → [m56a-copy]（有没有 GPU 写入落进 0x228600..0x628600）
#   → [m56a-img]（1024² 深度图 + 谁和谁 pool 地址重叠）
#   两组 passdump：d1 = 从 pass 0 起（级联深度，含 228600 的远平面 100%）/ d2 = 从 pass 40 起（pass 50 的 800x450 mask）
#   m47a_save=2 存 presented 全屏 PPM 给整屏 nearwhite/mean；另存 jpeg 截图给人眼。
# usage: bash m56a_run1.sh [tag] [dur]
set -uo pipefail
TAG="${1:-m56a_r1}"; DUR="${2:-300}"
HDC="/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc"
TGT="127.0.0.1:5555"; BUNDLE="com.vibecc.smo"
ROOT="/Users/zpp/VibeCoding/switch2hos"
OUT="$ROOT/logs/m56a/$TAG"
FILES="/data/app/el2/100/base/com.vibecc.smo/files"
LOG="$FILES/vk_boot.log"
mkdir -p "$OUT/screens" "$OUT/d1" "$OUT/d2" "$OUT/pres"
log(){ printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*" | tee -a "$OUT/run.log"; }
h(){ "$HDC" -t "$TGT" "$@"; }
shot(){ h shell "rm -f /data/local/tmp/${TAG}_$1.jpeg; snapshot_display -f /data/local/tmp/${TAG}_$1.jpeg" >/dev/null 2>&1
        h file recv "/data/local/tmp/${TAG}_$1.jpeg" "$OUT/screens/t$1.jpeg" >/dev/null 2>&1; }
knob(){ h shell "echo $2 > $FILES/$1" >/dev/null 2>&1; G=$(h shell "cat $FILES/$1 2>/dev/null" | tr -d '\r\n '); [ "$G" = "$2" ] && log "knob $1=$2 ok" || log "X knob $1=$2 readback=[$G]"; }
lst(){ h shell "ls $FILES/m45a_dump 2>/dev/null" | tr -d '\r'; }
cleandumps(){ for f in $(lst); do case "$f" in pass_*.ppm|pass_*.f32|m47a_pres_*.ppm) h shell "rm -f $FILES/m45a_dump/$f" >/dev/null 2>&1;; esac; done; }
pulldumps(){  # $1 = dest subdir, $2 = shell glob pattern
  local n=0
  for f in $(lst); do
    case "$f" in $2)
      B=$(h shell "stat -c %s $FILES/m45a_dump/$f 2>/dev/null" | tr -dc '0-9'); B=${B:-0}
      [ "$B" -lt 100 ] && continue
      h file recv "$FILES/m45a_dump/$f" "$OUT/$1/$(basename "$f")" >/dev/null 2>&1; n=$((n+1));;
    esac
  done
  log "pulled $n files -> $OUT/$1"
}

: > "$OUT/run.log"
log "=== M56A run1 tag=$TAG dur=${DUR}s out=$OUT"
h shell "echo ok" >/dev/null 2>&1 || { log "X target offline"; exit 1; }
log "emu_rss_before=$(ps aux | grep 'tools/emulator/Emulator' | grep -v grep | awk '{print int($6/1024)"MiB"}' | head -1)"
for i in 1 2 3; do P=$(h shell "pidof $BUNDLE" | tr -d '[:space:]'); [ -z "$P" ] && break; log "force-stop pid=$P"; h shell "aa force-stop $BUNDLE" >/dev/null 2>&1; sleep 3; done
# 0) 种子：新开关 smo_m56a 的文件由 app 构造期种（el2 沙箱里 hdc 只能覆盖不能新建）⇒ 缺就起一次 app 种完立刻停
if [ "$(h shell "test -f $FILES/smo_m56a && echo yes" | tr -d '\r\n ')" != "yes" ]; then
  log "smo_m56a missing -> start app once to seed knob files"
  h shell "power-shell wakeup" >/dev/null 2>&1
  h shell "aa start -a EntryAbility -b $BUNDLE" >/dev/null 2>&1
  for i in $(seq 1 20); do sleep 2; [ "$(h shell "test -f $FILES/smo_m56a && echo yes" | tr -d '\r\n ')" = "yes" ] && { log "smo_m56a seeded after $((i*2))s"; break; }; done
  h shell "aa force-stop $BUNDLE" >/dev/null 2>&1; sleep 3
fi
cleandumps
log "dump dir after clean: [$(lst | tr '\n' ' ' | cut -c1-160)]"

knob smo_m45a 1; knob smo_m47a 1; knob smo_m47a_save 0; knob smo_m47a_trace 0
knob smo_m48a 0; knob smo_m48a_passdump 0; knob smo_m48a_rb10 0; knob smo_m48a_rbpool 0; knob smo_m48a_srgb 0
knob smo_m45a_dump_at 99999; knob smo_m45a_dump_frames 1
knob smo_m50a_pdskip 0; knob smo_m50a_trace_pass 0; knob smo_m50a_ubo_pass 0
knob smo_m50a_sh_pass 0; knob smo_m50a_pddepth 0; knob smo_m50a_watch 0
knob smo_m55a 0
knob smo_m56a 2

h shell ": > $LOG" >/dev/null 2>&1
h shell "hilog -r" >/dev/null 2>&1
CF0=$(h shell "ls /data/log/faultlog/faultlogger/ 2>/dev/null" | tr -d '\r')
h shell "power-shell wakeup" >/dev/null 2>&1
h shell "power-shell timeout -o 1800000" >/dev/null 2>&1
h shell "aa start -a EntryAbility -b $BUNDLE" >/dev/null 2>&1
log "app start (m56a=2 probes only)"
START=$(date +%s); FIRST=""; DEAD=""; LASTP=""; PEAK=0; DID=" "; NEXT_PROG=60
once(){ case "$DID" in *" $1 "*) return 0;; esac; DID="$DID$1 "; "$@"; }
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
  # 进 gameplay 的判据：目标 offset 0x228600 在日志里出现（标题/载入场景的 1024² 深度偏移不是它）
  if [ -z "${T0:-}" ] && [ $((EL % 5)) -eq 0 ]; then
    if [ "$(h shell "grep -ac 228600 $LOG 2>/dev/null" | tr -dc '0-9')" != "0" ] && [ -n "$(h shell "grep -ac 228600 $LOG 2>/dev/null" | tr -dc '0-9')" ]; then T0=$EL; log "target 0x228600 first seen @${EL}s -> gameplay"; fi
  fi
  # passdump 窗口 A：从 pass 0 起（级联深度/远平面）
  if [ -n "${T0:-}" ] && [ "$EL" -ge $((T0+12)) ] && [ -z "${PA:-}" ]; then PA=1; log "passdump A (skip=0) armed @${EL}s"; knob smo_m50a_pdskip 0; knob smo_m48a_passdump 3; fi
  if [ -n "${PA:-}" ] && [ "$EL" -ge $((T0+24)) ] && [ -z "${PAC:-}" ]; then PAC=1; knob smo_m48a_passdump 0; pulldumps d1 'pass_*.ppm|pass_*.f32'; cleandumps; fi
  # passdump 窗口 B：skip=40 → 覆盖 pass 50 的 800x450 mask
  if [ -n "${PAC:-}" ] && [ "$EL" -ge $((T0+34)) ] && [ -z "${PB:-}" ]; then PB=1; log "passdump B (skip=40) armed @${EL}s"; knob smo_m50a_pdskip 40; knob smo_m48a_passdump 3; fi
  if [ -n "${PB:-}" ] && [ "$EL" -ge $((T0+46)) ] && [ -z "${PBC:-}" ]; then PBC=1; knob smo_m48a_passdump 0; knob smo_m50a_pdskip 0; pulldumps d2 'pass_*.ppm|pass_*.f32'; cleandumps; fi
  # 全屏 presented PPM（整屏 nearwhite/mean）
  if [ -n "${PBC:-}" ] && [ "$EL" -ge $((T0+56)) ] && [ -z "${SV:-}" ]; then SV=1; log "m47a_save=2 armed"; knob smo_m47a_save 2; fi
  if [ -n "${SV:-}" ] && [ "$EL" -ge $((T0+86)) ] && [ -z "${SVC:-}" ]; then SVC=1; knob smo_m47a_save 0; log "m47a_save off"; pulldumps pres 'm47a_pres_*.ppm'; fi
  [ -n "${T0:-}" ] && [ "$EL" -ge $((T0+5)) ] && once shot g05
  [ -n "${T0:-}" ] && [ "$EL" -ge $((T0+40)) ] && once shot g40
  [ -n "${T0:-}" ] && [ "$EL" -ge $((T0+75)) ] && once shot g75
  [ -n "${T0:-}" ] && [ "$EL" -ge $((T0+115)) ] && once shot g115
  if [ "$EL" -ge "$NEXT_PROG" ]; then
    NEXT_PROG=$(( NEXT_PROG + 60 ))
    SNAP=$(h shell "tail -c 4000 $LOG 2>/dev/null | grep -a -E 'm56a-|\[m45a\] exec' | tail -2" | tr -d '\r' | cut -c1-190)
    log "PROGRESS t=${EL}s rss=$((RSSKB/1024))MiB :: $SNAP"
  fi
  [ "$EL" -ge "$DUR" ] && { log "t=${EL}s reached duration"; break; }
  sleep 2
done
shot end
log "alive=$(( $(date +%s) - START ))s dead=${DEAD:-no} peak_rss=$((PEAK/1024))MiB"
NEW=0
for cf in $(h shell "ls /data/log/faultlog/faultlogger/ 2>/dev/null" | tr -d '\r'); do
  grep -qxF "$cf" <<<"$CF0" && continue; NEW=$((NEW+1)); log "!! NEW faultlog $cf"
  h file recv "/data/log/faultlog/faultlogger/$cf" "$OUT/$cf" >/dev/null 2>&1
done
log "new_faultlog_count=$NEW"
h shell "cp $LOG /data/local/tmp/${TAG}_vk_boot.log 2>/dev/null; chmod 644 /data/local/tmp/${TAG}_vk_boot.log" >/dev/null 2>&1
h file recv "/data/local/tmp/${TAG}_vk_boot.log" "$OUT/vk_boot.log" >/dev/null 2>&1 && log "vk_boot.log $(wc -c < "$OUT/vk_boot.log")B"
for k in m56a-img m56a-dpass m56a-copy m56a-retarget; do
  n=$(grep -ac "$k" "$OUT/vk_boot.log" 2>/dev/null); log "count $k = ${n:-0}"
done
knob smo_m56a 0; knob smo_m50a_pdskip 0; knob smo_m48a_passdump 0; knob smo_m50a_sh_pass 0; knob smo_m50a_watch 0; knob smo_m47a_save 0; knob smo_m55a 0
log "emu_rss_after=$(ps aux | grep 'tools/emulator/Emulator' | grep -v grep | awk '{print int($6/1024)"MiB"}' | head -1)"
log "done"
