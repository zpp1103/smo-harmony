import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import { fileIo as fs } from '@kit.CoreFileKit';
import { BusinessError } from '@kit.BasicServicesKit';

// {{...}} 占位由 scripts/build_hap_from_profile.py 从 games/<titleID>/game.tsv 实例化：
//   GAME_ID_DIR  游戏沙箱/rawfile 目录名（title_id）；与 cpp/game_id.h 的 GAME_ID_DIR 同值
//   RAWFILE_MODULES  rawfile 里随包分发的模块镜像（rtld + game.tsv modules 键倒序），smo 为 4 件
const TAG = 'GameEntry';
const GAME_ID_DIR = '{{GAME_ID_DIR}}';
const RAWFILE_MODULES: string[] = [{{RAWFILE_MODULES}}];

// 首启把 rawfile 里的游戏镜像解到 files/<game>/（HAP rawfile 不能 mmap，loader 需要真实文件路径）
async function extractGameImages(context: Context, destDir: string): Promise<void> {
  if (!fs.accessSync(destDir)) {
    fs.mkdirSync(destDir, true);
  }
  for (let i = 0; i < RAWFILE_MODULES.length; i++) {
    const name: string = RAWFILE_MODULES[i];
    const dst: string = `${destDir}/${name}.patched.bin`;
    hilog.info(0x0000, TAG, 'extract %{public}s', name);
    const buf: Uint8Array = await context.resourceManager.getRawFileContent(`${GAME_ID_DIR}/${name}.patched.bin`);
    const file = fs.openSync(dst, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
    // API22 坑：writeSync(ArrayBuffer) 不带显式 length 会少写尾部（sdk 少 832B），必须带 byteOffset/length
    const written: number = fs.writeSync(file.fd, buf.buffer,
      { offset: buf.byteOffset, length: buf.length });
    fs.closeSync(file);
    hilog.info(0x0000, TAG, 'extract %{public}s want=%{public}d wrote=%{public}d', name, buf.length, written);
  }
}

export default class EntryAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    hilog.info(0x0000, TAG, 'onCreate');
    const destDir: string = `${this.context.filesDir}/${GAME_ID_DIR}`;
    AppStorage.setOrCreate('filesDir', this.context.filesDir);
    // 调试通道：aa start --ps autopath 1 --ps boot 1 时，页面自动拉取+自动点火（锁屏也能跑）
    const params = want.parameters as Record<string, Object>;
    if (params && params['autopath'] === '1') {
      AppStorage.setOrCreate('autopath', '1');
      hilog.info(0x0000, TAG, 'autopath=1, page will auto-pull');
    }
    if (params && params['boot'] === '1') {
      AppStorage.setOrCreate('boot', '1');
      hilog.info(0x0000, TAG, 'boot=1, page will auto-start game');
    }
    extractGameImages(this.context, destDir)
      .then(() => {
        AppStorage.setOrCreate('gameReady', true);
        hilog.info(0x0000, TAG, 'game images ready at %{public}s', destDir);
      })
      .catch((e: BusinessError) => {
        AppStorage.setOrCreate('gameErr', `解包失败 ${e.code}: ${e.message}`);
        hilog.error(0x0000, TAG, 'extract failed code=%{public}d msg=%{public}s', e.code, e.message ?? '');
      });
  }

  onDestroy(): void {
    hilog.info(0x0000, TAG, 'onDestroy');
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    windowStage.loadContent('pages/Index')
      .then(() => hilog.info(0x0000, TAG, 'loadContent ok'))
      .catch((e: BusinessError) => {
        hilog.error(0x0000, TAG, 'loadContent failed: %{public}s code=%{public}d', e.message ?? '', e.code);
      });
  }
}
