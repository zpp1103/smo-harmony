#!/bin/bash
# M59A boot1：回退验证 + 种新开关 smo_m59a（默认 0）
#   纪律：只碰 127.0.0.1:5555；装 HAP 后已重投 bundle（size 191,472,541 / md5 28c4914e…）。
#   判据：smo_* 全 0（含新种的 smo_m59a）→ nvn_vk2_tri_1.ppm md5 4ed86a580bb785e49c4a4990c1f8aed6 + tools/m42a_ppm_check.py max rgb error == 0
set -uo pipefail
HDC="/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc"
TGT="127.0.0.1:5555"; B="com.vibecc.smo"
F="/data/app/el2/100/base/com.vibecc.smo/files"; D="$F/m45a_dump"
ROOT=/Users/zpp/VibeCoding/switch2hos; OUT="$ROOT/logs/m59a/boot1"; mkdir -p "$OUT"
h(){ "$HDC" -t "$TGT" "$@"; }
log(){ printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*" | tee -a "$OUT/run.log"; }
knob(){ h shell "echo $2 > $F/$1" >/dev/null 2>&1; G=$(h shell "cat $F/$1 2>/dev/null" | tr -d '\r\n '); [ "$G" = "$2" ] && log "knob $1=$2 ok" || log "X knob $1=$2 readback=[$G]"; }
: > "$OUT/run.log"
log "=== M59A boot1 (rollback + seed smo_m59a)"
h shell "echo ok" >/dev/null 2>&1 || { log "X target offline"; exit 1; }
log "emu_rss=$(ps aux | grep 'tools/emulator/Emulator' | grep -v grep | awk '{print int($6/1024)"MiB"}' | head -1)"
for i in 1 2 3; do P=$(h shell "pidof $B" | tr -d '[:space:]'); [ -z "$P" ] && break; log "force-stop pid=$P"; h shell "aa force-stop $B" >/dev/null 2>&1; sleep 3; done
knob smo_m45a 1; knob smo_m47a 1; knob smo_m47a_save 0; knob smo_m47a_trace 0
knob smo_m48a 0; knob smo_m48a_passdump 0; knob smo_m48a_rb10 0; knob smo_m48a_rbpool 0
knob smo_m48a_fmt10 0; knob smo_m48a_srgb 0; knob smo_m48a_swzlog 0
knob smo_m45a_dump_at 99999; knob smo_m45a_dump_frames 1
knob smo_m50a_trace_pass 0; knob smo_m50a_ubo_pass 0; knob smo_m50a_sh_pass 0; knob smo_m50a_pddepth 0; knob smo_m50a_watch 0
knob smo_m50a_pdskip 0
knob smo_m55a 0; knob smo_m56a 0; knob smo_m57a 0
knob DM_VK2_ALIAS 0; knob DM_VK2_SELFTEST 1
h shell "rm -f $D/nvn_vk2_tri_1.ppm" >/dev/null 2>&1
h shell ": > $F/vk_boot.log" >/dev/null 2>&1
CF0=$(h shell "ls /data/log/faultlog/faultlogger/ 2>/dev/null" | tr -d '\r')
h shell "power-shell wakeup" >/dev/null 2>&1
h shell "power-shell timeout -o 1800000" >/dev/null 2>&1
h shell "aa start -a EntryAbility -b $B" >/dev/null 2>&1
START=$(date +%s); log "app start"
for i in $(seq 1 50); do
  sleep 2
  SZ=$(h shell "stat -c %s $D/nvn_vk2_tri_1.ppm 2>/dev/null" | tr -dc '0-9'); SZ=${SZ:-0}
  if [ "$SZ" -ge 1000000 ]; then log "tri ppm written: ${SZ}B after $(( $(date +%s) - START ))s"; break; fi
done
sleep 10
log "--- new faultlog ---"
for cf in $(h shell "ls /data/log/faultlog/faultlogger/ 2>/dev/null" | tr -d '\r'); do grep -qxF "$cf" <<<"$CF0" || log "!! NEW faultlog $cf"; done
log "--- smo_* readback ---"
for k in smo_m45a smo_m47a smo_m47a_save smo_m47a_trace smo_m48a smo_m48a_passdump smo_m48a_rb10 smo_m48a_rbpool smo_m48a_fmt10 smo_m48a_srgb smo_m48a_swzlog \
         smo_m45a_dump_at smo_m45a_dump_frames smo_m50a_trace_pass smo_m50a_ubo_pass smo_m50a_sh_pass smo_m50a_pddepth smo_m50a_watch smo_m50a_pdskip \
         smo_m55a smo_m56a smo_m57a smo_m59a DM_VK2_ALIAS DM_VK2_SELFTEST; do
  V=$(h shell "cat $F/$k 2>/dev/null" | tr -d '\r\n '); log "knob $k = [${V:-MISSING}]"
done
h shell "ls -l $D/nvn_vk2_tri_1.ppm" | tr -d '\r' | tee -a "$OUT/run.log"
h file recv "$D/nvn_vk2_tri_1.ppm" "$OUT/nvn_vk2_tri_1.ppm" >/dev/null 2>&1
MD5=$(md5 -q "$OUT/nvn_vk2_tri_1.ppm" 2>/dev/null); SZ=$(stat -f%z "$OUT/nvn_vk2_tri_1.ppm" 2>/dev/null)
log "tri ppm: ${SZ}B md5=$MD5 (want 4ed86a580bb785e49c4a4990c1f8aed6)"
knob DM_VK2_SELFTEST 0
python3 "$ROOT/tools/m42a_ppm_check.py" "$OUT/nvn_vk2_tri_1.ppm" 2>&1 | tail -5 | tee -a "$OUT/run.log"
h shell "aa force-stop $B" >/dev/null 2>&1
log "force-stop done; emu_rss_after=$(ps aux | grep 'tools/emulator/Emulator' | grep -v grep | awk '{print int($6/1024)"MiB"}' | head -1)"
log "done"
