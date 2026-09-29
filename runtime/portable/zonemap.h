/* The zone's map for the overlay's radar: its collision mesh, from the install's own zone DAT,
 * drawn from above: where you can walk, shaded by height, and the edges of it. Read from the
 * player's install at run time, as the game reads it; nothing of the game's is kept here.
 *
 * Adapted from the MogHouse client's zone reader (XI Test Client, MIT, John Leighow):
 * renderer/ffxi/{dat,mzb,filetable}.cpp and the walkable raster in renderer/collision.cpp. */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The install (for VTABLE.DAT, FTABLE.DAT and the ROM folders) and the game's own 256-byte table
 * its zone layouts are obscured with (meta/builds.json "mzb_keys", read from its memory). */
void zonemap_init(const char* game_dir, const uint8_t keys[256]);

typedef struct
{
    int zone;
    int size;        /* size x size RGBA, row 0 the north edge, column 0 the west */
    float cx, cz;    /* the world point at its middle (x east, z north) */
    float half;      /* world units from the middle to an edge */
    uint8_t* rgba;   /* NULL once taken */
} ZoneMap;

/* Starts making the map for a zone (in the background), if it is not the one made or being made:
 * the ground reachable from where the player stands (x, height, z) is the map, the rest dimmed. */
void zonemap_want(int zone, float x, float y, float z);
/* The finished map for a zone, once: the caller takes its pixels (and frees them with free()). */
int zonemap_take(int zone, ZoneMap* out);

#ifdef __cplusplus
}
#endif
