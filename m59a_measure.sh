#!/bin/bash
# M59A 测量（**一个 app 进程内**三个窗口；开关 2 s 热读，不再起第三次）
#   W0 基线（smo_m59a=0）                     —— 与历史基线逐位可比
#   W1 决定窗口（19：bit0 2.1 探针 + bit1 slab 别名 + bit4 采样身份）
#   W2 修候选①（23 = 19|bit2 保字节：跳过零填充 + 跳过整块 clear）
#   W3 修候选①②（31 = 23|bit3 A→B copy 同步）  —— 两个修候选同一进程内各测一个窗口，不再起第三次 app
# 时间线用固定时刻，每窗口都做：passdump（pdskip=40，覆盖每提交的 pass 40..88）+ presented PPM + 截图
# usage: bash m59a_measure.sh <tag> <dur> [w2bit]
set -uo pipefail
TAG="${1:-m59a_A}"; DUR="${2:-720}"; W2BIT="${3:-23}"; W3BIT="${4:-31}"
HDC="/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc"
TGT="127.0.0.1:5555"; BUNDLE="com.vibecc.smo"
ROOT="/Users/zpp/VibeCoding/switch2hos"; OUT="$ROOT/logs/m59a/$TAG"
FILES="/data/app/el2/100/base/com.vibecc.smo/files"; DUMP="$FILES/m45a_dump"; LOG="$FILES/vk_boot.log"
mkdir -p "$OUT/screens" "$OUT/pres"
log(){ printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*" | tee -a "$OUT/run.log"; }
h(){ "$HDC" -t "$TGT" "$@"; }
shot(){ h shell "rm -f /data/local/tmp/${TAG}_$1.jpeg; snapshot_display -f /data/local/tmp/${TAG}_$1.jpeg" >/dev/null 2>&1
        h file recv "/data/local/tmp/${TAG}_$1.jpeg" "$OUT/screens/t$1.jpeg" >/dev/null 2>&1; }
knob(){ h shell "echo $2 > $FILES/$1" >/dev/null 2>&1; G=$(h shell "cat $FILES/$1 2>/dev/null" | tr -d '\r\n '); [ "$G" = "$2" ] && log "knob $1=$2 ok" || log "X knob $1=$2 readback=[$G]"; }
lst(){ h shell "ls $DUMP 2>/dev/null" | tr -d '\r'; }
pull(){ local n=0 f B; mkdir -p "$OUT/$1"
  for f in $(lst); do
    case "$f" in $2) ;; *) continue;; esac
    B=$(h shell "stat -c %s $DUMP/$f 2>/dev/null" | tr -dc '0-9'); B=${B:-0}; [ "$B" -lt 100 ] && continue
    h file recv "$DUMP/$f" "$OUT/$1/$f" >/dev/null 2>&1 && n=$((n+1))
  done; log "pulled $n -> $OUT/$1"; }

: > "$OUT/run.log"
log "=== M59A measure tag=$TAG dur=${DUR}s w2bit=$W2BIT w3bit=$W3BIT"
h shell "echo ok" >/dev/null 2>&1 || { log "X target offline"; exit 1; }
log "emu_rss_before=$(ps aux | grep 'tools/emulator/Emulator' | grep -v grep | awk '{print int($6/1024)"MiB"}' | head -1)"
for i in 1 2 3; do P=$(h shell "pidof $BUNDLE" | tr -d '[:space:]'); [ -z "$P" ] && break; log "force-stop pid=$P"; h shell "aa force-stop $BUNDLE" >/dev/null 2>&1; sleep 3; done
h shell "rm -f $DUMP/pass_* $DUMP/m59a_* $DUMP/m55a_* $DUMP/m47a_pres_* 2>/dev/null" >/dev/null 2>&1
knob smo_m45a 1; knob smo_m47a 1; knob smo_m47a_save 0; knob smo_m47a_trace 0
knob smo_m48a 0; knob smo_m48a_passdump 0; knob smo_m48a_rb10 0; knob smo_m48a_rbpool 0
knob smo_m45a_dump_at 99999; knob smo_m45a_dump_frames 1
knob smo_m50a_trace_pass 0; knob smo_m50a_ubo_pass 0; knob smo_m50a_sh_pass 0; knob smo_m50a_pddepth 0; knob smo_m50a_watch 0
knob smo_m50a_pdskip 40
knob smo_m55a 1; knob smo_m56a 0; knob smo_m57a 0
knob smo_m59a 0; knob DM_VK2_SELFTEST 0; knob DM_VK2_ALIAS 0
h shell ": > $LOG" >/dev/null 2>&1
h shell "hilog -r" >/dev/null 2>&1
CF0=$(h shell "ls /data/log/faultlog/faultlogger/ 2>/dev/null" | tr -d '\r')
h shell "power-shell wakeup" >/dev/null 2>&1
h shell "power-shell timeout -o 1800000" >/dev/null 2>&1
h shell "aa start -a EntryAbility -b $BUNDLE" >/dev/null 2>&1
log "app start (W0 baseline, smo_m59a=0)"
START=$(date +%s); PEAK=0; DEAD=""; FIRST=""; DONE=" "
# ---- 固定时刻的动作表 ----
#   W0: t=90 arm→t=140/160 拉, t=125..151 PPM→拉, t=165 shot
#   W1: t=185 切 19; t=200 arm→250/270 拉, t=225..251 PPM, t=265 shot, t=285 抓 [m59a-*]
#   W2: t=320 切 23; t=350 arm→400/420 拉, t=360..386 PPM, t=400 shot, t=430 抓日志
#   W3: t=455 切 31; t=490 arm→540/560 拉, t=495..521 PPM, t=535 shot, t=565 抓日志
#   t=620 起稳定性尾巴；t=DUR 收尾
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
  # passdump 窗口
  for WT in 90 200 350 490; do
    if [ "$EL" -ge "$WT" ] && [ "$EL" -lt $((WT+6)) ]; then
      case " $DONE " in *" w$WT "*) ;; *) DONE="$DONE w$WT"; log "passdump arm @${EL}s (window t=$WT)"; h shell "rm -f $DUMP/pass_*" >/dev/null 2>&1; knob smo_m48a_passdump 1;; esac
    fi
    # arm→落盘实测约 50 s（模拟器一帧慢），所以 pull 放在 +50 与 +70 两次，前者拿到就算
    if [ "$EL" -ge $((WT+50)) ] && [ "$EL" -lt $((WT+54)) ]; then
      case " $DONE " in *" p$WT "*) ;; *) DONE="$DONE p$WT"; knob smo_m48a_passdump 0; pull "dump_w$WT" 'pass_*';; esac
    fi
    if [ "$EL" -ge $((WT+70)) ] && [ "$EL" -lt $((WT+74)) ]; then
      case " $DONE " in *" q$WT "*) ;; *) DONE="$DONE q$WT"; pull "dump_w${WT}b" 'pass_*';; esac
    fi
  done
  # presented PPM（整屏 nearwhite/mean）
  for PT in 125 225 360 495; do
    if [ "$EL" -ge "$PT" ] && [ "$EL" -lt $((PT+4)) ] && [ -z "${SV:-}" ]; then SV=1; SVT=$PT; log "m47a_save=2 @${EL}s (window t=$PT)"; knob smo_m47a_save 2; fi
    if [ "$EL" -ge $((PT+26)) ] && [ "$EL" -lt $((PT+30)) ] && [ -n "${SV:-}" ] && [ "$SVT" = "$PT" ]; then SVC=$PT; knob smo_m47a_save 0; log "m47a_save off (t=$PT)"; pull "pres_$PT" 'm47a_pres_*'; fi
  done
  # 窗口切换
  if [ "$EL" -ge 185 ] && [ -z "${W1:-}" ]; then W1=1; log "== W1: smo_m59a=19 (2.1 probe + slab alias + sample id)"; knob smo_m59a 19; fi
  if [ "$EL" -ge 320 ] && [ -z "${W2:-}" ]; then W2=1; log "== W2: smo_m59a=$W2BIT (fix ①: keep the bytes)"; knob smo_m59a "$W2BIT"; fi
  if [ "$EL" -ge 455 ] && [ -z "${W3:-}" ]; then W3=1; log "== W3: smo_m59a=$W3BIT (fix ①②: keep + copy-sync)"; knob smo_m59a "$W3BIT"; fi
  case "$EL" in 165|265|400|535|600) shotat_now=$EL;; *) shotat_now="";; esac
  [ -n "$shotat_now" ] && { case " $DONE " in *" s$shotat_now "*) ;; *) DONE="$DONE s$shotat_now"; shot "$shotat_now";; esac; }
  # 进度取样：把新出现的 [m59a-*] / [m55a-stat] 抓进 run.log（不做整读）
  if [ "$EL" -ge 270 ] && [ -z "${GRAB1:-}" ]; then GRAB1=1
    log "--- grab W1 [m59a-*] ---"
    h shell "grep -a -E 'm59a-|m57a-alias|m57a-slab|m55a-stat' $LOG 2>/dev/null | tail -46" | tr -d '\r' | cut -c1-260 | sed 's/^/  | /' | tee -a "$OUT/run.log"; fi
  if [ "$EL" -ge 420 ] && [ -z "${GRAB2:-}" ]; then GRAB2=1
    log "--- grab W2 [m59a-*] ---"
    h shell "grep -a -E 'm59a-|m57a-alias|m57a-slab|m55a-stat' $LOG 2>/dev/null | tail -46" | tr -d '\r' | cut -c1-260 | sed 's/^/  | /' | tee -a "$OUT/run.log"; fi
  if [ "$EL" -ge 555 ] && [ -z "${GRAB3:-}" ]; then GRAB3=1
    log "--- grab W3 [m59a-*] ---"
    h shell "grep -a -E 'm59a-|m55a-stat' $LOG 2>/dev/null | tail -40" | tr -d '\r' | cut -c1-260 | sed 's/^/  | /' | tee -a "$OUT/run.log"; fi
  if [ $((EL % 80)) -eq 0 ] && [ "$EL" -gt 0 ]; then
    case " $DONE " in *" g$EL "*) ;; *) DONE="$DONE g$EL"
      SNAP=$(h shell "tail -c 6000 $LOG 2>/dev/null | grep -a -E 'm59a-|m55a-stat' | tail -2" | tr -d '\r' | cut -c1-190)
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
pull pres_final 'm47a_pres_*'
for f in $(lst); do case "$f" in m55a_*.ppm|m47a_pres_*.ppm) h file recv "$DUMP/$f" "$OUT/screens/$f" >/dev/null 2>&1;; esac; done
h shell "cp $LOG /data/local/tmp/${TAG}_vk_boot.log 2>/dev/null; chmod 644 /data/local/tmp/${TAG}_vk_boot.log" >/dev/null 2>&1
h file recv "/data/local/tmp/${TAG}_vk_boot.log" "$OUT/vk_boot.log" >/dev/null 2>&1 && log "vk_boot.log $(wc -c < "$OUT/vk_boot.log")B"
for k in m59a-pair m59a-stat m59a-corr m59a-samp m59a-sync m59a-prep m59a-knob 'm59a]' m57a-alias m57a-slab m57a-clear m55a-stat 'pass dump '; do n=$(grep -ac "$k" "$OUT/vk_boot.log" 2>/dev/null); log "count '$k' = ${n:-0}"; done
knob smo_m59a 0; knob smo_m57a 0; knob smo_m56a 0; knob smo_m55a 0; knob smo_m48a_passdump 0; knob smo_m50a_pdskip 0; knob smo_m47a_save 0
log "emu_rss_after=$(ps aux | grep 'tools/emulator/Emulator' | grep -v grep | awk '{print int($6/1024)"MiB"}' | head -1)"
h shell "aa force-stop $BUNDLE" >/dev/null 2>&1
log "done (app force-stopped)"
