"""Package identification for --data-dir: synthetic NSPs with real CNMT layouts; no game data or keys."""
import hashlib
import io
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
from test_game_containers import HEADER, AREA, TITLEKEK, CONTENT, encrypt, nca, partition
from cryptography.hazmat.primitives.ciphers import modes
from identify_packages import identify, scan
from extract_aoc_romfs import extract
from extract_exefs import load_keys
import build_app

BOTW, TOTK = 0x01007EF00011E000, 0x0100F2C0115B6000


def cnmt(title, version, kind, contents, application=None):
    """kind 0x80 base, 0x81 update, 0x82 DLC; contents: (type, nca bytes)."""
    ext = struct.pack('<QII', application or title, 0, 0) if kind != 0x80 else struct.pack('<QII', title | 0x800, 0, 0)
    records = b''.join(hashlib.sha256(data).digest() + hashlib.sha256(data).digest()[:16] +
                       len(data).to_bytes(6, 'little') + bytes([kind_, 0]) for kind_, data in contents)
    return struct.pack('<QIBxHHH', title, version, kind, len(ext), len(contents), 0) + bytes(12) + ext + records


def meta_nca(title, meta):
    """A Meta NCA (content type 1) whose hash-verified PFS0 section holds one .cnmt."""
    pfs = partition([(f'Application_{title:016x}.cnmt', meta)])
    fs = bytearray(512)
    fs[2:5] = bytes([1, 2, 3])
    hashes = hashlib.sha256(pfs).digest()
    fs[8:40] = hashlib.sha256(hashes).digest()
    struct.pack_into('<IIQQQQ', fs, 0x28, 4096, 2, 0, len(hashes), 512, len(pfs))
    data = hashes.ljust(512, b'\0') + pfs.ljust((len(pfs) + 511) // 512 * 512, b'\0')
    hdr = bytearray(0xc00)
    hdr[0x200:0x204] = b'NCA3'
    hdr[0x205] = 1
    hdr[0x206] = 1
    struct.pack_into('<Q', hdr, 0x210, title)
    hdr[0x300:0x340] = encrypt(AREA, modes.ECB(), bytes(32) + CONTENT + bytes(16))
    struct.pack_into('<II', hdr, 0x240, 0xc00 // 512, (0xc00 + len(data)) // 512)
    hdr[0x400:0x600] = fs
    hdr[0x280:0x2a0] = hashlib.sha256(fs).digest()
    struct.pack_into('<Q', hdr, 0x208, 0xc00 + len(data))
    encrypted = b''.join(encrypt(HEADER, modes.XTS(i.to_bytes(16, 'big')), hdr[i*512:(i+1)*512]) for i in range(6))
    return encrypted + encrypt(CONTENT, modes.CTR(bytes(8) + (0xc00 // 16).to_bytes(8, 'big')), data)


def package(title, version, kind, application=None, corrupt=False, missing=False):
    content = nca(title=title if kind == 0x82 else application or title, content=0 if kind != 0x82 else 5)
    meta = cnmt(title, version, kind, [(1 if kind != 0x82 else 2, content)], application)
    if corrupt:
        content = content[:-1] + bytes([content[-1] ^ 1])
    files = [] if missing else [(meta[-0x18:-0x8].hex() + '.nca', content)]   # named by its CNMT record's content ID
    return partition(files + [(f'{hashlib.sha256(meta).hexdigest()[:32]}.cnmt.nca', meta_nca(title, meta))])


class Packages(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.keys = self.root / 'prod.keys'
        self.keys.write_text('\n'.join(f'{n} = {k.hex()}' for n, k in
                                      [('header_key', HEADER), ('key_area_key_application_00', AREA), ('titlekek_00', TITLEKEK)]))
        self.data = self.root / 'data'
        self.data.mkdir()
        for name, raw in [('a.nsp', package(BOTW, 0, 0x80)), ('b.nsp', package(BOTW | 0x800, 1114112, 0x81, BOTW)),
                          ('c.nsp', package(BOTW | 0x800, 786432, 0x81, BOTW)),
                          ('d.nsp', package(0x01007EF00011F001, 196608, 0x82, BOTW)),
                          ('e.nsp', package(0x01007EF00011F002, 196608, 0x82, BOTW)),
                          ('f.nsp', package(TOTK, 0, 0x80)), ('notes.txt', b'not a package')]:
            (self.data / name).write_bytes(raw)

    def test_identifies_by_cnmt_and_verifies_hashes(self):
        results = {Path(p['file']).name: p for p in scan([self.data], load_keys(self.keys), verify=True)}
        self.assertNotIn('notes.txt', results)
        self.assertEqual([(m['type'], m['title_id'], m['version'], m['application_id']) for m in results['b.nsp']['metas']],
                         [('update', '01007EF00011E800', 1114112, '01007EF00011E000')])
        self.assertEqual(results['d.nsp']['metas'][0]['type'], 'dlc')
        self.assertEqual(results['d.nsp']['metas'][0]['application_id'], '01007EF00011E000')
        self.assertEqual(results['f.nsp']['metas'][0]['application_id'], '0100F2C0115B6000')
        self.assertTrue(all(p['verified'] and p['checked_ncas'] == 1 for p in results.values()))

    def test_corrupt_and_missing_content(self):
        keys = load_keys(self.keys)
        bad = self.root / 'bad.nsp'
        bad.write_bytes(package(BOTW, 0, 0x80, corrupt=True))
        self.assertEqual(identify(bad, keys)['problems'], [])   # a read without --verify does not hash
        self.assertIn('SHA-256 does not match', identify(bad, keys, verify=True)['problems'][0])
        bad.write_bytes(package(BOTW, 0, 0x80, missing=True))
        self.assertIn('missing program NCA', identify(bad, keys)['problems'][0])
        bad.write_bytes(b'PFS0' + bytes(12))
        self.assertIn('No content metadata', identify(bad, keys)['problems'][0])

    def test_cache_reuses_until_the_file_changes(self):
        cache, keys = self.root / 'cache.json', load_keys(self.keys)
        scan([self.data], keys, verify=True, cache=cache)
        with patch('identify_packages.identify', side_effect=AssertionError('not reused')):
            scan([self.data], keys, verify=True, cache=cache)
        (self.data / 'a.nsp').write_bytes(package(BOTW, 0, 0x80, corrupt=True))
        results = {Path(p['file']).name: p for p in scan([self.data], keys, verify=True, cache=cache)}
        self.assertTrue(results['a.nsp']['problems'])

    def test_selects_base_newest_supported_update_and_dlc(self):
        packages = scan([self.data], load_keys(self.keys), verify=True)
        with patch.object(build_app, 'SELECTED', build_app.GAMES['botw']), patch.object(build_app, 'say') as said:
            base, update, dlcs = build_app.select_packages(packages, packages, self.data.resolve())
        self.assertEqual((base.name, update.name, sorted(p.name for p in dlcs)), ('a.nsp', 'b.nsp', ['d.nsp', 'e.nsp']))
        lines = '\n'.join(c.args[0] for c in said.call_args_list)
        self.assertIn('c.nsp: update 01007EF00011E800 v786432, not used: unsupported update version (supported: 1.9.0)', lines)
        self.assertIn('f.nsp: base 0100F2C0115B6000 v0, another game', lines)

    def test_missing_base_names_the_other_game(self):
        packages = scan([self.data], load_keys(self.keys))
        botw = [p for p in packages if p['metas'][0]['application_id'] != '0100F2C0115B6000']
        with patch.object(build_app, 'SELECTED', build_app.GAMES['totk']), patch.object(build_app, 'say'):
            with self.assertRaisesRegex(build_app.BuildError, r'no verified Tears of the Kingdom base game.*--game botw'):
                build_app.select_packages(botw, packages, self.data.resolve())

    def test_data_dir_excludes_game_file_and_update(self):
        for extra in (['game.nsp'], ['--update', 'u.nsp']):
            with self.assertRaisesRegex(build_app.BuildError, 'replaces the game file'):
                build_app.main(['--data-dir', str(self.data), '-k', str(self.keys), *extra])
        with self.assertRaisesRegex(build_app.BuildError, 'or --data-dir'):
            build_app.main(['-k', str(self.keys)])

    def test_dlc_romfs_extraction_and_build_step(self):
        out = self.root / 'aoc'
        extract(self.data / 'd.nsp', self.keys, out, '01007EF00011F001')
        report = json.loads((out / 'index.json').read_text())
        self.assertEqual([f['path'] for f in report['files']], ['icon_AmericanEnglish.dat'])
        self.assertEqual((out / 'runtime-index.bin').read_bytes()[:8], b'TOTKRFS1')
        with self.assertRaisesRegex(ValueError, 'belongs to 01007EF00011F001'):
            extract(self.data / 'd.nsp', self.keys, self.root / 'other', '01007EF00011F002')
        tree = self.root / 'tree'
        (tree / '.venv/bin').mkdir(parents=True)
        (tree / '.venv/bin/python').symlink_to(sys.executable)
        (tree / 'scripts').symlink_to(build_app.GAME / 'scripts')
        recorded = {'01007EF00011F001': dict(version=196608, size=report['romfs_size'], sha256=report['romfs_sha256']),
                    '01007EF00011F002': dict(version=1, size=1, sha256='0' * 64)}
        versions = dict(build_app.VERSIONS, v190=dict(build_app.VERSIONS['v190'], aoc=recorded))
        dlcs = [self.data / 'd.nsp', self.data / 'e.nsp']
        with patch.object(build_app, 'SELECTED', build_app.GAMES['botw']), patch.object(build_app, 'VERSIONS', versions), \
                patch.object(build_app, 'say') as said:
            build_app.step_dlc(tree, dlcs, self.keys, self.root / 'logs', version='v190')
            rows = [l.split('\t') for l in (tree / 'extracted/v190/aoc/manifest.tsv').read_text().splitlines() if l[0] != '#']
            self.assertEqual([r[:4] for r in rows], [['1', '01007EF00011F001', str(report['romfs_size']), report['romfs_sha256']]])
            self.assertIn('v196608 has no recorded image', '\n'.join(c.args[0] for c in said.call_args_list))   # e.nsp
            build_app.step_dlc(tree, [], self.keys, self.root / 'logs', version='v190')   # a build without DLC lists none
            self.assertEqual((tree / 'extracted/v190/aoc/manifest.tsv').read_text().count('\n'), 1)
            with self.assertRaisesRegex(build_app.BuildError, 'is not a verified Breath of the Wild DLC'):
                build_app.step_dlc(tree, [self.data / 'f.nsp'], self.keys, self.root / 'logs', version='v190')


if __name__ == '__main__':
    unittest.main()
