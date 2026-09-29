/* The overlay (docs/OVERLAY.md): windows the host draws over the game, at the screen's own
 * resolution. Display only: it sends nothing to the server and presses nothing for the player.
 * overlay.cpp (Dear ImGui) with overlay_metal.mm on Metal; overlay_none.c where a back end has no
 * overlay yet (everything a no-op). */
#pragma once

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Once the game's window exists (the graphics back end's init). */
void overlay_init(SDL_Window* window);
/* Where the windows' places and sizes are kept (host64 --data-dir): before the first frame. */
void overlay_set_ini(const char* path);
/* Every SDL event, before the game sees it: 1 when the overlay took it (a click on one of its
 * windows, typing in it, its show/hide key), and the game must not see it. */
int overlay_event(const SDL_Event* e);
/* Whether it is shown (Cmd+U / Ctrl+Shift+U shows and hides it). */
int overlay_shown(void);

/* What the present did (the back end, each frame): for the status window. */
void overlay_note_present(int frame_w, int frame_h, int screen_w, int screen_h, int metalfx, float fps);

/* How a line the player sends from the overlay reaches the game (host64: the game's own parser of
 * a typed line); without one, the overlay has no chat box. */
void overlay_set_line_runner(int (*run)(const char* line));

/* How the overlay hides the game's own windows its own stand in for (host64: moved off the screen
 * and back), called every frame: the game's chat log, its party list, its target box; and which of the game's
 * windows has the keyboard ("" none, else its 8-character name). */
void overlay_set_game_windows(void (*hide)(int log, int party, int target), const char* (*focus)(void));

/* The names over heads, when the overlay draws them (host64's nameplate hook, where the build has
 * one): whether it wants them now, and each one as the game placed it this frame (where in its 3D
 * view; the game's text, with its own codes; its color, 0x80 full in each channel). */
void overlay_set_nameplates_available(int yes);
int overlay_nameplates_wanted(void);
void overlay_nameplate(float fx, float fy, float z, const char* text, uint32_t color); /* fx, fy: 0-1 across the 3D view */
/* Names behind walls: the back end, when the world's scene is done, copies the scene's depth where
 * the last frame's names were (overlay_plate_points: their places, 0-1 across the view, and a token
 * for them), and hands the depths back when the GPU has them (overlay_plate_depths, with the
 * projection's third column, to make distances of both); a name further than what is drawn there
 * is hidden. */
int overlay_plate_points(float* xy, int max, int* token);
void overlay_plate_depths(int token, const float* depths, int n, float proj10, float proj14);

/* The game's own close of one of its windows by its 8-character name (host64), and the launcher's
 * settings (host64: asks the launcher to show them), for the overlay's bar. */
void overlay_set_game_window_closer(int (*close)(const char* name8));
void overlay_set_settings_opener(void (*open)(void));
/* where the game's window with the keyboard is (host64: its rectangle in the game's back buffer
 * pixels), so the overlay's windows can keep out of its way */
/* and moves it (its top left, fractions of the screen; x < 0 lets it go) */
void overlay_set_focus_rect(int (*rect)(float* x, float* y, float* w, float* h), void (*place)(float x, float y)); /* fractions of the screen */

/* The back end's part: overlay.cpp builds the frame, the back end draws it (overlay_metal.mm). */
void overlay_build_frame(void);

#ifdef __cplusplus
}
#endif
