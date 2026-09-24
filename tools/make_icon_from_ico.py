# favicon.ico -> app_icon.png (192x192)：纯标准库（支持 PNG/BMP 两种 ICO 条目）
import struct, sys, zlib

SRC = sys.argv[1] if len(sys.argv) > 1 else 'favicon.ico'
DST = sys.argv[2] if len(sys.argv) > 2 else 'app_icon.png'
N = 192

def decode_png(d):
    assert d[:8] == b'\x89PNG\r\n\x1a\n', 'not png'
    pos = 8; idat = b''; plte = None; w = h = bd = ct = None
    while pos < len(d):
        ln, typ = struct.unpack('>I4s', d[pos:pos+8]); pos += 8
        c = d[pos:pos+ln]; pos += ln + 4
        if typ == b'IHDR': w, h, bd, ct = struct.unpack('>IIBB', c[:10])
        elif typ == b'PLTE': plte = c
        elif typ == b'IDAT': idat += c
        elif typ == b'IEND': break
    assert bd == 8, 'bit depth %d unsupported' % bd
    ch = {0:1, 2:3, 3:1, 4:2, 6:4}[ct]
    raw = zlib.decompress(idat)
    stride = w * ch
    out = bytearray(); prev = bytearray(stride); i = 0
    for y in range(h):
        f = raw[i]; i += 1
        line = bytearray(raw[i:i+stride]); i += stride
        if f == 1:
            for x in range(ch, stride): line[x] = (line[x] + line[x-ch]) & 255
        elif f == 2:
            for x in range(stride): line[x] = (line[x] + prev[x]) & 255
        elif f == 3:
            for x in range(stride):
                a = line[x-ch] if x >= ch else 0
                line[x] = (line[x] + ((a + prev[x]) >> 1)) & 255
        elif f == 4:
            for x in range(stride):
                a = line[x-ch] if x >= ch else 0
                b = prev[x]; c = prev[x-ch] if x >= ch else 0
                p = a + b - c; pa = abs(p-a); pb = abs(p-b); pc = abs(p-c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 255
        # expand to RGBA
        if ct == 6:
            out += line
        elif ct == 2:
            for x in range(0, stride, 3): out += line[x:x+3] + b'\xff'
        elif ct == 0:
            for x in range(stride): out += bytes((line[x], line[x], line[x], 255))
        elif ct == 4:
            for x in range(0, stride, 2): out += bytes((line[x], line[x], line[x], line[x+1]))
        elif ct == 3:
            for idx in line:
                out += plte[idx*3:idx*3+3] + b'\xff'
        prev = line
    return bytes(out), w, h

def decode_bmp(b, w, h):
    hdr = struct.unpack('<IiiHHIIiiII', b[:40])
    bisize, bw, bh, planes, bpp, comp = hdr[0], hdr[1], hdr[2], hdr[3], hdr[4], hdr[5]
    assert comp == 0 and bpp in (24, 32), 'bmp bpp=%d comp=%d' % (bpp, comp)
    rowsz = ((bw * bpp + 31) // 32) * 4
    # XOR 位图从 biSize 开始（INFO=40，V4/V5=108/124），不是固定 40；
    # ICO 的 biHeight = 图标高×2（XOR+AND 各半）→ 装不下就按减半重试
    xor_off = bisize
    ih = abs(bh)
    if len(b) < xor_off + rowsz * ih:
        ih = abs(bh) // 2
    xor = b[xor_off:xor_off + rowsz * ih]
    assert len(xor) == rowsz * ih, 'xor truncated: need %d have %d (bisize=%d bw=%d bh=%d)' % (
        rowsz * ih, len(xor), bisize, bw, bh)
    out = bytearray(bw * ih * 4)
    has_alpha = False
    for y in range(ih):
        src_row = (ih - 1 - y) if bh > 0 else y   # bottom-up
        for x in range(bw):
            s = src_row * rowsz + x * (bpp // 8)
            d = (y * bw + x) * 4
            bl, g, r = xor[s], xor[s+1], xor[s+2]
            a = xor[s+3] if bpp == 32 else 255
            if a:
                has_alpha = True
            out[d:d+4] = bytes((r, g, bl, a))
    # 透明度：24bpp 用 AND 掩膜；32bpp 有 alpha 用 alpha，alpha 全零时回退掩膜（否则圆角糊成黑底）
    if bpp == 24 or (bpp == 32 and not has_alpha):
        and_off = xor_off + rowsz * ih
        mrowsz = ((bw + 31) // 32) * 4
        if len(b) >= and_off + mrowsz * ih:
            for y in range(ih):
                src_row = (ih - 1 - y) if bh > 0 else y
                for x in range(bw):
                    byte = b[and_off + src_row * mrowsz + (x >> 3)]
                    if byte & (0x80 >> (x & 7)):
                        out[(y * bw + x) * 4 + 3] = 0
        elif bpp == 32:
            for i in range(3, len(out), 4):
                out[i] = 255
    return bytes(out), bw, ih

def scale_nearest(rgba, iw, ih, n):
    out = bytearray(n * n * 4)
    for y in range(n):
        sy = y * ih // n
        row = sy * iw * 4
        for x in range(n):
            s = row + (x * iw // n) * 4
            d = (y * n + x) * 4
            out[d:d+4] = rgba[s:s+4]
    return bytes(out)

def png_encode(rgba, w, h):
    def chunk(t, data):
        c = t + data
        return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)
    raw = b''.join(b'\x00' + rgba[y*w*4:(y+1)*w*4] for y in range(h))
    ihdr = struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr)
            + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))

data = open(SRC, 'rb').read()
res, typ, cnt = struct.unpack_from('<HHH', data, 0)
assert typ == 1 and cnt >= 1, 'not an icon'
ents = []
off = 6
for _ in range(cnt):
    w, h, cc, rv, planes, bpp, size, ofs = struct.unpack_from('<BBBBHHII', data, off); off += 16
    ents.append((w or 256, h or 256, size, ofs, bpp))
ents.sort(key=lambda e: e[0] * e[1], reverse=True)
w, h, size, ofs, bpp = ents[0]
blob = data[ofs:ofs+size]
if blob[:8] == b'\x89PNG\r\n\x1a\n':
    rgba, iw, ih = decode_png(blob)
else:
    rgba, iw, ih = decode_bmp(blob, w, h)
print('source entry: %dx%d bpp=%d' % (iw, ih, bpp))
scaled = scale_nearest(rgba, iw, ih, N)
open(DST, 'wb').write(png_encode(scaled, N, N))
print('wrote %s (%d bytes, %dx%d)' % (DST, len(open(DST,'rb').read()), N, N))