/* BOTW 1.9.0 game patches (docs/botw-port.md). The output resolution is unchanged; TOTK's frame-step and
   output-resolution sites do not exist in this main. The functions below keep the interface the frontend and graphics
   layer call. */
#include "game_patches.h"
#include "game_version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void runtime_frame_step_pin(double ms) { (void)ms; }
/* No real-time frame step: BOTW advances game time by the present interval its own controller chose (main+0x1126cc0
   publishes 60 / interval as the frame rate), so the host paces frames as the console does: 60 Hz ticks, that
   interval. */
int runtime_frame_step_realtime(void) { return 0; }
void runtime_frame_step_update(uint64_t now_ns) { (void)now_ns; }
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
typedef struct { uint64_t off; uint32_t original, replacement; const char *what; } Patch;
static const Patch fps60[] = {
    {0x112670c, 0x1e204160, 0x1e204140, "floor: fmov s0, s10 (1.0; was s11 = 2.0)"},
    {0x1126748, 0x71000d1f, 0x7100091f, "raise path: cmp w8, #2 (was #3)"},
    {0x1126754, 0x1a98c109, 0x51000509, "raise path: sub w9, w8, #1 (was csel w9, w8, w24 = 3, gt)"},
    {0x112675c, 0x51000529, 0x1a9fc529, "raise path: csinc w9, w9, wzr, gt (next lower candidate floors at 1; was sub w9, w9, #1)"},
    {0x1126794, 0x71000d3f, 0x7100093f, "lower path: cmp w9, #2 (keep lowering down to 1; was #3)"},
};

void runtime_game_patches(Module *m) {
    if (strcmp(m->name, "main")) runtime_fail("BOTW game patches expect main");
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
