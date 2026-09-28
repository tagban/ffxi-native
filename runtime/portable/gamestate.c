/* gamestate.h: the server's packets, split and read. The layouts are the game protocol's; fields
 * here only as the overlay needs them. */
#include "gamestate.h"

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
        at += size;
    }
}

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
void gamestate_feed_out(const uint8_t* buf, uint32_t len)
{
    if (!buf || len <= HEADER)
        return;
    for (uint32_t at = HEADER; at + 4 <= len;)
    {
        uint32_t id = (buf[at] | buf[at + 1] << 8) & 0x1FF, size = 2u * (buf[at + 1] & 0xFEu);
        if (size < 4 || at + size > len)
            break;
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
            size_t j = i + 1;
            while (j < i + 8 && in[j] && in[j] != 0xFD)
                ++j;
            i = in[j] == 0xFD ? j + 1 : j;
            memcpy(out + o, "[AT]", 4), o += 4;
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
    int i = g_chat_next;
    g_chat[i].kind = (int)mode;
    g_chat[i].sender[0] = 0;
    log_text(g_chat[i].text, sizeof g_chat[i].text, text);
    g_chat_next = (i + 1) % CHAT_LINES;
    if (g_chat_count < CHAT_LINES)
        ++g_chat_count;
}
