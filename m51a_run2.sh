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
  # ---- 多窗口：每轮都开 ubo_pass=1（pass 0 的 type7/UBO），配截图判红/金 ----
  if [ "$EL" -ge 88 ] && [ -z "${U1:-}" ]; then U1=1; log "W1 open 88-118"; knob smo_m50a_ubo_pass 1; fi
  [ "$EL" -ge 90 ] && once shot 90
  [ "$EL" -ge 100 ] && once shot 100
  [ "$EL" -ge 110 ] && once shot 110
  if [ "$EL" -ge 118 ] && [ -z "${U1C:-}" ]; then U1C=1; knob smo_m50a_ubo_pass 0; log "W1 close"; fi
  # trace pass 50（=值 51）：拿它采样的 1024² shadow map 的 pool+off，与 shpass 记录的 depth pool+off 对齐
  if [ "$EL" -ge 120 ] && [ -z "${T50:-}" ]; then T50=1; log "trace pass 50 armed"; knob smo_m50a_trace_pass 51; fi
  if [ "$EL" -ge 168 ] && [ -z "${T50C:-}" ]; then T50C=1; knob smo_m50a_trace_pass 0; log "trace 50 off"; fi
  if [ "$EL" -ge 170 ] && [ -z "${U2:-}" ]; then U2=1; log "W2 open 170-200"; knob smo_m50a_ubo_pass 1; fi
  [ "$EL" -ge 174 ] && once shot 174
  [ "$EL" -ge 184 ] && once shot 184
  [ "$EL" -ge 194 ] && once shot 194
  if [ "$EL" -ge 200 ] && [ -z "${U2C:-}" ]; then U2C=1; knob smo_m50a_ubo_pass 0; log "W2 close"; fi
  if [ "$EL" -ge 215 ] && [ -z "${U3:-}" ]; then U3=1; log "W3 open 215-245"; knob smo_m50a_ubo_pass 1; fi
  [ "$EL" -ge 220 ] && once shot 220
  [ "$EL" -ge 232 ] && once shot 232
  [ "$EL" -ge 244 ] && once shot 244
  if [ "$EL" -ge 245 ] && [ -z "${U3C:-}" ]; then U3C=1; knob smo_m50a_ubo_pass 0; log "W3 close"; fi
  # 若一直停在关卡外：点屏幕中间推进（只点，不改任何开关）
  if [ "$EL" -ge 260 ] && [ $(( EL % 30 )) -lt 2 ]; then h shell "uitest uiInput click 800 450" >/dev/null 2>&1; log "click @t=$EL"; fi
  if [ "$EL" -ge 280 ] && [ -z "${U4:-}" ]; then U4=1; log "W4 open 280-310"; knob smo_m50a_ubo_pass 1; fi
  [ "$EL" -ge 290 ] && once shot 290
  [ "$EL" -ge 305 ] && once shot 305
  if [ "$EL" -ge 310 ] && [ -z "${U4C:-}" ]; then U4C=1; knob smo_m50a_ubo_pass 0; log "W4 close"; fi
  # gameplay 全 pass dump（带深度），晚一点，避开地图段
  if [ "$EL" -ge 320 ] && [ -z "${PD2:-}" ]; then PD2=1; log "passdump@320 (depth on)"; knob smo_m48a_passdump 3; fi
  if [ "$EL" -ge 330 ] && [ -z "${PD2C:-}" ]; then PD2C=1; knob smo_m48a_passdump 0; fi
  [ "$EL" -ge 380 ] && once shot 380
  [ "$EL" -ge 400 ] && once shot 400
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
