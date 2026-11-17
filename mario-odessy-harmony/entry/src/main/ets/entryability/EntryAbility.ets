import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import { fileIo as fs } from '@kit.CoreFileKit';
import { BusinessError } from '@kit.BasicServicesKit';

const TAG = 'SmoEntry';
const SMO_MODULES: string[] = ['rtld', 'sdk', 'subsdk0', 'main'];

// 首启把 rawfile 里的游戏镜像解到 files/smo/（HAP rawfile 不能 mmap，loader 需要真实文件路径）
async function extractSmoImages(context: Context, destDir: string): Promise<void> {
  if (!fs.accessSync(destDir)) {
    fs.mkdirSync(destDir, true);
  }
  for (let i = 0; i < SMO_MODULES.length; i++) {
    const name: string = SMO_MODULES[i];
    const dst: string = `${destDir}/${name}.patched.bin`;
    hilog.info(0x0000, TAG, 'extract %{public}s', name);
    const buf: Uint8Array = await context.resourceManager.getRawFileContent(`smo/${name}.patched.bin`);
    const file = fs.openSync(dst, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
    // API22 坑：writeSync(ArrayBuffer) 不带显式 length 会少写尾部（sdk 少 832B），必须带 byteOffset/length
    const written: number = fs.writeSync(file.fd, buf.buffer,
      { offset: buf.byteOffset, length: buf.length });
    fs.closeSync(file);
    hilog.info(0x0000, TAG, 'extract %{public}s want=%{public}d wrote=%{public}d', name, buf.length, written);
  }
}

// M3r：离线烘好的 shader 缓存（index.json + spv.bin 容器）随包带进来，首启解到 files/shader_cache/。
// 为什么不 hdc 推送：本机 SELinux 禁止 shell 域写 debug_hap_data_file（实测 mkdir/touch/file send 三路全 denied，
// 目录 0777 也不行），App 自己的目录只有 App 能写——rawfile 是唯一既到得了、又不依赖 hdc 的通道。
// M28A：index_m27a.json = index.json 的**工作副本**（键层多一个 "units" 字段，离线工具
// tools/m27a_add_units.py 生成；原语料 index.json 一字不动）。SELinux 禁 shell 写 app 目录，
// rawfile 是唯一能到设备的通道，所以工作副本走同一套分块拷贝，落到同一个 shader_cache 目录。
const SHADER_CACHE_FILES: string[] = ['index.json', 'spv.bin', 'index_m27a.json'];
const SHADER_CHUNK: number = 8 * 1024 * 1024;

// M4f P2：全量语料的 spv.bin 到 241MiB。getRawFileContent 会把整包塞进 ArkTS 堆（120MiB 用过，
// 241MiB 未验证），所以改走 getRawFd + 8MiB 分块读写；readSync/writeSync 的 offset 都是**文件位置**
//（对 rawfile 而言基准是 HAP 内起点，故读用 raw.offset+done、写用 done），缓冲区恒从 0 起算。
// 目标已存在且大小一致就跳过（每帧启动重拷 241MiB 会白白拖慢首帧）。
async function copyRawFileChunked(context: Context, name: string, dst: string): Promise<number> {
  const rawPath: string = `shadercache/${name}`;
  const raw = await context.resourceManager.getRawFd(rawPath);
  try {
    try {
      const st = fs.statSync(dst);
      if (st.size === raw.length) {
        hilog.info(0x0000, TAG, 'shadercache %{public}s skip (size=%{public}d)', name, st.size);
        return raw.length;
      }
    } catch (e) {
      // 不存在：正常首启，继续拷
    }
    const fout = fs.openSync(dst, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
    const buf = new ArrayBuffer(SHADER_CHUNK);
    let done: number = 0;
    while (done < raw.length) {
      const want: number = Math.min(SHADER_CHUNK, raw.length - done);
      // raw.fd 本身可读（fs.openSync 只收路径，不接受 fd），offset 是 HAP 内的绝对文件位置
      const got: number = fs.readSync(raw.fd, buf, { offset: raw.offset + done, length: want });
      if (got <= 0) {
        break;
      }
      const wrote: number = fs.writeSync(fout.fd, buf, { offset: done, length: got });
      if (wrote !== got) {
        break;
      }
      done += got;
    }
    fs.closeSync(fout);
    if (done !== raw.length) {
      throw new Error(`chunked copy short: ${done}/${raw.length}`);
    }
    return done;
  } finally {
    await context.resourceManager.closeRawFd(rawPath);
  }
}

async function extractShaderCache(context: Context, destDir: string): Promise<void> {
  if (!fs.accessSync(destDir)) {
    fs.mkdirSync(destDir, true);
  }
  for (let i = 0; i < SHADER_CACHE_FILES.length; i++) {
    const name: string = SHADER_CACHE_FILES[i];
    const dst: string = `${destDir}/${name}`;
    try {
      const written: number = await copyRawFileChunked(context, name, dst);
      hilog.info(0x0000, TAG, 'shadercache %{public}s wrote=%{public}d (chunked)', name, written);
    } catch (e) {
      // 分块路径失败（fd/offset 语义与预期不符等）⇒ 回落整块读取，保证不比旧行为更差
      hilog.error(0x0000, TAG, 'shadercache %{public}s chunked failed: %{public}s', name, JSON.stringify(e));
      try {
        const buf: Uint8Array = await context.resourceManager.getRawFileContent(`shadercache/${name}`);
        const file = fs.openSync(dst, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
        const written: number = fs.writeSync(file.fd, buf.buffer,
          { offset: buf.byteOffset, length: buf.length });
        fs.closeSync(file);
        hilog.info(0x0000, TAG, 'shadercache %{public}s want=%{public}d wrote=%{public}d', name, buf.length, written);
      } catch (e2) {
        hilog.error(0x0000, TAG, 'shadercache %{public}s failed: %{public}s', name, JSON.stringify(e2));
      }
    }
  }
}

export default class EntryAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    hilog.info(0x0000, TAG, 'onCreate');
    const destDir: string = `${this.context.filesDir}/smo`;
    AppStorage.setOrCreate('filesDir', this.context.filesDir);
    // 自动链默认开：点开即玩——无参启动也自动拉取+自动点火；--ps autopath 0 / --ps boot 0 可显式关掉
    const params = want.parameters as Record<string, Object>;
    const autopath = (params && params['autopath'] === '0') ? '0' : '1';
    AppStorage.setOrCreate('autopath', autopath);
    const boot = (params && params['boot'] === '0') ? '0' : '1';
    AppStorage.setOrCreate('boot', boot);
    hilog.info(0x0000, TAG, 'autopath=%{public}s boot=%{public}s (default on)', autopath, boot);
    extractSmoImages(this.context, destDir)
      .then(() => {
        AppStorage.setOrCreate('smoReady', true);
        hilog.info(0x0000, TAG, 'smo images ready at %{public}s', destDir);
        // M3r：shader 缓存与游戏镜像同一条通道（rawfile → filesDir），失败只报错不阻断启动
        return extractShaderCache(this.context, `${this.context.filesDir}/shader_cache`);
      })
      .catch((e: BusinessError) => {
        AppStorage.setOrCreate('smoErr', `解包失败 ${e.code}: ${e.message}`);
        hilog.error(0x0000, TAG, 'extract failed code=%{public}d msg=%{public}s', e.code, e.message ?? '');
      });
  }

  onDestroy(): void {
    hilog.info(0x0000, TAG, 'onDestroy');
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    // 沉浸全屏：铺满安全区并隐藏状态栏/导航条（module.json5 只能定方向，全屏靠窗口 API）
    try {
      windowStage.getMainWindowSync().setImmersiveModeEnabledState(true);
    } catch (e) {
      hilog.error(0x0000, TAG, 'immersive failed: %{public}s', JSON.stringify(e));
    }
    windowStage.loadContent('pages/Index')
      .then(() => hilog.info(0x0000, TAG, 'loadContent ok'))
      .catch((e: BusinessError) => {
        hilog.error(0x0000, TAG, 'loadContent failed: %{public}s code=%{public}d', e.message ?? '', e.code);
      });
  }
}
