# Front ends（对齐 nxnative docs/frontends.md 语义）

不动 nxnative-ohos 一行即可二次封装（GUI/CI/bot）的三件套。

## 1. 构建驱动：`--progress json`

`build_hap_from_nsp.py --progress json`（`build_hap_from_profile.py` 同）：stdout 每行一个 JSON 事件，人类可读输出全部走 stderr。

事件（对齐 nxnative schema）：

| type | 字段 | 说明 |
|---|---|---|
| `stage` | `stage`, `i`, `n`, `say` | 阶段开始（identify/extract/rewrite/prepare/hap…），i/n 为阶段序 |
| `step` | `stage`, `step`, `say`, `fraction?` | 阶段内步骤（模块重写、卷转换…） |
| `progress` | `stage`, `fraction` | 0..1（大文件拷贝/哈希） |
| `stage_end` | `stage`, `ok` | 阶段结束 |
| `say` | `say` | 自由文本进度提示 |

退出码：`0` 成功（最后一行 `stage_end ok=true` + stderr HAP 路径）；`1` 失败（事件含 `error` 字段）；`2` 未知游戏（已生成 survey，需 new_game + 人工补键）。

各工具原始输出在 `<work>/logs/`；指纹缓存（`<work>/keys/*.key`）命中时阶段整体跳过并发 `stage` 事件带 `reused=true`。

## 2. 档案与设置

- 档案键表（唯一权威）：`games/README.md`（43 键 + host_override 逃生门 + derived profile 机制）
- 每游戏运行时开关沿用宿主 `TOTK_*` 环境变量面（鸿蒙侧由点火参数/game_napi 传递；统一设置面板属 M5+，暂不做 GUI 层承诺）

## 3. 装机与运行（真机调试期）

`tools/run_cycle.sh`（源：`../../game-work/run_cycle.sh`，随主线演进同步）：

```
构建 → hdc install -r（相对路径！）→ aa force-stop → 流式 hilog
→ aa start --ps autopath 1 --ps boot 1 → pidof 存活监测 → 轮日志自动归档+验尸摘要
```

验尸摘要自动抓：死法（exit code/CRASH 行）、关键停点（FAIL/guest Break/shared font/unimplemented SVC/poison/host abort）、里程（SmoGame 行数）。

## 4. 已知缺口（与 nxnative 对齐中）

- 宿主运行期事件无人机可读接口（同 nxnative frontends.md:73-75——只有人类日志行）
- 设置清单（launcher.json/JSON Schema）未实现——M5+ 视需求
