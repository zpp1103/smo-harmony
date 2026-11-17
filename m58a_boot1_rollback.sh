#!/bin/bash
# M58A boot1：回退验证（smo_* 全 0 + DM_VK2_SELFTEST=1 → nvn_vk2_tri_1.ppm）+ 让新 build 的构造期种出 DM_VK2_ALIAS 开关文件
set -uo pipefail
HDC="/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc"
TGT="127.0.0.1:5555"; B="com.vibecc.smo"
F="/data/app/el2/100/base/com.vibecc.smo/files"; STO="/data/storage/el2/base/files"
OUT=/Users/zpp/VibeCoding/switch2hos/logs/m58a/rollback; mkdir -p "$OUT"
LOG="$F/vk_boot.log"
h(){ "$HDC" -t "$TGT" "$@"; }
log(){ printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*" | tee -a "$OUT/run.log"; }
: > "$OUT/run.log"

for i in 1 2 3; do P=$(h shell "pidof $B" | tr -d '[:space:]'); [ -z "$P" ] && break; h shell "aa force-stop $B" >/dev/null 2>&1; sleep 3; done
# 1) 所有 smo_* 归 0 并回读
BADS=0
for k in smo_m45a smo_m47a smo_m47a_save smo_m47a_trace smo_m48a smo_m48a_passdump smo_m48a_rb10 smo_m48a_rbpool smo_m48a_srgb \
         smo_m50a_pdskip smo_m50a_trace_pass smo_m50a_ubo_pass smo_m50a_sh_pass smo_m50a_pddepth smo_m50a_watch smo_m55a smo_m56a smo_m57a; do
  h shell "echo 0 > $F/$k" >/dev/null 2>&1
  G=$(h shell "cat $F/$k 2>/dev/null" | tr -d '\r\n ')
  [ "$G" = "0" ] || { BADS=$((BADS+1)); log "X knob $k=[$G]"; }
done
log "smo_* all-zero check: bad=$BADS"
h shell "echo 0 > $F/DM_VK2_ALIAS" >/dev/null 2>&1
log "DM_VK2_ALIAS before boot=[$(h shell "cat $F/DM_VK2_ALIAS 2>/dev/null" | tr -d '\r\n ')]"
h shell "rm -f $F/nvn_vk2_tri_*.ppm" >/dev/null 2>&1
h shell "echo 1 > $F/DM_VK2_SELFTEST" >/dev/null 2>&1; h shell "echo 1 > $STO/DM_VK2_SELFTEST" >/dev/null 2>&1
log "DM_VK2_SELFTEST=[$(h shell "cat $F/DM_VK2_SELFTEST" | tr -d '\r\n ')] DM_VK_SHOT=[$(h shell "cat $F/DM_VK_SHOT" | tr -d '\r\n ')]"
h shell ": > $LOG" >/dev/null 2>&1
h shell "power-shell wakeup" >/dev/null 2>&1
h shell "aa start -a EntryAbility -b $B" >/dev/null 2>&1
log "app start (rollback boot)"
for i in $(seq 1 40); do
  sleep 2
  if h shell "grep -ac 'shot:' $LOG 2>/dev/null" | tr -dc '0-9' | grep -qv '^0$'; then log "shot line after $((i*2))s"; break; fi
done
sleep 18   # shot_copy_later waits 15 s then copies into $F
for f in $(h shell "ls $F/nvn_vk2_tri_*.ppm 2>/dev/null" | tr -d '\r'); do
  BN=$(basename "$f"); h file recv "$f" "$OUT/$BN" >/dev/null 2>&1 && log "pulled $BN $(stat -f%z "$OUT/$BN" 2>/dev/null)B md5=$(md5 -q "$OUT/$BN" 2>/dev/null)"
done
h shell "grep -a 'selftest start\|shot:\|copy-back\|selftest done' $LOG | tail -6" | tr -d '\r' | sed 's/^/  log: /' | tee -a "$OUT/run.log"
h shell "aa force-stop $B" >/dev/null 2>&1
h shell "cp $LOG /data/local/tmp/m58a_boot1.log; chmod 644 /data/local/tmp/m58a_boot1.log" >/dev/null 2>&1
h file recv "/data/local/tmp/m58a_boot1.log" "$OUT/vk_boot.log" >/dev/null 2>&1 && log "vk_boot.log $(wc -c < "$OUT/vk_boot.log")B"
h shell "echo 0 > $F/DM_VK2_SELFTEST" >/dev/null 2>&1; h shell "echo 0 > $STO/DM_VK2_SELFTEST" >/dev/null 2>&1
log "DM_VK2_ALIAS after boot=[$(h shell "cat $F/DM_VK2_ALIAS 2>/dev/null" | tr -d '\r\n ')] (empty/nonexistent = seeded 0)"
for f in "$OUT"/nvn_vk2_tri_*.ppm; do [ -f "$f" ] || continue
  /usr/bin/env python3 /Users/zpp/VibeCoding/switch2hos/tools/m42a_ppm_check.py "$f" 2>&1 | tail -3 | tee -a "$OUT/run.log"; done
log "done"
