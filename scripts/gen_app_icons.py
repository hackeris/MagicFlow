#!/usr/bin/env python3
"""gen_app_icons.py —— 生成应用图标(桌面分层图标 + 启动窗口图标)。

正确命令/目录/产物:
  命令: python3 scripts/gen_app_icons.py        (在仓库根目录执行)
  产物: entry/src/main/resources/base/media/{background,foreground,startIcon}.png
        AppScope/resources/base/media/{background,foreground}.png
  验收: 尺寸 + 前景层透明度双重断言, 失败非零退出。

设计来源: 复用应用首页(entry/src/main/ets/pages/Index.ets)的视觉语言 ——
  深空渐变底 #0A0E1E→#3B2A78 + 紫蓝渐变圆 #8B5CF6→#4F46E5 + 四角星光。
  替换 DevEco 脚手架模板图(蓝色四方格, 2026-08-20 起从未更换)。
  详见 docs/superpowers/specs/2026-09-24-branding-cleanup-design.md §3.6

坑: 分层图标的 foreground 必须留出安全边距(系统会对 background 做圆角遮罩,
  前景若顶到边会被裁), 故断言"边角透明"。
"""
import sys

from PIL import Image, ImageDraw

BG_TOP, BG_BOTTOM = (0x0A, 0x0E, 0x1E), (0x3B, 0x2A, 0x78)      # 首页背景渐变起止
CIRCLE_IN, CIRCLE_OUT = (0x8B, 0x5C, 0xF6), (0x4F, 0x46, 0xE5)  # 首页 logo 渐变圆
SIZE = 1024


def lerp(a, b, t):
    return tuple(round(x + (y - x) * t) for x, y in zip(a, b))


def vertical_gradient(size, top, bottom):
    img = Image.new("RGBA", (size, size))
    d = ImageDraw.Draw(img)
    for y in range(size):
        d.line([(0, y), (size, y)], fill=lerp(top, bottom, y / (size - 1)) + (255,))
    return img


def make_background():
    return vertical_gradient(SIZE, BG_TOP, BG_BOTTOM)


def make_foreground():
    """前景层: 居中的紫蓝渐变圆 + 四角星光。"""
    img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    r = int(SIZE * 0.30)
    c = SIZE // 2
    for i in range(r, 0, -1):
        t = 1 - i / r
        d.ellipse([c - i, c - i, c + i, c + i], fill=lerp(CIRCLE_IN, CIRCLE_OUT, t) + (255,))
    # 四角星光(简洁几何, 与首页 sys.symbol.wand_and_stars 的星形呼应)
    star = int(SIZE * 0.085)
    for dx, dy, s in ((-1, -1, star), (1, -1, int(star * 0.7)),
                      (-1, 1, int(star * 0.7)), (1, 1, int(star * 0.55))):
        cx, cy = c + dx * int(SIZE * 0.26), c + dy * int(SIZE * 0.26)
        d.polygon([(cx, cy - s), (cx + s // 4, cy - s // 4), (cx + s, cy),
                   (cx + s // 4, cy + s // 4), (cx, cy + s),
                   (cx - s // 4, cy + s // 4), (cx - s, cy),
                   (cx - s // 4, cy - s // 4)], fill=(255, 255, 255, 235))
    return img


def main():
    bg, fg = make_background(), make_foreground()
    layered_targets = [
        ("entry/src/main/resources/base/media/background.png", bg),
        ("entry/src/main/resources/base/media/foreground.png", fg),
        ("AppScope/resources/base/media/background.png", bg),
        ("AppScope/resources/base/media/foreground.png", fg),
    ]
    for path, im in layered_targets:
        im.save(path)
        print(f"[write] {path} {im.size}")

    # 启动窗口图标: 背景 + 前景合成后缩到 144x144
    start_path = "entry/src/main/resources/base/media/startIcon.png"
    Image.alpha_composite(bg, fg).resize((144, 144), Image.LANCZOS).save(start_path)
    print(f"[write] {start_path} (144, 144)")

    # ── 验收断言(失败必须非零退出) ──
    for path, _ in layered_targets:
        im = Image.open(path)
        assert im.size == (SIZE, SIZE), f"FATAL: {path} 尺寸 {im.size} != ({SIZE}, {SIZE})"
    assert Image.open(start_path).size == (144, 144), "FATAL: startIcon.png 尺寸错误"

    fg_check = Image.open("entry/src/main/resources/base/media/foreground.png")
    assert fg_check.getpixel((SIZE // 2, SIZE // 2))[3] > 200, \
        "FATAL: 前景层中心应不透明(渐变圆缺失?)"
    assert fg_check.getpixel((4, 4))[3] < 10, \
        "FATAL: 前景层边角应透明(缺安全边距, 系统圆角遮罩会裁切)"
    print("[OK] 图标生成通过验收")


if __name__ == "__main__":
    sys.exit(main())
