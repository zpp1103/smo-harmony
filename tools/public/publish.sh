#!/usr/bin/env bash
# 把私有工作树导出成「公开快照」并强推 GitHub 镜像。
#
# 为什么需要它：这个仓库有两个 remote——
#   origin = NAS Gitea（私有，游戏图标/AGENTS.md/签名口令都可以有）
#   github = 公开镜像（这些一律不能有）
# 同一个工作树不可能同时满足两边，所以公开侧不能直接 push，必须过滤后再推。
#
# 用法：
#   bash tools/public/publish.sh                 # 导出当前 HEAD
#   bash tools/public/publish.sh <ref>           # 导出指定分支/提交
#   bash tools/public/publish.sh --dry-run <ref> # 只导出+自检，不推送
#
# 过滤规则见下方 FILTER 段；任何一条自检失败都会中止，不会推出去。
set -euo pipefail

ROOT=$(git rev-parse --show-toplevel)
REMOTE="${REMOTE:-github}"
BRANCH="${BRANCH:-main}"
DRY=0
if [ "${1:-}" = "--dry-run" ]; then DRY=1; shift; fi
REF="${1:-HEAD}"

TMP=$(mktemp -d "${TMPDIR:-/tmp}/smo-public.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

echo "==> 导出 $REF 到 $TMP"
git -C "$ROOT" archive "$REF" | tar -x -C "$TMP"

echo "==> FILTER 剔除私有用物"
rm -f  "$TMP/AGENTS.md"
rm -rf "$TMP/game-work" "$TMP/infra" "$TMP/third" "$TMP/logs" "$TMP/shots" \
       "$TMP/debug-shots" "$TMP/hap-archive" "$TMP/signing-mario" "$TMP/prod-keys"
find "$TMP" -type d -name '__pycache__' -prune -exec rm -rf {} + 2>/dev/null || true
find "$TMP" -type f \( -name '*.pyc' -o -name '*.log' -o -name 'shared_fonts_data.h' \) -delete
find "$TMP" -type f \( -name '*.nsp' -o -name '*.xci' -o -name '*.ncz' -o -name '*.nca' \
                     -o -name '*.img'  -o -name '*.hap'  -o -name '*.keys' \) -delete

echo "==> 图标替换为中性占位"
while IFS= read -r dst; do
  case "$dst" in
    *mario-odessy-harmony*)            src="$ROOT/tools/public/icons/smo.jpg"  ;;
    *0100F2C0115B6000*|*zelda-tk-harmony*) src="$ROOT/tools/public/icons/totk.jpg" ;;
    *nxnative-ohos/template*)          src="$ROOT/tools/public/icons/nx.jpg"   ;;
    *)                                 src="$ROOT/tools/public/icons/nx.jpg"   ;;
  esac
  cp "$src" "$dst"
  echo "    $dst <- $(basename "$src")"
done < <(find "$TMP" -path '*/resources/base/media/icon.jpg' -type f)

echo "==> 清空 build-profile.json5 的签名段"
python3 - "$TMP" <<'PY'
import pathlib, re, sys
root = pathlib.Path(sys.argv[1])
n = 0
for p in root.rglob('build-profile.json5'):
    s = p.read_text(encoding='utf-8')
    i = s.find('"signingConfigs"')
    if i < 0:
        continue
    j = s.find('[', i); depth = 0; k = j
    while k < len(s):
        if s[k] == '[': depth += 1
        elif s[k] == ']':
            depth -= 1
            if depth == 0: break
        k += 1
    s = s[:j+1] + ']' + s[k+1:]
    s = re.sub(r'\n\s*"signingConfig":\s*"default",', '', s)
    p.write_text(s, encoding='utf-8'); n += 1
print(f'    {n} 个 build-profile.json5 已处理')
PY

echo "==> 自检"
fail=0
# 1) 任天堂美术：字节级扫描
while IFS= read -r f; do
  if grep -qa 'Nintendo\|AuthoringTool' "$f"; then echo "  🔴 任天堂原图: ${f#$TMP/}"; fail=1; fi
done < <(find "$TMP" -type f \( -iname '*.jpg' -o -iname '*.png' -o -iname '*.webp' \))
# 2) 签名口令
if grep -rql 'keyPassword\|storePassword\|certpath' --include='build-profile.json5' "$TMP" 2>/dev/null; then
  echo "  🔴 build-profile 仍含签名口令"; fail=1
fi
# 3) 私有用物
for pat in AGENTS.md __pycache__ shared_fonts_data.h; do
  if find "$TMP" -name "*$pat*" | grep -q .; then echo "  🔴 仍有 $pat"; fail=1; fi
done
# 4) 游戏数据/密钥
if find "$TMP" -type f \( -name '*.nsp' -o -name '*.xci' -o -name '*.nca' -o -name '*.keys' -o -name '*.p12' -o -name '*.p7b' \) | grep -q .; then
  echo "  🔴 仍有游戏数据或密钥"; fail=1
fi
[ "$fail" -eq 0 ] && echo "  ✓ 全部通过"

FILES=$(find "$TMP" -type f | wc -l | tr -d ' ')
SIZE=$(du -sh "$TMP" | cut -f1)
echo "==> 快照：$FILES 个文件 / $SIZE"

if [ "$fail" -ne 0 ]; then
  echo "✗ 自检未通过，已中止（临时目录保留在 $TMP 供排查）"
  trap - EXIT
  exit 1
fi

if [ "$DRY" -eq 1 ]; then
  echo "==> --dry-run：不推送。导出内容在 $TMP"
  trap - EXIT
  exit 0
fi

echo "==> 提交并强推 $REMOTE/$BRANCH"
cd "$TMP"
git init -q -b "$BRANCH"
git config user.name  "$(git -C "$ROOT" config user.name || echo zpp1103)"
git config user.email "$(git -C "$ROOT" config user.email || echo zpp1103@users.noreply.github.com)"
# GitHub 对大 pack 的默认 1MB postBuffer 会直接 400，这两项必须设
git config http.postBuffer 524288000
git config http.version HTTP/1.1
git add -A
git commit -q -m "public snapshot $(date +%Y-%m-%d): $(git -C "$ROOT" log -1 --format=%s "$REF" | cut -c1-60)

来源：私有仓 $REF @ $(git -C "$ROOT" rev-parse --short "$REF")
经 tools/public/publish.sh 过滤：中性图标 / 无 AGENTS.md / 无签名口令 / 无游戏数据。"
PUBLISH_PUBLIC=1 git push --force "https://github.com/zpp1103/smo-harmony.git" "$BRANCH"

echo "✓ 完成：https://github.com/zpp1103/smo-harmony"
