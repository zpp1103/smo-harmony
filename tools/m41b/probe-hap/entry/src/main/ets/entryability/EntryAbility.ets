import { AbilityConstant, ConfigurationConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import { fileIo } from '@kit.CoreFileKit';
import vkprobe from 'libvkprobe.so';

const DOMAIN = 0x0000;
const TAG = 'M41B';

export default class EntryAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    this.context.getApplicationContext().setColorMode(ConfigurationConstant.ColorMode.COLOR_MODE_NOT_SET);
    hilog.info(DOMAIN, TAG, '%{public}s', 'm41b ability onCreate');

    // 启动即跑 Vulkan 探针：① hilog 逐行 ② 沙箱文件
    try {
      hilog.info(DOMAIN, TAG, '%{public}s', 'probe start');
      const report: string = vkprobe.runProbe();
      hilog.info(DOMAIN, TAG, '%{public}s', `probe done, bytes=${report.length}`);

      const lines: string[] = report.split('\n');
      for (let i = 0; i < lines.length; i++) {
        const ln = lines[i];
        if (ln.length === 0) {
          continue;
        }
        hilog.info(DOMAIN, TAG, 'L%{public}d|%{public}s', i, ln.substring(0, 800));
      }

      const path: string = this.context.filesDir + '/m41b_vk_table.txt';
      const f = fileIo.openSync(path, fileIo.OpenMode.CREATE | fileIo.OpenMode.READ_WRITE | fileIo.OpenMode.TRUNC);
      fileIo.writeSync(f.fd, report);
      fileIo.closeSync(f);
      hilog.info(DOMAIN, TAG, '%{public}s', `probe file written: ${path}`);
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'probe exception: %{public}s', JSON.stringify(e));
    }
  }

  onDestroy(): void {
    hilog.info(DOMAIN, TAG, '%{public}s', 'm41b onDestroy');
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    hilog.info(DOMAIN, TAG, '%{public}s', 'm41b onWindowStageCreate');
    windowStage.loadContent('pages/Index', (err) => {
      if (err.code) {
        hilog.error(DOMAIN, TAG, 'loadContent failed: %{public}s', JSON.stringify(err));
        return;
      }
      hilog.info(DOMAIN, TAG, '%{public}s', 'loadContent ok');
    });
  }

  onWindowStageDestroy(): void {}
  onForeground(): void {}
  onBackground(): void {}
}
