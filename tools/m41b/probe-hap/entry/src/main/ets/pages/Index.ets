@Entry
@Component
struct Index {
  @State message: string = 'm41b VK probe — see hilog tag M41B';

  build() {
    RelativeContainer() {
      Text(this.message)
        .id('M41B')
        .fontSize(18)
        .fontWeight(FontWeight.Bold)
        .alignRules({
          center: { anchor: '__container__', align: VerticalAlign.Center },
          middle: { anchor: '__container__', align: HorizontalAlign.Center }
        })
    }
    .height('100%')
    .width('100%')
  }
}
