# Breath of the Wild (in preparation)

Target: BOTW **1.9.0** (`01007EF00011E000`, update `01007EF00011E800`), whose `sdk`, `subsdk0` and `rtld` are
identical to TOTK 1.4.3's. Nothing here builds or runs the game yet; findings, verification and next steps are in
[docs/botw-port.md](../../docs/botw-port.md).

- `assets/expected-v1.9.0.json`: ExeFS and verified rewrite hashes.
- `scripts/find_signed_tables.py`: finds the `br x18` jump tables `patch_nso.py` needs listed in `SIGNED_TABLES`.

The shared code (rewriter, loader, HLE host, NVN → Metal) stays in `games/totk` for now.
