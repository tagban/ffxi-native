/* What the game knows, read from its incoming packets (docs/OVERLAY.md, phase 2): the host's hook
 * at the end of the game's own decrypt-and-decompress (meta/builds.json "packet_in") hands each UDP
 * packet here, readable. Read only: nothing here sends, or changes what the game sees. */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One incoming UDP packet, decrypted and decompressed: a 28-byte header, then the server's packets
 * (each: 9 bits of id, 7 of size in 4-byte words, a sequence, then its data). */
void gamestate_feed(const uint8_t* buf, uint32_t len);
/* One outgoing UDP packet, in the clear (the same framing): the player's own chat. */
void gamestate_feed_out(const uint8_t* buf, uint32_t len);

/* A line the game's own chat log shows (host64: its add-a-line), with its mode. */
void gamestate_chat_line(uint32_t mode, const uint8_t* text);

/* The auto-translate dictionary, from the install (ROM/76/23.DAT): how many phrases, 0 if none. */
int gamestate_load_autotranslate(const char* path);
/* Auto-translate phrases for what the player typed (the game's Tab): the English ones that start with
 * it first, then those with it further in (letters' case aside), each in the dictionary's order;
 * up to max, how many. The key is what goes between the two 0xFD bytes (the high byte first). */
int gamestate_autotranslate_find(const char* typed, uint32_t* keys, const char** texts, int max);

/* For the overlay */
uint32_t gamestate_udp_packets(void);
uint32_t gamestate_packets(uint16_t id); /* how many of this id have come */
/* the n-th latest chat line (0 the newest): its mode (0-255, the game's chat mode), sender and
 * text; 0 when there is none */
int gamestate_chat(int n, int* kind, const char** sender, const char** text);

/* The party (and alliance): the player first, then who the party table lists. */
typedef struct
{
    uint32_t id;
    char name[16];
    uint32_t hp, mp, tp;
    uint8_t hpp, mpp;
    uint8_t mjob, mjob_lv, sjob, sjob_lv;
    uint16_t zone;
    uint8_t party; /* 0 the player's party, 1 and 2 the alliance's others */
    uint8_t leader;
} GameMember;
int gamestate_members(GameMember* out, int max);
/* the player's zone (0 before the first) */
uint16_t gamestate_zone(void);

/* Who is around (the server's 0x00D and 0x00E, as it sends them for what is in range): position
 * on the ground (x east, z north; y is height), facing (0-255: 0 east, 64 south, 128 west). */
enum { ENTITY_PC = 1, ENTITY_NPC = 2 };
typedef struct
{
    uint32_t id;
    uint16_t index;
    uint8_t kind;    /* ENTITY_PC, ENTITY_NPC (NPCs and monsters alike) */
    uint8_t heading;
    uint8_t hpp;
    uint8_t claimed; /* a monster someone has claimed */
    uint8_t mob;     /* a monster (only monsters carry the battle byte; kept once seen) */
    uint8_t hidden;  /* not drawn by the game: hidden, cutscene only, a trigger, a door or
                      * transport, a marker with no body */
    float x, y, z;
    char name[24];
    uint16_t marks;  /* MARK_*: what the game shows by a player's name */
    uint8_t gm;      /* GM level, 0 none */
    uint32_t ls;     /* their linkshell's color, 0xRRGGBB (with MARK_LS) */
    uint8_t ship;    /* a ship, boat or airship (its look's type 4) */
    float hitbox;    /* how big the game takes it to be (the entity's +0x208, gamestate_entities), 0 unknown */
} GameEntity;

enum
{
    MARK_GM = 1, MARK_MENTOR = 2, MARK_NEW = 4, MARK_LFG = 8, MARK_AWAY = 16, MARK_ANON = 32, MARK_BAZAAR = 64, MARK_LS = 128,
};
/* The player's bags and what they wear (the server's 0x01C sizes, 0x01F / 0x020 an item in a slot,
 * 0x01E its count, 0x050 an equipment slot's item): containers 0 inventory, 1 safe, 2 storage,
 * 3 temporary, 4 locker, 5 satchel, 6 sack, 7 case, 8 wardrobe, 9 safe 2, 10-16 wardrobes 2-8. */
enum { BAGS = 18, BAG_SLOTS = 81, EQUIP_SLOTS = 16 };
typedef struct
{
    uint16_t item; /* 0 empty */
    uint32_t count;
    uint8_t locked; /* equipped, or otherwise bound in place */
} GameSlot;
int gamestate_bag_size(int bag);
const GameSlot* gamestate_slot(int bag, int slot);
/* what is worn in an equipment slot (0 main, 1 sub, 2 range, 3 ammo, 4 head, 5 body, 6 hands, 7 legs,
 * 8 feet, 9 neck, 10 waist, 11-12 ears, 13-14 rings, 15 back): where it is kept; 0 when nothing */
int gamestate_equipped(int equip_slot, int* bag, int* slot);

/* The player's own stats (the server's 0x061): jobs, max HP and MP, the seven attributes (base, and
 * what gear and the rest add: STR DEX VIT AGI INT MND CHR), attack, defense, elemental resistances
 * (fire ice wind earth lightning water light dark). */
typedef struct
{
    int known;
    uint8_t mjob, mjob_lv, sjob, sjob_lv;
    int32_t hp_max, mp_max;
    uint16_t base[7];
    int16_t add[7];
    int16_t attack, defense;
    int16_t resist[8];
    uint16_t exp_now, exp_next;
} GameStats;
const GameStats* gamestate_stats(void);
/* the player's own HP, MP and TP now (from the party's 0x0DF / 0x0DD): 0 before known */
int gamestate_self_vitals(uint32_t* hp, uint32_t* mp, uint32_t* tp);

/* A player's marks by name (the player too, from their own status): 0 when unknown */
int gamestate_marks(const char* name, uint16_t* marks, uint8_t* gm, uint32_t* ls);
/* Whether the player is knocked out; home_secs: the seconds until the game sends them home itself,
 * counting down (-1 before the server has said) */
int gamestate_dead(double* home_secs);
int gamestate_entities(GameEntity* out, int max);
/* the player's own position and facing (radians, 0 east, growing clockwise as the heading byte
 * does): from the game's own entity each frame where the build's entity table is known, else their
 * movement packets. 0 before known. */
int gamestate_self(float* x, float* y, float* z, float* facing);
/* The player's entity in the game's memory (its position at +0x04, +0x08 the height, more negative
 * higher, +0x0C), 0 when not yet found */
uint32_t gamestate_self_entity(void);
/* The player's id (0x00A's), 0 before it */
uint32_t gamestate_self_id(void);
/* Whether the server has the player flying (its wallhack flag): the client keeps their height */
int gamestate_flying(void);
/* The zone's weather as the server last said (LandSandBoat's xi::Weather: 6 rain, 7 squall, 12 snow,
 * 14 thunder, 15 thunderstorms ...) */
int gamestate_weather(void);
/* The zone's look from MogHouse's server (!skyfx, at the end of its weather packet): sky (0 none,
 * 1 an aurora), world (0 as it is, 1 the zone in wireframe, 2 everything), filter (0 none, 1 grey,
 * 2 sepia, 3 a color, 4 inverted, 5 night vision), 0, the sky's r g b strength, the filter's r g b
 * amount (0-255). 1 when there is one. */
int gamestate_look(uint8_t out[12]);
/* Every packet the game sends (but its position reports) to the log for this long: learning them */
void gamestate_log_out(double seconds);
/* The game's table of entity pointers (meta/builds.json "entity_map"; 0 unknown): positions and
 * facing read from it every frame, so the map moves smoothly. Read only. */
void gamestate_set_entity_map(uint32_t addr);
/* The install, and where the game keeps its zone layouts' key table ("mzb_keys"): each zone's map
 * is made from its layout when the player zones in (zonemap.h). */
void gamestate_set_zone_files(const char* game_dir, uint32_t keys_addr);
/* Where the game keeps a pointer to the player's target ("target_ptr"); 0 unknown. */
void gamestate_set_target_ptr(uint32_t addr);
/* The player's target now (from the game's own), as an entity: 0 when none. is_self: themselves. */
int gamestate_target(GameEntity* out, int* is_self);
/* Whether the player has anything targeted at all (the game's target window naming someone):
 * 1 when it does, or when that cannot be told yet (so Enter is left to the game). */
int gamestate_targeting(void);

#ifdef __cplusplus
}
#endif
