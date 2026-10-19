# Breath of the Wild: port preparation

Status: **preparation only**. `build_app.py --game botw` extracts, rewrites and verifies the BOTW 1.9.0 code and
merges and verifies its RomFS, then stops. There is no BOTW runtime, app, graphics or gameplay support yet.

```sh
python3.14 build_app.py --game botw "<BOTW base 1.0.0>.nsp" --update "<BOTW 1.9.0 update>.nsp" \
    -k <prod.keys> --firmware <unpacked firmware NCA directory>
```

**The merged RomFS hash is not recorded yet.** The first base NSP examined (2026-10-19) is damaged: its Program NCA
`861707001401bbce36c7e421efac76d4` hashes to `636e0d6f…` instead of its content ID, and hactool's IVFC check of the
base NCA alone fails at 12 blocks of level 5 (RomFS offsets `0xfe7c0000`–`0xfe7d4000`, `0x202288000`–`0x20229c000`,
`0x202474000`–`0x20247c000`). The merge with 1.9.0 fails at exactly those blocks, all read from base-sourced BKTR
relocations, so the BKTR path itself is not in question. The base ExeFS passed its own hash table, and the update's
NCAs all match their content IDs. Until a sound base dump is merged, the build accepts a merge on its six verified IVFC
levels and prints the SHA-256 to record in `expected-v1.9.0.json`.

The work folder defaults to `BOTW_build` next to the base game. The base game's ExeFS is checked against its known
hashes but not rewritten; base + 1.9.0 are merged through BKTR (the pinned hactool path used for TOTK updates), all six
IVFC levels are verified and the image must match the recorded SHA-256; the runtime RomFS index is then prepared. The
TOTK RomFS overlays, shader seed, game binary and app steps are skipped.

## Inputs examined

| Input | Title ID | Version | SDK | Modules |
| --- | --- | --- | --- | --- |
| Base game NSP | `01007EF00011E000` | 1.0.0 (v0) | NintendoSDK 0.12.14 | main, sdk, subsdk0 (NVN), subsdk1, subsdk2 (libcurl), subsdk3 (movie) |
| Update NSP | `01007EF00011E800` | 1.9.0 (v1114112) | NintendoSDK **20.5.22** | main, sdk, subsdk0 (movie) |

Both are AArch64 (NPDM 64-bit flag set; 1.0.0 uses the 36-bit address space, 1.9.0 the 39-bit one). Exact hashes are
in [`games/botw/assets/expected-v1.9.0.json`](../games/botw/assets/expected-v1.9.0.json).

## Main finding: 1.9.0 shares its SDK modules with TOTK 1.4.3

BOTW 1.9.0's `sdk`, `subsdk0` and `rtld` are byte-identical to TOTK 1.4.3's (same SHA-256), and the service access
lists in the two `main.npdm` files contain the same services. Only `main` differs. Consequences:

- The rewritten `sdk.patched.bin` and `subsdk0.patched.bin` are the already-verified TOTK 1.4.3 rewrites (same hashes).
- The HLE host built for 1.4.3 (IPC layouts, REV15 audio renderer, NVN 20.5.22 driver interface, movie library,
  logical permissions and locks) targets the SDK this game uses. The NVN → Metal layer receives the same API version.
- Version **1.9.0 is the port target**. The 1.0.0 code (an early 2017 SDK with a different module layout) is not; the
  base NSP is still needed for its RomFS, which the update patches through BKTR.

## What was verified

| Check | Result |
| --- | --- |
| ExeFS extraction (`extract_exefs.py --title-id 01007EF00011E000`, base and `--update`) | Byte-identical to an independent hactool extraction |
| Rewrite of sdk / subsdk0 | Same SHA-256 as the verified TOTK 1.4.3 rewrites |
| Rewrite of main | 12,529 sites: 12,429 x18, 94 svc, 1 exclusive window, 5 call windows, 6 jump tables |
| `structck.py` on main | `STRUCT main OK`, 0 static violations |
| `equiv.py` on main (`--min-eff 2 --max-extra 16 --mutate 300`) | `EQUIV main OK`: 12,435 sites, 0 divergent sites, 300/300 injected mutations caught |
| TOTK regression | TOTK 1.4.3 main rewrites to the unchanged verified hash `4d69b643…`; the 38 container tests pass |

Not run: `test_exclusive_windows.py` / `test_v143_atomics.py` for BOTW's single exclusive window, the native call
probe, and anything at runtime.

### Rewriter changes

- `patch_nso.py`: BOTW main has three `br x18` dispatches through **register-relative** signed 32-bit tables
  (`ldrsw x18, [x26, x18, lsl #2]; add x18, x18, x26`, base set earlier by `adrp`/`add`). They are listed in
  `SIGNED_TABLES` under BOTW's build ID, with optional checks of the base-register setup instructions in addition to
  the dispatch bytes and all table entries.
  [`games/botw/scripts/find_signed_tables.py`](../games/botw/scripts/find_signed_tables.py) regenerates these entries
  from an NSO and reports any other unrecognised dispatch.
- `extract_exefs.py`: `--title-id` selects the game (default TOTK; error messages for TOTK are unchanged).

### Differences from TOTK's main

- BOTW's main contains **94 `svc` instructions** of its own (TOTK's main has none). The rewriter routes them through
  gates like the SDK's; `loader.c` dispatches gates per module, but this has not been exercised.
- Movies are VP9 `.webm` (`Movie/*.webm`, 30 files), like TOTK, so `movie_host.c` should apply.
- Assets are Yaz0/SARC packs (`.sbactorpack`, `.sbfres`, `.sstera`, `.bfstm`…), not TOTK's zstd dictionaries.
  `prepare_shader_seed.py` and the RomFS overlays cannot be reused as they are.

## Reproduce

Everything below writes outside the repository (`~/Documents/NSP/BOTW_build` here).

```sh
W=~/Documents/NSP/BOTW_build
python3.14 -m venv $W/venv && $W/venv/bin/pip install -r games/totk/requirements-dev.txt
cd games/totk
$W/venv/bin/python scripts/extract_exefs.py "<BOTW 1.9.0 update>.nsp" --update \
    --title-id 01007EF00011E000 --keys <prod.keys> --output $W/v190/exefs
for m in main sdk subsdk0; do
  DM_RULESET=native DM_PRIVATE_GATES=0 $W/venv/bin/python third_party/d2r/patch_nso.py $W/v190/exefs/$m $W/v190/rewritten $m
done
LLVM_BIN=/opt/homebrew/opt/llvm/bin DM_VCACHE=$W/llvm-cache $W/venv/bin/python third_party/d2r/structck.py $W/v190/rewritten main $W/v190/exefs/main
LLVM_BIN=/opt/homebrew/opt/llvm/bin DM_VCACHE=$W/llvm-cache $W/venv/bin/python third_party/d2r/equiv.py $W/v190/rewritten main $W/v190/exefs/main \
    --jobs 6 --min-eff 2 --max-extra 16 --mutate 300 --tsv $W/main-equivalence.tsv
```

The equivalence run took a few minutes on an M4 with 6 jobs. The DLC pack was not examined.

## Next steps

1. ~~Game selection in the build~~ (`--game`, done for the code and data steps) and ~~RomFS~~ (done). The runtime side
   still hard-codes TOTK: the Makefile, `fs_host.c`, `services.c` and `extract_app_icon.py` use its title ID, names
   and paths (`TOTK_*` environment variables, the `MacNativeGames/TOTK` profile folder). Proposed: let `games/botw`
   reuse `games/totk`'s runtime with a `BOTW_GAME_V190` profile in `game_version.h` and a `runtime-v190` target.
2. **First boot.** Load the three rewritten modules; leave out every TOTK-specific `GAME_ADDR` patch (the 60 FPS
   controller, output resolution, crash filter at `0x15ecb0c`, startup checkpoint, swkbd PLT/GOT, world survey).
   Follow the logs to the first NVN frames.
3. **BOTW-specific patches**, to be found again in BOTW's main: frame pacing and the 30 FPS cap, output resolution,
   the software keyboard entry (horse naming). The zeldaret/botw decompilation (Switch 1.5.0) can help locate functions,
   but its addresses do not apply to 1.9.0.
4. **Shaders and data.** Startup shader seeding from BOTW's `.sbfres` / shader archives; any heap or RomFS overlays the
   higher output resolution needs.
