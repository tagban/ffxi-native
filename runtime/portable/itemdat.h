/* What an item is, from the install's own item DATs (read at run time, as the game reads them):
 * its name, description, kind, the slots and jobs it is for, its icon. The layout is as the
 * MogHouse client documents it (docs/wiki/Items.md). */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    uint16_t id;
    uint16_t flags, stack, type, targets, level, slots, races;
    uint32_t jobs;
    char name[64];     /* as the game shows it ("Bronze Sword") */
    char desc[512];    /* its description, plain text ("\n" between lines, elements by name) */
    uint8_t icon[32 * 32 * 4]; /* RGBA, row 0 the top */
    int has_icon;
} ItemInfo;

/* item types (ItemInfo.type) */
enum { ITEM_GENERAL = 1, ITEM_WEAPON = 4, ITEM_ARMOR = 5, ITEM_USABLE = 7, ITEM_CRYSTAL = 8, ITEM_FURNISHING = 10 };

/* An item by id (read once, then kept): NULL when the install has no such item. */
const ItemInfo* item_info(uint16_t id);

#ifdef __cplusplus
}
#endif
