# 派生接口信息（DERIVED）

本仓库不含游戏内容，但为了让移植能跑起来，保留了少量**从游戏可执行文件 / 资源中提取的
接口信息**。它们的共同性质是：描述“怎么接上”，而不是“游戏本身是什么”——与
[d2r-mobile 的 docs/DERIVED.md](https://github.com/jhaizhou-ops/d2r-mobile/blob/main/docs/DERIVED.md) 同类。

这些文件对每个游戏副本都要由使用者自行生成；仓库里这一份只对应
`0100000000010000` v1.4.1（SMO）与 `0100F2C0115B6000` 1.1.2（TotK）的构建记录。

| 文件 | 内容 | 为什么不算游戏内容 |
|---|---|---|
| `mario-odessy-harmony/entry/src/main/cpp/games/<title>/generated/runtime_tables.h` | 重定位表（`{addr, type, addend, sym}`）与符号地址 | 地址与重定位元数据，无游戏字节 |
| `mario-odessy-harmony/entry/src/main/cpp/games/<title>/game_patches.c` | 指令级补丁表（文件偏移 + 原始/替换指令字） | 只记录被改写的少数指令位置与替换值 |
| `mario-odessy-harmony/entry/src/main/cpp/games/<title>/generated/romfs_config.h` | romfs 挂载配置（marker、分卷参数） | 不含资源数据 |
| `.../platform/generated/shared_fonts_config.h` | 共享字体槽位表（偏移 / 长度 / SHA-256） | 只有索引与校验值，**不含字体字节** |

## 未入库的生成物

| 文件 / 目录 | 原因 |
|---|---|
| `platform/generated/shared_fonts_data.h` | 由**系统字体**（Windows TTF）转成的 BFTTF 替身 blob，字体本身不可再分发 |
| `game-work/`、`infra/`、`third/` | 游戏数据、外部参考实现克隆 |
| `*.nsp`、`*.xci`、`*.nca`、`*.img`、`*.hap` | 游戏数据与构建产物 |
| `prod.keys`、`title.keys`、`*.p12`、`*.p7b` | 密钥与签名材料 |
| `build-profile.json5` 里的 `signingConfigs` | 签名口令与证书路径（本仓已清空，需在 DevEco 中自行配置） |

## 字体如何自行生成

`shared_font.c` 需要一个 6 槽位的字体包：槽位表在 `shared_fonts_config.h`
（每行 `{路径, 长度, 文件长度, SHA-256}`），字节数据在 `shared_fonts_data.h`
（`SHARED_FONT_DATA[SHARED_FONT_DATA_LEN]`）。原生成脚本在 `game-work/`，未随本仓发布
（因为它会把系统字体字节烘进源码）。自建时按上面的契约生成即可：

- `:embedded:N` 前缀表示该槽位取自内嵌 blob 的第 N 段，否则按路径从外部字体目录读取；
- 每段都需要在 `shared_fonts_config.h` 里登记长度与 SHA-256，加载器会校验（`shared_font.c`）；
- TotK 档案另留了 `TOTK_SHARED_FONTS=<dir>` 通道，可指向自己的固件字体目录。

请只使用你有权使用的字体；不要把字体字节提交到本仓。
