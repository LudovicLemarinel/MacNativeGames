/* BOTW 1.9.0 game patches (docs/botw-port.md). The output resolution is unchanged; TOTK's output-resolution sites do
   not exist in this main. The functions below keep the interface the frontend and graphics layer call. */
#include "game_patches.h"
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
int runtime_output_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }
uint64_t runtime_output_res_extra_bytes(void) { return 0; }
int runtime_output_res_toggle(void) { return 0; }
int runtime_output_mode_cycle(void) { return 0; }

/* 60 FPS, dynamic. BOTW's adaptive present-interval controller (framework frame loop main+0x112660c, controller object
   at framework+0x228) turns the measured frame work time (+0x250, in ticks) into 60 Hz frames, adds 1 (2 above a 0.9
   fraction), and clamps the result to [2, maximum]; slower frames raise the interval, six steady frames lower it again.
   Its setter (main+0x1126c70) already accepts 1. The 1.6.0 "60 FPS static" patch disables this controller and forces
   interval 1, so the game slows down whenever a frame takes longer than 16.7 ms. Here the controller is kept and its
   floor lowered from 2 to 1, as TOTK's 60 FPS patch does: 60 FPS when frames fit, 30 or 20 with correct game speed
   otherwise. Same controller in 1.6.0 at main+0x16148c0 (s11 = 1.0, s12 = 2.0, w20 = 2). TOTK_FPS60=0 leaves it as
   shipped. */
static const Patch fps60[] = {
    {0x112670c, 0x1e204160, 0x1e204140, "floor: fmov s0, s10 (1.0; was s11 = 2.0)"},
    {0x1126748, 0x71000d1f, 0x7100091f, "raise path: cmp w8, #2 (was #3)"},
    {0x1126754, 0x1a98c109, 0x51000509, "raise path: sub w9, w8, #1 (was csel w9, w8, w24 = 3, gt)"},
    {0x112675c, 0x51000529, 0x1a9fc529, "raise path: csinc w9, w9, wzr, gt (next lower candidate floors at 1; was sub w9, w9, #1)"},
    {0x1126794, 0x71000d3f, 0x7100093f, "lower path: cmp w9, #2 (keep lowering down to 1; was #3)"},
};

void runtime_game_patches(Module *m) {
    if (strcmp(m->name, "main")) runtime_fail("BOTW game patches expect main");
    patch_realtime(m);
    const char *e = getenv("TOTK_FPS60");
    if (e && !strcmp(e, "0")) {
        fprintf(stderr, "[totk] BOTW %s: shipping frame controller (TOTK_FPS60=0)\n", TOTK_GAME_VERSION);
        return;
    }
    for (unsigned i = 0; i < sizeof fps60 / sizeof *fps60; i++) {
        uint32_t word;
        if (fps60[i].off + 4 > m->text_end) runtime_fail("BOTW FPS patch outside text");
        memcpy(&word, m->base + fps60[i].off, 4);
        if (word != fps60[i].original) {
            fprintf(stderr, "[totk] BOTW FPS patch: main+%#llx is %08x, expected %08x (%s)\n", (unsigned long long)fps60[i].off, word, fps60[i].original, fps60[i].what);
            runtime_fail("BOTW FPS patch original instruction mismatch");
        }
    }
    for (unsigned i = 0; i < sizeof fps60 / sizeof *fps60; i++) memcpy(m->base + fps60[i].off, &fps60[i].replacement, 4);
    fprintf(stderr, "[totk] BOTW %s 60 FPS: controller floor lowered to interval 1 (%zu verified instructions; TOTK_FPS60=0 restores 30 FPS)\n",
            TOTK_GAME_VERSION, sizeof fps60 / sizeof *fps60);
}
