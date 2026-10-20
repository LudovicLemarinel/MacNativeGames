#!/usr/bin/env python3
"""Build a bounded binary index for the native read-only RomFS backend."""
import argparse,hashlib,json,struct
from pathlib import Path

def runtime_index(files):
    """runtime-index.bin (romfs.c, aoc_romfs.c): header, one (offset, size, name offset, name length) row per file, then the names."""
    strings=bytearray();rows=bytearray()
    for item in files:
        name=item['path'].encode()+b'\0';rows.extend(struct.pack('<QQII',item['offset'],item['size'],len(strings),len(name)));strings.extend(name)
    return struct.pack('<8sQQ',b'TOTKRFS1',len(files),24+len(rows))+rows+strings

if __name__=='__main__':
    root=Path(__file__).resolve().parents[1]
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--version', choices=('v0','v112','v121','v143','v190','botw160'), default='v0')
    parser.add_argument('--aoc', action='store_true', help='Breath of the Wild 1.6.0: the add-on content volumes extracted/<version>/aoc/<index> -> aoc_config.h')
    args=parser.parse_args();version=args.version
    out=root/('build' if version=='v0' else f'build/{version}');out.mkdir(parents=True,exist_ok=True)
    if args.aoc:
        # One volume per add-on content index (games/botw/scripts/extract_aoc_romfs.py); a missing index has size 0 and is not listed.
        lines=[];present=[]
        for index in (1,2):
            folder=root/f'extracted/{version}/aoc/{index}';size=0;sha='';isha=''
            if (folder/'index.json').exists():
                manifest=json.loads((folder/'index.json').read_text())
                assert manifest['status']=='pass' and manifest['index']==index and (folder/'romfs.bin').stat().st_size==manifest['romfs_size']
                index_bytes=runtime_index(manifest['files'])
                (folder/'runtime-index.bin').write_bytes(index_bytes)
                size=manifest['romfs_size'];sha=manifest['romfs_sha256'];isha=hashlib.sha256(index_bytes).hexdigest();present.append(index)
            lines.append(f'#define AOC{index}_IMAGE_SIZE {size}ULL\n#define AOC{index}_IMAGE_SHA256 "{sha}"\n#define AOC{index}_INDEX_SHA256 "{isha}"\n')
        (out/'aoc_config.h').write_text(f'#define AOC_PRESENT_MASK {sum(1<<(i-1) for i in present)}\n'+''.join(lines))
        print(json.dumps({'aoc_present':present}));raise SystemExit
    folder=root/f'extracted/{version}/romfs';manifest=json.loads((folder/'index.json').read_text())
    assert manifest['status']=='pass' and (folder/'romfs.bin').stat().st_size==manifest['romfs_size']
    index=runtime_index(manifest['files'])
    (folder/'runtime-index.bin').write_bytes(index)
    (out/'romfs_config.h').write_text(f'#define ROMFS_IMAGE_SIZE {manifest["romfs_size"]}ULL\n#define ROMFS_IMAGE_SHA256 "{manifest["romfs_sha256"]}"\n#define ROMFS_INDEX_SHA256 "{hashlib.sha256(index).hexdigest()}"\n')
    print(json.dumps({'files':len(manifest['files']),'index_bytes':len(index)}))
