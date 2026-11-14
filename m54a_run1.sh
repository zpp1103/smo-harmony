#!/bin/bash
# M54A run1（纯测量，不改代码）：
#   smo_m45a=1（skip_prog/skip_pipe/skip_rt 计数器 —— Q2「录了但 exec 丢了」判别）
#   smo_m47a=1（present 统计，确认游戏在跑）
#   smo_m50a_sh_pass=1（1024²/256² 深度 pass 的逐 draw 行 + END 汇总：frame/pass/pool+off/draws）
#   smo_m50a_watch 分时采样 4 张 1024² 深度图（228600/200200/828a00/628800）的
#     image 指针 / nvn flags / 重叠数 / fmt —— Q3「可观测差异」
#   两次 app 启动（t≈0 与 t≈165）：intro 的「多级联同帧」窗口会重放 ⇒ 228600 样本翻倍。
# usage: bash m54a_run1.sh [tag] [dur]
set -uo pipefail
TAG="${1:-m54a_r1}"; DUR="${2:-330}"
HDC="/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc"
TGT="127.0.0.1:5555"; BUNDLE="com.vibecc.smo"
ROOT="/Users/zpp/VibeCoding/switch2hos"
OUT="$ROOT/logs/m54a/$TAG"
FILES="/data/app/el2/100/base/com.vibecc.smo/files"
LOG="$FILES/vk_boot.log"
mkdir -p "$OUT/screens"
log(){ printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*" | tee -a "$OUT/run.log"; }
h(){ "$HDC" -t "$TGT" "$@"; }
shot(){ h shell "rm -f /data/local/tmp/${TAG}_$1.jpeg; snapshot_display -f /data/local/tmp/${TAG}_$1.jpeg" >/dev/null 2>&1
        h file recv "/data/local/tmp/${TAG}_$1.jpeg" "$OUT/screens/t$1.jpeg" >/dev/null 2>&1; }
knob(){ h shell "echo $2 > $FILES/$1" >/dev/null 2>&1; G=$(h shell "cat $FILES/$1 2>/dev/null" | tr -d '\r\n '); [ "$G" = "$2" ] && log "knob $1=$2 ok" || log "X knob $1=$2 readback=[$G]"; }
startapp(){ h shell "aa start -a EntryAbility -b $BUNDLE" >/dev/null 2>&1; }

log "=== M54A run1 tag=$TAG dur=${DUR}s out=$OUT"
h shell "echo ok" >/dev/null 2>&1 || { log "X target offline"; exit 1; }
log "rss_before=$(ps aux | grep 'tools/emulator/Emulator' | grep -v grep | awk '{print int($6/1024)"MiB"}' | head -1)"
for i in 1 2 3; do P=$(h shell "pidof $BUNDLE" | tr -d '[:space:]'); [ -z "$P" ] && break; log "force-stop pid=$P"; h shell "aa force-stop $BUNDLE" >/dev/null 2>&1; sleep 3; done
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
startapp; log "app start #1"
START=$(date +%s); FIRST=""; DEAD=""; LASTP=""; PEAK=0; NEXT_PROG=60; DID=" "
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
  # ---- t=30：sh_pass 探针开（watch 依次采样 4 张图）----
  if [ "$EL" -ge 30 ] && [ -z "${PRB:-}" ]; then PRB=1; log "probe on: sh_pass=1 watch=228600#60"; knob smo_m50a_sh_pass 1; knob smo_m50a_watch "228600#60"; fi
  if [ "$EL" -ge 80 ] && [ -z "${W2:-}" ]; then W2=1; log "watch -> 200200#30"; knob smo_m50a_watch "200200#30"; fi
  if [ "$EL" -ge 100 ] && [ -z "${W3:-}" ]; then W3=1; log "watch -> 828a00#30"; knob smo_m50a_watch "828a00#30"; fi
  if [ "$EL" -ge 120 ] && [ -z "${W4:-}" ]; then W4=1; log "watch -> 628800#30"; knob smo_m50a_watch "628800#30"; fi
  if [ "$EL" -ge 140 ] && [ -z "${W5:-}" ]; then W5=1; log "watch off"; knob smo_m50a_watch 0; fi
  # ---- t=165：force-stop + 重启 app（第二遍 intro，多一组 228600 样本）----
  if [ "$EL" -ge 165 ] && [ -z "${RS:-}" ]; then
    RS=1; log "restart app for 2nd intro pass"; h shell "aa force-stop $BUNDLE" >/dev/null 2>&1; sleep 3
    knob smo_m50a_watch "228600#60"; startapp; log "app start #2"
  fi
  if [ "$EL" -ge 200 ] && [ -z "${W6:-}" ]; then W6=1; log "watch -> 200200#20 (2nd)"; knob smo_m50a_watch "200200#20"; fi
  if [ "$EL" -ge 215 ] && [ -z "${W7:-}" ]; then W7=1; log "watch -> 828a00#20 (2nd)"; knob smo_m50a_watch "828a00#20"; fi
  if [ "$EL" -ge 230 ] && [ -z "${W8:-}" ]; then W8=1; log "watch -> 628800#20 (2nd)"; knob smo_m50a_watch "628800#20"; fi
  if [ "$EL" -ge 245 ] && [ -z "${W9:-}" ]; then W9=1; log "watch off (2nd)"; knob smo_m50a_watch 0; fi
  [ "$EL" -ge 60 ] && once shot 60
  [ "$EL" -ge 130 ] && once shot 130
  [ "$EL" -ge 230 ] && once shot 230
  [ "$EL" -ge 300 ] && once shot 300
  if [ "$EL" -ge "$NEXT_PROG" ]; then
    NEXT_PROG=$(( NEXT_PROG + 60 ))
    SNAP=$(h shell "tail -c 6000 $LOG 2>/dev/null | grep -a -E '\[m45a\] exec|shpass: pass .* END' | tail -2" | tr -d '\r' | cut -c1-170)
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
  echo "--- shpass END 汇总 ---"; grep -a 'shpass: pass .* END' "$OUT/vk_boot.log" 2>/dev/null | sed -E 's/.*pass ([0-9]+) END [0-9]+x[0-9]+ draws ([0-9]+).*pool (0x[0-9a-f]+\+0x[0-9a-f]+).*/\3 draws=\2/' | sort | uniq -c | sort -rn | head -20;
  echo "--- watch 汇总 ---"; grep -a 'watch: present' "$OUT/vk_boot.log" 2>/dev/null | sed -E 's/.*pool (0x[0-9a-f]+\+0x[0-9a-f]+) \(image ([^,]+), nvn flags ([^,]+), ([0-9]+) overlapping\).*/\1 flags=\3 overlap=\4/' | sort | uniq -c | sort -rn | head -12;
} > "$OUT/crash_summary.txt"
log "done"
