# Breath of the Wild: performance work (branch perf/botw-shaders, 2026-10-20)

This branch moves main's graphics and shader work over to Breath of the Wild. It then fixes what play sessions showed:
- a crash after 20–40 minutes;
- stutters at every frame rate;
- a 60 FPS ceiling.

Every change is measured in the game log (`~/Library/Logs/MacNativeGames/botw.log`) or with `sample(1)`. Each one has an
environment switch so it can be compared on and off.

## 1. Shaders (bcd54c1, a856ab0, 2fe9ce6)

- **Stable shader key.**
  - **Problem:** BOTW's shader control block is 0x7d1 or 0x848 bytes long, with its size at +0x20, and the key hashed a fixed
    0x880 bytes. The bytes past the block are garbage that changes between runs, so 1,803 shaders gave 8,750 keys.
  - **Fix:** the key now hashes only the block itself.
- **BOTW's own startup seed.** The build reads the game's shader archives (Yaz0, `tools/yaz0.c`) and translates them ahead of
  time: 9,861 variants are ready before the first frame.
- **BOTW's own shared list.**
  - Variants found while playing go to `games/botw/assets/shader_list.tsv`, with their own GitHub issue form.
  - TOTK's list stays separate.
- **First-world gate.**
  - **Problem:** the wait for the pipeline warm-up opened on BOTW's title screen, which already draws 2,700 times a frame.
  - **Fix:** it now opens on the first world frame after the player's `game_data.sav` is read.

## 2. Crash after 20–40 minutes (73923fb)

- **Cause:** `nvnMemoryPoolInvalidateMappedRange` runs on a game thread. The GPU write-back behind it (`wb_level_back`) created a
  Metal buffer and a command buffer each time, in an autorelease pool that thread never drains.
- **Effect:** IOAccelerator regions grew by about 1,700 a minute until the 5 GB GPU budget guard aborted the game.
- **Fix:** the write-back runs in its own `@autoreleasepool`. Regions stay around 1,050 for hours.

## 3. Stutters: virtual-core preemption (0c70b43)

The runtime runs guest threads on three virtual cores. A thread keeps its core while it runs guest code, and used to give
it up only at an SVC.

- **Cause:** BOTW's recording threads (priority 44) run guest code for 10 ms and more without an SVC. The game's priority-32
  thread was woken and then waited for their core. In 19.4 s it waited 721 times, at 5 ms and more each.
- **Fix: asynchronous preemption.**
  - When a higher-priority thread needs a core, the owner is sent `SIGURG`.
  - Its handler gives the core up at once, but only if the owner is in guest code: the host-stack slot is non-zero and the PC
    is in a guest module.
  - A thread inside a host call gives it up when it returns to the guest (`runtime_hle_switch` checks
    `runtime_preempt_pending`).
- **Result:**

  | | Before | After |
  |---|---|---|
  | Priority-32 core waits | 721 in 19.4 s | 2 waits, 54 ms in all |
  | Frame time p95 | 43.6 ms | 26.4 ms |

- **Switches:**
  - `TOTK_ASYNC_PREEMPT=0` turns the preemption off.
  - `TOTK_WAIT_LOG=1` logs main-thread waits over 20 ms and core waits over 5 ms, with who held what.
  - 8f93ca4 adds the guest handle to thread names (`guest-0x1004` in `sample` and crash reports) and the host thread ids to
    the wait log.

## 4. FPS graph (adba88a)

The corner panel (top right) shows the last 5 seconds of game frames as their instantaneous FPS, with guides at 30 and 60.
Below the graph, one line gives:
- the average FPS;
- the slowest and fastest frame;
- the 1% low;
- the worst frame time.

It is redrawn ten times a second, and key 1 hides it with the other overlays.

The graph shows display quantisation as it is. A 60 FPS game on a 75 Hz display alternates 13.3 ms and 26.7 ms frames (75 / 37
FPS). At 60 Hz pacing, a late frame waits for the next 60 Hz slot (33.3 ms, so 30 FPS).

## 5. Above 60 FPS (5c9be62, bd1fde6)

- **The cap.** `runtime_frame_max_hz()` held BOTW at 60 game frames a second (b461c55), because its title screen and menus
  advance one step per frame.
- **The world.** BOTW's real-time frame step makes world time follow real time at any frame rate, so the world is now
  uncapped.
- **Menus and loading screens are held at 60 again.**
  - They show a still picture of the world and draw no scene.
  - A frame with under 200 draws into a depth target at least 1280 wide (the 1600x900 scene) counts as a menu.
  - Three frames in a row switch the cap either way.
  - Each switch is logged: `[totk] BOTW world: frames uncapped` / `BOTW menu or loading: at most 60 frames a second`.
  - `TOTK_FRAME_CAP=<n>` forces a cap all the time, and `0` removes it.
- **Key 2.** The launcher starts BOTW with key 2 at "max" (`TOTK_MFX=interpmax`), unless the variable is already set.
- **Result:** on the 120 Hz built-in display, world frames take 8.5 ms in the median (about 118 FPS).

## 6. Render-thread cost (bd1fde6)

BOTW's render thread (`guest-0x1004`, priority 30) holds the game's frame lock while it records and submits the frame.
The main thread waits for that lock, 2,021 times for more than 20 ms in one session. A `sample` in the world found the render
thread 78% busy inside `nvnQueueSubmitCommands`, which is our NVN-to-Metal translation.

The largest single item was the copies of uniform blocks that the game updates inline (`nvnCommandBufferUpdateUniformBuffer`).
Each draw that binds such a block gets a private copy of it (`upd_shadow`), so draws before and after the update each see
their own version. There were 283 copies of 64 KB a frame, 18 MB a frame in all. Two changes cut that cost:

- **Copy only the bytes the shader reads.**
  - The translator declares every uniform block as 64 KB (`float4 _m0[4096]`), and BOTW binds 64 KB.
  - When the shader's MSL indexes a binding with constants only, as in `(*spvDescriptorSet0.fp_c7)._m0[33]`, it reads
    (largest index + 1) x 16 bytes. That is 89% of the bindings, 393 bytes on average.
  - `refl_cbuf_need` computes this when a shader loads. Bindings with a computed index are still copied whole.
  - `TOTK_CBUF_NEED=0` copies whole bindings.
- **Build each copy from the previous one.**
  - A newer version of a block copied earlier in the same replay starts from that copy, plus the writes noted since.
  - Before, each copy re-read the snapshot and walked every pending write, thousands of them, for each of the ~540 copies a
    frame.
  - It applies only while nothing else changed: a generation counter (`g_mat_gen`) moves with a new submission, a rebuilt or
    trimmed snapshot table, a GPU copy, or a full table. It also requires that no GPU write completed in between. Otherwise
    the copy is built the full way.
  - 96% of the copies take the short way. `TOTK_SHADOW_INCR=0` turns it off.

Results from one session, in the world:

| | Before | After |
|---|---|---|
| Render thread in `nvnQueueSubmitCommands` | 88% busy | 65% busy |
| `upd_shadow` | 11.8% | 6.2% |
| Pending-write walk | 5.9% | 0.2% |
| Frame time p95 | 24–35 ms | 13–14 ms |
| Worst frame | 95–144 ms | 15–28 ms |

## Still open

- **Render passes.** Opening them takes 11% of the render thread. The game opens about 135 a frame, and Metal's encoder
  creation dominates.
- **Long frames outside NVN.** Frames of 20–170 ms still happen while NVN and the GPU take 4–6 ms. That is guest CPU work on
  the three virtual cores.
- **Display rate.** `mtl_display_hz()` reads the main display's refresh rate, not the rate of the display the window is on.
- **UI glitch with the seed cache.** The bottom text and the horse icons sometimes have dithered edges. It is seen only with
  the seed-imported cache and is unexplained.
- **`TOTK_FREE_CORES=1` crashes BOTW at boot** ("command handle not a live recording").
