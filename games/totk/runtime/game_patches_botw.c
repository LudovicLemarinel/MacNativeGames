/* BOTW 1.9.0: no game-code patches yet. The shipping frame controller and output resolution run unchanged; TOTK's
   frame-step, 60 FPS and output-resolution sites do not exist in this main (docs/botw-port.md). The functions below
   keep the interface the frontend and graphics layer call, reporting that nothing is patched. */
#include "game_patches.h"
#include "game_version.h"
#include <stdio.h>
#include <string.h>

void runtime_frame_step_pin(double ms) { (void)ms; }
/* No real-time frame step: BOTW advances a fixed step per frame, so the host paces it as the console does. */
int runtime_frame_step_realtime(void) { return 0; }
void runtime_frame_step_update(uint64_t now_ns) { (void)now_ns; }
int runtime_output_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }
uint64_t runtime_output_res_extra_bytes(void) { return 0; }
int runtime_output_res_toggle(void) { return 0; }
int runtime_output_mode_cycle(void) { return 0; }

void runtime_game_patches(Module *m) {
    if (strcmp(m->name, "main")) runtime_fail("BOTW game patches expect main");
    fprintf(stderr, "[totk] BOTW %s: no game-code patches (shipping frame controller and output resolution)\n", TOTK_GAME_VERSION);
}
