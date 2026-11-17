#!/bin/bash
# M57A 回退验收：所有 smo_* 归 0 + DM_VK2_SELFTEST=1 → nvn_vk2_tri_1.ppm 的 md5 与 max rgb error
set -uo pipefail
HDC="/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc"
TGT="127.0.0.1:5555"; BUNDLE="com.vibecc.smo"
FILES="/data/app/el2/100/base/com.vibecc.smo/files"; STO="/data/storage/el2/base/files"
OUT=/Users/zpp/VibeCoding/switch2hos/logs/m57a/rollback; mkdir -p "$OUT"
h(){ "$HDC" -t "$TGT" "$@"; }
log(){ printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*" | tee -a "$OUT/run.log"; }
: > "$OUT/run.log"
for i in 1 2 3; do P=$(h shell "pidof $BUNDLE" | tr -d '[:space:]'); [ -z "$P" ] && break; h shell "aa force-stop $BUNDLE" >/dev/null 2>&1; sleep 3; done
for k in smo_m45a smo_m47a smo_m47a_save smo_m47a_trace smo_m48a smo_m48a_passdump smo_m48a_rb10 smo_m48a_rbpool smo_m48a_srgb \
         smo_m50a_pdskip smo_m50a_trace_pass smo_m50a_ubo_pass smo_m50a_sh_pass smo_m50a_pddepth smo_m50a_watch smo_m55a smo_m56a smo_m57a; do
  h shell "echo 0 > $FILES/$k" >/dev/null 2>&1; h shell "echo 0 > $STO/$k" >/dev/null 2>&1
  G=$(h shell "cat $FILES/$k 2>/dev/null" | tr -d '\r\n '); log "knob $k=[$G]"
done
h shell "rm -f $STO/nvn_vk2_tri_*.ppm" >/dev/null 2>&1
h shell "echo 1 > $FILES/DM_VK2_SELFTEST" >/dev/null 2>&1
h shell "echo 1 > $STO/DM_VK2_SELFTEST" >/dev/null 2>&1
log "DM_VK2_SELFTEST=[$(h shell "cat $FILES/DM_VK2_SELFTEST" | tr -d '\r\n ')]"
h shell "power-shell wakeup" >/dev/null 2>&1
h shell "aa start -a EntryAbility -b $BUNDLE" >/dev/null 2>&1
for i in $(seq 1 30); do sleep 2; N=$(h shell "ls $STO/nvn_vk2_tri_*.ppm 2>/dev/null | wc -l" | tr -dc '0-9'); [ "${N:-0}" -ge 1 ] && { log "ppm appears after $((i*2))s (n=$N)"; break; }; done
sleep 3
for f in $(h shell "ls $STO/nvn_vk2_tri_*.ppm 2>/dev/null" | tr -d '\r'); do
  B=$(basename "$f"); h file recv "$f" "$OUT/$B" >/dev/null 2>&1 && log "pulled $B $(stat -f%z "$OUT/$B" 2>/dev/null)B md5=$(md5 -q "$OUT/$B" 2>/dev/null)"
done
h shell "aa force-stop $BUNDLE" >/dev/null 2>&1
h shell "echo 0 > $FILES/DM_VK2_SELFTEST" >/dev/null 2>&1
h shell "echo 0 > $STO/DM_VK2_SELFTEST" >/dev/null 2>&1
log "selftest knob back to 0"
for f in "$OUT"/nvn_vk2_tri_*.ppm; do [ -f "$f" ] || continue
  /usr/bin/env python3 /Users/zpp/VibeCoding/switch2hos/tools/m42a_ppm_check.py "$f" 2>&1 | tail -4 | tee -a "$OUT/run.log"; done
log "done"
