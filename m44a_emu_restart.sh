#!/bin/bash
# M44A：模拟器冷重启（宿主 RSS > 10 GB 的硬规则要求）。只重启 127.0.0.1:5555 这台模拟器，
# 不碰真机、不 hdc kill。拉起后等 target 在线 + shell 可用 + boot 完成。
set -uo pipefail
HDC="/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc"
TGT="127.0.0.1:5555"
EMU="/Applications/DevEco-Studio.app/Contents/tools/emulator/Emulator"
log(){ printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*"; }

P=$(pgrep -f "tools/emulator/Emulator -hvd" | head -1)
echo "emu pid=$P rss=$(ps -o rss= -p "$P" | tr -d ' ')KB"
[ -z "$P" ] && { log "X no emulator pid found"; exit 1; }

log "1) force-stop app + shutdown emulator (TERM) pid=$P"
"$HDC" -t "$TGT" shell "aa force-stop com.vibecc.smo" >/dev/null 2>&1
kill -TERM "$P"
for i in $(seq 1 60); do kill -0 "$P" 2>/dev/null || break; sleep 1; done
if kill -0 "$P" 2>/dev/null; then log "2) still alive after 60s -> KILL"; kill -9 "$P"; sleep 5; fi
pgrep -f "tools/emulator/Emulator -hvd" >/dev/null && { log "X emulator still running"; exit 1; }
log "2) emulator down"

log "3) relaunch"
nohup "$EMU" -hvd "nova 16 Pro" -path /Users/zpp/.Huawei/Emulator/deployed -imageRoot /Users/zpp/Library/Huawei/Sdk \
  > /tmp/m44a_emu_restart.log 2>&1 &
sleep 5
pgrep -f "tools/emulator/Emulator -hvd" >/dev/null || { log "X relaunch failed (see /tmp/m44a_emu_restart.log)"; tail -20 /tmp/m44a_emu_restart.log; exit 1; }
log "4) waiting for target $TGT ..."
for i in $(seq 1 150); do
  if "$HDC" -t "$TGT" shell "echo ok" 2>/dev/null | grep -q ok; then log "5) shell alive after ${i}s"; break; fi
  sleep 2
done
"$HDC" -t "$TGT" shell "echo ok" >/dev/null 2>&1 || { log "X target never came back"; exit 1; }
for i in $(seq 1 60); do
  B=$("$HDC" -t "$TGT" shell "param get bootevent.boot.completed" 2>/dev/null | tr -d '\r')
  case "$B" in *true*) log "6) boot completed after ${i}s"; break;; esac
  sleep 2
done
log "7) state: $(pgrep -f 'tools/emulator/Emulator -hvd' | head -1 | xargs -I{} ps -o rss= -p {} | tr -d ' ')KB rss; disk: $("$HDC" -t "$TGT" shell 'df -h /data | tail -1' | tr -d '\r' | tr -s ' ')"
log "done"
