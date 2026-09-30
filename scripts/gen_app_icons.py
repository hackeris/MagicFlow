#!/usr/bin/env python3
"""gen_app_icons.py —— 生成应用图标(桌面分层图标 + 启动窗口图标)。

正确命令/目录/产物:
  命令: python3 scripts/gen_app_icons.py        (在仓库根目录执行)
  产物: entry/src/main/resources/base/media/{background,foreground,startIcon}.png
        AppScope/resources/base/media/{background,foreground}.png
  验收: 尺寸 + 分层结构 + 符号墨迹三重断言, 失败非零退出。

设计(2026-09-26 用户定版):
  background = 紫色对角渐变 #8B5CF6→#4F46E5(与首页 logo 的 linearGradient 135° 同源)
  foreground = 白色 wand_and_stars 符号, 无底
  两层分工而非"一层画完" —— 分层图标的意义就在此: 系统对 background 做圆角遮罩、
  对 foreground 做视差(放大+位移), 背景色交给 background 层才能被正确遮罩。
  (初版把渐变圆+深色底都画在 foreground 上, 结果圆外露出一圈深色"相框", 且符号被
   圆径二次约束、放不大。)

符号来源:
  字体: /apps/harmony/sdk/default/hms/previewer/resources/fonts/HMSymbolVF.ttf
        (SDK 预览器目录内, 可直接读; 设备侧 /system/fonts/ 是系统分区, shell 无权读)
  码点: U+0F0157 —— sys.symbol.* 是 PUA 编码, 自 0xF0000 起按序分配。
        SDK 的 sysResource.js 里 wand_and_stars 的资源 ID = 125831700(=0x780_0A14),
        与码点不是同一套编号, 勿据资源 ID 反推(本次踩过: 按低位 0xA14 猜 0xF0A14
        得到的是 battery_bolt_mirroring)。查码点的正确方式是遍历 cmap 按 glyph 名匹配。
  ⚠ SDK 升级后若字体路径变化/码点位移, 本脚本会在断言处失败 —— 那时重新按上述方法查名。

坑:
  - foreground 必须留安全边距: 系统对 foreground 做视差放大(约 1.1~1.2×)后若顶到边会被裁,
    故断言"边角透明", 且符号墨迹宽不超过画布的 0.55;
  - 符号按**墨迹**而非 em 框居中 —— glyph 墨迹在 em 内是偏的(包围盒 y -23~783, em 中心 500),
    直接 anchor='mm' 会视觉偏下。
"""
import os
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFont

BG_IN, BG_OUT = (0x8B, 0x5C, 0xF6), (0x4F, 0x46, 0xE5)   # 紫色渐变(与首页 logo 同源)
SIZE = 1024

SYMBOL_FONT = "/apps/harmony/sdk/default/hms/previewer/resources/fonts/HMSymbolVF.ttf"
SYMBOL_CP = 0xF0157          # sys.symbol.wand_and_stars
# 符号墨迹宽 / 画布 —— 无圆形约束后可放大; 0.50 兼顾"饱满"与"系统视差放大后不裁"
SYMBOL_INK_RATIO = 0.50


def lerp(a, b, t):
    return tuple(round(x + (y - x) * t) for x, y in zip(a, b))


def diagonal_gradient(size, c_in, c_out):
    """左上→右下 的对角线性渐变(对应 ArkUI linearGradient angle:135)。"""
    img = Image.new("RGBA", (size, size))
    px = img.load()
    for y in range(size):
        for x in range(size):
            px[x, y] = lerp(c_in, c_out, (x + y) / (2 * (size - 1))) + (255,)
    return img


def make_background():
    return diagonal_gradient(SIZE, BG_IN, BG_OUT)


def render_symbol(target_ink_width):
    """把 wand_and_stars 渲染为白色 RGBA, 按墨迹裁剪后缩放到指定墨迹宽度。"""
    if not os.path.exists(SYMBOL_FONT):
        raise SystemExit(
            f"FATAL: 符号字体缺失 {SYMBOL_FONT}\n"
            f"  SDK 升级可能改了路径 —— 见本文件头「符号来源」节重新定位。")
    font = ImageFont.truetype(SYMBOL_FONT, 1000)
    canvas = Image.new("RGBA", (2400, 2400), (0, 0, 0, 0))
    ImageDraw.Draw(canvas).text(
        (1200, 1200), chr(SYMBOL_CP), font=font,
        fill=(255, 255, 255, 255), anchor="mm")
    ink = canvas.crop(canvas.getbbox())          # 非零像素包围盒 = 墨迹
    if ink.width < 100:
        raise SystemExit(
            f"FATAL: 码点 U+{SYMBOL_CP:04X} 渲染为空 —— 字体版本可能已变, "
            f"见本文件头「符号来源」节按 glyph 名重查。")
    scale = target_ink_width / ink.width
    return ink.resize(
        (round(ink.width * scale), round(ink.height * scale)), Image.LANCZOS)


def make_foreground():
    """前景层: 透明底 + 居中的白色符号(背景色归 background 层)。"""
    img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    sym = render_symbol(int(SIZE * SYMBOL_INK_RATIO))
    img.alpha_composite(sym, ((SIZE - sym.width) // 2, (SIZE - sym.height) // 2))
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

    bg_check = Image.open("entry/src/main/resources/base/media/background.png")
    edge = int(SIZE * 0.02)
    tl = bg_check.getpixel((edge, edge))[:3]
    br = bg_check.getpixel((SIZE - edge, SIZE - edge))[:3]
    for name, got, want in (("左上", tl, BG_IN), ("右下", br, BG_OUT)):
        assert all(abs(a - b) < 14 for a, b in zip(got, want)), \
            f"FATAL: 背景{name}应为 rgb{want}, 实为 {got}(紫色渐变缺失?)"

    fg_check = Image.open("entry/src/main/resources/base/media/foreground.png")
    assert fg_check.getpixel((4, 4))[3] < 10, \
        "FATAL: 前景层边角应透明(缺安全边距, 系统视差放大后会裁切)"
    assert fg_check.getpixel((SIZE // 2, 4))[3] < 10, \
        "FATAL: 前景层上边缘中点应透明(符号过大, 视差放大后会裁切)"

    # 符号存在性: 量「纯白墨迹」的包围盒宽度并与设计值比对。不用单点探测 ——
    # 魔杖+三星有大量空隙, 单点极易落在空隙上(首版取圆心上方即踩到)。
    r_ch, g_ch, b_ch, _ = fg_check.split()

    def binarize(v):
        return 255 if v > 240 else 0

    white = ImageChops.multiply(
        ImageChops.multiply(r_ch.point(binarize), g_ch.point(binarize)),
        b_ch.point(binarize))
    bb = white.getbbox()
    assert bb, "FATAL: 前景层无白色符号墨迹(wand_and_stars 未渲染?)"
    ink_w = bb[2] - bb[0]
    expect = int(SIZE * SYMBOL_INK_RATIO)
    assert abs(ink_w - expect) <= expect * 0.12, \
        f"FATAL: 符号墨迹宽 {ink_w}px 偏离设计值 {expect}px 超 ±12%"
    print(f"[OK] 背景紫色对角渐变 + 符号墨迹 {ink_w}px(设计 {expect}px)")


if __name__ == "__main__":
    sys.exit(main())
