#!/bin/bash
# g1_check.sh — G1 判据的自动验证（设备侧帧缓冲截图 + 三重防伪）
#
# 用法: tools/g1_check.sh [等待秒数=25] [采样张数=3]
#
# 为什么用 snapshot_display 而不是桌面 screencapture：
#   桌面截图要裁模拟器窗口，机身边框/工具栏/背景窗口会混进来——实测同一块黑屏，
#   桌面截图算 10.37%，设备侧只有 0.33%。差 30 倍。
#
# 为什么还要三重防伪（血泪教训）：
#   第一次真机验收时，应用中途退出、屏幕回到桌面壁纸，工具报出「100.00% 非黑 = 达标」。
#   壁纸也有大量像素，只看非黑比例必然被骗。所以每一帧必须同时满足：
#     1) 应用进程在场（pidof 非空）        —— 排除「应用已退出」
#     2) 画面为横屏（W > H）                —— 排除「桌面/锁屏是竖屏」
#     3) 颜色种类少（< 300 桶）             —— 我们的填色是纯色；壁纸通常上千种
#   任一不满足 → 该帧作废，不计入达标判断。
#
# 判据: 任一**有效帧**非黑 > 5% 视为达标
# 退出码: 0=达标 1=未达标 2=环境问题（无采样/全部作废）
set -u
WAIT="${1:-25}"; SHOTS="${2:-3}"
BUNDLE="com.vibecc.smo"
HDC="/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc"
OUT="/tmp/g1_$(date +%H%M%S)"; mkdir -p "$OUT"

"$HDC" shell "aa force-stop $BUNDLE" >/dev/null 2>&1; sleep 2
"$HDC" shell "aa start -a EntryAbility -b $BUNDLE" >/dev/null 2>&1
echo "[g1] started $BUNDLE, waiting ${WAIT}s"
sleep "$WAIT"

BEST=0; VALID=0
for i in $(seq 1 "$SHOTS"); do
  ALIVE=$("$HDC" shell "pidof $BUNDLE" 2>/dev/null | tr -d '[:space:]')
  "$HDC" shell "snapshot_display -f /data/local/tmp/g1_$i.jpeg" >/dev/null 2>&1
  "$HDC" file recv "/data/local/tmp/g1_$i.jpeg" "$OUT/shot$i.jpeg" >/dev/null 2>&1
  R=$(python3 - "$OUT/shot$i.jpeg" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB'); px = im.load(); W, H = im.size
tot = nb = 0; buckets = set()
for y in range(0, H, 4):
    for x in range(0, W, 4):
        r, g, b = px[x, y]; tot += 1
        if r + g + b > 30:
            nb += 1
            if len(buckets) < 8000: buckets.add((r//16, g//16, b//16))
print("%.2f %d %d %d" % (100.0*nb/max(tot,1), len(buckets), W, H))
PY
)
  RATIO=$(echo "$R"|cut -d' ' -f1); NB=$(echo "$R"|cut -d' ' -f2); W=$(echo "$R"|cut -d' ' -f3); H=$(echo "$R"|cut -d' ' -f4)
  WHY=""
  [ -z "$ALIVE" ] && WHY="app not running"
  [ -z "$WHY" ] && [ "$W" -le "$H" ] && WHY="portrait ${W}x${H} (launcher/lock screen, not our landscape surface)"
  [ -z "$WHY" ] && [ "$NB" -ge 300 ] && WHY="${NB} colour buckets (looks like a photo/wallpaper, not a solid fill)"
  if [ -n "$WHY" ]; then
    echo "[g1] shot$i ${W}x${H} nonblack=${RATIO}% colors=${NB}  VOID: $WHY"
  else
    VALID=$((VALID+1))
    echo "[g1] shot$i ${W}x${H} nonblack=${RATIO}% colors=${NB}  valid (app pid=$ALIVE)"
    BEST=$(python3 -c "print(max($BEST,$RATIO))")
  fi
  sleep 2
done

echo "[g1] valid frames=$VALID/$SHOTS  best nonblack=${BEST}% (threshold 5%)  artifacts: $OUT"
[ "$VALID" -eq 0 ] && { echo "[g1] no valid frame — cannot judge"; exit 2; }
python3 -c "import sys; sys.exit(0 if $BEST > 5.0 else 1)"
