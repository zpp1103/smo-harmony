#!/bin/bash
# M49A / run3：**gameplay 帧的逐 pass dump**（找整屏过曝/白雾到底在哪一笔 pass）
#   新工具：smo_m45a_dump_at=<秒>（默认 90，本单加）把 dump 窗口从世界地图挪到 gameplay。
#   同时 t≈245 起存 16 张 present PPM（过曝量化的 RGB 原始数据，不用 JPEG）。
set -uo pipefail
TAG="${1:-m49a_r3}"; DUR="${2:-320}"; DAT="${3:-255}"
HDC="/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc"
TGT="127.0.0.1:5555"; BUNDLE="com.vibecc.smo"
ROOT="/Users/zpp/VibeCoding/switch2hos"; OUT="$ROOT/logs/m49a/$TAG"
FILES="/data/app/el2/100/base/com.vibecc.smo/files"; LOG="$FILES/vk_boot.log"
mkdir -p "$OUT/screens"
log(){ printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*" | tee -a "$OUT/run.log"; }
h(){ "$HDC" -t "$TGT" "$@"; }
shot(){ h shell "rm -f /data/local/tmp/${TAG}_$1.jpeg; snapshot_display -f /data/local/tmp/${TAG}_$1.jpeg" >/dev/null 2>&1
        h file recv "/data/local/tmp/${TAG}_$1.jpeg" "$OUT/screens/t$1.jpeg" >/dev/null 2>&1; }
knob(){ h shell "echo $2 > $FILES/$1" >/dev/null 2>&1; G=$(h shell "cat $FILES/$1 2>/dev/null" | tr -d '\r\n '); [ "$G" = "$2" ] && log "knob $1=$2 ok" || log "X knob $1=$2 readback=[$G]"; }

log "=== M49A run3 tag=$TAG dur=${DUR}s dump_at=$DAT"
for i in 1 2 3; do P=$(h shell "pidof $BUNDLE" | tr -d '[:space:]'); [ -z "$P" ] && break; h shell "aa force-stop $BUNDLE" >/dev/null 2>&1; sleep 3; done
# 新包首次启动后目录是 app 建的；M49A 已把 m45a_dump chmod 0777 -> hdc 可逐文件删（真因是目录权限，不是 glob）
log "dump dir perms: $(h shell "ls -ld $FILES/m45a_dump" | tr -d '\r')"
for f in $(h shell "ls $FILES/m45a_dump 2>/dev/null" | tr -d '\r'); do
  case "$f" in pass_*.ppm|pass_*.f32|m47a_pres_*.ppm) h shell "rm -f $FILES/m45a_dump/$f" >/dev/null 2>&1;; esac
done
log "dump dir now: $(h shell "ls $FILES/m45a_dump 2>/dev/null" | tr -d '\r' | tr '\n' ' ' | cut -c1-200)"
knob smo_m45a 1; knob smo_m47a 1; knob smo_m45a_dump_frames 1; knob smo_m45a_dump_at "$DAT"
knob smo_m47a_save 0; knob smo_m47a_trace 0; knob smo_m47a_texdump 0
knob smo_m48a_passdump 0; knob smo_m48a 0
h shell ": > $LOG" >/dev/null 2>&1; h shell "hilog -r" >/dev/null 2>&1
CF0=$(h shell "ls /data/log/faultlog/faultlogger/ 2>/dev/null" | tr -d '\r')
h shell "power-shell wakeup" >/dev/null 2>&1; h shell "power-shell timeout -o 1800000" >/dev/null 2>&1
h shell "aa start -a EntryAbility -b $BUNDLE" >/dev/null 2>&1
START=$(date +%s); FIRST=""; DEAD=""; LASTP=""; PEAK=0; NEXT_PROG=60; DID_SAVE=0
while :; do
  EL=$(( $(date +%s) - START ))
  P=$(h shell "pidof $BUNDLE" | tr -d '[:space:]')
  if [ -z "$P" ]; then
    if [ -z "$FIRST" ] && [ "$EL" -lt 60 ]; then sleep 2; continue; fi
    DEAD=$EL; log "t=${EL}s pid=<dead>"; break
  fi
  [ -z "$FIRST" ] && { FIRST="$P"; log "t=${EL}s pid=$P"; }
  LASTP="$P"
  RSSKB=$(h shell "grep VmRSS /proc/$P/status 2>/dev/null" | tr -dc '0-9'); RSSKB=${RSSKB:-0}
  [ "$RSSKB" -gt "$PEAK" ] && PEAK=$RSSKB
  [ "$DID_SAVE" = 0 ] && [ "$EL" -ge 245 ] && { DID_SAVE=1; knob smo_m47a_save 16; knob smo_m47a_trace 92; }
  if [ "$EL" -ge "$NEXT_PROG" ]; then
    NEXT_PROG=$(( NEXT_PROG + 60 ))
    SNAP=$(h shell "tail -c 3000 $LOG 2>/dev/null | grep -a -E '\[m47a\] pres|RT dump armed|pass dump [0-9][0-9]:' | tail -2" | tr -d '\r' | cut -c1-190)
    log "PROGRESS t=${EL}s rss=$((RSSKB/1024))MiB :: $SNAP"; shot "$EL"
  fi
  [ -n "$DEAD" ] && break
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
NP=0
for f in $(h shell "ls $FILES/m45a_dump 2>/dev/null" | tr -d '\r'); do
  case "$f" in
    pass_*.ppm|m47a_pres_*.ppm)
      B=$(h shell "stat -c %s $FILES/m45a_dump/$f 2>/dev/null" | tr -dc '0-9'); B=${B:-0}
      [ "$B" -lt 100 ] && continue
      h file recv "$FILES/m45a_dump/$f" "$OUT/$(basename "$f")" >/dev/null 2>&1; NP=$((NP+1));;
  esac
done
log "dumps pulled=$NP"
{ echo "alive=$ALIVE dead=$([ -n "$DEAD" ] && echo 1 || echo 0) peak_rss=$((PEAK/1024))MiB new_faultlog=$NEW";
  echo "--- dump map ---"; grep -a 'pass dump [0-9]' "$OUT/vk_boot.log" 2>/dev/null | grep -a 'is the submission' | head -40 | tr -d '\r';
} > "$OUT/crash_summary.txt"
log "done"
