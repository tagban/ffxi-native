/* gamestate.h: the server's packets, split and read. The layouts are the game protocol's; fields
 * here only as the overlay needs them. */
#include "gamestate.h"

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

static const char* phrase(uint32_t key)
{
    Phrase k = { key, NULL };
    const Phrase* p = g_phrases ? (const Phrase*)bsearch(&k, g_phrases, (size_t)g_nphrases, sizeof(Phrase), phrase_cmp) : NULL;
    return p ? p->text : NULL;
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
