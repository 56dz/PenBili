# make_icon_v2 第一步：favicon.ico -> tools/icon_colors.json
# 从 ICO 解码像素，统计出背景色(bg)与图形色(fg)：
#   bg = 量化后最大占比色；
#   fg = 与 bg 亮度差 >= 60 的最大占比色（无则取白色——bilibili 图标即蓝底白图形）。
# 复用 make_icon_from_ico 的 PNG/BMP 双格式解码（纯标准库）。
import json
import struct
import sys
import zlib

SRC = sys.argv[1] if len(sys.argv) > 1 else 'favicon.ico'
DST = sys.argv[2] if len(sys.argv) > 2 else 'tools/icon_colors.json'


def decode_png(d):
    assert d[:8] == b'\x89PNG\r\n\x1a\n', 'not png'
    pos, idat, plte = 8, b'', None
    w = h = bd = ct = None
    while pos < len(d):
        ln, typ = struct.unpack('>I4s', d[pos:pos + 8])
        pos += 8
        c = d[pos:pos + ln]
        pos += ln + 4
        if typ == b'IHDR':
            w, h, bd, ct = struct.unpack('>IIBB', c[:10])
        elif typ == b'PLTE':
            plte = c
        elif typ == b'IDAT':
            idat += c
        elif typ == b'IEND':
            break
    assert bd == 8, 'bit depth %d unsupported' % bd
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ct]
    raw = zlib.decompress(idat)
    stride = w * ch
    out, prev, i = bytearray(), bytearray(stride), 0
    for y in range(h):
        f = raw[i]
        i += 1
        line = bytearray(raw[i:i + stride])
        i += stride
        if f == 1:
            for x in range(ch, stride):
                line[x] = (line[x] + line[x - ch]) & 255
        elif f == 2:
            for x in range(stride):
                line[x] = (line[x] + prev[x]) & 255
        elif f == 3:
            for x in range(stride):
                a = line[x - ch] if x >= ch else 0
                line[x] = (line[x] + ((a + prev[x]) >> 1)) & 255
        elif f == 4:
            for x in range(stride):
                a = line[x - ch] if x >= ch else 0
                b = prev[x]
                c = prev[x - ch] if x >= ch else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 255
        if ct == 6:
            out += line
        elif ct == 2:
            for x in range(0, stride, 3):
                out += line[x:x + 3] + b'\xff'
        elif ct == 0:
            for x in range(stride):
                out += bytes((line[x], line[x], line[x], 255))
        elif ct == 4:
            for x in range(0, stride, 2):
                out += bytes((line[x], line[x], line[x], line[x + 1]))
        elif ct == 3:
            for idx in line:
                out += plte[idx * 3:idx * 3 + 3] + b'\xff'
        prev = line
    return bytes(out), w, h


def decode_bmp(b, w, h):
    hdr = struct.unpack('<IiiHHIIiiII', b[:40])
    bisize, bw, bh, planes, bpp, comp = hdr[0], hdr[1], hdr[2], hdr[3], hdr[4], hdr[5]
    assert comp == 0 and bpp in (24, 32), 'bmp bpp=%d comp=%d' % (bpp, comp)
    rowsz = ((bw * bpp + 31) // 32) * 4
    xor_off = bisize
    ih = abs(bh)
    if len(b) < xor_off + rowsz * ih:
        ih = abs(bh) // 2
    xor = b[xor_off:xor_off + rowsz * ih]
    out = bytearray(bw * ih * 4)
    for y in range(ih):
        src_row = (ih - 1 - y) if bh > 0 else y
        for x in range(bw):
            s = src_row * rowsz + x * (bpp // 8)
            d = (y * bw + x) * 4
            bl, g, r = xor[s], xor[s + 1], xor[s + 2]
            a = xor[s + 3] if bpp == 32 else 255
            out[d:d + 4] = bytes((r, g, bl, a))
    if bpp == 24:
        and_off = xor_off + rowsz * ih
        mrowsz = ((bw + 31) // 32) * 4
        if len(b) >= and_off + mrowsz * ih:
            for y in range(ih):
                src_row = (ih - 1 - y) if bh > 0 else y
                for x in range(bw):
                    byte = b[and_off + src_row * mrowsz + (x >> 3)]
                    if byte & (0x80 >> (x & 7)):
                        out[(y * bw + x) * 4 + 3] = 0
    return bytes(out), bw, ih


def luminance(r, g, b):
    return 0.299 * r + 0.587 * g + 0.114 * b


data = open(SRC, 'rb').read()
_, typ, cnt = struct.unpack_from('<HHH', data, 0)
assert typ == 1 and cnt >= 1, 'not an icon'
ents, off = [], 6
for _ in range(cnt):
    w, h, cc, rv, planes, bpp, size, ofs = struct.unpack_from('<BBBBHHII', data, off)
    off += 16
    ents.append((w or 256, h or 256, size, ofs, bpp))
ents.sort(key=lambda e: e[0] * e[1], reverse=True)
w, h, size, ofs, bpp = ents[0]
blob = data[ofs:ofs + size]
if blob[:8] == b'\x89PNG\r\n\x1a\n':
    rgba, iw, ih = decode_png(blob)
else:
    rgba, iw, ih = decode_bmp(blob, w, h)
print('ico entry: %dx%d' % (iw, ih))

# 量化统计（每通道 4bit 量化到 4096 桶，滤噪声）
buckets = {}
n = iw * ih
for p in range(n):
    o = p * 4
    a = rgba[o + 3]
    if a < 128:
        continue
    r, g, b = rgba[o], rgba[o + 1], rgba[o + 2]
    key = ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)
    cur = buckets.get(key)
    if cur is None:
        buckets[key] = [1, r, g, b]
    else:
        cur[0] += 1
        cur[1] += r
        cur[2] += g
        cur[3] += b
total = sum(v[0] for v in buckets.values())
ranked = sorted(buckets.values(), key=lambda v: v[0], reverse=True)
top = []
for v in ranked:
    cnt_, r, g, b = v[0], v[1] // v[0], v[2] // v[0], v[3] // v[0]
    top.append({'rgb': (r, g, b), 'ratio': cnt_ / total})
    if len(top) >= 8:
        break

bg = top[0]['rgb']
bg_lum = luminance(*bg)
fg = None
for t in top[1:]:
    if abs(luminance(*t['rgb']) - bg_lum) >= 60:
        fg = t['rgb']
        break
if fg is None:
    fg = (255, 255, 255)


def hx(c):
    return '#%02x%02x%02x' % c


out = {
    'bg': hx(bg),
    'fg': hx(fg),
    'top': [{'color': hx(t['rgb']), 'ratio': round(t['ratio'], 4)} for t in top[:5]],
}
open(DST, 'w').write(json.dumps(out, ensure_ascii=False, indent=1))
print('bg=%s fg=%s' % (out['bg'], out['fg']))
print('top:', out['top'])
