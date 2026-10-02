/* gamestate.h: the server's packets, split and read. The layouts are the game protocol's; fields
 * here only as the overlay needs them. */
#include "gamestate.h"
#include "guest.h"
#include "gwin.h"
#include "zonemap.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { HEADER = 28, CHAT_LINES = 64 };

static uint32_t g_udp, g_by_id[512];

static struct
{
    int kind;
    char sender[16], text[400];
} g_chat[CHAT_LINES];
static int g_chat_next, g_chat_count;

/* the game's text to plain ASCII for now: its two-byte characters and auto-translate phrases as '?' */
static void plain(char* out, size_t n, const uint8_t* in, size_t max)
{
    size_t o = 0;
    for (size_t i = 0; i < max && in[i] && o + 1 < n; ++i)
        out[o++] = in[i] >= 0x20 && in[i] < 0x7F ? (char)in[i] : '?';
    out[o] = 0;
}

/* 0x017: a chat line (kind, attribute, a 16-bit value, the sender's name, the message) */
static void chat(const uint8_t* p, uint32_t size)
{
    if (size < 0x18)
        return;
    int i = g_chat_next;
    g_chat[i].kind = p[0x04];
    plain(g_chat[i].sender, sizeof g_chat[i].sender, p + 0x08, 15);
    plain(g_chat[i].text, sizeof g_chat[i].text, p + 0x17, size - 0x17);
    g_chat_next = (i + 1) % CHAT_LINES;
    if (g_chat_count < CHAT_LINES)
        ++g_chat_count;
}

static void zone_in(const uint8_t* p, uint32_t size);
static void group_attr(const uint8_t* p, uint32_t size);
static void group_list(const uint8_t* p, uint32_t size);
static void group_table(const uint8_t* p, uint32_t size);
static void entity_update(const uint8_t* p, uint32_t size, int pc);
static void self_status(const uint8_t* p, uint32_t size);
static void bags(uint32_t id, const uint8_t* p, uint32_t size);
static void stats(const uint8_t* p, uint32_t size);

/* every so often, to the log: what has come (the ids seen most) */
static void summary(void)
{
    extern void rt_log(const char* fmt, ...);
    uint32_t top[6] = { 0 }, n[6] = { 0 };
    for (uint32_t id = 0; id < 512; ++id)
        for (int k = 0; k < 6; ++k)
            if (g_by_id[id] > n[k])
            {
                memmove(top + k + 1, top + k, (5 - k) * sizeof *top);
                memmove(n + k + 1, n + k, (5 - k) * sizeof *n);
                top[k] = id, n[k] = g_by_id[id];
                break;
            }
    rt_log("[recomp] packets: %u in, the most: %03x x%u %03x x%u %03x x%u %03x x%u; chat %u\n", g_udp, top[0], n[0], top[1],
        n[1], top[2], n[2], top[3], n[3], g_by_id[0x017]);
}

void gamestate_feed(const uint8_t* buf, uint32_t len)
{
    if (!buf || len <= HEADER)
        return;
    if (++g_udp % 100 == 0)
        summary();
    for (uint32_t at = HEADER; at + 4 <= len;)
    {
        uint16_t head = (uint16_t)(buf[at] | buf[at + 1] << 8);
        uint32_t id = head & 0x1FF, size = 2u * (buf[at + 1] & 0xFEu);
        if (size < 4 || at + size > len)
            break;
        ++g_by_id[id];
        (void)chat; /* the game's log (gamestate_chat_line) has every line, these too */
        switch (id)
        {
        case 0x00A: zone_in(buf + at, size); break;
        case 0x0DF: group_attr(buf + at, size); break;
        case 0x0DD: group_list(buf + at, size); break;
        case 0x0C8: group_table(buf + at, size); break;
        case 0x00D: entity_update(buf + at, size, 1); break;
        case 0x00E: entity_update(buf + at, size, 0); break;
        case 0x037: self_status(buf + at, size); break;
        case 0x01C: case 0x01E: case 0x01F: case 0x020: case 0x050: bags(id, buf + at, size); break;
        case 0x061: stats(buf + at, size); break;
        default: break;
        }
        at += size;
    }
}

/* --- the party --------------------------------------------------------------------------------- */
enum { MEMBERS = 20 };
static GameMember g_members[MEMBERS]; /* id 0: free */
static uint32_t g_self;
static uint16_t g_zone;
static int g_table; /* whether a party table (0x0C8) has come: until then, whoever has been seen */
static uint32_t g_listed[MEMBERS];

static uint32_t u32(const uint8_t* p) { return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); }
static uint16_t u16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }

static GameMember* member(uint32_t id)
{
    GameMember* free_one = NULL;
    for (int i = 0; i < MEMBERS; ++i)
    {
        if (g_members[i].id == id)
            return &g_members[i];
        if (!g_members[i].id && !free_one)
            free_one = &g_members[i];
    }
    if (free_one)
    {
        memset(free_one, 0, sizeof *free_one);
        free_one->id = id;
    }
    return free_one;
}

static void name_of(char* out, const uint8_t* in)
{
    int i = 0;
    for (; i < 15 && in[i] >= 0x20 && in[i] < 0x7F; ++i)
        out[i] = (char)in[i];
    out[i] = 0;
}

/* --- who is around ------------------------------------------------------------------------------ */
enum { ENTITIES = 0x900 }; /* the game's own table's size: the index is the entity's place in it */
static GameEntity g_ents[ENTITIES];
static struct
{
    int known;
    float x, y, z;
    uint8_t heading;
} g_me;
static uint16_t g_self_index;
static uint32_t g_entity_map;

void gamestate_set_entity_map(uint32_t addr) { g_entity_map = addr; }

static uint32_t g_target_ptr;
void gamestate_set_target_ptr(uint32_t addr) { g_target_ptr = addr; }

static char g_game_dir[1024];
static uint32_t g_mzb_keys;
void gamestate_set_zone_files(const char* game_dir, uint32_t keys_addr)
{
    snprintf(g_game_dir, sizeof g_game_dir, "%s", game_dir ? game_dir : "");
    g_mzb_keys = keys_addr;
}

/* the zone's map (zonemap.h), from its layout: the key table read from the game once it is running */
static void want_map(int zone, float x, float y, float z)
{
    static int ready;
    if (!ready && g_mzb_keys && g_game_dir[0] && gwin_is_committed(g_mzb_keys) && gwin_is_committed(g_mzb_keys + 255))
    {
        uint8_t keys[256];
        memcpy(keys, GUEST_PTR(g_mzb_keys), 256);
        zonemap_init(g_game_dir, keys);
        ready = 1;
    }
    if (ready)
        zonemap_want(zone, x, y, z);
}

/* The game's own entity for an index, if it is the one with this id. Its layout, as far as read
 * here: the position (x, height, z floats) at 0x04, the facing (radians) at 0x18, the server's id
 * at 0x78. */
static uint32_t entity_at(uint16_t index, uint32_t id)
{
    if (!g_entity_map || index >= ENTITIES)
        return 0;
    uint32_t slot = g_entity_map + 4u * index;
    if (!gwin_is_committed(slot))
        return 0;
    uint32_t p = rd32(slot);
    if (!p || !gwin_is_committed(p) || !gwin_is_committed(p + 0x100))
        return 0;
    return rd32(p + 0x78) == id ? p : 0;
}

static float f32(const uint8_t* p)
{
    uint32_t u = (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

/* 0x00A: a new zone, where the player stands in it; nobody around yet */
static void zone_entities(const uint8_t* p)
{
    memset(g_ents, 0, sizeof g_ents);
    g_self_index = u16(p + 0x08);
    g_me.heading = p[0x0B];
    g_me.x = f32(p + 0x0C), g_me.y = f32(p + 0x10), g_me.z = f32(p + 0x14);
    g_me.known = 1;
}

/* 0x00D (another player) and 0x00E (an NPC or monster), as the MogHouse client reads them (its
 * docs/wiki/Entity-Visibility.md): the id, its index, which blocks this update carries (0x01
 * position, 0x02 claim and status, 0x04 general, 0x08 name, 0x20 gone), facing and position. What
 * a block does not carry is kept from before: a position-only update says nothing about the rest.
 *   0x00E general: HP% at 0x1E, the battle byte at 0x25 (set only for a living monster)
 *   0x00E claim/status: who claimed it at 0x2C, status at 0x20 (2, 3, 6: not drawn), flags at
 *     0x21 (0x80 no model, 0x800 untargetable: triggers), the look at 0x30 (kind 2-4: a door or
 *     transport; a standard look with no model: a marker)
 *   0x00D general: flags at 0x20 (bits 1 and 29: hidden)
 *   name: 0x00E at 0x34, 0x00D at 0x5A */
static void entity_update(const uint8_t* p, uint32_t size, int pc)
{
    if (size < 0x20)
        return;
    uint32_t id = u32(p + 0x04);
    uint16_t index = u16(p + 0x08);
    uint8_t parts = p[0x0A];
    if (!id || index >= ENTITIES || id == g_self)
        return;
    GameEntity* e = &g_ents[index];
    if (parts & 0x20)
    {
        memset(e, 0, sizeof *e);
        return;
    }
    if (e->id != id)
    {
        memset(e, 0, sizeof *e);
        e->id = id, e->index = index, e->hpp = 100;
    }
    e->kind = pc ? ENTITY_PC : ENTITY_NPC;
    if (parts & 0x01)
    {
        e->heading = p[0x0B];
        e->x = f32(p + 0x0C), e->y = f32(p + 0x10), e->z = f32(p + 0x14);
    }
    if ((parts & 0x04) && size > 0x25)
    {
        e->hpp = p[0x1E];
        if (!pc && p[0x25])
            e->mob = 1;
        if (pc && size >= 0x2C)
        {
            /* flags (LandSandBoat's char_update.cpp): the first word hide, LFG (11), anonymous (12),
             * away (14), linkshell (17), GM level (24-26), invisible (29), bazaar (31); the second
             * the linkshell's color (its low three bytes) and the GM icon (28); the third new
             * adventurer (23) and mentor (24) */
            uint32_t f1 = u32(p + 0x20), f2 = u32(p + 0x24), f3 = u32(p + 0x28);
            e->hidden = (f1 >> 1 & 1) || (f1 >> 29 & 1);
            e->gm = (uint8_t)(f1 >> 24 & 7);
            e->marks = (uint16_t)((e->gm || (f2 >> 28 & 1) ? MARK_GM : 0) | (f3 >> 24 & 1 ? MARK_MENTOR : 0) | (f3 >> 23 & 1 ? MARK_NEW : 0) |
                                  (f1 >> 11 & 1 ? MARK_LFG : 0) | (f1 >> 14 & 1 ? MARK_AWAY : 0) | (f1 >> 12 & 1 ? MARK_ANON : 0) |
                                  (f1 >> 31 & 1 ? MARK_BAZAAR : 0) | (f1 >> 17 & 1 ? MARK_LS : 0));
            e->ls = f2 & 0xFFFFFF;
        }
    }
    if (!pc && (parts & 0x02) && size >= 0x34)
    {
        e->claimed = u32(p + 0x2C) != 0;
        uint8_t status = p[0x20];
        uint32_t flags = u32(p + 0x21);
        uint16_t look = u16(p + 0x30), model = u16(p + 0x32);
        e->hidden = status == 2 || status == 3 || status == 6 || (flags & 0x80) || (flags & 0x800) ||
                    (look >= 2 && look <= 4) || (look == 0 && model == 0);
    }
    uint32_t at = pc ? 0x5A : 0x34;
    if ((parts & 0x08) && size > at)
    {
        uint32_t i = 0;
        for (; i + 1 < sizeof e->name && at + i < size && p[at + i] >= 0x20 && p[at + i] < 0x7F; ++i)
            e->name[i] = p[at + i] == '_' ? ' ' : (char)p[at + i]; /* the server's names have _ for spaces */
        e->name[i] = 0;
    }
}

static float mem_f32(uint32_t a)
{
    float f;
    uint32_t u = rd32(a);
    memcpy(&f, &u, 4);
    return f;
}

/* The name the game itself shows for an entity (its own, from its data: what the server calls an
 * NPC is not always what the player sees), right after the server's id in the entity, at 0x7C.
 * Trusted once it has agreed with the server's name for a few entities, and logged. */
static int g_mem_names; /* 0 not yet known, 1 agrees, -1 does not */
static void entity_name(uint32_t p, GameEntity* e)
{
    if (g_mem_names < 0 || !gwin_is_committed(p + 0x7C + 24))
        return;
    char n[24];
    int i = 0;
    for (; i < 23; ++i)
    {
        uint8_t c = rd8(p + 0x7C + (uint32_t)i);
        if (!c)
            break;
        if (c < 0x20 || c >= 0x7F)
            return; /* not a name */
        n[i] = (char)c;
    }
    n[i] = 0;
    if (g_mem_names == 0)
    {
        static int agree, differ;
        extern void rt_log(const char* fmt, ...);
        if (!e->name[0] || !n[0])
            return;
        if (!strcmp(n, e->name))
            ++agree;
        else if (++differ <= 3)
            rt_log("[recomp] names: the game's \"%s\", the server's \"%s\"\n", n, e->name);
        if (agree >= 5 || differ >= 12)
        {
            g_mem_names = agree >= 5 && agree * 2 > differ ? 1 : -1;
            rt_log("[recomp] names: the game's own names at +7C %s (%d agree, %d differ)\n", g_mem_names > 0 ? "used" : "not used", agree,
                differ);
        }
        return;
    }
    if (i)
        memcpy(e->name, n, (size_t)i + 1);
}

/* 0x037, the player's own status (LandSandBoat's char_status.cpp): flags at 0x28 (LFG 4, anonymous 5,
 * away 7, linkshell 25, GM level 29-31), 0x2C (bazaar 29, GM icon 31), the linkshell's color at 0x31,
 * and 0x38 (new adventurer 3, mentor 4) */
static struct
{
    uint16_t marks;
    uint8_t gm;
    uint32_t ls;
} g_self_marks;

/* Knocked out (the player's own server_status 3, LandSandBoat's Animation DEATH), and the seconds
 * then left until the game sends the player home itself: 0x037's dead_counter1 at 0x3C is 60 times
 * the seconds left plus six minutes (below six, the client returns the player home) */
static struct
{
    int dead;
    double home_secs;
    uint64_t at;
} g_death;

extern uint64_t rt_monotonic_ns(void);

static void self_death(uint8_t status, const uint8_t* counter)
{
    int dead = status == 3;
    if (dead && counter)
    {
        double left = (double)u32(counter) / 60.0 - 360.0;
        g_death.home_secs = left > 0 ? left : 0, g_death.at = rt_monotonic_ns();
    }
    else if (dead && !g_death.dead)
        g_death.home_secs = -1, g_death.at = rt_monotonic_ns(); /* the time comes with the next 0x037 */
    g_death.dead = dead;
}

int gamestate_dead(double* home_secs)
{
    if (home_secs)
    {
        double left = g_death.home_secs;
        if (left > 0)
        {
            left -= (double)(rt_monotonic_ns() - g_death.at) / 1e9;
            if (left < 0)
                left = 0;
        }
        *home_secs = left;
    }
    return g_death.dead;
}

static void self_status(const uint8_t* p, uint32_t size)
{
    if (size < 0x3C)
        return;
    self_death(p[0x30], size >= 0x40 ? p + 0x3C : NULL);
    uint32_t f0 = u32(p + 0x28), f1 = u32(p + 0x2C), f3 = u32(p + 0x38);
    g_self_marks.gm = (uint8_t)(f0 >> 29 & 7);
    g_self_marks.marks = (uint16_t)((g_self_marks.gm || (f1 >> 31 & 1) ? MARK_GM : 0) | (f3 >> 4 & 1 ? MARK_MENTOR : 0) |
                                    (f3 >> 3 & 1 ? MARK_NEW : 0) | (f0 >> 4 & 1 ? MARK_LFG : 0) | (f0 >> 7 & 1 ? MARK_AWAY : 0) |
                                    (f0 >> 5 & 1 ? MARK_ANON : 0) | (f1 >> 29 & 1 ? MARK_BAZAAR : 0) | (f0 >> 25 & 1 ? MARK_LS : 0));
    g_self_marks.ls = (uint32_t)p[0x31] << 16 | (uint32_t)p[0x32] << 8 | p[0x33];
}

/* --- the bags (the layouts as the MogHouse client reads them, FfxiInventory.cs) ----------------------- */
static GameSlot g_bag[BAGS][BAG_SLOTS];
static uint16_t g_bag_size[BAGS];
static struct
{
    uint8_t bag, slot, worn;
} g_equip[EQUIP_SLOTS];

static void bags(uint32_t id, const uint8_t* p, uint32_t size)
{
    switch (id)
    {
    case 0x01C: /* sizes: a byte each at 0x04, and a short each (when not 0, the one to use) at 0x24 */
        if (size >= 0x24 + 2 * BAGS)
            for (int i = 0; i < BAGS; ++i)
            {
                uint16_t wide = u16(p + 0x24 + 2 * i);
                g_bag_size[i] = wide ? wide : p[0x04 + i];
            }
        break;
    case 0x01F: /* an item in a slot: count 0x04, item 0x08, bag 0x0A, slot 0x0B, locked 0x0C */
    case 0x020: /* the same, with more: count 0x04, price 0x08, item 0x0C, bag 0x0E, slot 0x0F, locked 0x10 */
    {
        int wide = id == 0x020;
        if (size < (wide ? 0x11u : 0x0Du))
            break;
        uint8_t bag = p[wide ? 0x0E : 0x0A], slot = p[wide ? 0x0F : 0x0B];
        if (bag < BAGS && slot < BAG_SLOTS)
        {
            GameSlot* s = &g_bag[bag][slot];
            s->count = u32(p + 0x04), s->item = u16(p + (wide ? 0x0C : 0x08)), s->locked = p[wide ? 0x10 : 0x0C];
            if (!s->count)
                s->item = 0;
        }
        break;
    }
    case 0x01E: /* a count: 0x04, bag 0x08, slot 0x09, locked 0x0A */
        if (size >= 0x0B && p[0x08] < BAGS && p[0x09] < BAG_SLOTS)
        {
            GameSlot* s = &g_bag[p[0x08]][p[0x09]];
            s->count = u32(p + 0x04), s->locked = p[0x0A];
            if (!s->count)
                s->item = 0;
        }
        break;
    case 0x050: /* worn: the bag slot 0x04, the equipment slot 0x05, the bag 0x06 */
        if (size >= 0x07 && p[0x05] < EQUIP_SLOTS)
        {
            g_equip[p[0x05]].slot = p[0x04], g_equip[p[0x05]].bag = p[0x06], g_equip[p[0x05]].worn = p[0x04] != 255;
        }
        break;
    }
}

/* 0x061 (LandSandBoat's s2c/0x061_clistatus.h): max HP 0x04, max MP 0x08, jobs 0x0C-0x0F, experience
 * 0x10, the attributes' base 0x14 and what is added 0x22 (seven each), attack 0x30, defense 0x32,
 * resistances 0x34 (eight) */
static GameStats g_stats;

static void stats(const uint8_t* p, uint32_t size)
{
    if (size < 0x44)
        return;
    g_stats.hp_max = (int32_t)u32(p + 0x04), g_stats.mp_max = (int32_t)u32(p + 0x08);
    g_stats.mjob = p[0x0C], g_stats.mjob_lv = p[0x0D], g_stats.sjob = p[0x0E], g_stats.sjob_lv = p[0x0F];
    g_stats.exp_now = u16(p + 0x10), g_stats.exp_next = u16(p + 0x12);
    for (int i = 0; i < 7; ++i)
        g_stats.base[i] = u16(p + 0x14 + 2 * i), g_stats.add[i] = (int16_t)u16(p + 0x22 + 2 * i);
    g_stats.attack = (int16_t)u16(p + 0x30), g_stats.defense = (int16_t)u16(p + 0x32);
    for (int i = 0; i < 8; ++i)
        g_stats.resist[i] = (int16_t)u16(p + 0x34 + 2 * i);
    g_stats.known = 1;
}

const GameStats* gamestate_stats(void) { return &g_stats; }

int gamestate_self_vitals(uint32_t* hp, uint32_t* mp, uint32_t* tp)
{
    for (int i = 0; i < MEMBERS; ++i)
        if (g_members[i].id && g_members[i].id == g_self)
        {
            *hp = g_members[i].hp, *mp = g_members[i].mp, *tp = g_members[i].tp;
            return 1;
        }
    return 0;
}

int gamestate_bag_size(int bag) { return bag >= 0 && bag < BAGS ? g_bag_size[bag] : 0; }

const GameSlot* gamestate_slot(int bag, int slot)
{
    return bag >= 0 && bag < BAGS && slot >= 0 && slot < BAG_SLOTS && g_bag[bag][slot].item ? &g_bag[bag][slot] : NULL;
}

int gamestate_equipped(int equip_slot, int* bag, int* slot)
{
    if (equip_slot < 0 || equip_slot >= EQUIP_SLOTS || !g_equip[equip_slot].worn)
        return 0;
    *bag = g_equip[equip_slot].bag, *slot = g_equip[equip_slot].slot;
    return 1;
}

int gamestate_marks(const char* name, uint16_t* marks, uint8_t* gm, uint32_t* ls)
{
    if (!name || !name[0])
        return 0;
    for (int i = 0; i < MEMBERS; ++i)
        if (g_members[i].id && g_members[i].id == g_self && !strcmp(g_members[i].name, name))
        {
            *marks = g_self_marks.marks, *gm = g_self_marks.gm, *ls = g_self_marks.ls;
            return 1;
        }
    for (int i = 0; i < ENTITIES; ++i)
        if (g_ents[i].id && g_ents[i].kind == ENTITY_PC && !strcmp(g_ents[i].name, name))
        {
            *marks = g_ents[i].marks, *gm = g_ents[i].gm, *ls = g_ents[i].ls;
            return 1;
        }
    return 0;
}

int gamestate_entities(GameEntity* out, int max)
{
    int n = 0;
    for (int i = 0; i < ENTITIES && n < max; ++i)
        if (g_ents[i].id && g_ents[i].kind)
        {
            GameEntity* e = &out[n++];
            *e = g_ents[i];
            uint32_t p = entity_at((uint16_t)i, e->id);
            if (p) /* where the game draws it now, between the server's updates */
            {
                e->x = mem_f32(p + 0x04), e->y = mem_f32(p + 0x08), e->z = mem_f32(p + 0x0C);
                entity_name(p, e);
            }
        }
    return n;
}

/* The player's target. [target_ptr] is the game's target window, which holds who it shows; which
 * of its words that is was not known, so it is found: while something is targeted, the window's
 * words are compared with the ids and the entity pointers of who is around, and the offset that
 * keeps matching is kept (and logged). */
static int g_tgt_off = -1, g_tgt_is_ptr;

static uint32_t entity_ptr_of(uint16_t index)
{
    uint32_t slot = g_entity_map + 4u * index;
    return g_entity_map && index < ENTITIES && gwin_is_committed(slot) ? rd32(slot) : 0;
}

/* who a word of the window names: the index of the entity it is (the player's own index for
 * themselves), or -1 */
static int who_is(uint32_t v, int as_ptr)
{
    if (!v)
        return -1;
    if (as_ptr)
    {
        if (v == entity_ptr_of(g_self_index))
            return g_self_index;
        for (int i = 0; i < ENTITIES; ++i)
            if (g_ents[i].id && entity_ptr_of((uint16_t)i) == v)
                return i;
        return -1;
    }
    if (v == g_self)
        return g_self_index;
    uint16_t guess = (uint16_t)(v & 0xFFF); /* an NPC's id carries its index */
    if (guess < ENTITIES && g_ents[guess].id == v)
        return guess;
    for (int i = 0; i < ENTITIES; ++i)
        if (g_ents[i].id == v)
            return i;
    return -1;
}

int gamestate_targeting(void)
{
    if (!g_target_ptr || g_tgt_off < 0 || !gwin_is_committed(g_target_ptr))
        return 1;
    uint32_t t = rd32(g_target_ptr);
    if (!t || !gwin_is_committed(t + (uint32_t)g_tgt_off))
        return 0;
    return rd32(t + (uint32_t)g_tgt_off) != 0;
}

int gamestate_target(GameEntity* out, int* is_self)
{
    *is_self = 0;
    if (!g_target_ptr || !gwin_is_committed(g_target_ptr))
        return 0;
    uint32_t t = rd32(g_target_ptr);
    if (!t || !gwin_is_committed(t) || !gwin_is_committed(t + 0x200))
        return 0;
    if (g_tgt_off < 0)
    {
        static int hits[128][2], frame;
        if (++frame % 6)
            return 0;
        /* who is around, by id and by entity pointer (not the player: the window may name them
         * for another reason) */
        static uint32_t ids[ENTITIES], ptrs[ENTITIES];
        int n = 0;
        for (int i = 0; i < ENTITIES; ++i)
            if (g_ents[i].id)
                ids[n] = g_ents[i].id, ptrs[n] = entity_ptr_of((uint16_t)i), ++n;
        for (int o = 1; o < 128; ++o)
            for (int k = 0; k < 2; ++k)
            {
                uint32_t v = rd32(t + 4u * o);
                int found = 0;
                for (int i = 0; i < n && v && !found; ++i)
                    found = v == (k ? ptrs[i] : ids[i]);
                if (found)
                {
                    if (++hits[o][k] >= 10)
                    {
                        g_tgt_off = 4 * o, g_tgt_is_ptr = k;
                        extern void rt_log(const char* fmt, ...);
                        rt_log("[recomp] target: the target window holds its %s at +%02x\n", k ? "entity pointer" : "id", g_tgt_off);
                    }
                }
            }
        return 0;
    }
    int index = who_is(rd32(t + (uint32_t)g_tgt_off), g_tgt_is_ptr);
    if (index < 0)
        return 0;
    if (index == g_self_index)
    {
        memset(out, 0, sizeof *out);
        out->id = g_self, out->kind = ENTITY_PC, out->hpp = 100;
        float f;
        gamestate_self(&out->x, &out->y, &out->z, &f);
        for (int i = 0; i < MEMBERS; ++i)
            if (g_members[i].id == g_self)
                snprintf(out->name, sizeof out->name, "%s", g_members[i].name), out->hpp = g_members[i].hpp;
        *is_self = 1;
        return 1;
    }
    *out = g_ents[index];
    uint32_t p = entity_at((uint16_t)index, out->id);
    if (p)
    {
        out->x = mem_f32(p + 0x04), out->y = mem_f32(p + 0x08), out->z = mem_f32(p + 0x0C);
        entity_name(p, out);
    }
    return 1;
}

uint32_t gamestate_self_entity(void)
{
    return g_me.known ? entity_at(g_self_index, g_self) : 0;
}

int gamestate_self(float* x, float* y, float* z, float* facing)
{
    static const float STEP = 6.28318531f / 256.0f;
    *x = g_me.x, *y = g_me.y, *z = g_me.z, *facing = g_me.heading * STEP;
    uint32_t p = g_me.known ? entity_at(g_self_index, g_self) : 0;
    static int told;
    if (!p && g_me.known && g_entity_map && told < 1)
    {
        /* the entity is not where it was looked for: its first bytes, to learn the layout */
        ++told;
        extern void rt_log(const char* fmt, ...);
        uint32_t slot = g_entity_map + 4u * g_self_index, e = gwin_is_committed(slot) ? rd32(slot) : 0;
        rt_log("[recomp] map: no entity for index %u id %u (slot %08x -> %08x)\n", g_self_index, g_self, slot, e);
        if (e && gwin_is_committed(e) && gwin_is_committed(e + 0x100))
            for (uint32_t o = 0; o < 0xA0; o += 16)
                rt_log("[recomp]   +%02x: %08x %08x %08x %08x\n", o, rd32(e + o), rd32(e + o + 4), rd32(e + o + 8), rd32(e + o + 12));
    }
    if (!p)
        return g_me.known;
    *x = mem_f32(p + 0x04), *y = mem_f32(p + 0x08), *z = mem_f32(p + 0x0C);
    float yaw = mem_f32(p + 0x18);
    /* The memory's facing may count the other way, or from another direction, than the packets'
     * byte: each time a new byte comes, the eight ways it could be are scored against it, and the
     * best so far is used. */
    static float score[8];
    static int last_heading = -1, best;
    if (g_me.heading != last_heading)
    {
        last_heading = g_me.heading;
        float want = g_me.heading * STEP;
        for (int k = 0; k < 8; ++k)
        {
            float d = ((k & 1) ? -yaw : yaw) + (float)(k >> 1) * 1.57079633f - want;
            d = fmodf(d, 6.28318531f);
            if (d < 0)
                d += 6.28318531f;
            if (d > 3.14159265f)
                d = 6.28318531f - d;
            score[k] = score[k] * 0.9f + d;
        }
        for (int k = 1; k < 8; ++k)
            if (score[k] < score[best])
                best = k;
    }
    *facing = ((best & 1) ? -yaw : yaw) + (float)(best >> 1) * 1.57079633f;
    if (told < 6)
    {
        /* the memory's facing against the packets' byte, a few times: the two should agree */
        static uint32_t calls;
        if (++calls % 300 == 1)
        {
            ++told;
            extern void rt_log(const char* fmt, ...);
            rt_log("[recomp] map: self at %.1f %.1f %.1f (the packets' %.1f %.1f %.1f), facing %.3f (the packets' %u = %.3f, way %d)\n",
                *x, *y, *z, g_me.x, g_me.y, g_me.z, yaw, g_me.heading, g_me.heading * STEP, best);
        }
    }
    return 1;
}

/* 0x00A, zoning in: the player's id (and name, at 0x84), the zone */
static void zone_in(const uint8_t* p, uint32_t size)
{
    if (size < 0x94)
        return;
    g_self = u32(p + 0x04);
    g_zone = u16(p + 0x30);
    self_death(p[0x1F], NULL);
    zone_entities(p);
    want_map(g_zone, g_me.x, g_me.y, g_me.z);
    GameMember* m = member(g_self);
    if (m)
    {
        name_of(m->name, p + 0x84);
        m->zone = g_zone;
        extern void rt_log(const char* fmt, ...);
        rt_log("[recomp] zone in: zone %u, player %u \"%s\"\n", g_zone, g_self, m->name);
    }
}

/* 0x0DF: a member's (or the player's) HP, MP, TP, jobs */
static void group_attr(const uint8_t* p, uint32_t size)
{
    if (size < 0x24)
        return;
    GameMember* m = member(u32(p + 0x04));
    if (!m)
        return;
    m->hp = u32(p + 0x08), m->mp = u32(p + 0x0C), m->tp = u32(p + 0x10);
    m->hpp = p[0x16], m->mpp = p[0x17];
    m->zone = u16(p + 0x1A);
    m->mjob = p[0x20], m->mjob_lv = p[0x21], m->sjob = p[0x22], m->sjob_lv = p[0x23];
}

/* 0x0DD: a member, with their name and party */
static void group_list(const uint8_t* p, uint32_t size)
{
    if (size < 0x38)
        return;
    GameMember* m = member(u32(p + 0x04));
    if (!m)
        return;
    m->hp = u32(p + 0x08), m->mp = u32(p + 0x0C), m->tp = u32(p + 0x10);
    uint32_t attr = u32(p + 0x14);
    m->party = (uint8_t)(attr & 3), m->leader = (uint8_t)(attr >> 2 & 1);
    m->hpp = p[0x1D], m->mpp = p[0x1E];
    m->zone = u16(p + 0x20);
    m->mjob = p[0x22], m->mjob_lv = p[0x23], m->sjob = p[0x24], m->sjob_lv = p[0x25];
    name_of(m->name, p + 0x28);
}

/* 0x0C8: who is in the party and alliance now */
static void group_table(const uint8_t* p, uint32_t size)
{
    g_table = 1;
    memset(g_listed, 0, sizeof g_listed);
    for (int i = 0; i < MEMBERS && 0x08 + 12u * (uint32_t)(i + 1) <= size; ++i)
    {
        const uint8_t* e = p + 0x08 + 12 * i;
        uint32_t id = u32(e);
        g_listed[i] = id;
        GameMember* m = id ? member(id) : NULL;
        if (m)
        {
            m->party = (uint8_t)(e[6] & 3), m->leader = (uint8_t)(e[6] >> 2 & 1);
            m->zone = u16(e + 8);
        }
    }
}

int gamestate_members(GameMember* out, int max)
{
    int n = 0;
    /* the player first */
    for (int i = 0; i < MEMBERS && n < max; ++i)
        if (g_members[i].id && g_members[i].id == g_self)
            out[n++] = g_members[i];
    for (int i = 0; i < MEMBERS && n < max; ++i)
    {
        const GameMember* m = &g_members[i];
        if (!m->id || m->id == g_self || !m->name[0])
            continue;
        int listed = !g_table;
        for (int k = 0; k < MEMBERS && !listed; ++k)
            listed = g_listed[k] == m->id;
        if (listed)
            out[n++] = *m;
    }
    return n;
}

uint16_t gamestate_zone(void) { return g_zone; }

uint32_t gamestate_udp_packets(void) { return g_udp; }
uint32_t gamestate_packets(uint16_t id) { return g_by_id[id & 0x1FF]; }

int gamestate_chat(int n, int* kind, const char** sender, const char** text)
{
    if (n < 0 || n >= g_chat_count)
        return 0;
    int i = (g_chat_next - 1 - n + CHAT_LINES) % CHAT_LINES;
    *kind = g_chat[i].kind;
    *sender = g_chat[i].sender;
    *text = g_chat[i].text;
    return 1;
}

/* 0x0B5 from the client: the player's own chat line (kind, a spare byte, the text); the server does
 * not send it back to them */
static uint64_t g_out_log_until; /* gamestate_log_out: every outgoing packet logged until then */
void gamestate_log_out(double seconds)
{
    g_out_log_until = rt_monotonic_ns() + (uint64_t)(seconds * 1e9);
}

void gamestate_feed_out(const uint8_t* buf, uint32_t len)
{
    if (!buf || len <= HEADER)
        return;
    for (uint32_t at = HEADER; at + 4 <= len;)
    {
        uint32_t id = (buf[at] | buf[at + 1] << 8) & 0x1FF, size = 2u * (buf[at + 1] & 0xFEu);
        if (size < 4 || at + size > len)
            break;
        if (g_out_log_until && rt_monotonic_ns() < g_out_log_until && id != 0x015)
        {
            extern void rt_log(const char* fmt, ...);
            char hex[3 * 24 + 1] = "";
            for (uint32_t i = 4; i < size && i < 28; ++i)
                snprintf(hex + 3 * (i - 4), 4, "%02x ", buf[at + i]);
            rt_log("[recomp] out: %03x size %u: %s\n", id, size, hex);
        }
        if (id == 0x015 && size >= 0x18)
        {
            /* the player's position report: x, height, z, and facing at 0x14 */
            const uint8_t* p = buf + at;
            g_me.x = f32(p + 0x04), g_me.y = f32(p + 0x08), g_me.z = f32(p + 0x0C);
            g_me.heading = p[0x14];
            g_me.known = 1;
        }
        if (id == 0x05D && size >= 0x10)
        {
            /* an emote (LandSandBoat's GP_CLI_COMMAND_MOTION): its number at 0x0A, mode 0x0B, param
             * 0x0C, to the log (a few), for the server's own uses of them (a jump) */
            static int told;
            extern void rt_log(const char* fmt, ...);
            if (told++ < 20)
                rt_log("[recomp] emote: number %u, mode %u, param %u\n", buf[at + 0x0A], buf[at + 0x0B], u16(buf + at + 0x0C));
        }
        if (0 && id == 0x0B5 && size > 6) /* the game's log has the player's lines too */
        {
            int i = g_chat_next;
            g_chat[i].kind = buf[at + 4];
            strcpy(g_chat[i].sender, "You");
            plain(g_chat[i].text, sizeof g_chat[i].text, buf + at + 6, size - 6);
            g_chat_next = (i + 1) % CHAT_LINES;
            if (g_chat_count < CHAT_LINES)
                ++g_chat_count;
        }
        at += size;
    }
}

/* The auto-translate dictionary (the install's ROM/76/23.DAT): groups of phrases, each phrase under
 * the 4-byte key the chat text carries between two 0xFD bytes. A group: its key, its title (32
 * bytes) and name (32), how many phrases and their bytes; a phrase: its key, then a length and its
 * text (English groups, key 02 ..), or two (Japanese, 04 ..: the text and its reading). */
typedef struct
{
    uint32_t key;
    char* text;
} Phrase;
static Phrase* g_phrases;
static int g_nphrases;

static int phrase_cmp(const void* a, const void* b)
{
    uint32_t x = ((const Phrase*)a)->key, y = ((const Phrase*)b)->key;
    return x < y ? -1 : x > y;
}

static uint32_t key_of(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

int gamestate_load_autotranslate(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* d = n > 0 ? (uint8_t*)malloc((size_t)n) : NULL;
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n)
    {
        fclose(f);
        free(d);
        return 0;
    }
    fclose(f);
    int cap = 4096;
    g_phrases = (Phrase*)malloc(sizeof(Phrase) * (size_t)cap);
    for (long o = 0; o + 0x4C <= n;)
    {
        uint32_t count = (uint32_t)(d[o + 0x44] | d[o + 0x45] << 8 | d[o + 0x46] << 16 | (uint32_t)d[o + 0x47] << 24);
        uint32_t size = (uint32_t)(d[o + 0x48] | d[o + 0x49] << 8 | d[o + 0x4A] << 16 | (uint32_t)d[o + 0x4B] << 24);
        int two = d[o] == 0x04; /* Japanese: the text, then its reading */
        long p = o + 0x4C, end = p + (long)size;
        if (end > n)
            break;
        for (uint32_t i = 0; i < count && p + 5 <= end; ++i)
        {
            uint32_t key = key_of(d + p);
            uint8_t len = d[p + 4];
            const uint8_t* text = d + p + 5;
            p += 5 + len;
            if (two && p < end)
                p += 1 + d[p];
            if (g_nphrases == cap)
                g_phrases = (Phrase*)realloc(g_phrases, sizeof(Phrase) * (size_t)(cap *= 2));
            g_phrases[g_nphrases].key = key;
            g_phrases[g_nphrases].text = (char*)malloc((size_t)len + 1);
            memcpy(g_phrases[g_nphrases].text, text, len);
            g_phrases[g_nphrases].text[len] = 0;
            ++g_nphrases;
        }
        o = end;
    }
    free(d);
    qsort(g_phrases, (size_t)g_nphrases, sizeof(Phrase), phrase_cmp);
    return g_nphrases;
}

static void resolve_phrase_refs(void);

static const char* phrase(uint32_t key)
{
    resolve_phrase_refs();
    Phrase k = { key, NULL };
    const Phrase* p = g_phrases ? (const Phrase*)bsearch(&k, g_phrases, (size_t)g_nphrases, sizeof(Phrase), phrase_cmp) : NULL;
    return p ? p->text : NULL;
}

/* A d_msg file of the install (its name lists: areas, jobs, spells, abilities), by file id: the text
 * of entry i, or NULL. Its header (0x40): 0x0A encoded (each byte after the header inverted), 0x18
 * the header's size, 0x1C the table's (0: entries of one size, 0x20), 0x28 how many. The table: an
 * offset (past the table) and a length each. An entry: how many fields, then each one's offset and
 * kind; a text field four bytes of count and 24 of attributes, then the text. */
typedef struct
{
    unsigned file;
    uint8_t* body;
    uint32_t size, table, each, count;
} Dmsg;

static const char* dmsg_text(Dmsg* m, uint32_t i)
{
    if (!m->body && m->file)
    {
        char path[1024];
        unsigned id = m->file;
        m->file = 0; /* read once, whether it is there or not */
        FILE* f = zonemap_file_path(id, path, sizeof path) ? fopen(path, "rb") : NULL;
        if (!f)
            return NULL;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t* b = n > 0x40 ? (uint8_t*)malloc((size_t)n) : NULL;
        int ok = b && fread(b, 1, (size_t)n, f) == (size_t)n && !memcmp(b, "d_msg", 5);
        fclose(f);
        if (!ok)
        {
            free(b);
            return NULL;
        }
        uint32_t head = b[0x18] | b[0x19] << 8 | b[0x1A] << 16 | (uint32_t)b[0x1B] << 24;
        if (head >= (uint32_t)n)
        {
            free(b);
            return NULL;
        }
        m->table = b[0x1C] | b[0x1D] << 8 | b[0x1E] << 16 | (uint32_t)b[0x1F] << 24;
        m->each = b[0x20] | b[0x21] << 8 | b[0x22] << 16 | (uint32_t)b[0x23] << 24;
        m->count = b[0x28] | b[0x29] << 8 | b[0x2A] << 16 | (uint32_t)b[0x2B] << 24;
        if (b[0x0A])
            for (long k = head; k < n; ++k)
                b[k] = (uint8_t)~b[k];
        m->size = (uint32_t)n - head;
        m->body = (uint8_t*)malloc(m->size + 1);
        memcpy(m->body, b + head, m->size);
        m->body[m->size] = 0;
        free(b);
    }
    if (!m->body || i >= m->count)
        return NULL;
#define DM32(o) ((o) + 4 <= m->size ? (uint32_t)(m->body[o] | m->body[(o) + 1] << 8 | m->body[(o) + 2] << 16 | (uint32_t)m->body[(o) + 3] << 24) : 0u)
    uint32_t e = m->table ? m->table + DM32(8 * i) : m->each * i;
    uint32_t t = e + DM32(e + 4) + 28;
#undef DM32
    return t < m->size ? (const char*)m->body + t : NULL;
}

/* The dictionary's phrases that name something by its number in one of the game's lists ("@A F5":
 * area 0xF5, Lower Jeuno), by that list's name, once the install's files can be found. A phrase
 * whose name is not there (or is only ".") is left out of the search. */
static int g_refs_done;
static void resolve_phrase_refs(void)
{
    if (g_refs_done)
        return;
    char path[1024];
    if (!zonemap_file_path(55465, path, sizeof path))
        return; /* the install is not known yet */
    g_refs_done = 1;
    static Dmsg areas = { 55465 }, jobs = { 55467 }, spells = { 55702 }, abilities = { 55701 };
    int named = 0, dropped = 0;
    for (int i = 0; i < g_nphrases; ++i)
    {
        char* t = g_phrases[i].text;
        if (t[0] != '@' || !t[1] || !t[2])
            continue;
        Dmsg* m = t[1] == 'A' ? &areas : t[1] == 'J' ? &jobs : t[1] == 'C' ? &spells : t[1] == 'Y' ? &abilities : NULL;
        char* end;
        unsigned long n = strtoul(t + 2, &end, 16);
        const char* name = m && end != t + 2 ? dmsg_text(m, (uint32_t)n) : NULL;
        if (name && name[0] && strcmp(name, "."))
        {
            size_t len = strlen(name);
            char* copy = (char*)malloc(len + 1);
            memcpy(copy, name, len + 1);
            free(t);
            g_phrases[i].text = copy;
            ++named;
        }
        else
            t[0] = 0, ++dropped;
    }
    extern void rt_log(const char* fmt, ...);
    rt_log("[recomp] auto-translate: %d phrases named from the game's lists (areas, jobs, spells, abilities), %d without a name\n", named, dropped);
}

/* case aside: whether t starts with w at i */
static int at_word(const char* t, const char* w)
{
    for (; *w; ++t, ++w)
        if (!*t || tolower((unsigned char)*t) != tolower((unsigned char)*w))
            return 0;
    return 1;
}

int gamestate_autotranslate_find(const char* typed, uint32_t* keys, const char** texts, int max)
{
    int n = 0;
    if (!typed || !typed[0] || !g_phrases)
        return 0;
    resolve_phrase_refs();
    for (int pass = 0; pass < 2 && n < max; ++pass)
        for (int i = 0; i < g_nphrases && n < max; ++i)
        {
            const Phrase* p = &g_phrases[i];
            if ((p->key >> 24) != 0x02) /* English (04: Japanese) */
                continue;
            int starts = at_word(p->text, typed), within = 0;
            if (pass == 1 && !starts)
                for (const char* t = p->text + 1; *t && !within; ++t)
                    within = !isalnum((unsigned char)t[-1]) && at_word(t, typed); /* a word in it ("flower" is not "lower") */
            if (pass == 0 ? starts : within)
                keys[n] = p->key, texts[n] = p->text, ++n;
        }
    return n;
}

/* the log's text as plain ASCII: its colour and control codes (0x1E, 0x1F, 0x7F, each with a byte)
 * dropped, auto-translate phrases (0xFD ... 0xFD) as "[AT]", its two-byte characters as '?' */
static void log_text(char* out, size_t n, const uint8_t* in)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && i < 1024 && o + 5 < n;)
    {
        uint8_t c = in[i];
        if (c == 0x1E || c == 0x1F || c == 0x7F)
            i += in[i + 1] ? 2 : 1;
        else if (c == 0xFD)
        {
            /* an auto-translate phrase: its key between two 0xFD, shown in braces as the game does */
            const char* t = in[i + 1] && in[i + 2] && in[i + 3] && in[i + 4] && in[i + 5] == 0xFD ? phrase(key_of(in + i + 1)) : NULL;
            size_t j = i + 1;
            while (j < i + 8 && in[j] && in[j] != 0xFD)
                ++j;
            i = in[j] == 0xFD ? j + 1 : j;
            if (t)
                o += (size_t)snprintf(out + o, n - o, "{%s}", t);
            else
                memcpy(out + o, "[AT]", 4), o += 4;
            if (o >= n)
                o = n - 1;
        }
        else if ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC))
            out[o++] = '?', i += in[i + 1] ? 2 : 1;
        else
            out[o++] = c >= 0x20 && c < 0x7F ? (char)c : ' ', ++i;
    }
    out[o] = 0;
}

void gamestate_chat_line(uint32_t mode, const uint8_t* text)
{
    /* the game adds an NPC's line three times (its dialogue box and its log): the copies have
     * their third header byte set */
    if ((mode >> 16 & 0xFF) == 1)
        return;
    mode &= 0xFF;
    int i = g_chat_next;
    g_chat[i].kind = (int)mode;
    g_chat[i].sender[0] = 0;
    log_text(g_chat[i].text, sizeof g_chat[i].text, text);
    g_chat_next = (i + 1) % CHAT_LINES;
    if (g_chat_count < CHAT_LINES)
        ++g_chat_count;
}
