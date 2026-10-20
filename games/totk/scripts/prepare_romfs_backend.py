#!/usr/bin/env python3
"""Build a bounded binary index for the native read-only RomFS backend."""
import argparse,hashlib,json,struct
from pathlib import Path

def runtime_index(files):
    """runtime-index.bin (romfs.c): header, one (offset, size, name offset, name length) row per file, then the names."""
    strings=bytearray();rows=bytearray()
    for item in files:
        name=item['path'].encode()+b'\0';rows.extend(struct.pack('<QQII',item['offset'],item['size'],len(strings),len(name)));strings.extend(name)
    return struct.pack('<8sQQ',b'TOTKRFS1',len(files),24+len(rows))+rows+strings

if __name__=='__main__':
    root=Path(__file__).resolve().parents[1]
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--version', choices=('v0','v112','v121','v143','v190'), default='v0');version=parser.parse_args().version
    out=root/('build' if version=='v0' else f'build/{version}');out.mkdir(parents=True,exist_ok=True)
    folder=root/f'extracted/{version}/romfs';manifest=json.loads((folder/'index.json').read_text())
    assert manifest['status']=='pass' and (folder/'romfs.bin').stat().st_size==manifest['romfs_size']
    index=runtime_index(manifest['files'])
    (folder/'runtime-index.bin').write_bytes(index)
    (out/'romfs_config.h').write_text(f'#define ROMFS_IMAGE_SIZE {manifest["romfs_size"]}ULL\n#define ROMFS_IMAGE_SHA256 "{manifest["romfs_sha256"]}"\n#define ROMFS_INDEX_SHA256 "{hashlib.sha256(index).hexdigest()}"\n')
    print(json.dumps({'files':len(manifest['files']),'index_bytes':len(index)}))
