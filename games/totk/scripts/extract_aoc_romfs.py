#!/usr/bin/env python3
"""Extract a DLC (add-on content) package's RomFS for the runtime's aoc: mounts (romfs.c).

Reads one AddOnContent NSP in place: its common ticket, its one Data NCA, all six IVFC levels verified block by block.
Writes <output>/romfs.bin, index.json (the file table and the image's SHA-256) and runtime-index.bin. The caller checks
the image against its recorded hash. Keys stay in memory.
"""
import argparse
import json
from pathlib import Path
from extract_exefs import require, load_keys, container_entries, common_tickets, nca_inventory, nca_ctr_key, record_offset
from extract_romfs import write_romfs
from prepare_romfs_backend import runtime_index

DATA, PUBLIC_DATA = 4, 5   # NCA content types


def extract(package, keys_path, output, title_id):
    require(not output.exists(), 'Output already exists')
    keys = load_keys(keys_path)
    with package.open('rb') as f:
        kind, entries = container_entries(f, package.stat().st_size)
        require(kind == 'nsp', 'A DLC must be an NSP')
        data = [(r, h) for r, h in nca_inventory(f, entries, keys) if r['content_type'] in (DATA, PUBLIC_DATA)]
        require(len(data) == 1, 'Expected one DLC data NCA')
        record, hdr = data[0]
        require(record['title_id'] == title_id, f'DLC data NCA belongs to {record["title_id"]}, not {title_id}')
        section, counts, size, digest, files = write_romfs(f, record_offset(record), hdr,
                                                           nca_ctr_key(hdr, keys, common_tickets(f, entries)), output)
    (output / 'runtime-index.bin').write_bytes(runtime_index(files))
    report = dict(status='pass', title_id=title_id, data=record['name'], section=section, ivfc_blocks=counts,
                  romfs_size=size, romfs_sha256=digest, files=files,
                  integrity='FS header and all six IVFC levels verified; RSA signature not checked')
    (output / 'index.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(status='pass', title_id=title_id, file_count=len(files), size=size, sha256=digest)), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('package', type=Path)
    parser.add_argument('--keys', required=True, type=Path)
    parser.add_argument('--title-id', required=True, type=str.upper, help='the DLC title ID (from its CNMT)')
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    extract(args.package, args.keys, args.output, args.title_id)
