/* itemdat.h: the install's item DATs, as the MogHouse client documents them (docs/wiki/Items.md).
 *
 * Four files split the ids: general (file 73, from 0x0000), usable (74, 0x1000), armour (76,
 * 0x2800) and weapons (75, 0x4000). A record is at (id - first) * its size, every byte
 * stored rotated right by five bits. At 0 the id, flags, stack, type, targets, level, slots, races,
 * jobs (in the newer layout, 0x1400 a record, the flags are a word and the rest two bytes on);
 * after the file's fixed header a string table (a count, then an offset and flags each; each
 * string four bytes of count, 24 of attributes, then the text): 0 the name, 4 the description.
 * (A record is 0xC00 bytes in older installs, 0x1400 in newer: taken from the file's size.)
 * The icon at 0x280: a 32x32 8-bit image with a BGRA palette at 0x2BD, rows bottom-up. */
#include "itemdat.h"
#include "zonemap.h" /* zonemap_file_path: the install's file table */

#include <stdio.h>
#include <string.h>

#include <mutex>
#include <unordered_map>

namespace
{
const struct
{
    unsigned file;
    uint16_t first, count, header;
} FILES[] = {
    { 73, 0x0000, 4096, 0x18 }, /* general */
    { 74, 0x1000, 4096, 0x1C }, /* usable */
    { 76, 0x2800, 6144, 0x2C }, /* armour */
    { 75, 0x4000, 6656, 0x38 }, /* weapons */
};
const size_t RECORD_MAX = 0x1400; /* a record's size is the file's over its count: 0xC00 in older
                                   * installs, 0x1400 in this one */

std::mutex g_lock;
std::unordered_map<uint16_t, ItemInfo*> g_items; /* NULL: looked for, not there */

uint16_t u16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
uint32_t u32(const uint8_t* p) { return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); }

/* the game's text to plain: its element symbols (0xEF, then 0x1F-0x26) by name, the punctuation it
 * borrows from Japanese (0x81 and a byte) as ASCII, anything else not plain dropped */
void plain(char* out, size_t n, const uint8_t* in, size_t max)
{
    static const char* const ELEMENTS[] = { "Fire", "Ice", "Wind", "Earth", "Lightning", "Water", "Light", "Dark" };
    size_t o = 0;
    for (size_t i = 0; i < max && in[i] && o + 12 < n; ++i)
    {
        uint8_t c = in[i];
        if (c == 0xEF && i + 1 < max && in[i + 1] >= 0x1F && in[i + 1] <= 0x26)
        {
            o += (size_t)snprintf(out + o, n - o, "%s", ELEMENTS[in[++i] - 0x1F]);
            continue;
        }
        if (c == 0x81 && i + 1 < max)
        {
            uint8_t d = in[++i];
            const char* r = d == 0x60 ? "~" : d == 0x45 ? "*" : d == 0xCB || d == 0xCC ? "->" : "";
            o += (size_t)snprintf(out + o, n - o, "%s", r);
            continue;
        }
        if (c == '\n' || (c >= 0x20 && c < 0x7F))
            out[o++] = (char)c;
    }
    out[o] = 0;
}

ItemInfo* load(uint16_t id)
{
    for (const auto& f : FILES)
    {
        if (id < f.first || id >= f.first + f.count)
            continue;
        char path[1024];
        if (!zonemap_file_path(f.file, path, sizeof path))
            return NULL;
        FILE* fh = fopen(path, "rb");
        if (!fh)
            return NULL;
        fseek(fh, 0, SEEK_END);
        size_t RECORD = (size_t)ftell(fh) / f.count;
        if (RECORD < 0x400 || RECORD > RECORD_MAX)
        {
            fclose(fh);
            return NULL;
        }
        uint8_t r[RECORD_MAX];
        bool ok = fseek(fh, (long)((id - f.first) * RECORD), SEEK_SET) == 0 && fread(r, 1, RECORD, fh) == RECORD;
        fclose(fh);
        if (!ok)
            return NULL;
        for (size_t i = 0; i < RECORD; ++i)
            r[i] = (uint8_t)(r[i] >> 5 | r[i] << 3);
        if (u32(r) != id)
            return NULL;
        ItemInfo* it = new ItemInfo();
        memset(it, 0, sizeof *it);
        it->id = id;
        if (RECORD >= 0x1400)
        {
            /* the newer layout: the flags a word, the rest two bytes on */
            it->flags = u16(r + 0x04), it->stack = u16(r + 0x08), it->type = u16(r + 0x0A), it->targets = u16(r + 0x0E);
            if (it->type == ITEM_WEAPON || it->type == ITEM_ARMOR)
                it->level = u16(r + 0x10), it->slots = u16(r + 0x12), it->races = u16(r + 0x14), it->jobs = u32(r + 0x18);
        }
        else
        {
            it->flags = u16(r + 0x04), it->stack = u16(r + 0x06), it->type = u16(r + 0x08), it->targets = u16(r + 0x0A);
            if (it->type == ITEM_WEAPON || it->type == ITEM_ARMOR)
                it->level = u16(r + 0x0E), it->slots = u16(r + 0x10), it->races = u16(r + 0x12), it->jobs = u32(r + 0x14);
        }
        /* the strings: where the table is differs by file and by layout, so it is found by what it
         * looks like (a count, 1-8, then its first string just past the count's entries) */
        size_t table = f.header;
        for (size_t t = 0x10; t + 8 <= 0x80; t += 4)
        {
            uint32_t c = u32(r + t);
            if (c >= 1 && c <= 8 && u32(r + t + 4) == 4 + 8 * c)
            {
                table = t;
                break;
            }
        }
        uint32_t count = u32(r + table);
        for (uint32_t k = 0; k < count && k < 8; ++k)
        {
            size_t e = table + 4 + 8 * k;
            if (e + 8 > RECORD)
                break;
            size_t at = table + u32(r + e);
            if (at + 4 > RECORD)
                continue;
            size_t text = at + 4 + 24 * (size_t)u32(r + at);
            if (text >= RECORD)
                continue;
            if (k == 0)
                plain(it->name, sizeof it->name, r + text, RECORD - text);
            else if (k == 4)
                plain(it->desc, sizeof it->desc, r + text, RECORD - text);
        }
        /* the icon */
        if (u32(r + 0x280) >= 40 + 1024 + 32 * 32 && u32(r + 0x295) == 40 && u32(r + 0x299) == 32 && u32(r + 0x29D) == 32)
        {
            const uint8_t *pal = r + 0x2BD, *px = pal + 1024;
            for (int y = 0; y < 32; ++y)
                for (int x = 0; x < 32; ++x)
                {
                    const uint8_t* e = pal + 4 * px[(31 - y) * 32 + x];
                    uint8_t* o = it->icon + (y * 32 + x) * 4;
                    /* the palette's alpha counts 0x80 as solid, as the game's colors do */
                    o[0] = e[2], o[1] = e[1], o[2] = e[0], o[3] = (uint8_t)(e[3] >= 0x80 ? 255 : e[3] * 2);
                }
            it->has_icon = 1;
        }
        return it;
    }
    return NULL;
}
} // namespace

extern "C" const ItemInfo* item_info(uint16_t id)
{
    std::lock_guard<std::mutex> hold(g_lock);
    auto found = g_items.find(id);
    if (found != g_items.end())
        return found->second;
    ItemInfo* it = id ? load(id) : NULL;
    g_items[id] = it;
    return it;
}
