# -*- coding: utf-8 -*-
"""把 svg/ 下的 B站官方 SVG 图标栅格化成 src/assets/icons/ 用的 PNG。

只用标准库 + Pillow：这三个 path 只用 M/L/H/V/C/Z（C 是三次贝塞尔），
自己写扫描线多边形填充不现实 → 用 Pillow 的 ImageDraw 在「拍平后的贝塞尔」
折线（每段 C 细分 24 步）上填充，4x 超采样后 LANCZOS 缩到 60x60。
"""
import os
import re
import sys
from PIL import Image, ImageDraw

SS = 4
OUT = 60
S = OUT * SS
WHITE = (233, 236, 240, 255)
PINK = (251, 114, 153, 255)
ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
DEST = os.path.join(ROOT, 'src', 'assets', 'icons')


def tokenize(d):
    for m in re.finditer(r'([MLHVCSQTAZmlhvcstaz])|(-?\d*\.?\d+(?:e[-+]?\d+)?)', d):
        if m.group(1):
            yield ('cmd', m.group(1))
        else:
            yield ('num', float(m.group(2)))


def flatten(d, steps=24):
    """SVG path → 折线列表（每条闭合子路径一个 list[(x,y)]）。"""
    toks = list(tokenize(d))
    i = 0
    cur = (0.0, 0.0)
    start = (0.0, 0.0)
    sub = []
    polys = []

    def flush():
        nonlocal sub
        if len(sub) >= 3:
            polys.append(sub)
        sub = []

    def num():
        nonlocal i
        v = toks[i][1]
        i += 1
        return v

    last_cmd = None
    while i < len(toks):
        kind, val = toks[i]
        if kind == 'cmd':
            cmd = val
            i += 1
        else:
            cmd = last_cmd  # 隐式重复
        last_cmd = cmd
        rel = cmd.islower()
        c = cmd.upper()
        if c == 'M':
            x, y = num(), num()
            if rel:
                x += cur[0]; y += cur[1]
            flush()
            cur = (x, y); start = cur
            sub = [cur]
        elif c == 'L':
            x, y = num(), num()
            if rel:
                x += cur[0]; y += cur[1]
            cur = (x, y); sub.append(cur)
        elif c == 'H':
            x = num()
            if rel:
                x += cur[0]
            cur = (x, cur[1]); sub.append(cur)
        elif c == 'V':
            y = num()
            if rel:
                y += cur[1]
            cur = (cur[0], y); sub.append(cur)
        elif c == 'C':
            x1, y1 = num(), num()
            x2, y2 = num(), num()
            x, y = num(), num()
            if rel:
                x1 += cur[0]; y1 += cur[1]
                x2 += cur[0]; y2 += cur[1]
                x += cur[0]; y += cur[1]
            p0 = cur
            for t in [k / steps for k in range(1, steps + 1)]:
                mt = 1 - t
                bx = mt**3 * p0[0] + 3 * mt**2 * t * x1 + 3 * mt * t**2 * x2 + t**3 * x
                by = mt**3 * p0[1] + 3 * mt**2 * t * y1 + 3 * mt * t**2 * y2 + t**3 * y
                sub.append((bx, by))
            cur = (x, y)
        elif c == 'Z':
            if sub:
                sub.append(start)
                cur = start
            flush()
        else:
            raise SystemExit('unsupported cmd ' + cmd)
    flush()
    return polys


def evenodd_mask(polys, size):
    """evenodd 填充：逐行扫描线，求所有边交点，按 x 排序后成对填充。"""
    im = Image.new('L', (size, size), 0)
    px = im.load()
    edges = []
    for poly in polys:
        n = len(poly)
        for k in range(n):
            x0, y0 = poly[k]
            x1, y1 = poly[(k + 1) % n]
            if y0 != y1:
                edges.append((x0, y0, x1, y1))
    for yy in range(size):
        yc = yy + 0.5
        xs = []
        for x0, y0, x1, y1 in edges:
            if (y0 <= yc < y1) or (y1 <= yc < y0):
                xs.append(x0 + (yc - y0) * (x1 - x0) / (y1 - y0))
        xs.sort()
        for k in range(0, len(xs) - 1, 2):
            a = max(0, int(round(xs[k])))
            b = min(size, int(round(xs[k + 1])))
            if b > a:
                for xx in range(a, b):
                    px[xx, yy] = 255
    return im


def raster(svg_path, size):
    txt = open(svg_path, encoding='utf-8').read()
    vb = re.search(r'viewBox="([\d\.\s\-]+)"', txt)
    vx, vy, vw, vh = [float(x) for x in vb.group(1).split()]
    scale = size / max(vw, vh)
    ox = (size - vw * scale) / 2 - vx * scale
    oy = (size - vh * scale) / 2 - vy * scale
    all_polys = []
    for m in re.finditer(r'<path[^>]*d="([^"]+)"', txt):
        for poly in flatten(m.group(1)):
            all_polys.append([(x * scale + ox, y * scale + oy) for x, y in poly])
    return evenodd_mask(all_polys, size)


def emit(mask_im, color, name):
    im = Image.new('RGBA', (S, S), color[:3] + (0,))
    im.putalpha(mask_im)
    im = im.resize((OUT, OUT), Image.LANCZOS)
    os.makedirs(DEST, exist_ok=True)
    p = os.path.join(DEST, name + '.png')
    im.save(p)
    print('wrote %s (%d bytes)' % (p, os.path.getsize(p)))


def back_mask(size):
    """指向左边的直角（「<」形）：两段粗胶囊，左端相接。"""
    im = Image.new('L', (size, size), 0)
    dr = ImageDraw.Draw(im)
    w = int(size * 0.16)
    # 左上 → 左中；左中 → 左下（两段斜线组成直角尖）
    dr.line([(size * 0.72, size * 0.14), (size * 0.28, size * 0.50)], fill=255, width=w)
    dr.line([(size * 0.28, size * 0.50), (size * 0.72, size * 0.86)], fill=255, width=w)
    # 圆头
    r = w / 2
    for (cx, cy) in [(size * 0.72, size * 0.14), (size * 0.28, size * 0.50), (size * 0.72, size * 0.86)]:
        dr.ellipse([cx - r, cy - r, cx + r, cy + r], fill=255)
    return im


def logo_mask(size):
    """PenBili 软件标志：电视圆角矩形 + 顶部两根天线 + 屏幕里的播放三角（挖空）。"""
    im = Image.new('L', (size, size), 0)
    dr = ImageDraw.Draw(im)
    # 天线
    aw = int(size * 0.06)
    dr.line([(size * 0.32, size * 0.20), (size * 0.46, size * 0.34)], fill=255, width=aw)
    dr.line([(size * 0.68, size * 0.20), (size * 0.54, size * 0.34)], fill=255, width=aw)
    # 机身
    dr.rounded_rectangle([size * 0.08, size * 0.32, size * 0.92, size * 0.88], radius=size * 0.10, fill=255)
    # 屏幕（挖空）
    dr.rounded_rectangle([size * 0.16, size * 0.40, size * 0.84, size * 0.80], radius=size * 0.06, fill=0)
    # 播放三角
    dr.polygon([(size * 0.42, size * 0.48), (size * 0.66, size * 0.60), (size * 0.42, size * 0.72)], fill=255)
    return im


def emit_mask(mask_im, color, name):
    im = Image.new('RGBA', (S, S), color[:3] + (0,))
    im.putalpha(mask_im)
    im = im.resize((OUT, OUT), Image.LANCZOS)
    os.makedirs(DEST, exist_ok=True)
    p = os.path.join(DEST, name + '.png')
    im.save(p)
    print('wrote %s (%d bytes)' % (p, os.path.getsize(p)))


def main():
    base = os.path.join(ROOT, 'svg')
    jobs = [
        ('点赞.svg', 'like', WHITE), ('点赞.svg', 'like_on', PINK),
        ('投币.svg', 'coin', WHITE), ('投币.svg', 'coin_on', PINK),
        ('收藏.svg', 'star', WHITE), ('收藏.svg', 'star_on', PINK),
    ]
    for fn, name, color in jobs:
        m = raster(os.path.join(base, fn), S)
        emit(m, color, name)
    # 「返回」直角箭头 + 软件 logo（自绘剪影）
    emit_mask(back_mask(S), WHITE, 'back')
    emit_mask(logo_mask(S), PINK, 'logo')


if __name__ == '__main__':
    main()
