/* BOTW 1.9.0 game patches (docs/botw-port.md). The output resolution is unchanged; TOTK's output-resolution sites do
   not exist in this main. The functions below keep the interface the frontend and graphics layer call. */
#include "game_patches.h"
#include "aspect_patch.h"
#include "game_version.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t off; uint32_t original, replacement; const char *what; } Patch;

/* Real-time frame step. ksys::VFR::updateInterval (main+0x151d3d0, called every frame with the framework's present
   interval) sets the interval ratio = interval / base interval (2), and VFR::setDelta scales every game delta by it:
   one frame of game time is 1/30 s times that ratio. Without this patch game time follows the interval the controller
   chose, so the host must pace frames exactly by it (60 Hz ticks). With it, the ratio is the frame's real duration
   times 30, written by the host at every present: game time follows real time at any frame rate, which allows the
   uncapped mode (TOTK_UNCAPPED) and every interpolation mode, as in TOTK. TOTK_REALTIME=0 keeps the interval ratio. */
static _Atomic uint32_t *frame_ratio;   // float bits, on the page after main
static _Atomic uint32_t pinned_step_us;
static uint64_t last_present_ns;
static double frame_samples[5];
static unsigned frame_sample_count, frame_sample_next;
static void frame_step_store(double seconds) {
    float ratio = (float)(seconds * 30.0); uint32_t bits; memcpy(&bits, &ratio, 4);
    atomic_store_explicit(frame_ratio, bits, memory_order_release);
}
void runtime_frame_step_pin(double ms) {
    uint32_t us = ms > 0 ? (uint32_t)(ms * 1000.0 + 0.5) : 0; atomic_store(&pinned_step_us, us);
    if (frame_ratio && us) frame_step_store(us / 1e6);
    fprintf(stderr, "[totk] BOTW frame step %s (pin %.3f ms)\n", us ? "pinned" : "real time", us / 1000.0);
}
int runtime_frame_step_realtime(void) { return frame_ratio != NULL; }
/* At most 60 game frames a second (hle_nvn.c, divided by the game's interval) outside the world: the real-time step
   covers the world, but the title and the menus advance per frame and ran far too fast above 60 on a 120 Hz display.
   The world is uncapped (user, 2026-10-20: the render must go past 60) once the player's game_data.sav is read
   (runtime_world_loading, fs_host.c) and while frames draw the world's scene: the menus (inventory, map, pause) show a
   still picture of it and draw no scene, loading screens neither. A frame counts as the world from SCENE_DRAWS scene
   draws; three frames in a row switch the cap either way (no flicker on a single odd frame). TOTK_FRAME_CAP=<n>: that
   cap all the time, 0: none. */
#define SCENE_DRAWS 200
void dm_nvn_frame_draws(uint64_t *all, uint64_t *scene);
int runtime_world_loading(void);
int runtime_frame_max_hz(void) {
    static int hz = -2; if (hz == -2) { const char *e = getenv("TOTK_FRAME_CAP"); hz = e && *e ? atoi(e) : -1; if (hz < -1) hz = -1; }
    if (hz >= 0) return hz;
    if (runtime_world_loading() <= 0) return 60;
    static int world, run; uint64_t all, scene; dm_nvn_frame_draws(&all, &scene);
    int now = scene >= SCENE_DRAWS;
    if (now == world) run = 0;
    else if (++run >= 3) {
        world = now; run = 0;
        fprintf(stderr, "[totk] BOTW %s: %s (frame draws %llu, scene %llu)\n", world ? "world" : "menu or loading", world ? "frames uncapped" : "at most 60 frames a second",
                (unsigned long long)all, (unsigned long long)scene);
    }
    return world ? 0 : 60;
}
void runtime_frame_step_update(uint64_t now_ns) {
    if (!frame_ratio || !now_ns) return;
    if (atomic_load_explicit(&pinned_step_us, memory_order_relaxed)) { last_present_ns = now_ns; return; }
    if (!last_present_ns) { last_present_ns = now_ns; return; }
    double dt = (double)(now_ns - last_present_ns) * 1e-9; last_present_ns = now_ns;
    if (dt > 0.25 || dt < 0.001) { frame_sample_count = frame_sample_next = 0; return; }   // pause, load or hitch: keep the last step
    if (dt < 1.0 / 120.0) dt = 1.0 / 120.0; if (dt > 1.0 / 15.0) dt = 1.0 / 15.0;
    frame_samples[frame_sample_next++ % 5] = dt; if (frame_sample_count < 5) frame_sample_count++;
    double sum = 0; for (unsigned i = 0; i < frame_sample_count; i++) sum += frame_samples[i];
    frame_step_store(sum / frame_sample_count);
}
static uint32_t adrp(unsigned reg, uint64_t pc, uint64_t target) {
    int64_t pages = (int64_t)(target >> 12) - (int64_t)(pc >> 12);
    if (pages < -(1 << 20) || pages >= (1 << 20)) runtime_fail("BOTW frame-step data outside ADRP range");
    uint32_t imm = (uint32_t)pages & 0x1fffff;
    return 0x90000000 | ((imm & 3) << 29) | ((imm >> 2) << 5) | reg;
}
static uint32_t branch(uint64_t pc, uint64_t target) {
    int64_t delta = (int64_t)target - (int64_t)pc;
    if ((delta & 3) || delta < -(1 << 27) || delta >= (1 << 27)) runtime_fail("BOTW patch branch outside range");
    return 0x14000000 | (((uint32_t)(delta >> 2)) & 0x3ffffff);
}
static void patch_realtime(Module *m) {
    const char *enabled = getenv("TOTK_REALTIME"); if (enabled && !strcmp(enabled, "0")) return;
    const uint64_t site = 0x151d484;   // fcmp s1, s0: the new ratio against the stored mIntervalRatioA (+0x3c)
    uint64_t data = (m->file_size + 0x3fff) & ~0x3fffULL, stub = (m->file_size + 15) & ~15ULL;
    if (data + 0x4000 != m->size || stub + 16 > data) runtime_fail("BOTW frame-step patch layout");
    const Patch guards[] = {{0x151d470, 0x1e210841, 0, "fmul s1, s2, s1: ratio = interval / base"},
                            {0x151d484, 0x1e202020, 0, "fcmp s1, s0"}, {0x151d490, 0xbd003e61, 0, "str s1, [x19, #0x3c]"},
                            {0x151d4a4, 0xf9404269, 0, "ldr x9, [x19, #0x80]: x9 is dead from the site to here"}};
    for (unsigned i = 0; i < sizeof guards / sizeof *guards; i++) {
        uint32_t word; memcpy(&word, m->base + guards[i].off, 4);
        if (word != guards[i].original) {
            fprintf(stderr, "[totk] BOTW frame step: main+%#llx is %08x, expected %08x (%s)\n", (unsigned long long)guards[i].off, word, guards[i].original, guards[i].what);
            runtime_fail("BOTW real-time original instruction mismatch");
        }
    }
    for (unsigned i = 0; i < 16; i++) if (m->base[stub + i]) runtime_fail("BOTW frame-step veneer padding not empty");
    /* adrp x9, data; ldr s1, [x9]; fcmp s1, s0; b site + 4 */
    uint32_t veneer[] = {adrp(9, (uintptr_t)m->base + stub, (uintptr_t)m->base + data), 0xbd400121, 0x1e202020, branch(stub + 12, site + 4)};
    memcpy(m->base + stub, veneer, sizeof veneer);
    frame_ratio = (_Atomic uint32_t *)(void *)(m->base + data);
    frame_step_store(1.0 / 30.0);   // the shipping 30 FPS step until the first measured frame
    uint32_t jump = branch(site, stub); memcpy(m->base + site, &jump, 4);
    fprintf(stderr, "[totk] BOTW real-time frame step: VFR interval ratio from the host (veneer +%#llx); TOTK_REALTIME=0 restores the interval ratio\n", (unsigned long long)stub);
}
/* Output resolution (experimental, TOTK_OUTPUT_RES=<w>x<h>, e.g. 2560x1440; unset or 0: the shipping 1920x1080).
   BOTW sizes its frame from two places in 1.9.0:
   - the default display size, a 1920x1080 global (main+0x1e43dc0) initialised lazily at three inlined sites;
   - the docked case of the display-mode switch (main+0x12660bc): output and scene widths as floats at +0x1210/+0x1214
     (1920.0 / 1600.0: the 1600x900 scene), then the 1920x1080 size handed to the display (main+0x106dc70) and to the
     crop (main+0xe9bb04).
   Here every 1920x1080 becomes <w>x<h> and the scene width equals the output width (native resolution scene). */
static unsigned out_w, out_h;
int runtime_output_res(unsigned *w, unsigned *h) {
    static int decided;
    if (!decided) {
        decided = 1;
        const char *e = getenv("TOTK_OUTPUT_RES"); char tail;
        if (e && *e && strcmp(e, "0") && strcmp(e, "auto")) {
            if (sscanf(e, "%ux%u%c", &out_w, &out_h, &tail) != 2 || out_w < 640 || out_h < 360 || out_w > 7680 || out_h > 4320 || out_w > 0xffff || out_h > 0xffff)
                runtime_fail("TOTK_OUTPUT_RES must be 0 or widthxheight");
        }
    }
    if (out_w && w && h) { *w = out_w; *h = out_h; }
    return out_w != 0;
}
static uint32_t movz(unsigned rd, unsigned imm, unsigned shift, int x) { return (x ? 0xd2800000u : 0x52800000u) | (shift / 16) << 21 | imm << 5 | rd; }
static uint32_t movk(unsigned rd, unsigned imm, unsigned shift) { return 0xf2800000u | (shift / 16) << 21 | imm << 5 | rd; }
/* TOTK_SCENE_WIDTH=<w> (experimental): only the docked scene width (shipping 1600; 1920 renders the scene at the
   1080p output size), the output stays as it is. */
static void patch_scene_width(Module *m) {
    const char *e = getenv("TOTK_SCENE_WIDTH"); if (!e || !*e) return;
    float fw = (float)atoi(e); uint32_t bits; memcpy(&bits, &fw, 4);
    if (fw < 640 || fw > 7680 || (bits & 0xffff)) runtime_fail("TOTK_SCENE_WIDTH must be a width like 1920 or 2560");
    uint32_t word; memcpy(&word, m->base + 0x12660c8, 4);
    if (word != 0xf2e89908) runtime_fail("BOTW scene width original instruction mismatch");
    word = movk(8, bits >> 16, 48); memcpy(m->base + 0x12660c8, &word, 4);
    fprintf(stderr, "[totk] BOTW docked scene width %.0f (experimental)\n", fw);
}
static void patch_output_res(Module *m) {
    unsigned w, h; if (!runtime_output_res(&w, &h)) return;
    float fw = (float)w; uint32_t fbits; memcpy(&fbits, &fw, 4);
    if (fbits & 0xffff) runtime_fail("TOTK_OUTPUT_RES width is not exact in a float's upper half (use a width like 2560, 3840)");
    unsigned fhi = fbits >> 16;
    const Patch p[] = {
        {0xc61ee8, 0xd280f009, movz(9, w, 0, 1), "default display size: mov x9, #w"}, {0xc61ef4, 0xf2c08709, movk(9, h, 32), "movk x9, #h, lsl #32"},
        {0xc61f1c, 0xd280f009, movz(9, w, 0, 1), "default display size: mov x9, #w"}, {0xc61f28, 0xf2c08709, movk(9, h, 32), "movk x9, #h, lsl #32"},
        {0x12618b8, 0xd280f009, movz(9, w, 0, 1), "default display size: mov x9, #w"}, {0x12618c4, 0xf2c08709, movk(9, h, 32), "movk x9, #h, lsl #32"},
        {0x12660bc, 0xd2a89e08, movz(8, fhi, 16, 1), "docked: output width float (was 1920.0)"},
        {0x12660c8, 0xf2e89908, movk(8, fhi, 48), "docked: scene width float (was 1600.0)"},
        {0x12660d0, 0xd280f008, movz(8, w, 0, 1), "docked: display size width"}, {0x12660d4, 0xf2c08708, movk(8, h, 32), "docked: display size height"},
        {0x12660f0, 0x52808704, movz(4, h, 0, 0), "docked: crop height"}, {0x1266104, 0x5280f003, movz(3, w, 0, 0), "docked: crop width"},
    };
    for (unsigned i = 0; i < sizeof p / sizeof *p; i++) {
        uint32_t word; memcpy(&word, m->base + p[i].off, 4);
        if (word != p[i].original) {
            fprintf(stderr, "[totk] BOTW output resolution: main+%#llx is %08x, expected %08x (%s)\n", (unsigned long long)p[i].off, word, p[i].original, p[i].what);
            runtime_fail("BOTW output resolution original instruction mismatch");
        }
    }
    /* TOTK_OUTPUT_RES_SITES (diagnostic bit mask, default 7): 1 default display size, 2 docked widths, 4 display size and crop */
    const char *ms = getenv("TOTK_OUTPUT_RES_SITES"); unsigned mask = ms ? (unsigned)strtoul(ms, NULL, 0) : 7;
    for (unsigned i = 0; i < sizeof p / sizeof *p; i++) if (mask >> (i < 6 ? 0 : i < 8 ? 1 : 2) & 1) memcpy(m->base + p[i].off, &p[i].replacement, 4);
    fprintf(stderr, "[totk] BOTW output resolution sites mask %#x\n", mask);
    fprintf(stderr, "[totk] BOTW output resolution %ux%u, scene at the same size (experimental, %zu verified instructions)\n", w, h, sizeof p / sizeof *p);
}
uint64_t runtime_output_res_extra_bytes(void) { return 0; }
int runtime_render_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }   /* no scene-size heap overlay (romfs.c) */
int runtime_output_res_toggle(void) { return 0; }
uint64_t runtime_output_size_current(void) { return out_w ? (uint64_t)out_h << 32 | out_w : (uint64_t)1080 << 32 | 1920; }
/* Key 3: MetalFX upscaling of the finished picture to the window (full screen or a large window shows the 1920x1080
   picture bigger; MetalFX instead of the bilinear stretch). The host's output stage (nvn_mtl.m output_upscale) is
   game-independent; TOTK's other stage (its own upscale shader replaced by MetalFX) does not apply to BOTW. The scene
   resolution is a launch setting (TOTK_SCENE_WIDTH): BOTW sizes its scene targets when the display mode is applied. */
int runtime_output_mode_cycle(void) {
    extern int mfxup_output_enabled(void), mfxup_enabled(void); extern void dm_gfx_set_flag(const char *, int);
    int on = !(mfxup_enabled() && mfxup_output_enabled());
    dm_gfx_set_flag("mfx_upscale", on); dm_gfx_set_flag("mfx_output", on);
    fprintf(stderr, "[totk] key 3: MetalFX output upscaling %s\n", on ? "on" : "off");
    return 1;
}

/* 60 FPS, dynamic. BOTW's adaptive present-interval controller (framework frame loop main+0x112660c, controller object
   at framework+0x228) turns the measured frame work time (+0x250, in ticks) into 60 Hz frames, adds 1 (2 above a 0.9
   fraction), and clamps the result to [2, maximum]; slower frames raise the interval, six steady frames lower it again.
   Its setter (main+0x1126c70) already accepts 1. The 1.6.0 "60 FPS static" patch disables this controller and forces
   interval 1, so the game slows down whenever a frame takes longer than 16.7 ms. Here the controller is kept and its
   floor lowered from 2 to 1, as TOTK's 60 FPS patch does: 60 FPS when frames fit, 30 or 20 with correct game speed
   otherwise. Same controller in 1.6.0 at main+0x16148c0 (s11 = 1.0, s12 = 2.0, w20 = 2). TOTK_FPS60=0 leaves it as
   shipped.
   Its maximum (+0xc, read only here through main+0x1126cb8) is held at 2 (2026-10-19): key 2's 30+30 mode paces every
   frame to 33.3 ms, which the controller reads as 1.99 frames -> 1 + 2 = interval 3, so menus fell to 20 FPS and
   stayed there (19 FPS, slow response). With the real-time frame step an interval of 3 only costs frames. */
static const Patch fps60[] = {
    {0x112670c, 0x1e204160, 0x1e204140, "floor: fmov s0, s10 (1.0; was s11 = 2.0)"},
    {0x1126748, 0x71000d1f, 0x7100091f, "raise path: cmp w8, #2 (was #3)"},
    {0x1126754, 0x1a98c109, 0x51000509, "raise path: sub w9, w8, #1 (was csel w9, w8, w24 = 3, gt)"},
    {0x112675c, 0x51000529, 0x1a9fc529, "raise path: csinc w9, w9, wzr, gt (next lower candidate floors at 1; was sub w9, w9, #1)"},
    {0x1126794, 0x71000d3f, 0x7100093f, "lower path: cmp w9, #2 (keep lowering down to 1; was #3)"},
    {0x1126cb8, 0xb9400c00, 0x52800040, "maximum: mov w0, #2 (was ldr w0, [x0, #0xc] = 3)"},
};

/* Dynamic resolution off (the "Disable Dynamic Resolution" mod by theboy181 for 1.6.0, main+0x150aba4). The resolution
   controller (main+0xf82e00 in 1.9.0) measures each frame's GPU time (ticks x 0.001 x 100 / 16666.67 us) and shifts its
   history (+0x78 <- +0x80, +0x88) before choosing the render scale. Without the shift it never sees the load and keeps
   the full 1600x900 (docked) scene. TOTK_DYNAMIC_RES=1 keeps the game's dynamic resolution. */
static void patch_dynamic_res(Module *m) {
    const char *e = getenv("TOTK_DYNAMIC_RES"); if (e && !strcmp(e, "1")) return;
    const Patch p = {0xf82e80, 0xa907a668, 0xd503201f, "nop (was stp x8, x9, [x19, #0x78]: GPU time history shift)"};
    uint32_t word; memcpy(&word, m->base + p.off, 4);
    if (word != p.original) {
        fprintf(stderr, "[totk] BOTW dynamic resolution: main+%#llx is %08x, expected %08x\n", (unsigned long long)p.off, word, p.original);
        runtime_fail("BOTW dynamic resolution original instruction mismatch");
    }
    memcpy(m->base + p.off, &p.replacement, 4);
    fprintf(stderr, "[totk] BOTW dynamic resolution off: full render scale (TOTK_DYNAMIC_RES=1 keeps the game's)\n");
}

void runtime_game_patches(Module *m) {
    if (strcmp(m->name, "main")) runtime_fail("BOTW game patches expect main");
    patch_realtime(m);
    patch_dynamic_res(m);
    patch_scene_width(m);
    patch_output_res(m);
    /* Key 4, ultrawide (aspect_patch.h). The places that hand the camera its aspect ratio, as a community 21:9 mod for
       emulators changes them in 1.9.0 (only its addresses are used): three viewport routines dividing width by height
       (fdiv s0, s0, s1) and the camera's ratio getter (ldr s0, [x0, #0xb0], the same sead field as TOTK). That mod also
       sets the 16:9 constant at main+0x16a18b8 in read-only data, which a key cannot follow; left as it is. The stubs go
       to main's text padding (0x390 bytes before the read-only data), the frame-step veneer to the file's padding. */
    { static const uint64_t sites[] = {0xc2ab70, 0xeb4a28, 0x10edb0c, 0x10ee114};
      aspect_install(m, TOTK_GAME_VERSION, sites, 4, (m->text_end + 15) & ~15ULL, (m->file_size + 0x3fff) & ~0x3fffULL); }
    const char *e = getenv("TOTK_FPS60");
    if (e && !strcmp(e, "0")) {
        fprintf(stderr, "[totk] BOTW %s: shipping frame controller (TOTK_FPS60=0)\n", TOTK_GAME_VERSION);
        return;
    }
    /* the maximum held at 2 (the last entry) only with the real-time frame step: without it the interval sets game speed */
    unsigned n = sizeof fps60 / sizeof *fps60 - (frame_ratio ? 0 : 1);
    for (unsigned i = 0; i < n; i++) {
        uint32_t word;
        if (fps60[i].off + 4 > m->text_end) runtime_fail("BOTW FPS patch outside text");
        memcpy(&word, m->base + fps60[i].off, 4);
        if (word != fps60[i].original) {
            fprintf(stderr, "[totk] BOTW FPS patch: main+%#llx is %08x, expected %08x (%s)\n", (unsigned long long)fps60[i].off, word, fps60[i].original, fps60[i].what);
            runtime_fail("BOTW FPS patch original instruction mismatch");
        }
    }
    for (unsigned i = 0; i < n; i++) memcpy(m->base + fps60[i].off, &fps60[i].replacement, 4);
    fprintf(stderr, "[totk] BOTW %s 60 FPS: controller interval %s (%u verified instructions; TOTK_FPS60=0 restores 30 FPS)\n",
            TOTK_GAME_VERSION, frame_ratio ? "1 to 2" : "from 1", n);
}
