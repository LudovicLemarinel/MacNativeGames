#!/usr/bin/env python3
"""Identify the Switch packages (.nsp, .nsz, .xci) of a folder by their content metadata (CNMT), never by file name.

For each package: its content metas (base game, update or DLC, title ID, version, owning game) and, with --verify,
the SHA-256 of each NCA against its CNMT record (the NCA's name is the first half of that hash). NSZ contents are
compressed and checked when the build decompresses them. Keys stay in memory; no key value is printed.

    identify_packages.py <folder or files...> --keys prod.keys [--verify] [--json out.json] [--cache cache.json]
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path
from extract_exefs import (require, load_keys, container_entries, common_tickets, read_nca_header, nca_ctr_key,
                           nca_pfs0, GAME_NAMES)

SUFFIXES = ('.nsp', '.nsz', '.xci')
META_TYPES = {0x80: 'base', 0x81: 'update', 0x82: 'dlc', 0x83: 'delta'}
CONTENT_TYPES = ('meta', 'program', 'data', 'control', 'html', 'legal', 'delta')


def parse_cnmt(data):
    """Packaged content meta: header 0x20, extended header, then 0x38-byte content records."""
    require(len(data) >= 0x20, 'Truncated CNMT')
    title, version, kind, ext_size, count = struct.unpack_from('<QIBxHH', data, 0)
    require(0x20 + ext_size + count * 0x38 <= len(data), 'Truncated CNMT content records')
    # The extended header of an update or DLC starts with its game's title ID; a base game owns itself.
    application = struct.unpack_from('<Q', data, 0x20)[0] if kind in (0x81, 0x82, 0x83) and ext_size >= 8 else title
    contents = []
    for i in range(count):
        record = data[0x20 + ext_size + i * 0x38:0x20 + ext_size + (i + 1) * 0x38]
        contents.append(dict(id=record[0x20:0x30].hex(), sha256=record[:0x20].hex(),
                             size=int.from_bytes(record[0x30:0x35], 'little'),   # newer CNMTs use byte 0x35 for attributes
                             type=CONTENT_TYPES[record[0x36]] if record[0x36] < len(CONTENT_TYPES) else record[0x36]))
    return dict(type=META_TYPES.get(kind, f'0x{kind:02x}'), title_id=f'{title:016X}', version=version,
                application_id=f'{application:016X}', contents=contents)


def sha256_range(f, offset, size):
    h = hashlib.sha256()
    f.seek(offset)
    while size:
        chunk = f.read(min(size, 8 << 20))
        require(chunk, 'Truncated content')
        h.update(chunk)
        size -= len(chunk)
    return h.hexdigest()


def identify(path, keys, verify=False):
    stat = path.stat()
    result = dict(file=str(path), size=stat.st_size, mtime_ns=stat.st_mtime_ns, container=None, metas=[], problems=[],
                  verified=False)
    try:
        with path.open('rb') as f:
            kind, entries = container_entries(f, stat.st_size)
            result['container'] = 'nsz' if path.suffix.lower() == '.nsz' else kind
            files = {name.lower(): (offset, size) for name, offset, size in entries}
            try:
                tickets = common_tickets(f, entries)
            except ValueError as e:
                tickets = {}
                result['problems'].append(str(e))
            for name, offset, size in entries:
                if not name.lower().endswith('.cnmt.nca'):
                    continue
                hdr = read_nca_header(f, offset, size, keys)
                require(hdr[0x205] == 1, f'{name} is not a Meta NCA')
                record, key = dict(nca_offset=offset), nca_ctr_key(hdr, keys, tickets)
                cnmts = [data for i in range(4) for n, data in (nca_pfs0(f, record, hdr, key, i) or []) if n.endswith('.cnmt')]
                require(len(cnmts) == 1, f'{name}: expected one CNMT')
                result['metas'].append(parse_cnmt(cnmts[0]))
            require(result['metas'], 'No content metadata (CNMT) found')
            checked = 0
            for meta in result['metas']:
                for content in meta['contents']:
                    if content['type'] == 'delta':
                        continue   # delta fragments are optional in update packages
                    if f'{content["id"]}.ncz' in files:
                        continue   # compressed: verified by the NSZ decompressor
                    where = files.get(f'{content["id"]}.nca')
                    if where is None:
                        result['problems'].append(f'missing {content["type"]} NCA {content["id"]}')
                    elif where[1] != content['size']:
                        result['problems'].append(f'{content["type"]} NCA {content["id"]}: size {where[1]}, CNMT says {content["size"]}')
                    elif verify:
                        if sha256_range(f, where[0], where[1]) != content['sha256']:
                            result['problems'].append(f'{content["type"]} NCA {content["id"]}: SHA-256 does not match its CNMT')
                        checked += 1
            result['verified'] = verify and not result['problems']
            result['checked_ncas'] = checked
    except (ValueError, OSError) as e:
        result['problems'].append(str(e))
    return result


def describe(package):
    """One line per content meta: kind, title, version and owning game."""
    if not package['metas']:
        return ['not identified']
    lines = []
    for meta in package['metas']:
        game = GAME_NAMES.get(meta['application_id'], meta['application_id'])
        lines.append(f'{meta["type"]:<6} {game}, {meta["title_id"]} v{meta["version"]}')
    return lines


def scan(sources, keys, verify=False, cache=None):
    paths = []
    for source in sources:
        if source.is_dir():
            paths += sorted(p for p in source.iterdir() if p.is_file() and p.suffix.lower() in SUFFIXES
                            and not p.name.startswith('.'))
        else:
            paths.append(source)
    known = {}
    if cache and cache.is_file():
        try:
            known = {p['file']: p for p in json.loads(cache.read_text())}
        except (ValueError, KeyError, TypeError):
            known = {}
    results = []
    for path in paths:
        path = path.resolve()
        stat = path.stat()
        previous = known.get(str(path))
        # A package with problems is read again: the cause may have been the keys rather than the file.
        if previous and previous['size'] == stat.st_size and previous['mtime_ns'] == stat.st_mtime_ns and (
                not previous['problems'] and (previous['verified'] or not verify)):
            results.append(previous)
            continue
        print(f'{"verifying" if verify else "reading"} {path.name}', file=sys.stderr, flush=True)
        results.append(identify(path, keys, verify))
    if cache:
        merged = {**known, **{p['file']: p for p in results}}
        cache.parent.mkdir(parents=True, exist_ok=True)
        cache.write_text(json.dumps(list(merged.values()), indent=2) + '\n')
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('sources', nargs='+', type=Path, help='a folder of packages, or package files')
    parser.add_argument('--keys', required=True, type=Path)
    parser.add_argument('--verify', action='store_true', help='hash every NCA against its CNMT record')
    parser.add_argument('--json', type=Path, help='write the results here')
    parser.add_argument('--cache', type=Path, help='results reused while a file keeps its size and modification time')
    args = parser.parse_args()
    results = scan(args.sources, load_keys(args.keys), args.verify, args.cache)
    for package in results:
        state = 'PROBLEM' if package['problems'] else 'verified' if package['verified'] else 'read'
        print(f'{Path(package["file"]).name}  [{package["container"] or "?"}, {state}]')
        for line in describe(package) + package['problems']:
            print(f'    {line}')
    if args.json:
        args.json.write_text(json.dumps(results, indent=2) + '\n')


if __name__ == '__main__':
    main()
