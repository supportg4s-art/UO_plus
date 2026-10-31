"""Картинки сетки UO+ (гампы окна-сетки и значок #) и verdata с ними.
gen.py <исходный verdata.mul> <выход verdata> [картинки.png для проверки]"""
import struct, sys
CELL, PAD, HEAD = 44, 6, 22
SIZES = ([(8, r) for r in range(1, 9)] + [(10, r) for r in range(7, 14)]   # (столбцов, строк) — как GSIZES в grid.cpp
         + [(12, 11), (12, 12), (12, 13), (14, 12), (14, 13), (14, 14), (16, 14), (16, 15), (16, 16)])
GUMP0, ART_HASH = 0xF000, 0x3F1F

def c15(r, g, b): return ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3) or 1

def grid_pixels(cols, rows):
    w, h = PAD * 2 + cols * CELL, HEAD + rows * CELL + PAD
    bg, cell, line, gold, head, gold2 = c15(22, 20, 18), c15(34, 32, 30), c15(70, 66, 60), c15(190, 155, 80), c15(44, 38, 30), c15(120, 95, 50)
    px = [[bg] * w for _ in range(h)]
    for y in range(HEAD):
        for x in range(w): px[y][x] = head
    for i in range(cols):
        for j in range(rows):
            x0, y0 = PAD + i * CELL, HEAD + j * CELL
            for y in range(y0, y0 + CELL):
                for x in range(x0, x0 + CELL):
                    edge = x in (x0, x0 + CELL - 1) or y in (y0, y0 + CELL - 1)
                    px[y][x] = line if edge else cell
    for x in range(w):
        px[0][x] = px[h - 1][x] = gold; px[1][x] = px[h - 2][x] = gold2; px[HEAD - 1][x] = gold2
    for y in range(h):
        px[y][0] = px[y][w - 1] = gold; px[y][1] = px[y][w - 2] = gold2
    return w, h, px

def enc_gump(w, h, px):
    rows = []
    for y in range(h):
        runs, x = [], 0
        while x < w:
            c, n = px[y][x], 1
            while x + n < w and px[y][x + n] == c: n += 1
            runs.append(struct.pack('<HH', c, n)); x += n
        rows.append(b''.join(runs))
    off, table, data = h, [], b''
    for r in rows:
        table.append(off); off += len(r) // 4; data += r
    return struct.pack('<%dI' % h, *table) + data

def hash_pixels():
    s = 16
    gold, dark = c15(235, 200, 110), c15(60, 45, 20)
    s = 18
    bgc, edge = c15(52, 44, 32), c15(120, 95, 50)
    px = [[bgc] * s for _ in range(s)]   # сплошной квадрат — чтобы клик попадал везде
    for i in range(s):
        px[0][i] = px[s - 1][i] = px[i][0] = px[i][s - 1] = edge
    for y in range(3, s - 3):
        for x in range(3, s - 3):
            if x in (6, 7, 11, 12) or y in (6, 7, 11, 12): px[y][x] = gold
    return s, s, px

def enc_art(w, h, px):   # статика: флаг, w, h, таблица строк (в словах), (xoff, run, пиксели)...
    rows = []
    for y in range(h):
        r, x, last = b'', 0, 0
        while x < w:
            if px[y][x] == 0: x += 1; continue
            st = x
            while x < w and px[y][x] != 0: x += 1
            r += struct.pack('<HH', st - last, x - st) + struct.pack('<%dH' % (x - st), *px[y][st:x]); last = x
        rows.append(r + struct.pack('<HH', 0, 0))
    table, off, data = [], 0, b''
    for r in rows: table.append(off); off += len(r) // 2; data += r
    return struct.pack('<IHH', 1234, w, h) + struct.pack('<%dH' % h, *table) + data

def build(src, dst):
    d = open(src, 'rb').read()
    n = struct.unpack_from('<i', d)[0]
    ents = [list(struct.unpack_from('<iiiii', d, 4 + 20 * i)) for i in range(n)]
    ents = [e for e in ents if not (e[0] == 12 and GUMP0 <= e[1] < GUMP0 + len(SIZES)) and not (e[0] == 4 and e[1] == ART_HASH + 0x4000)]
    blobs = [(e, d[e[2]:e[2] + e[3]]) for e in ents]
    for k, (c, r) in enumerate(SIZES):
        w, h, px = grid_pixels(c, r)
        blobs.append(([12, GUMP0 + k, 0, 0, (w << 16) | h], enc_gump(w, h, px)))
    w, h, px = hash_pixels()
    blobs.append(([4, ART_HASH + 0x4000, 0, 0, 0], enc_art(w, h, px)))
    head = 4 + 20 * len(blobs); out = bytearray(); pos = head; tab = b''
    for e, b in blobs:
        tab += struct.pack('<iiiii', e[0], e[1], pos, len(b), e[4]); out += b; pos += len(b)
    open(dst, 'wb').write(struct.pack('<i', len(blobs)) + tab + bytes(out))
    print('verdata', len(blobs), 'записей,', pos, 'байт')

if __name__ == '__main__':
    build(sys.argv[1], sys.argv[2])
    if len(sys.argv) > 3:
        from PIL import Image
        w, h, px = grid_pixels(8, 3)
        im = Image.new('RGB', (w, h)); im.putdata([((c >> 10 & 31) * 8, (c >> 5 & 31) * 8, (c & 31) * 8) for row in px for c in row]); im.save(sys.argv[3])
