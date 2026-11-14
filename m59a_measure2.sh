#!/bin/bash
# M59A 测量（一个 app 进程：别名从启动就开，否则 A/B 两张图在关态下已建成、后面再开也不会重新绑 slab）
#   W19 t=0..   smo_m59a=19 = 2.1 探针 + slab 真别名 + 采样身份（决定窗口：B 到底读到什么）
#   W23 t=165  smo_m59a=23 = 19|bit2 保字节（跳过 vk_tex_prepare 零填充 + 跳过整块 render-pass clear）
#   W31 t=250  smo_m59a=31 = 23|bit3 A→B copy 同步
# 每窗口：passdump（pdskip=40）+ presented PPM（整屏 nearwhite/mean）+ 截图
# usage: bash m59a_measure2.sh <tag> <dur>
set -uo pipefail
TAG="${1:-m59a_B}"; DUR="${2:-510}"
HDC="/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc"
TGT="127.0.0.1:5555"; BUNDLE="com.vibecc.smo"
ROOT="/Users/zpp/VibeCoding/switch2hos"; OUT="$ROOT/logs/m59a/$TAG"; BAK="/tmp/m59a_ev/$TAG"
FILES="/data/app/el2/100/base/com.vibecc.smo/files"; DUMP="$FILES/m45a_dump"; LOG="$FILES/vk_boot.log"
mkdir -p "$OUT/screens" "$BAK"
log(){ printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*" | tee -a "$OUT/run.log"; }
h(){ "$HDC" -t "$TGT" "$@"; }
shot(){ h shell "rm -f /data/local/tmp/${TAG}_$1.jpeg; snapshot_display -f /data/local/tmp/${TAG}_$1.jpeg" >/dev/null 2>&1
        h file recv "/data/local/tmp/${TAG}_$1.jpeg" "$OUT/screens/t$1.jpeg" >/dev/null 2>&1
        cp -f "$OUT/screens/t$1.jpeg" "$BAK/" 2>/dev/null; }
knob(){ h shell "echo $2 > $FILES/$1" >/dev/null 2>&1; G=$(h shell "cat $FILES/$1 2>/dev/null" | tr -d '\r\n '); [ "$G" = "$2" ] && log "knob $1=$2 ok" || log "X knob $1=$2 readback=[$G]"; }
lst(){ h shell "ls $DUMP 2>/dev/null" | tr -d '\r'; }
pull(){ local n=0 f B; mkdir -p "$OUT/$1"
  for f in $(lst); do
    case "$f" in $2) ;; *) continue;; esac
    B=$(h shell "stat -c %s $DUMP/$f 2>/dev/null" | tr -dc '0-9'); B=${B:-0}; [ "$B" -lt 100 ] && continue
    h file recv "$DUMP/$f" "$OUT/$1/$f" >/dev/null 2>&1 && n=$((n+1))
  done; log "pulled $n -> $OUT/$1"; cp -R "$OUT/$1" "$BAK/" 2>/dev/null; }
once(){ case " $DONE " in *" $1 "*) return 1;; *) DONE="$DONE $1"; return 0;; esac; }
grab(){ log "--- grab $1 ($2) ---"
  h shell "grep -a -E '$3' $LOG 2>/dev/null | tail -${4:-30}" | tr -d '\r' | cut -c1-300 | sed 's/^/  | /' | tee -a "$OUT/run.log"; }

: > "$OUT/run.log"
log "=== M59A measure2 tag=$TAG dur=${DUR}s (W19 → W23 → W31, alias from app start)"
h shell "echo ok" >/dev/null 2>&1 || { log "X target offline"; exit 1; }
log "emu_rss_before=$(ps aux | grep 'tools/emulator/Emulator' | grep -v grep | awk '{print int($6/1024)"MiB"}' | head -1)"
for i in 1 2 3; do P=$(h shell "pidof $BUNDLE" | tr -d '[:space:]'); [ -z "$P" ] && break; log "force-stop pid=$P"; h shell "aa force-stop $BUNDLE" >/dev/null 2>&1; sleep 3; done
h shell "rm -f $DUMP/pass_* $DUMP/m55a_* $DUMP/m47a_pres_* 2>/dev/null" >/dev/null 2>&1
knob smo_m45a 1; knob smo_m47a 1; knob smo_m47a_save 0; knob smo_m47a_trace 0
knob smo_m48a 0; knob smo_m48a_passdump 0; knob smo_m48a_rb10 0; knob smo_m48a_rbpool 0
knob smo_m45a_dump_at 99999; knob smo_m45a_dump_frames 1
knob smo_m50a_trace_pass 0; knob smo_m50a_ubo_pass 0; knob smo_m50a_sh_pass 0; knob smo_m50a_pddepth 0; knob smo_m50a_watch 0
knob smo_m50a_pdskip 40
knob smo_m55a 1; knob smo_m56a 0; knob smo_m57a 0
knob smo_m59a 19
knob DM_VK2_SELFTEST 0; knob DM_VK2_ALIAS 0
h shell ": > $LOG" >/dev/null 2>&1
h shell "hilog -r" >/dev/null 2>&1
CF0=$(h shell "ls /data/log/faultlog/faultlogger/ 2>/dev/null" | tr -d '\r')
h shell "power-shell wakeup" >/dev/null 2>&1
h shell "power-shell timeout -o 1800000" >/dev/null 2>&1
h shell "aa start -a EntryAbility -b $BUNDLE" >/dev/null 2>&1
log "app start (smo_m59a=19: alias + 2.1 probe + sample id)"
START=$(date +%s); PEAK=0; DEAD=""; FIRST=""; DONE=" "
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
  # ---- dump 窗口 ----
  if [ "$EL" -ge 95 ] && once w19; then log "dump arm @${EL}s (W19)"; h shell "rm -f $DUMP/pass_*" >/dev/null 2>&1; knob smo_m48a_passdump 1; fi
  if [ "$EL" -ge 150 ] && once p19; then knob smo_m48a_passdump 0; pull dump_w19 'pass_*'; fi
  if [ "$EL" -ge 185 ] && once w23; then log "dump arm @${EL}s (W23)"; h shell "rm -f $DUMP/pass_*" >/dev/null 2>&1; knob smo_m48a_passdump 1; fi
  if [ "$EL" -ge 240 ] && once p23; then knob smo_m48a_passdump 0; pull dump_w23 'pass_*'; fi
  if [ "$EL" -ge 270 ] && once w31; then log "dump arm @${EL}s (W31)"; h shell "rm -f $DUMP/pass_*" >/dev/null 2>&1; knob smo_m48a_passdump 1; fi
  if [ "$EL" -ge 325 ] && once p31; then knob smo_m48a_passdump 0; pull dump_w31 'pass_*'; fi
  # ---- presented PPM ----
  if [ "$EL" -ge 118 ] && once s19; then knob smo_m47a_save 2; fi
  if [ "$EL" -ge 143 ] && once e19; then knob smo_m47a_save 0; pull pres_w19 'm47a_pres_*'; fi
  if [ "$EL" -ge 208 ] && once s23; then knob smo_m47a_save 2; fi
  if [ "$EL" -ge 233 ] && once e23; then knob smo_m47a_save 0; pull pres_w23 'm47a_pres_*'; fi
  if [ "$EL" -ge 293 ] && once s31; then knob smo_m47a_save 2; fi
  if [ "$EL" -ge 318 ] && once e31; then knob smo_m47a_save 0; pull pres_w31 'm47a_pres_*'; fi
  # ---- 窗口切换 ----
  if [ "$EL" -ge 165 ] && once x23; then log "== W23: smo_m59a=23 (fix① 保字节)"; knob smo_m59a 23; fi
  if [ "$EL" -ge 250 ] && once x31; then log "== W31: smo_m59a=31 (fix①② 保字节 + copy 同步)"; knob smo_m59a 31; fi
  # ---- 截图 ----
  for ST in 110 200 300 365 430; do
    if [ "$EL" -ge "$ST" ] && once s$ST; then shot "$ST"; fi
  done
  # ---- 抓日志 ----
  if [ "$EL" -ge 160 ] && once g1; then grab "W19 决定性一刀" "2.1" 'm59a-pair|m59a-stat|m59a-corr|m59a-samp|m57a-alias|m57a-slab|m57a-clear|m59a-prep|m59a-sync' 40; fi
  if [ "$EL" -ge 248 ] && once g2; then grab "W23 保字节" "23" 'm59a-prep|m59a-sync|m59a-stat|m57a-clear|m59a-pair' 24; fi
  if [ "$EL" -ge 340 ] && once g3; then grab "W31 保字节+同步" "31" 'm59a-prep|m59a-sync|m59a-stat|m57a-clear|m59a-pair' 24; fi
  [ $((EL % 80)) -eq 0 ] && [ "$EL" -gt 0 ] && if once g$EL; then
      SNAP=$(h shell "tail -c 6000 $LOG 2>/dev/null | grep -a -E 'm59a-|m55a-stat' | tail -2" | tr -d '\r' | cut -c1-190)
      log "PROGRESS t=${EL}s rss=$((RSSKB/1024))MiB :: $SNAP"; fi
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
h shell "cp $LOG /data/local/tmp/${TAG}_vk_boot.log 2>/dev/null; chmod 644 /data/local/tmp/${TAG}_vk_boot.log" >/dev/null 2>&1
h file recv "/data/local/tmp/${TAG}_vk_boot.log" "$OUT/vk_boot.log" >/dev/null 2>&1 && log "vk_boot.log $(wc -c < "$OUT/vk_boot.log")B"
cp -f "$OUT/vk_boot.log" "$BAK/" 2>/dev/null
for f in $(lst); do case "$f" in m55a_*.ppm|m47a_pres_*.ppm) h file recv "$DUMP/$f" "$OUT/screens/$f" >/dev/null 2>&1;; esac; done
for k in m59a-pair m59a-stat m59a-corr m59a-samp m59a-sync m59a-prep m57a-alias m57a-slab m57a-clear m55a-stat 'pass dump '; do n=$(grep -ac "$k" "$OUT/vk_boot.log" 2>/dev/null); log "count '$k' = ${n:-0}"; done
knob smo_m59a 0; knob smo_m57a 0; knob smo_m56a 0; knob smo_m55a 0; knob smo_m48a_passdump 0; knob smo_m50a_pdskip 0; knob smo_m47a_save 0
log "emu_rss_after=$(ps aux | grep 'tools/emulator/Emulator' | grep -v grep | awk '{print int($6/1024)"MiB"}' | head -1)"
h shell "aa force-stop $BUNDLE" >/dev/null 2>&1
log "done (app force-stopped)"
