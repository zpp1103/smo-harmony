#!/bin/bash
# deploy_emu.sh —— SMO 模拟器装机 + RomFS 资产灌入（幂等，可重复执行）
#
# 用法: ./deploy_emu.sh   [环境变量可覆盖: HDC_TARGET / HTTP_PORT / POLL_TIMEOUT]
#
# 流程:
#   1) 安装 entry-default-signed.hap（已装则 -r 原地升级保数据；签名不一致才卸载重装）
#   2) 确保应用至少启动过一次（生成 el2 沙箱 haps/entry/files）
#   3) 校验沙箱 smo/romfs 下 7 件资产字节数；全齐则直接跳过（0 字节重传）
#   4) 缺件 → Mac lo0 挂 192.168.0.110 别名（应用 pcAddr 默认直连地址）+ 起http.server(0.0.0.0)
#      → aa start --ps autopath 1 触发应用自带的 pullFromPc 把整包拉进 files/smo/romfs
#      → 应用直连 192.168.0.110:8000 → 模拟器NAT → 宿主lo0别名（本机发起连接，不受App防火墙限制）
#      （备胎: hdc rport 反向隧道——实测 ~543MB 后中继停摆，仅小文件可用，不作主通道）
#   5) 轮询逐件校验字节数，全部一致才算成功
#
# 为什么不用 hdc file send 直灌沙箱（2026-10-31 实测）:
#   模拟器镜像无 root：hdcd 以 uid2000(shell) 运行，setenforce 被拒（SELinux Enforcing），
#   shell/hdcd 对 /data/app/el2/*/haps/** 的写入被 DAC+SELinux 双拦
#   （cache/ 即便 0777 也 mkdir Permission denied）。
#   唯一合法写入者是应用本身，故走应用源码自带的 HTTP 拉取通道
#   （entry/src/main/ets/pages/Index.ets pullFromPc，注释即设计此用法）。
#
# 幂等性说明（受应用代码约束）:
#   pullFromPc 的下载循环包在单个 try 里，任一分卷非 200 会中止整个循环
#   （Index.ets:118-135），无法"只补缺件"。因此本脚本的幂等策略是:
#   - 7 件全齐且字节数一致 → 完全跳过传输（重复执行 0 开销）
#   - 缺任何一件 → 整包重拉（应用会覆盖重写全部 7 件，结果仍正确）

set -uo pipefail

HDC="/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc"
TARGET="${HDC_TARGET:-127.0.0.1:5555}"
BUNDLE="com.vibecc.smo"
HAP="/Users/zpp/VibeCoding/switch2hos/mario-odessy-harmony/entry/build/default/outputs/default/entry-default-signed.hap"
PARTS_DIR="/Users/zpp/ZCodeProject/switch2mac/switch2hos/game-work/romfs_parts"
HTTP_PORT="${HTTP_PORT:-8000}"
POLL_TIMEOUT="${POLL_TIMEOUT:-3600}"   # 秒；5.68GB 全量重拉的宽裕上限
POLL_INTERVAL="${POLL_INTERVAL:-5}"

# 应用 context.filesDir = /data/storage/el2/base/haps/entry/files（进程内挂载视图）
# 其磁盘真实路径（宿主/shell 视角）如下：
SANDBOX_FILES="/data/app/el2/100/base/${BUNDLE}/haps/entry/files"
DST="${SANDBOX_FILES}/smo/romfs"
FILES=(runtime-index.bin romfs.bin.000 romfs.bin.001 romfs.bin.002 romfs.bin.003 romfs.bin.004 romfs.bin.005)

log()  { printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*"; }
die()  { log "✗ $*"; exit 1; }
h()    { "$HDC" -t "$TARGET" "$@"; }

SRV_PID=""
RPORT_ADDED=0
cleanup() {
  if [ -n "$SRV_PID" ]; then
    kill "$SRV_PID" 2>/dev/null; wait "$SRV_PID" 2>/dev/null
    log "http.server($SRV_PID) 已停止"
  fi
  if [ "$RPORT_ADDED" -eq 1 ]; then
    "$HDC" -t "$TARGET" rport rm "tcp:${HTTP_PORT}" "tcp:${HTTP_PORT}" 2>/dev/null \
      || "$HDC" -t "$TARGET" fport rm "tcp:${HTTP_PORT}" "tcp:${HTTP_PORT}" 2>/dev/null \
      || true
    log "rport 隧道已清理（尽力而为）"
  fi
}
trap cleanup EXIT

# ---------- 0. 前置检查 ----------
[ -x "$HDC" ] || die "hdc 不存在: $HDC"
[ -f "$HAP" ] || die "HAP 不存在: $HAP"
for f in "${FILES[@]}"; do
  [ -f "$PARTS_DIR/$f" ] || die "分卷缺失: $PARTS_DIR/$f"
done
h shell "echo ok" >/dev/null 2>&1 || die "目标不在线: $TARGET（hdc list targets 检查）"
log "目标在线: $TARGET"

# 远端文件字节数（失败输出空）
remote_size() { h shell "ls -l '$1' 2>/dev/null" 2>/dev/null | awk '{print $5}' | tail -1; }

# ---------- 1. 安装/升级 HAP（幂等） ----------
if h shell "bm dump -n ${BUNDLE}" >/dev/null 2>&1 && \
   [ -n "$(h shell "bm dump -n ${BUNDLE}" 2>/dev/null | grep '"name"')" ]; then
  log "步骤1: 已安装，尝试 -r 原地升级（保沙箱数据）…"
  OUT="$(h install -r "$HAP" 2>&1)"
  log "$OUT"
  if ! grep -q "install bundle successfully" <<<"$OUT"; then
    if grep -q "sign info inconsistent" <<<"$OUT"; then
      log "签名不一致 → 卸载旧包重装（会清空沙箱，romfs 需重灌）"
      h uninstall "$BUNDLE" >/dev/null 2>&1
      OUT="$(h install "$HAP" 2>&1)"; log "$OUT"
    fi
  fi
else
  log "步骤1: 未安装，直接安装…"
  OUT="$(h install "$HAP" 2>&1)"; log "$OUT"
fi
grep -q "install bundle successfully" <<<"$OUT" || die "HAP 安装失败（见上）"
h shell "bm dump -n ${BUNDLE}" 2>/dev/null | grep -q '"name"' || die "安装后 bm dump 仍查不到包"

# ---------- 2. 确保沙箱已生成（应用至少跑过一次） ----------
if ! h shell "ls '${SANDBOX_FILES}/smo' " >/dev/null 2>&1; then
  log "步骤2: 沙箱未生成，启动应用一次…"
  h shell "aa start -b ${BUNDLE} -a EntryAbility" >/dev/null 2>&1
  for i in $(seq 1 30); do
    h shell "ls '${SANDBOX_FILES}/smo' " >/dev/null 2>&1 && break
    sleep 1
  done
fi
h shell "ls '${SANDBOX_FILES}/smo' " >/dev/null 2>&1 \
  || die "沙箱 smo 目录未出现（应用首启解包失败？看 hilog SmoEntry）"
log "步骤2: 沙箱就绪 → ${SANDBOX_FILES}/smo"

# ---------- 3. 校验已就位件（幂等跳过） ----------
MISSING=()
TOTAL_LOCAL=0; TOTAL_REMOTE_OK=0
for f in "${FILES[@]}"; do
  L=$(stat -f%z "$PARTS_DIR/$f"); TOTAL_LOCAL=$((TOTAL_LOCAL+L))
  R="$(remote_size "${DST}/${f}")"
  if [ -n "$R" ] && [ "$R" = "$L" ]; then
    TOTAL_REMOTE_OK=$((TOTAL_REMOTE_OK+L))
    log "  ✓ $f 已就位 ($L B)，跳过"
  else
    MISSING+=("$f")
    log "  ✗ $f 缺失/不符 (本地 $L B, 远端 ${R:-无})"
  fi
done
if [ "${#MISSING[@]}" -eq 0 ]; then
  log "全部 7 件已就位，无需传输。沙箱: ${DST}"
  exit 0
fi
log "步骤3: 缺 ${#MISSING[@]} 件（${MISSING[*]}）→ 整包重拉（应用下载循环遇 404 即中止，无法逐件补）"

# ---------- 4. 直连通道：lo0 别名 + http.server ----------
# 应用 pcAddr 默认 192.168.0.110:8000（Index.ets @State 初值），先直连失败才回落 rport 隧道；
# 把该 IP 挂到本机 lo0，让"直连"落在宿主本地环回（NAT 代理发起，绕开 App 防火墙），
# 且避开 rport 中继 ~543MB 停摆的坑（2026-10-31 实测）。
APP_DIRECT_IP="192.168.0.110"
if ! ifconfig lo0 2>/dev/null | grep -q "inet ${APP_DIRECT_IP} "; then
  log "步骤4: lo0 缺 ${APP_DIRECT_IP} 别名，尝试添加（需要 sudo）…"
  if [ -f /tmp/.zcode-sudo-pw ]; then
    sudo -S ifconfig lo0 alias "${APP_DIRECT_IP}" up < /tmp/.zcode-sudo-pw 2>/dev/null \
      && log "  别名已添加" || log "  别名添加失败（若无 sudo 通道可手动: sudo ifconfig lo0 alias ${APP_DIRECT_IP}）"
  else
    log "  无 sudo 通道，请手动执行: sudo ifconfig lo0 alias ${APP_DIRECT_IP} up"
  fi
fi
ifconfig lo0 2>/dev/null | grep -q "inet ${APP_DIRECT_IP} " \
  || die "lo0 别名 ${APP_DIRECT_IP} 不存在，应用直连无法落到本机"

log "步骤4: 本机 ${HTTP_PORT} 端口起 http.server（目录 ${PARTS_DIR}，绑 0.0.0.0）…"
python3 -m http.server "${HTTP_PORT}" --bind 0.0.0.0 --directory "$PARTS_DIR" \
  > /tmp/smo_http_server.log 2>&1 &
SRV_PID=$!
for i in $(seq 1 20); do
  CODE="$(curl -s -o /dev/null -w '%{http_code}' "http://${APP_DIRECT_IP}:${HTTP_PORT}/runtime-index.bin" 2>/dev/null || true)"
  [ "$CODE" = "200" ] && break
  sleep 1
done
[ "$CODE" = "200" ] || { cat /tmp/smo_http_server.log; die "http.server 未就绪(${APP_DIRECT_IP}:${HTTP_PORT})"; }
log "  http.server 就绪 (pid $SRV_PID)，直连地址 http://${APP_DIRECT_IP}:${HTTP_PORT}/"

# 备胎通道：rport 隧道（应用直连失败时自动回落用；大文件会停摆，仅供灾备）
OUT="$(h rport "tcp:${HTTP_PORT}" "tcp:${HTTP_PORT}" 2>&1 || true)"
grep -q "OK" <<<"$OUT" && RPORT_ADDED=1
log "  rport 隧道(备胎): $OUT"

# ---------- 5. 触发应用自拉（autopath=1） ----------
log "步骤5: 重启应用并带 autopath=1 自动拉取…"
h shell "aa force-stop ${BUNDLE}" >/dev/null 2>&1
sleep 1
h shell "aa start -b ${BUNDLE} -a EntryAbility --ps autopath 1" 2>&1 | sed 's/^/    /'

# ---------- 6. 轮询校验 ----------
log "步骤6: 轮询传输进度（每 ${POLL_INTERVAL}s，超时 ${POLL_TIMEOUT}s）…"
DEADLINE=$((SECONDS + POLL_TIMEOUT))
LAST_BYTES=-1; LAST_T=$SECONDS; STALL_LIMIT=420
while :; do
  DONE_BYTES=0; INFLIGHT=0; ALL_OK=1
  for f in "${FILES[@]}"; do
    L=$(stat -f%z "$PARTS_DIR/$f")
    R="$(remote_size "${DST}/${f}")"
    if [ -n "$R" ] && [ "$R" = "$L" ]; then
      DONE_BYTES=$((DONE_BYTES+L))
    else
      ALL_OK=0
      P="$(remote_size "${DST}/${f}.part")"
      [ -n "$P" ] && INFLIGHT=$((INFLIGHT+P))
    fi
  done
  CUR=$((DONE_BYTES+INFLIGHT))
  PCT=$((CUR*100/TOTAL_LOCAL))
  log "  进度: ${PCT}%  完成 $((DONE_BYTES/1048576))MB + 传输中 $((INFLIGHT/1048576))MB / 共 $((TOTAL_LOCAL/1048576))MB"
  if [ "$ALL_OK" -eq 1 ]; then log "  7 件全部就位 ✓"; break; fi
  if [ "$SECONDS" -ge "$DEADLINE" ]; then
    die "超时未完成（已完成 $((DONE_BYTES/1048576))MB）。重跑本脚本将从缺件起整包重拉"
  fi
  if [ "$CUR" -gt "$LAST_BYTES" ]; then LAST_BYTES=$CUR; LAST_T=$SECONDS
  elif [ $((SECONDS - LAST_T)) -ge "$STALL_LIMIT" ]; then
    die "传输停滞 ${STALL_LIMIT}s（应用侧拉取失败？看 hilog / 界面提示）。重跑本脚本重试"
  fi
  sleep "$POLL_INTERVAL"
done

# ---------- 7. 终验 + 汇总 ----------
sleep 2
log "步骤7: 终验（逐件比对字节数）…"
FINAL_OK=0
for f in "${FILES[@]}"; do
  L=$(stat -f%z "$PARTS_DIR/$f"); R="$(remote_size "${DST}/${f}")"
  if [ -n "$R" ] && [ "$R" = "$L" ]; then
    FINAL_OK=$((FINAL_OK+1)); log "  ✓ $f  $R B"
  else
    log "  ✗ $f  本地 $L B / 远端 ${R:-无}"
  fi
done
h shell "ls -l '${DST}'" 2>/dev/null | sed 's/^/    /'
log "======================================"
log "就位: ${FINAL_OK}/7 件"
log "沙箱: ${DST}"
[ "$FINAL_OK" -eq 7 ] || die "终验未过"
log "部署完成 ✓（重复执行将直接跳过传输）"
