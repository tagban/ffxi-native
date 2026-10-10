/* MogHouse's own mounts: creatures the game has no mount of, made mounts at launch from the player's
 * own install, as retail made its own (the tiger mount is the tiger's model with its motions renamed and
 * a seat for each race on its skeleton). Nothing of the game's is shipped: the files are built from the
 * install into a folder of the player's, served as a DAT overlay, and the game's file table (VTABLE.DAT,
 * FTABLE.DAT) is given their entries as the game reads it.
 *
 * The game finds a mount's model at file id 102704 + its mount id (retail's go to 39, and 64); the
 * server sends MogHouse's from 40 on (moghouse-lsb, modules/moghouse/cpp/mount.cpp):
 *   40, 41, 42  a bee, small (a Tarutaru rides it), middling and large (a Galka)
 *   43          a small airship (the one that flies into port, its static model made a mount's meshes)
 *   44          a boat (the sailing ship, flying)
 * Other clients have no file there and draw nothing. */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Builds the mounts from the install (the game folder's guest path, after vfs_init and any overlays)
 * into out_dir, a host folder laid out as a DAT overlay, and adds it. The number of mounts made. */
int mounts_build(const char* guest_game, const char* out_dir);

/* An open file of the game's (its host path): the file table's, to be given the mounts' entries as it
 * is read (mounts_patch), or not. Forgotten again at close. */
void mounts_track(const void* file, const char* host_path);
void mounts_untrack(const void* file);
int mounts_tracked(const void* file);
/* Bytes just read from a tracked file, from its offset `at`: the mounts' entries put in. */
void mounts_patch(const void* file, uint64_t at, uint8_t* buf, size_t n);
