#!/usr/bin/env python3
"""M42F：把「被选为源的 RT dump」与「同刻屏幕截图」并排拼成一张对照图（判据④）。

左 = 源 RT（800x450，PNG，来自 app 侧 PAM dump）   右 = 屏幕截图（2848x1320，JPEG）
两图各等比缩到同一高度，底部加一行文字标注文件名与尺寸。
用法: python3 tools/m42f_pair.py <src.png> <shot.jpeg> <out.png>
"""
import sys
from PIL import Image, ImageDraw

H = 620
PAD = 12


def fit(path, h):
    im = Image.open(path).convert('RGB')
    w = max(1, int(im.width * h / im.height))
    return im.resize((w, h), Image.LANCZOS)


def main():
    src, shot, out = sys.argv[1], sys.argv[2], sys.argv[3]
    a = fit(src, H)
    b = fit(shot, H)
    W = a.width + b.width + PAD * 3
    canvas = Image.new('RGB', (W, H + 40), (24, 24, 24))
    canvas.paste(a, (PAD, 8))
    canvas.paste(b, (PAD * 2 + a.width, 8))
    d = ImageDraw.Draw(canvas)
    lab = 'SOURCE RT (present src): %s  %dx%d' % (src.split('/')[-1], Image.open(src).width, Image.open(src).height)
    lab2 = 'SCREEN: %s  %dx%d' % (shot.split('/')[-1], Image.open(shot).width, Image.open(shot).height)
    d.text((PAD, H + 14), lab, fill=(240, 240, 240))
    d.text((PAD * 2 + a.width, H + 14), lab2, fill=(240, 240, 240))
    canvas.save(out, 'PNG')
    print(out)


if __name__ == '__main__':
    main()
