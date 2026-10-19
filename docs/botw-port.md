# Breath of the Wild: port preparation

Status: **preparation only**. `build_app.py --game botw` extracts, rewrites and verifies the BOTW 1.9.0 code and
merges and verifies its RomFS, then stops. There is no BOTW runtime, app, graphics or gameplay support yet.

```sh
python3.14 build_app.py --game botw "<BOTW base 1.0.0>.nsp" --update "<BOTW 1.9.0 update>.nsp" \
    -k <prod.keys> --firmware <unpacked firmware NCA directory>
# or, from a folder holding the base game, the update and possibly other versions and DLC:
python3.14 build_app.py --game botw --data-dir "<folder>" -k <prod.keys> --firmware <unpacked firmware NCA directory>
```

With `--data-dir`, packages are identified by their CNMT and every NCA of the BOTW packages is hashed against it before
anything is extracted (17.9 GB in about 10 s on an M5, then cached), so a damaged dump like the one below is named at
once. Other update versions (such as 1.6.0) are listed and ignored. Without `--data-dir`, give each DLC with `--dlc`.

## DLC (add-on content)

| Package | Index | Content | RomFS |
| --- | --- | --- | --- |
| `01007EF00011F001` v196608 | 1 | the Expansion Pass data (version 3.0): `Pack/Dungeon120…136`, `FinalTrial`, `Remains*`, `AocMainField`, `Map/AocField`, 6 movies, voices | 7,094 files, 1,390,370,680 bytes, `6fc0b5f0…d42b` |
| `01007EF00011F002` v196608 | 2 | only `System/AocVersion.txt` = `3.0` | 648 bytes, `a6638913…bd20` |

What 1.9.0 does with them (its function at `main+0x10cc2d0`): `nn::aoc::ListAddOnContent(list, 0, 2)` must return
**exactly two** entries, otherwise the DLC is ignored; if index 1 is listed it calls
`QueryMountAddOnContentCacheSize(&size, 1)` and `MountAddOnContent("aoc", 1, cache, size)` and reads `aoc:/…` (its
`Aoc/0010/` paths); index 3 only sets a flag. Index 2 is never mounted, but without it index 1 is not used either, so
both packages are needed. 407 paths of the DLC also exist in the game's RomFS (`Map/MainField/*`): the game picks the
`aoc:` copy itself, so the DLC is a second mount, never merged into `romfs.bin`.

Build (`step_dlc`): each DLC is identified by its CNMT and hashed, its Data NCA's RomFS is extracted with all six IVFC
levels verified (`games/totk/scripts/extract_aoc_romfs.py`, about 2 s for the 1.39 GB image) and must match the image
recorded in `expected-v1.9.0.json` (`aoc`); an unrecorded DLC version is listed and left out.
`extracted/v190/aoc/manifest.tsv` lists the packaged ones and the app carries them.

Runtime (`romfs.c`): the manifest's images are hashed at the first `nn::aoc` call (a record of the full hash is then
reused, like the game's RomFS, in `TOTK_ROMFS_PROOF_DIR`); `nn::aoc::CountAddOnContent` / `ListAddOnContent` report
these indices; `nn::fs::MountAddOnContent` mounts one as a second read-only volume and `nn::fs::Unmount` releases it;
`nn::err::ShowUnacceptableAddOnContentVersionError` stops with a message. `TOTK_AOC=<folder>` uses another manifest
folder, `TOTK_AOC=0` none. `totk-native-v190 rewritten/v190 probe-fs` lists, mounts, reads (byte-compared with the
image) and unmounts each DLC: pass (2026-10-19, 17 sampled DLC files); a damaged image stops the start.

In the game (2026-10-19): it reports the DLC as present. Not checked yet: the Master Trials and Champions' Ballad content.

Merged RomFS (2026-10-19, M4): 15,132,662,100 bytes, SHA-256 `826b373b…ae429dad`, all six IVFC levels verified, about
two minutes. Its 41,062 files are indexed for the runtime (`extracted/v190/romfs/runtime-index.bin`).

A damaged base dump fails this step. One examined earlier had a Program NCA (`861707001401bbce36c7e421efac76d4`)
hashing to `636e0d6f…` instead of its content ID; hactool's IVFC check of that NCA alone failed at 12 level-5 blocks,
and the merge failed at exactly those blocks, all read from base-sourced BKTR relocations. A fresh dump of the same
base merged cleanly. To check a dump quickly, hash each NCA in the NSP: the first 16 bytes of its SHA-256 are its name.

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

The equivalence run took a few minutes on an M4 with 6 jobs.

## Runtime profile and first boot

`make runtime-v190` (in the work folder's `totk` tree) builds `build/totk-native-v190` from the shared runtime with
`-DBOTW_GAME_V190`:

- `game_version.h`: `GAME_BOTW`, version `1.9.0`, data folder `v190` and the BOTW title ID (`GAME_TITLE_ID`, used by
  `fs_host.c` for save access and by `services.c` for the program ID). No TOTK `GAME_ADDR` entry exists for BOTW, so an
  unguarded TOTK site fails to compile. The crash filter, frame-controller diagnostic, startup checkpoint, world survey
  and keyboard probe are compiled out; `game_patches_botw.c` applies no game patch.
- `prepare_runtime.py --version v190`: BOTW's DT_INIT has the same layout as TOTK 1.4.3 (same compiler).
- The launcher keeps BOTW's profile, saves, shader cache and log apart (`MacNativeGames/BOTW`, `botw.log`).
- SDK functions BOTW uses and TOTK does not, answered by the host: `nn::hid` Labo VR "seven six-axis sensor" (absent),
  joy-con dual assignment, gyroscope zero-drift mode, `ShowControllerSupport(arg)`; `nn::account::ListAllUsers` (the
  local account); `nn::pl` VR mode (never entered) and `RequestSharedFontLoad`; `nn::pctl::CheckStereoVisionPermission`;
  `nn::oe::EnableRecording`; `nn::fs::CommitSaveData`; `IAudioRenderer` commands 12/13 (voice drop parameter).
- NVN depth mode: BOTW never calls `nvnDeviceSetDepthMode` and relies on NVN's default, `NEAR_IS_MINUS_W` (GL clip z).
  The host's flag now starts at that default, so vertex shaders get the `z = (z + w) / 2` conversion. Without it the
  title menu was rendered correctly into an intermediate target (frame-800 texture dump), but the final composition
  quad at clip z = -1 was clipped by Metal and the window stayed black. TOTK sets this mode explicitly and its seed
  preparation already assumes it, so TOTK is unaffected. Shader caches made before this change must be discarded (the
  depth flag is not part of the variant key).

First boot (2026-10-19, M5, test profile under `MacNativeGames/BOTW/tests`): the game reaches the "Nintendo présente"
intro and its title menu, with audio, at about 2,700 draws per frame. Not yet checked: controller input on the title
menu, loading a game, gameplay.

## Next steps

1. **App packaging** for `--game botw`: build `runtime-v190` and the launcher with `-DBOTW_GAME_V190`, copy the
   translator and SPIRV-Cross, and allow an app without TOTK's startup shader seed (it needs at least 8,000 shaders).
2. **BOTW-specific patches**, to be found again in BOTW's main: frame pacing and the 30 FPS cap, output resolution,
   the software keyboard entry (horse naming). The zeldaret/botw decompilation (Switch 1.5.0) can help locate functions,
   but its addresses do not apply to 1.9.0.
3. **Shaders and data.** Startup shader seeding from BOTW's `.sbfres` / shader archives; any heap or RomFS overlays the
   higher output resolution needs.
