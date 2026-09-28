/* DirectSound 8 on SDL3 audio (dsound.c). */
#pragma once

/* Registers the shims (before the images are mapped). */
void dsound_init(void);
/* Builds the COM vtables in guest memory (once the guest heap is up). */
void dsound_setup(void);

/* The player's volume (0..1) while in the world, and before it: the title and login screens, which
 * play at full volume in the game until its own volume setting applies (host64 --live). */
void dsound_set_volume(float in_world, float before_world);
/* Whether the game talks to a zone server (ws2.c: its UDP): the volume to use. */
void dsound_set_in_world(int on);
