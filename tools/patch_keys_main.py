#!/usr/bin/env python3
"""Добавить команды в список готовых команд окна клавиш UO+ (таблица K_MAIN в UOPlus.dll), без исходников.

Таблица K_MAIN — массив пар {const wchar_t *подпись, const char *команда}; её читает только keys_fill().
Скрипт копирует таблицу в новую секцию .uoplus, вставляет новые строки, переставляет 6 ссылок в keys_fill()
на новую таблицу и дописывает поправки (relocations) для новых указателей — DLL грузится с ASLR.

    python patch_keys_main.py UOPlus.dll UOPlus_new.dll
"""
import struct, sys
import pefile

NEW = [  # владелец 2026-11-18: «добавь в кнопки по дефолту команды для животных ... вендор селл, вендор буй и банк»
    ("Питомцы: все ко мне (all come)", "msg all come"),
    ("Питомцы: охранять (all guard)", "msg all guard"),
    ("Питомцы: стоп (all stop)", "msg all stop"),
    ("Питомцы: атаковать цель (all kill)", "msg all kill"),
    ("Торговец: продать (vendor sell)", "msg vendor sell"),
    ("Торговец: купить (vendor buy)", "msg vendor buy"),
    ("Открыть банк (bank)", "msg bank"),
]
K_MAIN_RVA = 0x6d462f40 - 0x6d380000   # адреса ниже — из UOPlus.dll v70 (ImageBase 0x6d380000)
COUNT = 57
INSERT_AT = 56                 # перед «Окно горячих клавиш»
# (RVA поля imm32, что там: 'begin' | 'begin+4' | 'end' | 'end+4')
REFS = [(0x6d3dd6aa - 0x6d380000, 'begin'), (0x6d3dda97 - 0x6d380000, 'begin'), (0x6d3dd73b - 0x6d380000, 'end'),
        (0x6d3ddc91 - 0x6d380000, 'end'), (0x6d3df1c8 - 0x6d380000, 'begin+4'), (0x6d3df2b2 - 0x6d380000, 'end+4')]


def align(x, a): return (x + a - 1) // a * a


def main(src, dst):
    pe = pefile.PE(src)
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()
    old_begin, old_end = base + K_MAIN_RVA, base + K_MAIN_RVA + COUNT * 8
    want = {'begin': old_begin, 'begin+4': old_begin + 4, 'end': old_end, 'end+4': old_end + 4}
    for rva, kind in REFS:
        v = struct.unpack_from('<I', img, rva)[0]
        assert v == want[kind], (hex(rva), hex(v), kind)
    pairs = [struct.unpack_from('<2I', img, K_MAIN_RVA + 8 * i) for i in range(COUNT)]
    assert pairs[-1] != (0, 0)
    relocs = {e.rva for b in pe.DIRECTORY_ENTRY_BASERELOC for e in b.entries if e.type == 3}
    for rva, _ in REFS:
        assert rva in relocs, hex(rva)

    sa, fa = pe.OPTIONAL_HEADER.SectionAlignment, pe.OPTIONAL_HEADER.FileAlignment
    last = pe.sections[-1]
    sec_rva = align(last.VirtualAddress + last.Misc_VirtualSize, sa)
    n = COUNT + len(NEW)
    # раскладка секции: таблица | строки | relocations
    body = bytearray(n * 8)
    strs = bytearray()
    def put_str(b):
        off = n * 8 + len(strs); strs.extend(b); strs.extend(b'\0' * (-len(strs) % 4)); return base + sec_rva + off
    new_ptrs = [(put_str(l.encode('utf-16-le') + b'\0\0'), put_str(c.encode('ascii') + b'\0')) for l, c in NEW]
    allp = pairs[:INSERT_AT] + new_ptrs + pairs[INSERT_AT:]
    for i, (a, b) in enumerate(allp):
        struct.pack_into('<2I', body, 8 * i, a, b)
    body += strs
    body += b'\0' * (-len(body) % 4)
    # relocations: старый каталог целиком + блоки для таблицы
    d = pe.OPTIONAL_HEADER.DATA_DIRECTORY[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_BASERELOC']]
    old_rel = img[d.VirtualAddress:d.VirtualAddress + d.Size]
    ptr_rvas = [sec_rva + 4 * k for k in range(2 * n)]
    blocks = bytearray()
    for page in sorted({r & ~0xFFF for r in ptr_rvas}):
        ents = [(3 << 12) | (r - page) for r in ptr_rvas if r & ~0xFFF == page]
        if len(ents) % 2: ents.append(0)
        blocks += struct.pack('<II', page, 8 + 2 * len(ents)) + struct.pack('<%dH' % len(ents), *ents)
    rel_off = len(body)
    body += old_rel + blocks
    vsize = len(body)
    raw = bytes(body) + b'\0' * (-len(body) % fa)

    data = bytearray(open(src, 'rb').read())
    raw_ptr = align(len(data), fa)
    data += b'\0' * (raw_ptr - len(data)) + raw
    # заголовок новой секции
    sh_off = pe.sections[-1].get_file_offset() + 40
    assert sh_off + 40 <= pe.OPTIONAL_HEADER.SizeOfHeaders
    assert data[sh_off:sh_off + 40] == b'\0' * 40
    data[sh_off:sh_off + 40] = struct.pack('<8sIIIIIIHHI', b'.uoplus', vsize, sec_rva, len(raw), raw_ptr, 0, 0, 0, 0, 0x40000040)
    fh = pe.FILE_HEADER.get_file_offset()
    struct.pack_into('<H', data, fh + 2, pe.FILE_HEADER.NumberOfSections + 1)
    oh = pe.OPTIONAL_HEADER.get_file_offset()
    struct.pack_into('<I', data, oh + 56, align(sec_rva + vsize, sa))          # SizeOfImage
    struct.pack_into('<I', data, oh + 64, 0)                                   # CheckSum
    dd = oh + 96 + 8 * 5                                                       # DataDirectory[5] = BASERELOC
    struct.pack_into('<II', data, dd, sec_rva + rel_off, len(old_rel) + len(blocks))
    # ссылки в keys_fill
    nb = base + sec_rva; ne = nb + n * 8
    nv = {'begin': nb, 'begin+4': nb + 4, 'end': ne, 'end+4': ne + 4}
    for rva, kind in REFS:
        struct.pack_into('<I', data, pe.get_offset_from_rva(rva), nv[kind])
    open(dst, 'wb').write(data)
    print(f'{dst}: команд было {COUNT}, стало {n}; секция .uoplus RVA {sec_rva:#x}, {vsize} байт')


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
