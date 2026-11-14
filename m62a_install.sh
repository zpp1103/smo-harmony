#!/bin/bash
# M62A：装 HAP（大包走 file send + bm install）+ 投 DMSV bundle + 复核 size/md5
set -uo pipefail
HDC="/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc"
T=192.168.0.109:41901
PKG=com.vibecc.smo
F=/data/app/el2/100/base/com.vibecc.smo/files
HAP=/tmp/m62a_build/entry.hap
SRC=/tmp/m42b/full.dmsv
DST="$F/m45a_dump/shaders.dmsv"
OUT=/Users/zpp/VibeCoding/switch2hos/logs/m62a
LOG="$OUT/install.log"; mkdir -p "$OUT"; : > "$LOG"
log(){ printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*" | tee -a "$LOG"; }
h(){ "$HDC" -t "$T" "$@"; }
sz(){ h shell "stat -c %s $1 2>/dev/null" | tr -dc '0-9'; }

log "=== m62a install: hap=$(stat -f%z "$HAP")B md5=$(md5 -q "$HAP")"
log "updateTime_before=$(h shell "bm dump -n $PKG | grep -a updateTime" | tr -d '\r' | head -1)"
log "df_data=$(h shell "df -h /data | tail -1" | tr -d '\r')"
for i in 1 2 3; do P=$(h shell "pidof $PKG" | tr -d '[:space:]'); [ -z "$P" ] && break; log "pre-stop pid=$P"; h shell "aa force-stop $PKG" >/dev/null 2>&1; sleep 3; done

log "push -> /data/local/tmp/m62a_e.hap"
T0=$(date +%s)
h file send "$HAP" /data/local/tmp/m62a_e.hap 2>&1 | tail -2 | while IFS= read -r l; do log "  send: $l"; done
log "push wall=$(( $(date +%s) - T0 ))s dev_size=$(sz /data/local/tmp/m62a_e.hap)"

log "bm install -r -p"
h shell "bm install -r -p /data/local/tmp/m62a_e.hap" 2>&1 | tr -d '\r' | sed 's/^/  bm: /' | tee -a "$LOG"
log "updateTime_after=$(h shell "bm dump -n $PKG | grep -a updateTime" | tr -d '\r' | head -1)"
h shell "rm -f /data/local/tmp/m62a_e.hap" >/dev/null 2>&1
log "bundle: send $SRC -> $DST"
T0=$(date +%s)
h file send "$SRC" "$DST" 2>&1 | tail -2 | while IFS= read -r l; do log "  send: $l"; done
log "send wall=$(( $(date +%s) - T0 ))s"
GOT=$(sz "$DST"); DM5=$(h shell "md5sum $DST 2>/dev/null" | tr -d '\r' | awk '{print $1}')
SRCM5=$(md5 -q "$SRC")
log "verify bundle: size=$GOT expect=191472541 match=$([ "$GOT" = 191472541 ] && echo YES || echo NO) md5=$DM5 match=$([ "$DM5" = "$SRCM5" ] && echo YES || echo NO)"
log "df_data_after=$(h shell "df -h /data | tail -1" | tr -d '\r')"
log "done"
