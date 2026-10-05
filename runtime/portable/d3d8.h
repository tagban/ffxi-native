/* Direct3D 8 for 64-bit hosts (d3d8.c): the D3D8 front end the Metal renderer goes under. */
#pragma once
#include <stdint.h>

/* Registers the shims (before the images are mapped). */
void d3d8_init(void);
/* Builds the COM vtables in guest memory (once the guest heap is up). */
void d3d8_setup(void);
/* Called at every Present, on the game's thread with the guest lock held, before the frame goes
 * out: where the host adjusts per-frame game state (host64: the frame-rate divisor). */
void d3d8_set_present_hook(void (*fn)(void));
/* Called at each frame's first BeginScene: after the game's own update, before anything is drawn. */
void d3d8_set_scene_hook(void (*fn)(void));
/* The size the frame is shown at: the device window's client area, else the back buffer's; 0x0
 * before the device exists. */
void d3d8_screen_size(uint32_t* w, uint32_t* h);
/* The viewport the game has set now, in its own units (x, y, width, height) */
void d3d8_viewport(uint32_t* x, uint32_t* y, uint32_t* w, uint32_t* h);
/* While on, the game's draws are dropped (one of its windows the overlay stands in for, drawing) */
void d3d8_drop_draws(int on);
/* while on, the interface's draws wholly inside rectangle i (0-3; the game's units) are dropped */
void d3d8_drop_rect(int i, int on, float x0, float y0, float x1, float y1);
uint32_t d3d8_dropped_rect_draws(void);
/* while on, the interface's draws wholly inside this rectangle are never dropped (a window asking) */
void d3d8_keep_rect(int on, float x0, float y0, float x1, float y1);
/* rectangle i: whether on, where, and what it dropped since the last call (a count, the box around
 * it); and the draws kept since the last call */
void d3d8_drop_rect_seen(int i, int* on, float r[4], uint32_t* n, float box[4]);
uint32_t d3d8_kept_draws(void);
/* the game's back buffer, in its own units (what its interface lays out in) */
void d3d8_backbuffer_size(uint32_t* w, uint32_t* h);
/* Adds a texture pack: <dir>/<hash>_<w>x<h>.dds replacements for the game's textures (see d3d8.c,
 * tools/make_texpack.py). Before the device is created. */
void d3d8_texture_pack(const char* dir);
