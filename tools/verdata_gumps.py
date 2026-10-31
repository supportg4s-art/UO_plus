# -*- coding: utf-8 -*-
"""Разобрать verdata/patchdata на картинки гампов (PNG) и собрать обратно.

  python verdata_gumps.py export patchdata.mul папка      — каждый гамп -> папка/0xXXXX.png (+ список index.txt)
  python verdata_gumps.py pack   patchdata.mul папка новый.mul — собрать новый файл: правленые PNG заменяют гампы,
                                                                 новые 0xXXXX.png добавляются, остальное как было
Формат: int count, затем записи по 20 байт (fileid, block, pos, len, extra). Гампы — fileid 12, block = номер гампа,
extra = (ширина << 16) | высота; данные — таблица строк (int на строку, смещение в dword) и пары (цвет, длина) по 2 байта.
Цвет 0 — прозрачно; в PNG прозрачность — альфа.
"""
import os, struct, sys
from PIL import Image

def read_entries(path):
    f = open(path, 'rb').read()
    n = struct.unpack_from('<i', f, 0)[0]
    return f, [struct.unpack_from('<5i', f, 4 + i * 20) for i in range(n)]

def c2rgb(c):
    r, g, b = (c >> 10) & 31, (c >> 5) & 31, c & 31
    return (r * 255 // 31, g * 255 // 31, b * 255 // 31, 255)

def decode(data, w, h):
    img = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    px = img.load()
    rows = struct.unpack_from('<%di' % h, data, 0)
    for y in range(h):
        o = rows[y] * 4
        end = rows[y + 1] * 4 if y + 1 < h else len(data)
        x = 0
        while o + 4 <= end and x < w:
            c, run = struct.unpack_from('<HH', data, o)
            o += 4
            if c:
                col = c2rgb(c)
                for i in range(run):
                    if x + i < w: px[x + i, y] = col
            x += run
    return img

def q(v):
    return (v * 31 + 127) // 255   # 8 бит -> 5 бит с округлением (туда-обратно без потерь)

def encode(img):
    img = img.convert('RGBA')
    w, h = img.size
    px = img.load()
    rows, body = [], b''
    for y in range(h):
        rows.append(h + len(body) // 4)
        x = 0
        while x < w:
            r, g, b, a = px[x, y]
            c = 0 if a < 128 else (q(r) << 10) | (q(g) << 5) | q(b)
            if a >= 128 and c == 0: c = 1   # чёрный непрозрачный: 0 занят прозрачностью
            run = 1
            while x + run < w and run < 0xFFFF:
                r2, g2, b2, a2 = px[x + run, y]
                c2 = 0 if a2 < 128 else (q(r2) << 10) | (q(g2) << 5) | q(b2)
                if a2 >= 128 and c2 == 0: c2 = 1
                if c2 != c: break
                run += 1
            body += struct.pack('<HH', c, run)
            x += run
    return struct.pack('<%di' % h, *rows) + body, (w << 16) | h

def export(src, out):
    f, es = read_entries(src)
    os.makedirs(out, exist_ok=True)
    lines, n = [], 0
    for i, (fid, blk, pos, ln, extra) in enumerate(es):
        if fid != 12 or ln <= 0: continue
        w, h = (extra >> 16) & 0xFFFF, extra & 0xFFFF
        name = '0x%04X.png' % blk
        decode(f[pos:pos + ln], w, h).save(os.path.join(out, name))
        lines.append('%s\t%dx%d' % (name, w, h)); n += 1
    open(os.path.join(out, 'index.txt'), 'w', encoding='utf-8').write(
        '# гампы из %s: имя файла = номер гампа; править и собирать обратно командой pack\n' % os.path.basename(src) + '\n'.join(lines) + '\n')
    print('гампов выгружено:', n, '->', out)

def pack(src, folder, dst):
    f, es = read_entries(src)
    pngs = {int(fn[2:6], 16): os.path.join(folder, fn) for fn in os.listdir(folder)
            if fn.lower().startswith('0x') and fn.lower().endswith('.png') and len(fn) == 10}
    out_e, blobs, changed, added, seen = [], [], 0, 0, set()
    for (fid, blk, pos, ln, extra) in es:
        data = f[pos:pos + ln]
        if fid == 12 and blk in pngs:
            seen.add(blk)
            p = pngs[blk]
            img = Image.open(p)
            old = decode(data, (extra >> 16) & 0xFFFF, extra & 0xFFFF)
            if img.convert('RGBA').tobytes() != old.tobytes() or img.size != old.size:
                data, extra = encode(img); changed += 1
        out_e.append([fid, blk, 0, len(data), extra]); blobs.append(data)
    for blk, p in sorted(pngs.items()):
        if blk in seen: continue
        data, extra = encode(Image.open(p)); added += 1
        out_e.append([12, blk, 0, len(data), extra]); blobs.append(data)
    pos = 4 + 20 * len(out_e)
    for e, d in zip(out_e, blobs): e[2] = pos; pos += len(d)
    with open(dst, 'wb') as o:
        o.write(struct.pack('<i', len(out_e)))
        for e in out_e: o.write(struct.pack('<5i', *e))
        for d in blobs: o.write(d)
    print('собрано: %s; изменено %d, добавлено %d, всего записей %d' % (dst, changed, added, len(out_e)))

if __name__ == '__main__':
    if len(sys.argv) >= 4 and sys.argv[1] == 'export': export(sys.argv[2], sys.argv[3])
    elif len(sys.argv) >= 5 and sys.argv[1] == 'pack': pack(sys.argv[2], sys.argv[3], sys.argv[4])
    else: print(__doc__)
