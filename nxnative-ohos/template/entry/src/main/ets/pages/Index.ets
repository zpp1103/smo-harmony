import game from 'libgame.so';
import { fileIo, fileUri, picker } from '@kit.CoreFileKit';
import { http } from '@kit.NetworkKit';
import { BusinessError } from '@kit.BasicServicesKit';

// {{...}} 占位由 scripts/build_hap_from_profile.py 从 games/<titleID>/game.tsv 实例化：
//   GAME_ID_DIR    游戏沙箱目录名（title_id）；与 cpp/game_id.h 的 GAME_ID_DIR 同值
//   ROMFS_VOLUMES  RomFS 分卷数（make_romfs_runtime 按 romfs_volume_mib 切卷，smo=6）
//   HAP_LABEL      应用名（hap_label 键，页面标题）
const GAME_ID_DIR = '{{GAME_ID_DIR}}';
const ROMFS_VOLUMES = {{ROMFS_VOLUMES}};
const GAME_LABEL = '{{HAP_LABEL}}';

// RomFS 就绪清单：runtime-index.bin + N 个分卷（卷号 3 位补零，末卷短）
function romfsFileList(): string[] {
  const all: string[] = ['runtime-index.bin'];
  for (let i = 0; i < ROMFS_VOLUMES; i++) {
    all.push(`romfs.bin.${i.toString().padStart(3, '0')}`);
  }
  return all;
}

@Entry
@Component
struct Index {
  @StorageLink('filesDir') filesDir: string = '';
  @StorageLink('gameReady') gameReady: boolean = false;
  @StorageLink('gameErr') gameErr: string = '';
  @State status: string = '就绪';
  @State started: boolean = false;
  @State surfaceReady: boolean = false;
  @State importing: string = '';
  @State romfsReady: boolean = false;
  @State pulling: string = '';
  @State pcAddr: string = '192.168.0.110:8000';
  private timerId: number = -1;
  private statusTimerId: number = -1;

  aboutToAppear() {
    this.checkRomfs();
    // autopath=1（aa start 带参）且 RomFS 未就绪时自动开拉，配合锁屏/无人值守传输
    if (AppStorage.get<string>('autopath') === '1' && !this.romfsReady) {
      setTimeout(() => { this.pullFromPc(); }, 800);
    }
    // boot=1（aa start 带参）：等游戏镜像解包完 + RomFS 就绪后自动点「启动游戏」
    if (AppStorage.get<string>('boot') === '1') {
      this.timerId = setInterval(() => {
        if (this.gameReady && this.romfsReady && !this.started) {
          clearInterval(this.timerId);
          this.timerId = -1;
          this.startGame();
        }
      }, 2000) as number;
    }
  }

  aboutToDisappear() {
    if (this.timerId >= 0) clearInterval(this.timerId);
    if (this.statusTimerId >= 0) clearInterval(this.statusTimerId);
  }

  // RomFS 就绪判定（boot 门禁用）：7 件齐全才放行点火，缺任何一件都算未就绪
  private checkRomfs(): void {
    try {
      const dir = `${this.filesDir}/${GAME_ID_DIR}/romfs`;
      let n = 0;
      const all: string[] = romfsFileList();
      for (let i = 0; i < all.length; i++) {
        if (fileIo.accessSync(`${dir}/${all[i]}`)) { n++; }
      }
      this.romfsReady = n === all.length;
    } catch (e) {
      this.romfsReady = false;
    }
  }

  // 导入游戏资源：选手机上放着 runtime-index.bin + romfs.bin.000…00N 的文件夹（PC 侧 game-work/romfs_parts
  // 经 USB/MTP 拷到手机），整包拷入 files/<游戏id>/romfs。模式抄 jhaizhou d2r-mobile 的 DocumentViewPicker FOLDER。
  private async importResources(): Promise<void> {
    if (this.importing !== '') { return; }
    try {
      const opts = new picker.DocumentSelectOptions();
      opts.selectMode = picker.DocumentSelectMode.FOLDER;
      opts.maxSelectNumber = 1;
      const p = new picker.DocumentViewPicker();
      const uris: string[] = await p.select(opts);
      if (!uris || uris.length === 0) { return; }
      const root: string = uris[0];
      const dst = `${this.filesDir}/${GAME_ID_DIR}/romfs`;
      if (!fileIo.accessSync(dst)) { fileIo.mkdirSync(dst, true); }
      const wanted: string[] = romfsFileList();
      let copied = 0;
      for (let i = 0; i < wanted.length; i++) {
        const name = wanted[i];
        const src = `${root}/${name}`;
        const target = `${dst}/${name}`;
        this.importing = `正在导入 ${name} …`;
        try { fileIo.unlinkSync(target); } catch (e) { }
        try {
          await fileIo.copy(src, fileUri.getUriFromPath(target), {
            progressListener: (pr: fileIo.Progress) => {
              this.importing = `${name}：${Math.floor(pr.processedSize / 1048576)} / ${Math.floor(pr.totalSize / 1048576)} MB`;
            }
          });
          copied++;
        } catch (e) {
          const code = (e as BusinessError).code;
          if (code !== 13900002) {   // 13900002 = 选中的文件夹里没有这一项（允许缺卷）
            this.importing = `导入失败：${name} (code ${code})，请重试`;
            return;
          }
        }
      }
      if (copied < 2) {
        this.importing = '这个文件夹里没有资源（需要 runtime-index.bin 和 romfs.bin 分卷）';
        return;
      }
      this.checkRomfs();
      this.importing = this.romfsReady ? '导入完成 ✓' : '导入不完整（缺 runtime-index.bin 或首卷）';
    } catch (e) {
      this.importing = `导入失败：${JSON.stringify(e)}`;
    }
  }

  // 从电脑拉取：HTTP 分块下载直写沙箱。两条路：
  //  1) 直连：电脑上 romfs_parts 目录起 `python -m http.server 8000`，此处填电脑局域网 IP:8000（需防火墙放行）
  //  2) 隧道：PC 侧 `hdc rport tcp:8000 tcp:8000` 后填 127.0.0.1:8000（走 hdc 反向转发，免防火墙）
  private async pullFromPc(): Promise<void> {
    if (this.pulling !== '' || this.pcAddr === '') { return; }
    const dst = `${this.filesDir}/${GAME_ID_DIR}/romfs`;
    const wanted: string[] = ['runtime-index.bin', ...romfsFileList()];
    try {
      if (!fileIo.accessSync(dst)) { fileIo.mkdirSync(dst, true); }
      for (let i = 0; i < wanted.length; i++) {
        const name = wanted[i];
        this.pulling = `拉取 ${name} …`;
        await this.downloadOne(`http://${this.pcAddr}/${name}`, `${dst}/${name}`, name);
      }
      this.checkRomfs();
      this.pulling = this.romfsReady ? '拉取完成 ✓' : '拉取不完整（缺 runtime-index.bin 或首卷）';
    } catch (e) {
      // 直连失败（多半是 PC 防火墙）→ 自动改走 hdc rport 隧道（127.0.0.1:8000）重试一次
      if (this.pcAddr !== '127.0.0.1:8000') {
        this.pcAddr = '127.0.0.1:8000';
        this.pulling = '直连失败，改走 hdc 隧道重试…';
        setTimeout(() => { this.pulling = ''; this.pullFromPc(); }, 500);
        return;
      }
      this.pulling = `拉取失败：${(e as BusinessError).message ?? JSON.stringify(e)} · 检查电脑侧服务/地址`;
    }
  }

  // 单文件流式下载：requestInStream → dataReceive 顺序写 .part → dataEnd 校验 200 后 moveFile 成品
  private downloadOne(url: string, target: string, name: string): Promise<void> {
    return new Promise<void>((resolve, reject) => {
      const part = `${target}.part`;
      const session = http.createHttp();
      let fd = -1;
      let received = 0;
      let total = 0;
      let respCode = 0;
      let settled = false;
      const fail = (msg: string): void => {
        if (settled) { return; }
        settled = true;
        if (fd >= 0) { try { fileIo.closeSync(fd); } catch (e) { } }
        try { fileIo.unlinkSync(part); } catch (e) { }
        session.destroy();
        reject(new Error(msg));
      };
      try { fileIo.unlinkSync(part); } catch (e) { }
      fd = fileIo.openSync(part,
        fileIo.OpenMode.READ_WRITE | fileIo.OpenMode.CREATE | fileIo.OpenMode.TRUNC).fd;
      session.on('headersReceive', (h: Object) => {
        const rec = h as Record<string, string>;
        const cl = rec['content-length'];
        if (cl) { total = parseInt(cl, 10); }
      });
      session.on('dataReceive', (chunk: ArrayBuffer) => {
        if (fd < 0) { return; }
        fileIo.writeSync(fd, chunk);
        received += chunk.byteLength;
        this.pulling = total > 0
          ? `${name}：${Math.floor(received / 1048576)} / ${Math.floor(total / 1048576)} MB`
          : `${name}：${Math.floor(received / 1048576)} MB`;
      });
      session.on('dataEnd', () => {
        if (settled) { return; }
        // requestInStream 的回调（err, code）可能比 dataEnd 晚到：快响应下 code 还没落地，
        // dataEnd 先按 respCode===0 误杀成「HTTP 0」。dataEnd 后轮询最多 2s 等回调补码再裁决。
        let waits = 0;
        const judge = (): void => {
          if (respCode === 0 && waits < 20) { waits++; setTimeout(judge, 100); return; }
          if (settled) { return; }
          settled = true;
          if (fd >= 0) { try { fileIo.closeSync(fd); } catch (e) { } }
          session.destroy();
          if (respCode !== 200) {
            try { fileIo.unlinkSync(part); } catch (e) { }
            reject(new Error(`HTTP ${respCode}（地址/服务不对？）`));
            return;
          }
          try { fileIo.unlinkSync(target); } catch (e) { }
          fileIo.moveFileSync(part, target);
          resolve();
        };
        judge();
      });
      const opts: http.HttpRequestOptions = {
        usingCache: false, connectTimeout: 15000, readTimeout: 300000
      };
      session.requestInStream(url, opts, (err: BusinessError, code: number) => {
        if (err) { fail(`连接失败：${err.message}`); return; }
        respCode = code;
      });
    });
  }

  // 启动游戏（CPU boot）：按钮与 boot=1 无人值守共用
  private startGame(): void {
    if (this.started) { return; }
    const dir = `${this.filesDir}/${GAME_ID_DIR}`;
    const rc: number = game.start(dir);
    this.status = rc === 0 ? 'boot 线程已启动，看日志推进' : `启动失败 rc=${rc}`;
    this.started = true;
    this.statusTimerId = setInterval(() => {
      const s: number = game.status();
      if (s === 2) {
        this.status = 'boot 线程已退出（看 hilog 定位卡点）';
        this.started = false;
        clearInterval(this.statusTimerId);
        this.statusTimerId = -1;
      }
    }, 1000) as number;
  }

  build() {
    Column({ space: 12 }) {
      Text(`${GAME_LABEL} · 鸿蒙版`)
        .fontSize(24)
        .fontWeight(FontWeight.Bold)
      Text(this.status).fontSize(14).fontColor('#4fc3f7')
      if (this.gameErr !== '') {
        Text(this.gameErr).fontSize(12).fontColor('#ff5252')
      }

      // 游戏画面：XComponent(surface) 承载，native 侧收窗口句柄（后续接 Vulkan swapchain）
      XComponent({ id: 'game', type: XComponentType.SURFACE, libraryname: 'game' })
        .width('100%')
        .height(420)
        .backgroundColor('#000000')
        .onLoad(() => {
          this.surfaceReady = game.surfaceProbe() === 1;
          this.status = this.surfaceReady ? 'surface 已就绪' : 'surface 未上报';
        })

      Button(this.importing !== '' ? this.importing : `导入游戏资源（${ROMFS_VOLUMES} 分卷文件夹）`)
        .width(300).height(44)
        .fontSize(14)
        .enabled(this.importing === '')
        .onClick(() => {
          this.importResources();
        })

      Row({ space: 8 }) {
        Text('电脑').fontSize(12).opacity(0.6)
        TextInput({ text: this.pcAddr, placeholder: '电脑地址 ip:8000' })
          .fontSize(12).height(40).layoutWeight(1)
          .onChange((v: string) => { this.pcAddr = v.trim(); })
      }
      .width(300)

      Button(this.pulling !== '' ? this.pulling : '从电脑拉取（HTTP）')
        .width(300).height(44)
        .fontSize(14)
        .enabled(this.pulling === '')
        .onClick(() => {
          this.pullFromPc();
        })

      Button(this.started ? '停止' : (this.gameReady ? '启动游戏（CPU boot）' : '游戏镜像解包中…'))
        .width(300).height(48)
        .enabled(this.gameReady && !this.started)
        .onClick(() => {
          if (!this.started) {
            this.startGame();
          } else {
            game.stop();
            this.status = '已停止';
            this.started = false;
            if (this.statusTimerId >= 0) { clearInterval(this.statusTimerId); this.statusTimerId = -1; }
          }
        })

      Text(`画面区域：XComponent surface（Vulkan swapchain 周末接入）· surface=${this.surfaceReady ? 'OK' : '...'} · RomFS=${this.romfsReady ? 'OK' : '未导入'}`)
        .fontSize(11).opacity(0.55)
    }
    .width('100%').height('100%')
    .justifyContent(FlexAlign.Center)
    .backgroundColor('#101014')
  }
}
