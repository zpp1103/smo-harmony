#!/bin/bash
# M53A boot#1（诊断 + before 基线）：
#   smo_m45a=1（每 5 s draws/issued/skip_prog/skip_pipe/skip_rt/passes 统计行 —— 判「draw 是被回放丢的还是游戏没发」）
#   smo_m50a_sh_pass=1（1024²/256² 深度 pass 逐 draw 状态表 + END 汇总）
#   smo_m50a_watch=228600#800（+0x228600 这张图的**全生命周期**：attachment / sampled / copy，带 present+pass）
#   smo_m50a_pddepth=1 + smo_m48a_passdump（两轮 passdump 带深度：同帧各 1024² 级联的远平面占比 + pass50 mask）
#   smo_m47a_save=2（t≈130 存 2 张 presented 全屏 PPM，给整屏 nearwhite/mean）
# usage: bash m53a_run1.sh [tag] [dur]
set -uo pipefail
TAG="${1:-m53a_r1}"; DUR="${2:-300}"
HDC="/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc"
TGT="127.0.0.1:5555"; BUNDLE="com.vibecc.smo"
ROOT="/Users/zpp/VibeCoding/switch2hos"
OUT="$ROOT/logs/m53a/$TAG"
FILES="/data/app/el2/100/base/com.vibecc.smo/files"
LOG="$FILES/vk_boot.log"
mkdir -p "$OUT/screens" "$OUT/d1" "$OUT/d2"
log(){ printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*" | tee -a "$OUT/run.log"; }
h(){ "$HDC" -t "$TGT" "$@"; }
shot(){ h shell "rm -f /data/local/tmp/${TAG}_$1.jpeg; snapshot_display -f /data/local/tmp/${TAG}_$1.jpeg" >/dev/null 2>&1
        h file recv "/data/local/tmp/${TAG}_$1.jpeg" "$OUT/screens/t$1.jpeg" >/dev/null 2>&1; }
knob(){ h shell "echo $2 > $FILES/$1" >/dev/null 2>&1; G=$(h shell "cat $FILES/$1 2>/dev/null" | tr -d '\r\n '); [ "$G" = "$2" ] && log "knob $1=$2 ok" || log "X knob $1=$2 readback=[$G]"; }
pulldumps(){  # $1 = dest subdir
  local n=0
  for f in $(h shell "ls $FILES/m45a_dump 2>/dev/null" | tr -d '\r'); do
    case "$f" in
      pass_*.ppm|pass_*.f32|m47a_pres_*.ppm)
        B=$(h shell "stat -c %s $FILES/m45a_dump/$f 2>/dev/null" | tr -dc '0-9'); B=${B:-0}
        [ "$B" -lt 100 ] && continue
        h file recv "$FILES/m45a_dump/$f" "$OUT/$1/$(basename "$f")" >/dev/null 2>&1; n=$((n+1));;
    esac
  done
  log "pulled $n dumps -> $OUT/$1"
}

log "=== M53A run1 tag=$TAG dur=${DUR}s out=$OUT"
h shell "echo ok" >/dev/null 2>&1 || { log "X target offline"; exit 1; }
for i in 1 2 3; do P=$(h shell "pidof $BUNDLE" | tr -d '[:space:]'); [ -z "$P" ] && break; log "force-stop pid=$P"; h shell "aa force-stop $BUNDLE" >/dev/null 2>&1; sleep 3; done
log "clean dump dir (per-file rm)"
for f in $(h shell "ls $FILES/m45a_dump 2>/dev/null" | tr -d '\r'); do
  case "$f" in pass_*.ppm|pass_*.f32|m47a_pres_*.ppm) h shell "rm -f $FILES/m45a_dump/$f" >/dev/null 2>&1;; esac
done
log "left after clean: $(h shell "ls $FILES/m45a_dump 2>/dev/null" | tr -d '\r' | tr '\n' ' ' | cut -c1-200)"

knob smo_m45a 1; knob smo_m47a 1; knob smo_m47a_save 0; knob smo_m47a_trace 0
knob smo_m48a_passdump 0; knob smo_m48a_rb10 0; knob smo_m48a_rbpool 0
knob smo_m45a_dump_at 99999; knob smo_m45a_dump_frames 1
knob smo_m50a_pdskip 0; knob smo_m50a_trace_pass 0; knob smo_m50a_ubo_pass 0
knob smo_m50a_sh_pass 0; knob smo_m50a_pddepth 0; knob smo_m50a_watch 0
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
  # ---- t=35：探针开（shpass + watch + pddepth）----
  if [ "$EL" -ge 35 ] && [ -z "${PRB:-}" ]; then PRB=1; log "probe on: sh_pass=1 pddepth=1 watch=228600#800"; knob smo_m50a_sh_pass 1; knob smo_m50a_pddepth 1; knob smo_m50a_watch "228600#800"; fi
  # ---- 存档全屏 PPM（t=130 起）----
  if [ "$EL" -ge 130 ] && [ -z "${SV:-}" ]; then SV=1; log "m47a_save=2 armed"; knob smo_m47a_save 2; fi
  if [ "$EL" -ge 160 ] && [ -z "${SVC:-}" ]; then SVC=1; knob smo_m47a_save 0; log "m47a_save off"; fi
  # ---- passdump A（t=145，级联窗口；含深度）----
  if [ "$EL" -ge 145 ] && [ -z "${PA:-}" ]; then PA=1; log "passdump A armed @145"; knob smo_m48a_passdump 3; fi
  if [ "$EL" -ge 152 ] && [ -z "${PAC:-}" ]; then PAC=1; knob smo_m48a_passdump 0; pulldumps d1; fi
  # ---- passdump B（t=235）----
  if [ "$EL" -ge 235 ] && [ -z "${PB:-}" ]; then PB=1; log "passdump B armed @235"; knob smo_m48a_passdump 3; fi
  if [ "$EL" -ge 242 ] && [ -z "${PBC:-}" ]; then PBC=1; knob smo_m48a_passdump 0; pulldumps d2; fi
  [ "$EL" -ge 60 ] && once shot 60
  [ "$EL" -ge 110 ] && once shot 110
  [ "$EL" -ge 140 ] && once shot 140
  [ "$EL" -ge 180 ] && once shot 180
  [ "$EL" -ge 220 ] && once shot 220
  [ "$EL" -ge 260 ] && once shot 260
  [ "$EL" -ge 290 ] && once shot 290
  if [ "$EL" -ge "$NEXT_PROG" ]; then
    NEXT_PROG=$(( NEXT_PROG + 60 ))
    SNAP=$(h shell "tail -c 6000 $LOG 2>/dev/null | grep -a -E '\[m45a\] exec|\[m51a\]|shpass: pass .* END' | tail -3" | tr -d '\r' | cut -c1-190)
    log "PROGRESS t=${EL}s rss=$((RSSKB/1024))MiB :: $SNAP"
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
log "knobs-off tail"
knob smo_m50a_ubo_pass 0; knob smo_m50a_sh_pass 0; knob smo_m50a_pddepth 0; knob smo_m50a_trace_pass 0
knob smo_m50a_watch 0; knob smo_m47a_save 0; knob smo_m48a_passdump 0
h file recv "$LOG" "$OUT/vk_boot.log" >/dev/null 2>&1 && log "vk_boot.log $(wc -c < "$OUT/vk_boot.log")B"
{ echo "alive=$ALIVE dead=$([ -n "$DEAD" ] && echo 1 || echo 0) peak_rss=$((PEAK/1024))MiB new_faultlog=$NEW";
  echo "--- [m45a] exec 最后 5 行 ---"; grep -a '\[m45a\] exec' "$OUT/vk_boot.log" 2>/dev/null | tail -5 | tr -d '\r';
  echo "--- shpass END 汇总 ---"; grep -a 'shpass: pass .* END' "$OUT/vk_boot.log" 2>/dev/null | sed -E 's/^([0-9.]+).*pass ([0-9]+) END [0-9]+x[0-9]+ draws ([0-9]+).*pool (0x[0-9a-f]+\+0x[0-9a-f]+).*/\2 \3 \4/' | sort | uniq -c | sort -rn | head -20;
  echo "--- watch 汇总（what 计数）---"; grep -a 'watch: present' "$OUT/vk_boot.log" 2>/dev/null | sed -E 's/.*: (attachment|sampled or image|copy source|copy destination).*/\1/' | sort | uniq -c;
} > "$OUT/crash_summary.txt"
log "done"
