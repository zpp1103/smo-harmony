#!/bin/bash
# M51A / run1：A 两窗口 type 7 实例 dump（红 t=70-106 / 金 t=176-208）+ B shadow 候选 pass 状态表 + 动态 trace shadow pass + 深度 passdump
# usage: bash m51a_run1.sh [tag] [dur]
set -uo pipefail
TAG="${1:-m51a_r1}"; DUR="${2:-340}"
HDC="/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc"
TGT="127.0.0.1:5555"; BUNDLE="com.vibecc.smo"
ROOT="/Users/zpp/VibeCoding/switch2hos"
OUT="$ROOT/logs/m51a/$TAG"
FILES="/data/app/el2/100/base/com.vibecc.smo/files"
LOG="$FILES/vk_boot.log"
mkdir -p "$OUT/screens"
log(){ printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*" | tee -a "$OUT/run.log"; }
h(){ "$HDC" -t "$TGT" "$@"; }
shot(){ h shell "rm -f /data/local/tmp/${TAG}_$1.jpeg; snapshot_display -f /data/local/tmp/${TAG}_$1.jpeg" >/dev/null 2>&1
        h file recv "/data/local/tmp/${TAG}_$1.jpeg" "$OUT/screens/t$1.jpeg" >/dev/null 2>&1; }
knob(){ h shell "echo $2 > $FILES/$1" >/dev/null 2>&1; G=$(h shell "cat $FILES/$1 2>/dev/null" | tr -d '\r\n '); [ "$G" = "$2" ] && log "knob $1=$2 ok" || log "X knob $1=$2 readback=[$G]"; }

log "=== M51A run1 tag=$TAG dur=${DUR}s out=$OUT"
h shell "echo ok" >/dev/null 2>&1 || { log "X target offline"; exit 1; }
for i in 1 2 3; do P=$(h shell "pidof $BUNDLE" | tr -d '[:space:]'); [ -z "$P" ] && break; log "force-stop pid=$P"; h shell "aa force-stop $BUNDLE" >/dev/null 2>&1; sleep 3; done
log "clean dump dir (per-file rm)"
for f in $(h shell "ls $FILES/m45a_dump 2>/dev/null" | tr -d '\r'); do
  case "$f" in pass_*.ppm|pass_*.f32|m47a_pres_*.ppm) h shell "rm -f $FILES/m45a_dump/$f" >/dev/null 2>&1;; esac
done
log "left after clean: $(h shell "ls $FILES/m45a_dump 2>/dev/null" | tr -d '\r' | tr '\n' ' ' | cut -c1-200)"

knob smo_m45a 1; knob smo_m47a 1; knob smo_m47a_save 0; knob smo_m47a_trace 0
knob smo_m48a_passdump 0; knob smo_m48a_rb10 0; knob smo_m48a_rbpool 0
knob smo_m45a_dump_at 96; knob smo_m45a_dump_frames 1
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
parse_sh(){   # 从设备日志里抽「1024² 候选 pass：index + draws」，按 draws 升序打印
  h shell "grep -a 'shpass: pass .* END 1024x1024' $LOG 2>/dev/null | tail -60" | tr -d '\r' \
   | sed -n 's/.*shpass: pass \([0-9][0-9]*\) END 1024x1024 draws \([0-9][0-9]*\).*/\1 \2/p' | sort -u -k2,2n | head -6
}
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
  # ---- 探针常开：shadow 候选 pass 状态表 + passdump 带深度 ----
  if [ "$EL" -ge 60 ] && [ -z "${PRB:-}" ]; then PRB=1; log "probe on: sh_pass=1 pddepth=1"; knob smo_m50a_sh_pass 1; knob smo_m50a_pddepth 1; fi
  # ---- W1 红帧：pass 0 的 type7/UBO dump ----
  if [ "$EL" -ge 70 ] && [ -z "${W1O:-}" ]; then W1O=1; log "W1 open (red map): ubo_pass=1"; knob smo_m50a_ubo_pass 1; fi
  [ "$EL" -ge 74 ] && once shot 74
  [ "$EL" -ge 84 ] && once shot 84
  [ "$EL" -ge 96 ] && once shot 96
  [ "$EL" -ge 104 ] && once shot 104
  if [ "$EL" -ge 106 ] && [ -z "${W1C:-}" ]; then W1C=1; log "W1 close"; knob smo_m50a_ubo_pass 0; fi
  # ---- 动态：解析 1024² 候选，trace 最可疑那一个 ----
  if [ "$EL" -ge 122 ] && [ -z "${TR1:-}" ]; then
    TR1=1; LIST=$(parse_sh | tr '\n' ';'); log "1024 candidates: [$LIST]"
    IDX=$(echo "$LIST" | awk -F'[ ;]' 'NF>=2 && $2!="" {print $1; exit}')
    if [ -n "${IDX:-}" ]; then log "trace shadow candidate pass $IDX"; knob smo_m50a_trace_pass $((IDX+1)); else log "X no 1024 candidate in log yet"; fi
  fi
  [ "$EL" -ge 168 ] && [ -z "${TR1C:-}" ] && { TR1C=1; log "trace off"; knob smo_m50a_trace_pass 0; }
  # ---- W2 金帧 ----
  if [ "$EL" -ge 176 ] && [ -z "${W2O:-}" ]; then W2O=1; log "W2 open (gold map): ubo_pass=1"; knob smo_m50a_ubo_pass 1; fi
  [ "$EL" -ge 178 ] && once shot 178
  [ "$EL" -ge 186 ] && once shot 186
  [ "$EL" -ge 200 ] && once shot 200
  [ "$EL" -ge 208 ] && once shot 208
  if [ "$EL" -ge 208 ] && [ -z "${W2C:-}" ]; then W2C=1; log "W2 close"; knob smo_m50a_ubo_pass 0; fi
  # ---- 第二个候选：trace + UBO（拿光源矩阵） ----
  if [ "$EL" -ge 214 ] && [ -z "${TR2:-}" ]; then
    TR2=1; LIST2=$(parse_sh | tr '\n' ';'); log "candidates@214: [$LIST2]"
    IDX2=$(echo "$LIST2" | awk -F'[ ;]' 'NF>=4 && $3!="" {print $3; exit}')
    if [ -n "${IDX2:-}" ]; then log "trace+ubo 2nd shadow candidate pass $IDX2"; knob smo_m50a_trace_pass $((IDX2+1)); knob smo_m50a_ubo_pass $((IDX2+1)); else log "only one candidate"; fi
  fi
  if [ "$EL" -ge 236 ] && [ -z "${TR2C:-}" ]; then TR2C=1; log "2nd trace/ubo off"; knob smo_m50a_trace_pass 0; knob smo_m50a_ubo_pass 0; fi
  # ---- gameplay 全 pass dump（带深度） ----
  if [ "$EL" -ge 240 ] && [ -z "${PD2:-}" ]; then PD2=1; log "passdump gameplay (depth on, skip 0)"; knob smo_m48a_passdump 3; fi
  if [ "$EL" -ge 250 ] && [ -z "${PD2C:-}" ]; then PD2C=1; knob smo_m48a_passdump 0; log "passdump knob released"; fi
  [ "$EL" -ge 300 ] && once shot 300
  if [ "$EL" -ge "$NEXT_PROG" ]; then
    NEXT_PROG=$(( NEXT_PROG + 60 ))
    SNAP=$(h shell "tail -c 4000 $LOG 2>/dev/null | grep -a -E '\[m51a\]|\[m50a\]|traced pass|pass dump: the next' | tail -2" | tr -d '\r' | cut -c1-170)
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
log "knobs-off tail"
knob smo_m50a_ubo_pass 0; knob smo_m50a_sh_pass 0; knob smo_m50a_pddepth 0; knob smo_m50a_trace_pass 0
h file recv "$LOG" "$OUT/vk_boot.log" >/dev/null 2>&1 && log "vk_boot.log $(wc -c < "$OUT/vk_boot.log")B"
NP=0
for f in $(h shell "ls $FILES/m45a_dump 2>/dev/null" | tr -d '\r'); do
  case "$f" in
    pass_*.ppm|pass_*.f32)
      B=$(h shell "stat -c %s $FILES/m45a_dump/$f 2>/dev/null" | tr -dc '0-9'); B=${B:-0}
      [ "$B" -lt 100 ] && continue
      h file recv "$FILES/m45a_dump/$f" "$OUT/$(basename "$f")" >/dev/null 2>&1
      NP=$((NP+1));;
  esac
done
log "dumps pulled=$NP"
{ echo "alive=$ALIVE dead=$([ -n "$DEAD" ] && echo 1 || echo 0) peak_rss=$((PEAK/1024))MiB new_faultlog=$NEW";
  echo "--- [m51a]/[m50a] ---"; grep -a -E '\[m51a\]|\[m50a\]' "$OUT/vk_boot.log" 2>/dev/null | tail -20 | tr -d '\r';
  echo "--- shpass END ---"; grep -a 'shpass: pass .* END' "$OUT/vk_boot.log" 2>/dev/null | tail -20 | tr -d '\r';
  echo "--- dump map ---"; grep -a 'pass dump: the next\|is the submission' "$OUT/vk_boot.log" 2>/dev/null | tr -d '\r' | head -60;
} > "$OUT/crash_summary.txt"
log "done"
