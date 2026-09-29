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
    float x, y, z;
    char name[24];
} GameEntity;
int gamestate_entities(GameEntity* out, int max);
/* the player's own position and facing (radians, 0 east, growing clockwise as the heading byte
 * does): from the game's own entity each frame where the build's entity table is known, else their
 * movement packets. 0 before known. */
int gamestate_self(float* x, float* y, float* z, float* facing);
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

#ifdef __cplusplus
}
#endif
