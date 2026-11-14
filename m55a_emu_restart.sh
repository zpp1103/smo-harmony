#!/bin/bash
# M55A Step 0：冷重启 127.0.0.1:5555 模拟器（上一单 RSS 9519 MiB）。
# 规矩：pkill -f "Emulator -hvd" → sleep 3 → 起 → 没 target 就等 8 s 再起一次。禁 hdc kill、不碰真机。
set -uo pipefail
HDC="/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc"
TGT="127.0.0.1:5555"
EMU="/Applications/DevEco-Studio.app/Contents/tools/emulator/Emulator"
LOG=/tmp/m55a_emu_restart.log
log(){ printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*"; }

P=$(pgrep -f "Emulator -hvd" | head -1)
echo "pre: emu pid=${P:-none} rss=$( [ -n "${P:-}" ] && ps -o rss= -p "$P" | tr -d ' ' || echo n/a )KB" | tee "$LOG"

"$HDC" list targets 2>/dev/null | tr -d '\r' | head -5

pkill -f "Emulator -hvd" 2>/dev/null
sleep 3
if pgrep -f "Emulator -hvd" >/dev/null; then
  log "still alive after TERM, wait 5s"
  sleep 5
  pgrep -f "Emulator -hvd" >/dev/null && { log "X emulator still alive"; exit 1; }
fi
log "emulator down"

start_emu(){
  log "launch: $*"
  nohup "$EMU" -hvd "nova 16 Pro" -path /Users/zpp/.Huawei/Emulator/deployed \
    -imageRoot /Users/zpp/Library/Huawei/Sdk >> "$LOG" 2>&1 &
  sleep 5
  pgrep -f "Emulator -hvd" >/dev/null
}
wait_target(){
  for i in $(seq 1 60); do
    if "$HDC" -t "$TGT" shell "echo ok" 2>/dev/null | grep -q ok; then log "shell alive after $((i*2))s"; return 0; fi
    sleep 2
  done
  return 1
}

start_emu || { log "X relaunch failed"; tail -20 "$LOG"; exit 1; }
if ! wait_target; then
  log "no target -> sleep 8s then relaunch once"
  pkill -f "Emulator -hvd"; sleep 3
  start_emu || { log "X relaunch(2) failed"; tail -20 "$LOG"; exit 1; }
  wait_target || { log "X target never came back"; exit 1; }
fi

for i in $(seq 1 60); do
  B=$("$HDC" -t "$TGT" shell "param get bootevent.boot.completed" 2>/dev/null | tr -d '\r')
  case "$B" in *true*) log "boot completed after $((i*2))s"; break;; esac
  sleep 2
done
NP=$(pgrep -f "Emulator -hvd" | head -1)
log "post: pid=$NP rss=$(( $(ps -o rss= -p "$NP" | tr -d ' ') / 1024 ))MiB"
log "done"
