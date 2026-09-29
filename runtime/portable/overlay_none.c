/* No overlay (a back end that has none yet, docs/OVERLAY.md phase 4): overlay.h as no-ops. */
#include "overlay.h"

void overlay_init(SDL_Window* window) { (void)window; }
void overlay_set_ini(const char* path) { (void)path; }
int overlay_event(const SDL_Event* e) { (void)e; return 0; }
int overlay_shown(void) { return 0; }
void overlay_note_present(int frame_w, int frame_h, int screen_w, int screen_h, int metalfx, float fps)
{
    (void)frame_w, (void)frame_h, (void)screen_w, (void)screen_h, (void)metalfx, (void)fps;
}
void overlay_build_frame(void) {}
void overlay_set_line_runner(int (*run)(const char* line)) { (void)run; }
void overlay_set_game_windows(void (*hide)(int log, int party)) { (void)hide; }

/* no overlay, so no zone maps either (zonemap.cpp is the overlay's) */
#include "zonemap.h"
void zonemap_init(const char* game_dir, const uint8_t keys[256]) { (void)game_dir, (void)keys; }
void zonemap_want(int zone, float x, float y, float z) { (void)zone, (void)x, (void)y, (void)z; }
int zonemap_take(int zone, ZoneMap* out) { (void)zone, (void)out; return 0; }
