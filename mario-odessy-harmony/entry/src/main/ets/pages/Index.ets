import game from 'libgame.so';
import { fileIo } from '@kit.CoreFileKit';
import { http } from '@kit.NetworkKit';
import { BusinessError } from '@kit.BasicServicesKit';

@Entry
@Component
struct Index {
  @StorageLink('filesDir') filesDir: string = '';
  @StorageLink('smoReady') smoReady: boolean = false;
  @StorageLink('smoErr') smoErr: string = '';
  @State status: string = '就绪';
  @State started: boolean = false;
  @State surfaceReady: boolean = false;
  @State romfsReady: boolean = false;
  @State pulling: string = '';
  private pcAddr: string = '192.168.0.110:8000';   // Mac 侧 deploy_emu.sh 在 lo0 挂的别名直连地址
  private timerId: number = -1;
  private statusTimerId: number = -1;

  aboutToAppear() {
    this.checkRomfs();
    // 自动链默认开（EntryAbility 写入，--ps autopath 0 可关）：RomFS 未就绪时自动开拉
    if (AppStorage.get<string>('autopath') !== '0' && !this.romfsReady) {
      setTimeout(() => { this.pullFromPc(); }, 800);
    }
    // 自动点火默认开（--ps boot 0 可关）：等 smo 镜像解包完 + RomFS 就绪后自动启动游戏
    if (AppStorage.get<string>('boot') !== '0') {
      this.timerId = setInterval(() => {
        if (this.smoReady && this.romfsReady && !this.started) {
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
      const dir = `${this.filesDir}/smo/romfs`;
      let n = 0;
      const all: string[] = ['runtime-index.bin',
        'romfs.bin.000', 'romfs.bin.001', 'romfs.bin.002', 'romfs.bin.003', 'romfs.bin.004', 'romfs.bin.005'];
      for (let i = 0; i < all.length; i++) {
        if (fileIo.accessSync(`${dir}/${all[i]}`)) { n++; }
      }
      this.romfsReady = n === all.length;
    } catch (e) {
      this.romfsReady = false;
    }
  }

  // 从电脑拉取（自动链通道，无按钮）：HTTP 分块下载直写沙箱。两条路：
  //  1) 直连（默认 192.168.0.110:8000）：Mac 侧 deploy_emu.sh 在 lo0 挂别名 + 起 http.server
  //  2) 隧道（直连失败自动回落）：PC 侧 `hdc rport tcp:8000 tcp:8000` 后走 127.0.0.1:8000
  private async pullFromPc(): Promise<void> {
    if (this.pulling !== '' || this.pcAddr === '') { return; }
    const dst = `${this.filesDir}/smo/romfs`;
    const wanted: string[] = ['runtime-index.bin',
      'romfs.bin.000', 'romfs.bin.001', 'romfs.bin.002', 'romfs.bin.003', 'romfs.bin.004', 'romfs.bin.005'];
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

  // 启动游戏（CPU boot）：boot 自动链点火入口（无按钮）
  private startGame(): void {
    if (this.started) { return; }
    const dir = `${this.filesDir}/smo`;
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
    Stack({ alignContent: Alignment.TopStart }) {
      // 游戏画面：XComponent(surface) 铺满全屏，native 侧收窗口句柄（后续接 Vulkan swapchain）
      XComponent({ id: 'game', type: XComponentType.SURFACE, libraryname: 'game' })
        .width('100%')
        .height('100%')
        .backgroundColor('#000000')
        .onLoad(() => {
          this.surfaceReady = game.surfaceProbe() === 1;
          this.status = this.surfaceReady ? 'surface 已就绪' : 'surface 未上报';
        })

      // 启动前的小字提示（就绪状态/拉取进度）；游戏一旦点火立即整体隐藏，不挡画面
      if (!this.started) {
        Column({ space: 6 }) {
          Text(this.status).fontSize(12).fontColor('#4fc3f7')
          if (this.smoErr !== '') {
            Text(this.smoErr).fontSize(11).fontColor('#ff5252')
          }
          if (this.pulling !== '') {
            Text(this.pulling).fontSize(11).fontColor('#b0bec5')
          }
          Text(`surface=${this.surfaceReady ? 'OK' : '...'} · RomFS=${this.romfsReady ? 'OK' : '未就绪'} · 就绪后自动启动`)
            .fontSize(11).opacity(0.55)
        }
        .align(Alignment.TopStart)
        .padding({ top: 18, left: 12, right: 12 })
      }
    }
    .width('100%').height('100%')
    .backgroundColor('#000000')
  }
}
