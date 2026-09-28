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

/* For the overlay */
uint32_t gamestate_udp_packets(void);
uint32_t gamestate_packets(uint16_t id); /* how many of this id have come */
/* the n-th latest chat line (0 the newest): its kind, sender and text; 0 when there is none */
int gamestate_chat(int n, int* kind, const char** sender, const char** text);

#ifdef __cplusplus
}
#endif
