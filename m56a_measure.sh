#!/bin/bash
# M56A 量化测量（每轮一个 app 进程；同一进程内可多次 passdump）
#   usage: bash m56a_measure.sh <tag> <m56a_mode> <dur> [m55a]
#   时间线（固定时刻，保证 before/after 同场景可比）：t=180/260/340/420 各开一个 passdump 窗口（skip=40，覆盖 40..88）
#     arm → 15 s 后全量拉回 dump_wN（不删）→ 关；t=200..235 存 presented 全屏 PPM；t=120/200/260/320/380/end 截图
set -uo pipefail
TAG="${1:-m56a_A}"; M56A="${2:-0}"; DUR="${3:-460}"; M55A="${4:-1}"
HDC="/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc"
TGT="127.0.0.1:5555"; BUNDLE="com.vibecc.smo"
ROOT="/Users/zpp/VibeCoding/switch2hos"; OUT="$ROOT/logs/m56a/$TAG"
FILES="/data/app/el2/100/base/com.vibecc.smo/files"; LOG="$FILES/vk_boot.log"
mkdir -p "$OUT/screens" "$OUT/pres"
log(){ printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*" | tee -a "$OUT/run.log"; }
h(){ "$HDC" -t "$TGT" "$@"; }
shot(){ h shell "rm -f /data/local/tmp/${TAG}_$1.jpeg; snapshot_display -f /data/local/tmp/${TAG}_$1.jpeg" >/dev/null 2>&1
        h file recv "/data/local/tmp/${TAG}_$1.jpeg" "$OUT/screens/t$1.jpeg" >/dev/null 2>&1; }
knob(){ h shell "echo $2 > $FILES/$1" >/dev/null 2>&1; G=$(h shell "cat $FILES/$1 2>/dev/null" | tr -d '\r\n '); [ "$G" = "$2" ] && log "knob $1=$2 ok" || log "X knob $1=$2 readback=[$G]"; }
lst(){ h shell "ls $FILES/m45a_dump 2>/dev/null" | tr -d '\r'; }
pull(){ local n=0 f B; mkdir -p "$OUT/$1"
  for f in $(lst); do
    case "$f" in $2) ;; *) continue;; esac
    B=$(h shell "stat -c %s $FILES/m45a_dump/$f 2>/dev/null" | tr -dc '0-9'); B=${B:-0}; [ "$B" -lt 100 ] && continue
    h file recv "$FILES/m45a_dump/$f" "$OUT/$1/$f" >/dev/null 2>&1 && n=$((n+1))
  done; log "pulled $n -> $OUT/$1"; }

: > "$OUT/run.log"
log "=== M56A measure tag=$TAG m56a=$M56A dur=${DUR}s m55a=$M55A"
h shell "echo ok" >/dev/null 2>&1 || { log "X target offline"; exit 1; }
log "emu_rss_before=$(ps aux | grep 'tools/emulator/Emulator' | grep -v grep | awk '{print int($6/1024)"MiB"}' | head -1)"
for i in 1 2 3; do P=$(h shell "pidof $BUNDLE" | tr -d '[:space:]'); [ -z "$P" ] && break; log "force-stop pid=$P"; h shell "aa force-stop $BUNDLE" >/dev/null 2>&1; sleep 3; done
for f in $(lst); do case "$f" in pass_*) h shell "rm -f $FILES/m45a_dump/$f" >/dev/null 2>&1;; esac; done
knob smo_m45a 1; knob smo_m47a 1; knob smo_m47a_save 0; knob smo_m47a_trace 0
knob smo_m48a 0; knob smo_m48a_passdump 0; knob smo_m48a_rb10 0; knob smo_m48a_rbpool 0
knob smo_m45a_dump_at 99999; knob smo_m45a_dump_frames 1
knob smo_m50a_trace_pass 0; knob smo_m50a_ubo_pass 0; knob smo_m50a_sh_pass 0; knob smo_m50a_pddepth 0; knob smo_m50a_watch 0
knob smo_m50a_pdskip 40
knob smo_m55a "$M55A"; knob smo_m56a "$M56A"
h shell ": > $LOG" >/dev/null 2>&1
h shell "hilog -r" >/dev/null 2>&1
CF0=$(h shell "ls /data/log/faultlog/faultlogger/ 2>/dev/null" | tr -d '\r')
h shell "power-shell wakeup" >/dev/null 2>&1
h shell "power-shell timeout -o 1800000" >/dev/null 2>&1
h shell "aa start -a EntryAbility -b $BUNDLE" >/dev/null 2>&1
log "app start"
START=$(date +%s); PEAK=0; DEAD=""; FIRST=""; W=0
once(){ case " $DONE " in *" $1 "*) return 0;; esac; DONE="$DONE $1"; "$@"; }
DONE=" "
shotat(){ local t=$1; [ "$EL" -ge "$t" ] && once s$t shot "$t"; }
while :; do
  EL=$(( $(date +%s) - START ))
  P=$(h shell "pidof $BUNDLE" | tr -d '[:space:]')
  if [ -z "$P" ]; then
    if [ -z "$FIRST" ] && [ "$EL" -lt 60 ]; then sleep 2; continue; fi
    DEAD=$EL; log "t=${EL}s pid=<dead>"; break
  fi
  [ -z "$FIRST" ] && { FIRST=$P; log "t=${EL}s pid=$P"; }
  RSSKB=$(h shell "grep VmRSS /proc/$P/status 2>/dev/null" | tr -dc '0-9'); RSSKB=${RSSKB:-0}
  [ "$RSSKB" -gt "$PEAK" ] && PEAK=$RSSKB
  # passdump 窗口：t=180/260/340/420，arm 后 15 s 拉回
  for WT in 180 260 340 420; do
    if [ "$EL" -ge "$WT" ] && [ "$EL" -lt $((WT+15)) ]; then
      case " $ARMI " in *" $WT "*) ;; *) ARMI="$ARMI $WT"; log "passdump window w$WT arm @${EL}s"; knob smo_m48a_passdump 3;; esac
    fi
    if [ "$EL" -ge $((WT+15)) ] && [ "$EL" -lt $((WT+17)) ]; then
      case " $PULLD " in *" $WT "*) ;; *) PULLD="$PULLD $WT"; knob smo_m48a_passdump 0; pull "dump_w$WT" 'pass_*'; esac
    fi
  done
  if [ "$EL" -ge 200 ] && [ -z "${SV:-}" ]; then SV=1; log "m47a_save=2 @${EL}s"; knob smo_m47a_save 2; fi
  if [ "$EL" -ge 235 ] && [ -n "${SV:-}" ] && [ -z "${SVC:-}" ]; then SVC=1; knob smo_m47a_save 0; log "m47a_save off"; pull pres 'm47a_pres_*'; fi
  shotat 120; shotat 200; shotat 260; shotat 320; shotat 380
  if [ $((EL % 60)) -eq 0 ] && [ "$EL" -gt 0 ]; then
    case " $PROG " in *" $EL "*) ;; *) PROG="$PROG $EL"
      SNAP=$(h shell "tail -c 4000 $LOG 2>/dev/null | grep -a -E 'm56a-retarget|m55a-stat|\[m45a\] exec' | tail -2" | tr -d '\r' | cut -c1-170)
      log "PROGRESS t=${EL}s rss=$((RSSKB/1024))MiB :: $SNAP";; esac
  fi
  [ "$EL" -ge "$DUR" ] && { log "t=${EL}s dur"; break; }
  sleep 2
done
ALIVE=$(( $(date +%s) - START )); [ -n "$DEAD" ] && ALIVE=$DEAD
log "alive=${ALIVE}s dead=${DEAD:-no} peak_app_rss=$((PEAK/1024))MiB"
shot end
NEW=0
for cf in $(h shell "ls /data/log/faultlog/faultlogger/ 2>/dev/null" | tr -d '\r'); do
  grep -qxF "$cf" <<<"$CF0" && continue; NEW=$((NEW+1)); log "!! NEW faultlog $cf"; h file recv "/data/log/faultlog/faultlogger/$cf" "$OUT/$cf" >/dev/null 2>&1
done
log "new_faultlog_count=$NEW"
pull final 'pass_*'
for f in $(lst); do case "$f" in m55a_*.ppm) h file recv "$FILES/m45a_dump/$f" "$OUT/screens/$f" >/dev/null 2>&1;; esac; done
h shell "cp $LOG /data/local/tmp/${TAG}_vk_boot.log 2>/dev/null; chmod 644 /data/local/tmp/${TAG}_vk_boot.log" >/dev/null 2>&1
h file recv "/data/local/tmp/${TAG}_vk_boot.log" "$OUT/vk_boot.log" >/dev/null 2>&1 && log "vk_boot.log $(wc -c < "$OUT/vk_boot.log")B"
for k in m56a-img m56a-dpass m56a-copy m56a-retarget m55a-stat 'pass dump '; do n=$(grep -ac "$k" "$OUT/vk_boot.log" 2>/dev/null); log "count '$k' = ${n:-0}"; done
knob smo_m56a 0; knob smo_m55a 0; knob smo_m48a_passdump 0; knob smo_m50a_pdskip 0; knob smo_m47a_save 0
log "emu_rss_after=$(ps aux | grep 'tools/emulator/Emulator' | grep -v grep | awk '{print int($6/1024)"MiB"}' | head -1)"
log "done"
