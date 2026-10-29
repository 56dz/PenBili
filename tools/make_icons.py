# -*- coding: utf-8 -*-
"""生成播放页左图标栏用的 PNG 图标（falcon 运行时没有图标字体，只能用位图）。

用法（Windows，需要 Pillow）：
  C:/Users/Daniel/.dsh/dsh-runtimes/dsh-primary-runtime/dependencies/python/python.exe \
      PenBili/tools/make_icons.py

输出目录：PenBili/src/assets/icons/<name>.png
尺寸：4 倍超采样绘制后缩小到 60x60（界面按 26x26 显示，2x 出图防锯齿）。
配色：默认白 #E9ECF0；高亮态（_on）用 B站粉 #FB7299。

画法：所有图标先画成「实心剪影」（灰度遮罩），
  - 实心态：遮罩直接上色；
  - 线框态：遮罩做形态学腐蚀，环 = 剪影 − 腐蚀 ⇒ 描边宽度处处一致，
    比 ImageDraw 的 outline 好看（outline 画凹多边形会出现交叉线）。
"""

import os
import math
from PIL import Image, ImageDraw, ImageFilter

SS = 4            # 超采样倍数
OUT = 60          # 输出边长
S = OUT * SS      # 绘制边长
STROKE = 0.095    # 描边宽度（相对边长）

WHITE = (233, 236, 240, 255)
PINK = (251, 114, 153, 255)

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
DEST = os.path.normpath(os.path.join(ROOT, 'src', 'assets', 'icons'))


def mask():
    im = Image.new('L', (S, S), 0)
    return im, ImageDraw.Draw(im)


def outline_of(m, px):
    """剪影遮罩 → 等宽描边遮罩。"""
    k = max(3, int(px) | 1)          # MinFilter 只接受奇数
    eroded = m.filter(ImageFilter.MinFilter(k))
    ring = Image.new('L', (S, S), 0)
    ring.paste(Image.eval(m, lambda v: 255 - v))
    # ring = m - eroded  ⇒  用 ImageChops 更直接
    from PIL import ImageChops
    ring = ImageChops.subtract(m, eroded)
    return ring


def colorize(m, color):
    im = Image.new('RGBA', (S, S), color[:3] + (0,))
    im.putalpha(m)
    return im


def emit(m, color, name, filled=True):
    mask_img = m if filled else outline_of(m, STROKE * S)
    im = colorize(mask_img, color)
    if not os.path.isdir(DEST):
        os.makedirs(DEST)
    out = im.resize((OUT, OUT), Image.LANCZOS)
    path = os.path.join(DEST, name + '.png')
    out.save(path)
    print('wrote %s (%d bytes)' % (path, os.path.getsize(path)))


# ---------------- 剪影 ----------------

def m_home():
    im, d = mask()
    d.polygon([(S * 0.50, S * 0.10), (S * 0.94, S * 0.52), (S * 0.06, S * 0.52)], fill=255)
    d.rounded_rectangle([S * 0.18, S * 0.46, S * 0.82, S * 0.92], radius=S * 0.06, fill=255)
    # 门（挖空）
    d.rounded_rectangle([S * 0.41, S * 0.62, S * 0.59, S * 0.92], radius=S * 0.03, fill=0)
    return im


def m_like():
    """点赞：拳（圆角矩形）+ 竖起的拇指（胶囊）+ 手腕。"""
    im, d = mask()
    # 手腕
    d.rounded_rectangle([S * 0.10, S * 0.52, S * 0.30, S * 0.88], radius=S * 0.05, fill=255)
    # 拳
    d.rounded_rectangle([S * 0.34, S * 0.40, S * 0.88, S * 0.88], radius=S * 0.09, fill=255)
    # 拇指（竖直胶囊，略向左倾的手感用两段）
    tw = int(S * 0.20)
    d.line([(S * 0.56, S * 0.44), (S * 0.50, S * 0.20)], fill=255, width=tw, joint='curve')
    d.ellipse([S * 0.50 - tw / 2, S * 0.20 - tw / 2, S * 0.50 + tw / 2, S * 0.20 + tw / 2], fill=255)
    # 拳与拇指之间的缝（让它读起来像手指压住）
    d.rounded_rectangle([S * 0.34, S * 0.44, S * 0.58, S * 0.52], radius=S * 0.02, fill=0)
    return im


def m_coin():
    """投币：圆环 + 中间竖槽（B站硬币简化）。"""
    im, d = mask()
    d.ellipse([S * 0.10, S * 0.10, S * 0.90, S * 0.90], fill=255)
    d.rounded_rectangle([S * 0.43, S * 0.28, S * 0.57, S * 0.72], radius=S * 0.04, fill=0)
    return im


def m_star():
    im, d = mask()
    cx = cy = S / 2.0
    R, r = S * 0.44, S * 0.44 * 0.42
    pts = []
    for i in range(10):
        ang = -math.pi / 2 + i * math.pi / 5
        rad = R if i % 2 == 0 else r
        pts.append((cx + rad * math.cos(ang), cy + rad * math.sin(ang)))
    d.polygon(pts, fill=255)
    return im


def m_comment():
    """评论：圆角气泡 + 尾巴 + 三个点（点为挖空）。"""
    im, d = mask()
    d.rounded_rectangle([S * 0.08, S * 0.14, S * 0.92, S * 0.72], radius=S * 0.20, fill=255)
    d.polygon([(S * 0.30, S * 0.64), (S * 0.22, S * 0.92), (S * 0.52, S * 0.64)], fill=255)
    for i in (-1, 0, 1):
        cx = S * 0.50 + i * S * 0.20
        d.ellipse([cx - S * 0.058, S * 0.375, cx + S * 0.058, S * 0.491], fill=0)
    return im


def m_play():
    im, d = mask()
    d.polygon([(S * 0.30, S * 0.12), (S * 0.88, S * 0.50), (S * 0.30, S * 0.88)], fill=255)
    return im


def m_pause():
    im, d = mask()
    d.rounded_rectangle([S * 0.24, S * 0.14, S * 0.44, S * 0.86], radius=S * 0.05, fill=255)
    d.rounded_rectangle([S * 0.56, S * 0.14, S * 0.76, S * 0.86], radius=S * 0.05, fill=255)
    return im


def m_folder():
    im, d = mask()
    d.rounded_rectangle([S * 0.06, S * 0.22, S * 0.94, S * 0.84], radius=S * 0.10, fill=255)
    d.rounded_rectangle([S * 0.18, S * 0.33, S * 0.82, S * 0.72], radius=S * 0.05, fill=0)
    # 上沿缺口（文件夹的标签页）
    d.rounded_rectangle([S * 0.30, S * 0.10, S * 0.62, S * 0.30], radius=S * 0.06, fill=255)
    return im


def m_prev():
    im, d = mask()
    d.polygon([(S * 0.82, S * 0.14), (S * 0.34, S * 0.50), (S * 0.82, S * 0.86)], fill=255)
    d.rounded_rectangle([S * 0.16, S * 0.14, S * 0.30, S * 0.86], radius=S * 0.04, fill=255)
    return im


def m_next():
    im, d = mask()
    d.polygon([(S * 0.18, S * 0.14), (S * 0.66, S * 0.50), (S * 0.18, S * 0.86)], fill=255)
    d.rounded_rectangle([S * 0.70, S * 0.14, S * 0.84, S * 0.86], radius=S * 0.04, fill=255)
    return im


def main():
    specs = [
        ('home', m_home, True, WHITE),
        ('like', m_like, False, WHITE),
        ('like_on', m_like, True, PINK),
        ('coin', m_coin, False, WHITE),
        ('coin_on', m_coin, True, PINK),
        ('star', m_star, False, WHITE),
        ('star_on', m_star, True, PINK),
        ('comment', m_comment, False, WHITE),
        ('comment_on', m_comment, True, PINK),
        ('play', m_play, True, WHITE),
        ('pause', m_pause, True, WHITE),
        ('folder', m_folder, False, WHITE),
        ('prev', m_prev, True, WHITE),
        ('next', m_next, True, WHITE),
    ]
    for name, fn, filled, color in specs:
        emit(fn(), color, name, filled)


if __name__ == '__main__':
    main()
