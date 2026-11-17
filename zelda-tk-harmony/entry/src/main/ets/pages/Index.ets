import { game } from 'libgame.so';

@Entry
@Component
struct Index {
  @State status: string = '就绪';
  @State started: boolean = false;

  build() {
    Column({ space: 16 }) {
      Text('塞尔达传说：王国之泪 · 鸿蒙版')
        .fontSize(28)
        .fontWeight(FontWeight.Bold)
      Text('转译自 Tears of the Kingdom 1.1.2 (ARM64 静态重编译)')
        .fontSize(13)
        .opacity(0.7)
      Text(this.status).fontSize(16).fontColor('#4fc3f7')

      Button(this.started ? '停止' : '启动游戏')
        .width(220).height(52)
        .onClick(() => {
          if (!this.started) {
            const rc = game.start('');   // 周末接通：传入 romfs 路径
            this.status = rc === 0 ? '游戏线程已启动（宿主接通后出画面）' : `启动失败 rc=${rc}`;
          } else {
            game.stop();
            this.status = '已停止';
          }
          this.started = !this.started;
        })

      // 周末：此处替换为 XComponent(type=surface) 承载 Vulkan swapchain，
      // 并叠加虚拟手柄组件（左摇杆/ABXY/扳机/陀螺仪转发）
      Text('画面区域：周末接入 XComponent + Vulkan').fontSize(12).opacity(0.5)
    }
    .width('100%').height('100%')
    .justifyContent(FlexAlign.Center)
    .backgroundColor('#101014')
  }
}
