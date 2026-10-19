#!/usr/bin/env python3
"""List `br x18` jump tables that patch_nso.py cannot recognise by itself, as SIGNED_TABLES entries.

Recognises the register-based form emitted for BOTW 1.9.0:

    adrp xB, page ; add xB, xB, #lo         (earlier in the function)
    cmp w18, #n-1 ; b.hi default ; ldrsw x18, [xB, x18, lsl #2] ; add x18, x18, xB ; br x18

Every target must lie in .text and be word aligned. Other unrecognised forms are reported for manual
analysis. Reads the NSO only; the output is Python source to review and paste into patch_nso.py.
"""
import argparse
import struct
import sys
from pathlib import Path

import capstone

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'totk/third_party/d2r'))
import nsoimg  # noqa: E402

BR_X18 = 0xd61f0240
SEARCH = 0x4000  # bytes before the dispatch searched for the base register's adrp/add


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('nso', type=Path)
    args = parser.parse_args()
    raw = args.nso.read_bytes()
    loaded = nsoimg.load(args.nso)
    img = loaded[0] if isinstance(loaded, tuple) else loaded
    text_end = struct.unpack_from('<I', raw, 0x18)[0]
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    entries, unknown = [], []
    for site in range(0, text_end, 4):
        if struct.unpack_from('<I', img, site)[0] != BR_X18:
            continue
        tail = list(md.disasm(bytes(img[site - 16:site + 4]), site - 16))
        if len(tail) != 5:
            unknown.append(site)
            continue
        cmp, bhi, ldrsw, add, _ = tail
        if any(i.mnemonic == 'add' and i.op_str.startswith('x18, x18,') and 'lsl #2' in i.op_str
               for i in md.disasm(bytes(img[site - 128:site]), site - 128)):
            continue  # byte/halfword table, handled by patch_nso.py
        if not (cmp.mnemonic == 'cmp' and cmp.op_str.startswith('w18, #') and bhi.mnemonic == 'b.hi'
                and ldrsw.mnemonic == 'ldrsw' and ldrsw.op_str.startswith('x18, [x') and ldrsw.op_str.endswith(', x18, lsl #2]')
                and add.mnemonic == 'add' and add.op_str.startswith('x18, x18, x')):
            unknown.append(site)
            continue
        base = ldrsw.op_str.split('[')[1].split(',')[0]
        if add.op_str.split(', ')[2] != base:
            unknown.append(site)
            continue
        count = int(cmp.op_str.split('#')[1], 0) + 1
        before = list(md.disasm(bytes(img[site - SEARCH:site - 16]), site - SEARCH))
        defs = [i for i in before if i.op_str.split(',')[0] == base
                and i.mnemonic not in ('str', 'stp', 'stur', 'cmp', 'cmn', 'tst', 'cbz', 'cbnz', 'tbz', 'tbnz')]
        if len(defs) < 2 or defs[-2].mnemonic != 'adrp' or defs[-1].mnemonic != 'add' \
                or not defs[-1].op_str.startswith(f'{base}, {base}, #'):
            unknown.append(site)
            continue
        adrp, lo = defs[-2], defs[-1]
        table = int(adrp.op_str.split('#')[1], 16) + int(lo.op_str.split('#')[1], 16)
        offsets = struct.unpack_from(f'<{count}i', img, table)
        targets = [table + o for o in offsets]
        if not all(0 <= t < text_end and t % 4 == 0 for t in targets):
            unknown.append(site)
            continue
        entries.append(f"    {site:#x}: ({table:#x}, {table:#x}, ({','.join(map(str, offsets))}),\n"
                       f"               '{img[site - 16:site + 4].hex()}', ({adrp.address:#x}, '{img[adrp.address:adrp.address + 4].hex()}'), "
                       f"({lo.address:#x}, '{img[lo.address:lo.address + 4].hex()}')),")
    print(f"'{raw[0x40:0x50].hex()}': {{")
    print('\n'.join(entries))
    print('},')
    for site in unknown:
        print(f'# unrecognised br x18 at {site:#x}: analyse manually', file=sys.stderr)


if __name__ == '__main__':
    main()
