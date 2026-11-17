#!/bin/bash
# M58A boot2：一次进程内做两件事
#   ① 判定 A（构造期 DM_VK2_ALIAS=1：两张 1024² D32 绑同一 VkDeviceMemory，offset 0 / 0x100000，clear+copy 后互读）
#   ② 判定 B（游戏起来后用现成探针在 800×450 帧型钉 pass 50 的采样对象）：smo_m55a=1 / smo_m50a_watch / smo_m50a_trace_pass / passdump(dpeth)
# 纪律：只碰 127.0.0.1:5555；收尾开关全 0；不看退出码、只看日志与字节。
set -uo pipefail
HDC="/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc"
TGT="127.0.0.1:5555"; B="com.vibecc.smo"
F="/data/app/el2/100/base/com.vibecc.smo/files"; STO="/data/storage/el2/base/files"
OUT=/Users/zpp/VibeCoding/switch2hos/logs/m58a/run_ab; mkdir -p "$OUT"
LOG="$F/vk_boot.log"; DUR=430
h(){ "$HDC" -t "$TGT" "$@"; }
log(){ printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*" | tee -a "$OUT/run.log"; }
knob(){ h shell "echo $2 > $F/$1" >/dev/null 2>&1; G=$(h shell "cat $F/$1 2>/dev/null" | tr -d '\r\n '); [ "$G" = "$2" ] && log "knob $1=$2 ok" || log "X knob $1=$2 readback=[$G]"; }
: > "$OUT/run.log"

log "=== M58A boot2 (A + B), dur=${DUR}s"
h shell "echo ok" >/dev/null 2>&1 || { log "X target offline"; exit 1; }
log "emu_rss_before=$(ps aux | grep 'tools/emulator/Emulator' | grep -v grep | awk '{print int($6/1024)"MiB"}' | head -1)"
for i in 1 2 3; do P=$(h shell "pidof $B" | tr -d '[:space:]'); [ -z "$P" ] && break; log "force-stop pid=$P"; h shell "aa force-stop $B" >/dev/null 2>&1; sleep 3; done

# ---- 开关：A 用 DM_VK2_ALIAS=1；B 的探针先全 0，起来后再 arm ----
knob DM_VK2_SELFTEST 0
knob DM_VK2_ALIAS 1
knob smo_m45a 1
knob smo_m47a 1
knob smo_m48a 0; knob smo_m48a_passdump 0; knob smo_m47a_save 0
knob smo_m50a_pdskip 40; knob smo_m50a_trace_pass 0; knob smo_m50a_ubo_pass 0
knob smo_m50a_sh_pass 0; knob smo_m50a_pddepth 0; knob smo_m50a_watch 0
knob smo_m55a 1; knob smo_m56a 0; knob smo_m57a 0
h shell "rm -f $F/m45a_dump/pass_* $F/m45a_dump/m55a_* 2>/dev/null" >/dev/null 2>&1
h shell ": > $LOG" >/dev/null 2>&1
h shell "hilog -r" >/dev/null 2>&1
CF0=$(h shell "ls /data/log/faultlog/faultlogger/ 2>/dev/null" | tr -d '\r')
h shell "power-shell wakeup" >/dev/null 2>&1
h shell "power-shell timeout -o 1800000" >/dev/null 2>&1
h shell "aa start -a EntryAbility -b $B" >/dev/null 2>&1
START=$(date +%s); log "app start => judgment A at load"
ALDONE=0
while :; do
  EL=$(( $(date +%s) - START ))
  if [ "$ALDONE" = 0 ] && [ $((EL % 4)) -eq 0 ]; then
    if h shell "grep -ac 'm58a-al' $LOG 2>/dev/null" | tr -dc '0-9' | grep -qv '^0$'; then
      ALDONE=$EL; log "judgment A lines appear @${EL}s"
      h shell "grep -a 'm58a-al' $LOG" | tr -d '\r' | sed 's/^/  A| /' | tee -a "$OUT/run.log"
    fi
  fi
  [ "$EL" -ge 95 ] && break
  sleep 2
done
log "judgment A capture end @${EL}s (aldone=${ALDONE}s)"

# ---- 判定 B：800×450 帧型里钉 pass 50 的采样对象 ----
log "B phase: arm trace/passdump/watch"
knob smo_m50a_watch "628800#40"
knob smo_m50a_trace_pass 51
WP1=$(( $(date +%s) - START ))
PD=0; WT2=0; PDONE=0; WSTOP=0
while :; do
  EL=$(( $(date +%s) - START ))
  # watch 628800 for ~50 s, then 228600 for ~50 s
  if [ "$WT2" = 0 ] && [ "$EL" -ge $((WP1+50)) ]; then WT2=1; log "watch -> 228600 @${EL}s"; knob smo_m50a_watch "228600#40"; fi
  if [ "$WSTOP" = 0 ] && [ "$WT2" = 1 ] && [ "$EL" -ge $((WP1+100)) ]; then WSTOP=1; log "watch off @${EL}s"; knob smo_m50a_watch 0; fi
  # 一次带深度的 passdump（40..96），约 t=200 开
  if [ "$PD" = 0 ] && [ "$EL" -ge 200 ]; then PD=1; log "passdump+depth arm @${EL}s"; knob smo_m50a_pddepth 1; knob smo_m48a_passdump 3; knob smo_m45a_dump_at 99999; fi
  if [ "$PD" = 1 ] && [ "$PDONE" = 0 ] && [ "$EL" -ge 216 ]; then PDONE=1; log "passdump off @${EL}s"; knob smo_m48a_passdump 0; knob smo_m50a_pddepth 0; fi
  P=$(h shell "pidof $B" | tr -d '[:space:]')
  if [ -z "$P" ]; then log "t=${EL}s pid=<dead>"; break; fi
  if [ $((EL % 60)) -eq 0 ] && [ "$EL" -gt 0 ]; then
    SNAP=$(h shell "tail -c 3000 $LOG 2>/dev/null | grep -a -E 'm55a-stat|watch:|m58a-al' | tail -2" | tr -d '\r' | cut -c1-150)
    log "PROGRESS t=${EL}s pid=$P :: $SNAP"
  fi
  [ "$EL" -ge "$DUR" ] && { log "t=${EL}s dur"; break; }
  sleep 2
done
ALIVE=$(( $(date +%s) - START ))
log "alive=${ALIVE}s"
NEW=0
for cf in $(h shell "ls /data/log/faultlog/faultlogger/ 2>/dev/null" | tr -d '\r'); do
  grep -qxF "$cf" <<<"$CF0" && continue; NEW=$((NEW+1)); log "!! NEW faultlog $cf"; done
log "new_faultlog_count=$NEW"
h shell "cp $LOG /data/local/tmp/m58a_ab.log; chmod 644 /data/local/tmp/m58a_ab.log" >/dev/null 2>&1
h file recv "/data/local/tmp/m58a_ab.log" "$OUT/vk_boot.log" >/dev/null 2>&1 && log "vk_boot.log $(wc -c < "$OUT/vk_boot.log")B"
log "A lines in pulled log: $(grep -ac 'm58a-al' "$OUT/vk_boot.log" 2>/dev/null)"
log "B: watch lines $(grep -ac 'watch:' "$OUT/vk_boot.log" 2>/dev/null), m55a-stat $(grep -ac 'm55a-stat' "$OUT/vk_boot.log" 2>/dev/null), m55a-dump $(grep -ac 'm55a-dump' "$OUT/vk_boot.log" 2>/dev/null)"
grep -a 'pass dump [0-9][0-9] is the submission' "$OUT/vk_boot.log" | tr -d '\r' | tail -50 > "$OUT/passdump_map.txt"
h shell "aa force-stop $B" >/dev/null 2>&1
knob DM_VK2_ALIAS 0; knob smo_m55a 0; knob smo_m50a_watch 0; knob smo_m50a_trace_pass 0
knob smo_m48a_passdump 0; knob smo_m50a_pddepth 0; knob smo_m50a_pdskip 0; knob smo_m45a 0; knob smo_m47a 0
log "emu_rss_after=$(ps aux | grep 'tools/emulator/Emulator' | grep -v grep | awk '{print int($6/1024)"MiB"}' | head -1)"
log "done"
