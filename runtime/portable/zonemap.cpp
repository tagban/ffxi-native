/* zonemap.h: a zone's walkable ground from its collision mesh, drawn from above.
 *
 * Adapted from the MogHouse client (XI Test Client, MIT, John Leighow): the DAT chunk walk
 * (renderer/ffxi/dat.cpp), MZB decryption and collision meshes (renderer/ffxi/mzb.cpp, format in
 * its docs/mzb-format.md), the install's file table (renderer/ffxi/filetable.cpp), and the walkable
 * test and raster (renderer/collision.cpp). Kept in the game's own frame: the server's positions
 * are in it too (x east, z north, y down), so nothing is turned. */
#include "zonemap.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

extern "C" void rt_log(const char* fmt, ...);

namespace
{
std::mutex g_lock;
std::string g_game;
uint8_t g_keys[256];
bool g_have_keys;
int g_wanted = -1;   /* the zone being made or made */
ZoneMap g_done = {};     /* the last one made, until taken */
ZoneMap g_done_art = {}; /* the game's map for it, when placed, until taken */

const int SIZE = 2048;
const int ZONE_FILE_OFFSET = 100; /* a zone's layout is file id zone + 100 */
const float WALKABLE_NORMAL_Y = 0.64f; /* steeper is a wall: about 50 degrees */
const float HUGE_FLOOR = 4000.0f;       /* square units, seen from above */

bool read_file(const std::string& path, std::vector<uint8_t>& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t)n : 0);
    bool ok = n > 0 && fread(out.data(), 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    return ok;
}

/* The install's index: VTABLE.DAT (the ROM number per file id, 0 not installed) and FTABLE.DAT
 * ((folder << 7) | file), then each expansion's pair over it, later ones winning. */
std::string file_path(size_t id)
{
    std::vector<uint8_t> v, f;
    if (!read_file(g_game + "/VTABLE.DAT", v) || !read_file(g_game + "/FTABLE.DAT", f) || f.size() != v.size() * 2)
        return "";
    for (int rom = 2; rom <= 9; ++rom)
    {
        std::vector<uint8_t> v2, f2;
        std::string dir = g_game + "/ROM" + std::to_string(rom) + "/";
        if (!read_file(dir + "VTABLE" + std::to_string(rom) + ".DAT", v2) ||
            !read_file(dir + "FTABLE" + std::to_string(rom) + ".DAT", f2) || v2.size() != v.size() || f2.size() != f.size())
            continue;
        if (id < v2.size() && v2[id])
            v[id] = v2[id], f[id * 2] = f2[id * 2], f[id * 2 + 1] = f2[id * 2 + 1];
    }
    if (id >= v.size() || !v[id])
        return "";
    unsigned packed = f[id * 2] | f[id * 2 + 1] << 8;
    std::string rom = v[id] == 1 ? "ROM" : "ROM" + std::to_string(v[id]);
    return g_game + "/" + rom + "/" + std::to_string(packed >> 7) + "/" + std::to_string(packed & 0x7F) + ".DAT";
}

template <typename T> bool rd(const std::vector<uint8_t>& b, size_t at, T& v)
{
    if (at + sizeof(T) > b.size())
        return false;
    memcpy(&v, b.data() + at, sizeof(T));
    return true;
}

/* The MZB's run-based obscuring (MogHouse docs/mzb-format.md): from offset 8, runs of 16-23 bytes,
 * the odd-keyed ones XORed with 0xFF. */
bool decrypt(std::vector<uint8_t>& b)
{
    if (b.size() < 8 || b[3] < 0x1B)
        return true;
    uint32_t length;
    rd(b, 0, length);
    length &= 0x00FFFFFF;
    if (length > b.size())
        return false;
    uint32_t key = g_keys[b[7] ^ 0xFF], counter = 0;
    for (uint32_t pos = 8; pos < length;)
    {
        uint32_t run = ((key >> 4) & 7) + 16;
        if ((key & 1) && pos + run < length)
            for (uint32_t i = 0; i < run; ++i)
                b[pos + i] ^= 0xFF;
        key += ++counter;
        pos += run;
    }
    return true;
}

struct Tri
{
    float ax, ay, az, bx, by, bz, cx, cy, cz;
};

/* One MZB's walkable triangles, in the world: its collision meshes (model space, reused), placed
 * by the spatial grid's (transform, mesh) pairs. */
void walkable(std::vector<uint8_t> b, std::vector<Tri>& out)
{
    if (!decrypt(b))
        return;
    uint32_t table = 0, count = 0, entry = 0, grid = 0;
    if (!rd(b, 8, table) || !table || !rd(b, table, count) || !rd(b, table + 4, entry) || !rd(b, table + 0x10, grid))
        return;
    struct Mesh
    {
        std::vector<float> v;
        std::vector<uint16_t> idx;
    };
    std::vector<Mesh> meshes;
    std::unordered_map<uint32_t, uint32_t> by_offset;
    for (uint32_t i = 0; i < count; ++i)
    {
        uint32_t vo, no, to;
        uint16_t tc;
        if (!rd(b, entry, vo) || !rd(b, entry + 4, no) || !rd(b, entry + 8, to) || !rd(b, entry + 12, tc) || no < vo || to < no ||
            to + (size_t)tc * 8 > b.size())
            return;
        by_offset[entry] = (uint32_t)meshes.size();
        Mesh m;
        m.v.resize((no - vo) / 4);
        memcpy(m.v.data(), b.data() + vo, m.v.size() * 4);
        for (uint16_t t = 0; t < tc; ++t)
            for (int k = 0; k < 3; ++k)
            {
                uint16_t ix;
                rd(b, to + (size_t)t * 8 + k * 2, ix);
                m.idx.push_back(ix & 0x3FFF);
            }
        meshes.push_back(std::move(m));
        entry = to + (uint32_t)tc * 8;
    }
    if (!grid || b.size() < 16)
        return;
    int across = b[12] * b[14] / 4, down = b[13] * b[15] / 4;
    for (int y = 0; y < down; ++y)
        for (int x = 0; x < across; ++x)
        {
            uint32_t list, n;
            if (!rd(b, grid + (size_t)(y * across + x) * 4, list) || !list || !rd(b, list, n))
                continue;
            n &= 0x3FFF;
            for (uint32_t i = 0; i < n; ++i)
            {
                uint32_t place, mesh;
                if (!rd(b, list + 4 + i * 8, place) || !rd(b, list + 8 + i * 8, mesh))
                    continue;
                auto found = by_offset.find(mesh);
                float m[16];
                if (found == by_offset.end() || place + 64 > b.size())
                    continue;
                memcpy(m, b.data() + place, 64);
                const Mesh& me = meshes[found->second];
                size_t nv = me.v.size() / 3;
                for (size_t t = 0; t + 2 < me.idx.size(); t += 3)
                {
                    uint16_t ia = me.idx[t], ib = me.idx[t + 1], ic = me.idx[t + 2];
                    if (ia >= nv || ib >= nv || ic >= nv)
                        continue;
                    float p[3][3];
                    const uint16_t c[3] = { ia, ib, ic };
                    for (int k = 0; k < 3; ++k)
                    {
                        const float* s = &me.v[c[k] * 3];
                        p[k][0] = m[0] * s[0] + m[4] * s[1] + m[8] * s[2] + m[12];
                        p[k][1] = m[1] * s[0] + m[5] * s[1] + m[9] * s[2] + m[13];
                        p[k][2] = m[2] * s[0] + m[6] * s[1] + m[10] * s[2] + m[14];
                    }
                    /* the face's normal: a floor if near level (the winding is not consistent,
                     * so either way up) */
                    float ux = p[1][0] - p[0][0], uy = p[1][1] - p[0][1], uz = p[1][2] - p[0][2];
                    float vx = p[2][0] - p[0][0], vy = p[2][1] - p[0][1], vz = p[2][2] - p[0][2];
                    float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
                    float len = sqrtf(nx * nx + ny * ny + nz * nz);
                    if (len < 1e-9f || fabsf(ny / len) < WALKABLE_NORMAL_Y)
                        continue;
                    /* a single floor bigger than a city block is a plane under the zone that
                     * catches what falls, not ground anyone walks (Bastok Markets has several) */
                    if (fabsf(ny) * 0.5f > HUGE_FLOOR)
                        continue;
                    out.push_back(Tri{ p[0][0], p[0][1], p[0][2], p[1][0], p[1][1], p[1][2], p[2][0], p[2][1], p[2][2] });
                }
            }
        }
}


/* --- the game's own maps -------------------------------------------------------------------------
 * Each is a DAT holding a texture named "menumap m_<zone>_<map>" (512 x 512: 8-bit with a palette,
 * rows bottom-up (0x81, 0xB1), or DXT1/DXT3 blocks top-down (0xA1)), and a 0x31 chunk whose quad puts the
 * world's origin on it: its first corner is minus the pixel the origin falls on; a byte after it
 * says whether it is a field's map. The scale is not stored: fields are 0.2 pixels a yalm and the
 * rest 0.8, checked by fitting the zone's edges from its collision against the map's ink. With
 * several maps (floors), the one that fits best is taken. */
struct MapFile
{
    int map;
    std::string path;
};
std::unordered_map<int, std::vector<MapFile>> g_map_files;
bool g_map_files_ready;

/* every map in the install, once: the name is in the first chunk's header */
void index_maps()
{
    if (g_map_files_ready)
        return;
    g_map_files_ready = true;
    for (int rom = 1; rom <= 9; ++rom)
    {
        std::string base = g_game + (rom == 1 ? "/ROM" : "/ROM" + std::to_string(rom));
        for (int dir = 0; dir < 1000; ++dir)
        {
            std::string d = base + "/" + std::to_string(dir) + "/";
            int misses = 0;
            for (int f = 0; f < 128 && misses < 8; ++f)
            {
                FILE* fh = fopen((d + std::to_string(f) + ".DAT").c_str(), "rb");
                if (!fh)
                {
                    ++misses;
                    continue;
                }
                misses = 0;
                char h[0x60] = { 0 };
                size_t n = fread(h, 1, sizeof h, fh);
                fclose(fh);
                for (size_t i = 0; i + 16 <= n; ++i)
                    if (!memcmp(h + i, "menumap m_", 10))
                    {
                        int zone = 0, map = 0;
                        if (sscanf(h + i + 10, "%d_%x", &zone, &map) == 2)
                            g_map_files[zone].push_back(MapFile{ map, d + std::to_string(f) + ".DAT" });
                        break;
                    }
            }
            if (misses >= 8 && dir > 200)
                break;
        }
    }
    size_t n = 0;
    for (auto& z : g_map_files)
        n += z.second.size();
    rt_log("[recomp] map: the install has %zu maps of %zu zones\n", n, g_map_files.size());
}

/* one 4x4 DXT colour block (the second half of a DXT3 block, or a DXT1 block) into 16 RGBA texels */
void dxt_colors(const uint8_t* b, uint8_t out[16][4], bool dxt1)
{
    uint16_t c0 = (uint16_t)(b[0] | b[1] << 8), c1 = (uint16_t)(b[2] | b[3] << 8);
    uint8_t pal[4][4];
    auto rgb = [](uint16_t c, uint8_t* o) {
        o[0] = (uint8_t)((c >> 11 & 31) * 255 / 31), o[1] = (uint8_t)((c >> 5 & 63) * 255 / 63), o[2] = (uint8_t)((c & 31) * 255 / 31), o[3] = 255;
    };
    rgb(c0, pal[0]);
    rgb(c1, pal[1]);
    for (int k = 0; k < 3; ++k)
        if (!dxt1 || c0 > c1)
            pal[2][k] = (uint8_t)((2 * pal[0][k] + pal[1][k]) / 3), pal[3][k] = (uint8_t)((pal[0][k] + 2 * pal[1][k]) / 3);
        else
            pal[2][k] = (uint8_t)((pal[0][k] + pal[1][k]) / 2), pal[3][k] = 0;
    pal[2][3] = 255, pal[3][3] = dxt1 && c0 <= c1 ? 0 : 255;
    for (int i = 0; i < 16; ++i)
        memcpy(out[i], pal[b[4 + i / 4] >> (2 * (i % 4)) & 3], 4);
}

/* a map file: its image (512 x 512 RGBA, row 0 the top) and the pixel the world's origin is on */
bool read_map(const std::string& path, std::vector<uint8_t>& rgba, float& ox, float& oy, int& field)
{
    std::vector<uint8_t> d;
    if (!read_file(path, d))
        return false;
    bool have_image = false, have_origin = false;
    for (size_t at = 0; at + 16 <= d.size();)
    {
        uint32_t packed;
        memcpy(&packed, d.data() + at + 4, 4);
        size_t len = (size_t)((packed >> 7) & 0x7FFFF) * 16;
        if (len < 16 || at + len > d.size())
            break;
        const uint8_t* c = d.data() + at + 16;
        size_t n = len - 16;
        uint32_t type = packed & 0x7F;
        int32_t w = 0, h = 0;
        if (type == 0x20 && n > 0x45)
        {
            memcpy(&w, c + 0x15, 4);
            memcpy(&h, c + 0x19, 4);
        }
        if (type == 0x20 && w == 512 && h == 512)
        {
            rgba.assign(512 * 512 * 4, 0);
            if ((c[0] == 0xB1 || c[0] == 0x81) && n >= 0x39 + 1024 + 512 * 512)
            {
                const uint8_t *pal = c + 0x39, *px = pal + 1024;
                for (int y = 0; y < 512; ++y)
                    for (int x = 0; x < 512; ++x)
                    {
                        const uint8_t* e = pal + 4 * px[(511 - y) * 512 + x];
                        uint8_t* o = &rgba[((size_t)y * 512 + x) * 4];
                        o[0] = e[2], o[1] = e[1], o[2] = e[0], o[3] = 255;
                    }
                have_image = true;
            }
            else if (c[0] == 0xA1 && n >= 0x45 && (c[0x39] == '3' || c[0x39] == '1'))
            {
                bool dxt1 = c[0x39] == '1';
                size_t bsize = dxt1 ? 8 : 16;
                if (n >= 0x45 + 128 * 128 * bsize)
                {
                    const uint8_t* blocks = c + 0x45;
                    uint8_t t[16][4];
                    for (int by = 0; by < 128; ++by)
                        for (int bx = 0; bx < 128; ++bx)
                        {
                            const uint8_t* b = blocks + ((size_t)by * 128 + bx) * bsize;
                            dxt_colors(dxt1 ? b : b + 8, t, dxt1);
                            for (int i = 0; i < 16; ++i)
                            {
                                /* the blocks run top-down, unlike the 8-bit images' rows */
                                int x = bx * 4 + i % 4, y = by * 4 + i / 4;
                                uint8_t* o = &rgba[((size_t)y * 512 + x) * 4];
                                memcpy(o, t[i], 4);
                                if (!dxt1)
                                    o[3] = (uint8_t)((b[i / 2] >> (4 * (i & 1)) & 15) * 17);
                            }
                        }
                    have_image = true;
                }
            }
        }
        else if (type == 0x31 && n >= 44)
        {
            /* the name (16), 1, the name again (16), 1 0 1, then the quad's corners (shorts) */
            int16_t x0, y0;
            memcpy(&x0, c + 36, 2);
            memcpy(&y0, c + 38, 2);
            /* 6 pixels right of it, on every map checked (Bastok Markets, Port Jeuno, East
             * Ronfaure): the frame the game draws the quad in, it seems */
            ox = (float)-x0 + 6.0f, oy = (float)-y0;
            field = n > 58 && c[58] == 0; /* 0: a field's map (zones 100-127), drawn smaller */
            have_origin = true;
        }
        at += len;
    }
    return have_image && have_origin;
}

/* distance to the nearest marked texel of a 512 x 512 mask, capped (two passes) */
void distances(std::vector<float>& dt)
{
    for (int y = 0; y < 512; ++y)
        for (int x = 0; x < 512; ++x)
        {
            float& v = dt[y * 512 + x];
            if (x) v = std::min(v, dt[y * 512 + x - 1] + 1);
            if (y) v = std::min(v, dt[(y - 1) * 512 + x] + 1);
            if (x && y) v = std::min(v, dt[(y - 1) * 512 + x - 1] + 1.4f);
            if (y && x < 511) v = std::min(v, dt[(y - 1) * 512 + x + 1] + 1.4f);
        }
    for (int y = 511; y >= 0; --y)
        for (int x = 511; x >= 0; --x)
        {
            float& v = dt[y * 512 + x];
            if (x < 511) v = std::min(v, dt[y * 512 + x + 1] + 1);
            if (y < 511) v = std::min(v, dt[(y + 1) * 512 + x] + 1);
            if (x < 511 && y < 511) v = std::min(v, dt[(y + 1) * 512 + x + 1] + 1.4f);
            if (y < 511 && x) v = std::min(v, dt[(y + 1) * 512 + x - 1] + 1.4f);
        }
}

/* How well a map sits on the zone at (scale, ox, oy): the zone's edges (world points) to the map's
 * ink, and the ink to the edges, each capped; lower is better */
struct Fit
{
    const std::vector<std::pair<float, float>>* edges;
    std::vector<float> ink_dt;  /* distance to the map's ink */
    std::vector<int> ink;       /* a sample of ink texels */
    float score(float s, float ox, float oy) const
    {
        const float CAP = 12.0f;
        double forward = 0;
        for (auto& p : *edges)
        {
            int x = (int)(ox + s * p.first), y = (int)(oy - s * p.second);
            forward += x < 0 || y < 0 || x >= 512 || y >= 512 ? CAP : std::min(ink_dt[y * 512 + x], CAP);
        }
        std::vector<float> e(512 * 512, 1e9f);
        for (auto& p : *edges)
        {
            int x = (int)(ox + s * p.first), y = (int)(oy - s * p.second);
            if (x >= 0 && y >= 0 && x < 512 && y < 512)
                e[y * 512 + x] = 0;
        }
        distances(e);
        double back = 0;
        for (int q : ink)
            back += std::min(e[q], CAP);
        return (float)(forward / std::max<size_t>(1, edges->size()) + back / std::max<size_t>(1, ink.size()));
    }
};

/* the zone's best-placed map of the game's: its image, and where the world lands on it */
bool place_art(int zone, const std::vector<std::pair<float, float>>& edges, std::vector<uint8_t>& best_art, float& bs, float& bx,
    float& by)
{
    index_maps();
    auto found = g_map_files.find(zone);
    if (found == g_map_files.end() || edges.size() < 50)
        return false;
    float best = 1e9f;
    for (const MapFile& mf : found->second)
    {
        std::vector<uint8_t> art;
        float ox = 0, oy = 0;
        int field = 0;
        if (!read_map(mf.path, art, ox, oy, field))
            continue;
        /* the ink: texels clearly darker than those around them */
        std::vector<float> lum(512 * 512);
        for (int i = 0; i < 512 * 512; ++i)
            lum[i] = 0.3f * art[i * 4] + 0.59f * art[i * 4 + 1] + 0.11f * art[i * 4 + 2];
        Fit fit;
        fit.edges = &edges;
        fit.ink_dt.assign(512 * 512, 1e9f);
        std::vector<int> all_ink;
        for (int y = 2; y < 510; ++y)
            for (int x = 2; x < 510; ++x)
            {
                float m = 0;
                for (int dy = -2; dy <= 2; ++dy)
                    for (int dx = -2; dx <= 2; ++dx)
                        m += lum[(y + dy) * 512 + x + dx];
                m /= 25;
                if (lum[y * 512 + x] < m - 25 && lum[y * 512 + x] < 140)
                    fit.ink_dt[y * 512 + x] = 0, all_ink.push_back(y * 512 + x);
            }
        if (all_ink.size() < 200)
            continue;
        distances(fit.ink_dt);
        for (size_t i = 0; i < all_ink.size(); i += all_ink.size() / 3000 + 1)
            fit.ink.push_back(all_ink[i]);
        /* The scale: a field's map is 0.2 pixels a yalm, the rest 0.8 (every one checked: Bastok
         * Markets, Port Bastok, Port Jeuno, East Ronfaure). Another round scale is taken only if it
         * fits clearly better (a dungeon drawn at its own), since a zone with much scenery around it
         * can make a wrong one look a little better. */
        float s_rule = field ? 0.2f : 0.8f, s_best = s_rule, v_best = fit.score(s_rule, ox, oy);
        for (float s : { 0.2f, 0.25f, 0.4f, 0.5f, 0.6f, 0.8f, 1.0f, 1.2f, 1.6f })
        {
            float v = fit.score(s, ox, oy);
            if (getenv("ZONEMAP_FIT_DEBUG"))
                rt_log("  scale %.2f: %.2f\n", s, v);
            if (s != s_rule && v < v_best * 0.75f)
                v_best = v, s_best = s;
        }
        /* the origin a pixel or two either way, where the fit is good enough to say */
        float fx = ox, fy = oy;
        if (v_best < 14.0f)
            for (float dx = -4; dx <= 4; dx += 2)
                for (float dy = -4; dy <= 4; dy += 2)
                {
                    float v = fit.score(s_best, ox + dx, oy + dy);
                    if (v < v_best)
                        v_best = v, fx = ox + dx, fy = oy + dy;
                }
        rt_log("[recomp] map: zone %d's map %02x (%s): scale %.3f, origin at %.0f,%.0f (the file's %.0f,%.0f), fit %.2f\n", zone, mf.map,
            mf.path.c_str() + g_game.size() + 1, s_best, fx, fy, ox, oy, v_best);
        if (v_best < best)
            best = v_best, best_art.swap(art), bs = s_best, bx = fx, by = fy;
    }
    return best < 22.0f; /* a fit this poor is a map of somewhere else (a floor below, say) */
}

/* From above: each texel the highest floor over it (y points down, so the least y). Then the
 * ground the player can reach from where they stand: out from there, texel to texel, across no
 * more than a step up or down. That is the map; the rest (the planes under a city that catch what
 * falls, rooftops, scenery past the edge of the world) stays, dimmed. In greys, for the overlay to
 * tint with the colors the player picks: walkable ground lighter the higher it is, with a paper's
 * grain, and its edges drawn dark. */
void make(int zone, float me_x, float me_y, float me_z)
{
    std::string path = file_path((size_t)zone + ZONE_FILE_OFFSET);
    std::vector<uint8_t> dat;
    if (path.empty() || !read_file(path, dat))
    {
        rt_log("[recomp] map: zone %d has no layout file installed\n", zone);
        return;
    }
    std::vector<Tri> tris;
    /* the DAT's chunks: 16-byte headers, (type:7, length/16:19); every MZB (type 0x1C) in it (the
     * ferries carry the vessel's as a second one) */
    for (size_t at = 0; at + 16 <= dat.size();)
    {
        uint32_t packed;
        memcpy(&packed, dat.data() + at + 4, 4);
        size_t len = (size_t)((packed >> 7) & 0x7FFFF) * 16;
        if (len < 16 || at + len > dat.size())
            break;
        if ((packed & 0x7F) == 0x1C)
            walkable(std::vector<uint8_t>(dat.begin() + (long)at + 16, dat.begin() + (long)(at + len)), tris);
        at += len;
    }
    if (tris.empty())
    {
        rt_log("[recomp] map: zone %d: no walkable ground in %s\n", zone, path.c_str());
        return;
    }
    float lo_x = 1e30f, hi_x = -1e30f, lo_z = 1e30f, hi_z = -1e30f;
    for (const Tri& t : tris)
        for (float x : { t.ax, t.bx, t.cx })
            lo_x = std::min(lo_x, x), hi_x = std::max(hi_x, x);
    for (const Tri& t : tris)
        for (float z : { t.az, t.bz, t.cz })
            lo_z = std::min(lo_z, z), hi_z = std::max(hi_z, z);
    float half = std::max(hi_x - lo_x, hi_z - lo_z) * 0.5f + 4.0f;
    float cx = (lo_x + hi_x) * 0.5f, cz = (lo_z + hi_z) * 0.5f;
    float scale = SIZE / (half * 2.0f), ox = cx - half, oz = cz + half; /* row 0 the north edge */

    std::vector<float> top((size_t)SIZE * SIZE, 1e30f);
    for (const Tri& t : tris)
    {
        float ax = (t.ax - ox) * scale, az = (oz - t.az) * scale;
        float bx = (t.bx - ox) * scale, bz = (oz - t.bz) * scale;
        float qx = (t.cx - ox) * scale, qz = (oz - t.cz) * scale;
        int x0 = std::max(0, (int)floorf(std::min({ ax, bx, qx }))), x1 = std::min(SIZE - 1, (int)ceilf(std::max({ ax, bx, qx })));
        int z0 = std::max(0, (int)floorf(std::min({ az, bz, qz }))), z1 = std::min(SIZE - 1, (int)ceilf(std::max({ az, bz, qz })));
        float area = (bz - az) * (qx - ax) - (bx - ax) * (qz - az);
        if (fabsf(area) < 1e-9f)
            continue;
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x)
            {
                float px = x + 0.5f, pz = z + 0.5f;
                float w0 = ((bz - pz) * (qx - px) - (bx - px) * (qz - pz)) / area;
                float w1 = ((qz - pz) * (ax - px) - (qx - px) * (az - pz)) / area;
                float w2 = 1.0f - w0 - w1;
                if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f)
                    continue;
                float y = w0 * t.ay + w1 * t.by + w2 * t.cy;
                float& here = top[(size_t)z * SIZE + x];
                here = std::min(here, y);
            }
    }
    /* the height range the shading spans: the middle 96% of the ground, so a tower or a pit
     * does not flatten the rest */
    std::vector<float> ys;
    for (size_t i = 0; i < top.size(); i += 7)
        if (top[i] < 1e29f)
            ys.push_back(top[i]);
    std::sort(ys.begin(), ys.end());
    float y_hi = ys.empty() ? 0 : ys[ys.size() * 2 / 100], y_lo = ys.empty() ? 1 : ys[ys.size() * 98 / 100];
    if (y_lo - y_hi < 1.0f)
        y_lo = y_hi + 1.0f;

    /* where the player stands: the texel under them whose floor is at their height, or the nearest
     * one that is */
    std::vector<uint8_t> reached((size_t)SIZE * SIZE, 0);
    const float STEP = 1.2f;
    int sx = (int)((me_x - ox) * scale), sz = (int)((oz - me_z) * scale), start = -1;
    for (int pass = 0; pass < 2 && start < 0; ++pass) /* at their height; else the nearest floor */
        for (int rad = 0; rad <= 40 && start < 0; ++rad)
            for (int dz = -rad; dz <= rad && start < 0; ++dz)
                for (int dx = -rad; dx <= rad && start < 0; ++dx)
                {
                    if (std::max(abs(dx), abs(dz)) != rad)
                        continue;
                    int x = sx + dx, z = sz + dz;
                    if (x < 0 || z < 0 || x >= SIZE || z >= SIZE)
                        continue;
                    float y = top[(size_t)z * SIZE + x];
                    if (y < 1e29f && (pass || fabsf(y - me_y) < 4.0f))
                        start = z * SIZE + x;
                }
    size_t reach_count = 0;
    if (start >= 0)
    {
        std::vector<int> queue;
        queue.push_back(start);
        reached[start] = 1;
        while (!queue.empty())
        {
            int at = queue.back();
            queue.pop_back();
            ++reach_count;
            int x = at % SIZE, z = at / SIZE;
            float y = top[at];
            for (int k = 0; k < 4; ++k)
            {
                int nx = x + (k == 0) - (k == 1), nz = z + (k == 2) - (k == 3);
                if (nx < 0 || nz < 0 || nx >= SIZE || nz >= SIZE)
                    continue;
                int n = nz * SIZE + nx;
                if (!reached[n] && top[n] < 1e29f && fabsf(top[n] - y) <= STEP)
                    reached[n] = 1, queue.push_back(n);
            }
        }
    }
    bool dim_rest = reach_count > 2000; /* a start that found only a ledge: show it all */

    uint8_t* rgba = (uint8_t*)calloc((size_t)SIZE * SIZE, 4);
    if (!rgba)
        return;
    for (int z = 0; z < SIZE; ++z)
        for (int x = 0; x < SIZE; ++x)
        {
            float y = top[(size_t)z * SIZE + x];
            if (y > 1e29f)
                continue;
            uint8_t* o = rgba + ((size_t)z * SIZE + x) * 4;
            /* an edge: next to where you cannot walk, or a drop of more than a step */
            bool edge = false;
            for (int k = 0; k < 4 && !edge; ++k)
            {
                int nx = x + (k == 0) - (k == 1), nz = z + (k == 2) - (k == 3);
                if (nx < 0 || nz < 0 || nx >= SIZE || nz >= SIZE)
                    continue;
                float ny = top[(size_t)nz * SIZE + nx];
                edge = ny > 1e29f || fabsf(ny - y) > 1.5f;
            }
            float h = std::clamp((y_lo - y) / (y_lo - y_hi), 0.0f, 1.0f); /* 1 the highest */
            /* a little grain, like paper: the same for a texel whatever the zone */
            uint32_t n = (uint32_t)x * 73856093u ^ (uint32_t)z * 19349663u;
            n ^= n >> 13, n *= 0x5bd1e995u, n ^= n >> 15;
            float grain = ((n & 255) / 255.0f - 0.5f) * 0.06f;
            float lum;
            uint8_t alpha;
            if (dim_rest && !reached[(size_t)z * SIZE + x])
                lum = 0.55f, alpha = edge ? 80 : 45;
            else if (edge)
                lum = 0.28f, alpha = 240;
            else
                lum = 0.78f + 0.22f * h + grain, alpha = 235;
            uint8_t v = (uint8_t)(std::clamp(lum, 0.0f, 1.0f) * 255.0f);
            o[0] = o[1] = o[2] = v, o[3] = alpha; /* grey: the overlay tints it with the ground color */
        }
    rt_log("[recomp] map: zone %d: %zu walkable triangles, %.0f units across, %zu%% of its ground reachable from %.0f %.0f %.0f (floor %.1f), from %s\n",
        zone, tris.size(), half * 2, ys.empty() ? 0 : reach_count * 100 / (ys.size() * 7), me_x, me_y, me_z, start >= 0 ? top[start] : 0.0f,
        path.c_str());
    {
        /* the drawn map now; the game's own follows */
        std::lock_guard<std::mutex> hold(g_lock);
        if (g_wanted != zone)
        {
            free(rgba); /* zoned again while it was being made */
            return;
        }
        free(g_done.rgba);
        g_done = ZoneMap{ zone, SIZE, cx, cz, half, rgba, NULL, 0, 0, 0 };
    }
    /* the game's own map of the zone, placed by its edges (every edge, reachable or not); the
     * drawn map's pixels are the overlay's now, so the edges come from the heights */
    std::vector<std::pair<float, float>> edges;
    for (int z = 1; z < SIZE - 1; z += 2)
        for (int x = 1; x < SIZE - 1; x += 2)
        {
            float y = top[(size_t)z * SIZE + x];
            if (y > 1e29f)
                continue;
            bool edge = false;
            for (int k = 0; k < 4 && !edge; ++k)
            {
                float ny = top[(size_t)(z + (k == 2) - (k == 3)) * SIZE + x + (k == 0) - (k == 1)];
                edge = ny > 1e29f || fabsf(ny - y) > 1.5f;
            }
            if (edge)
                edges.push_back({ ox + (x + 0.5f) / scale, oz - (z + 0.5f) / scale });
        }
    std::vector<std::pair<float, float>> sample;
    for (size_t i = 0; i < edges.size(); i += edges.size() / 1500 + 1)
        sample.push_back(edges[i]);
    std::vector<uint8_t> art;
    float as = 0, ax = 0, ay = 0;
    uint8_t* art_px = NULL;
    if (place_art(zone, sample, art, as, ax, ay))
    {
        art_px = (uint8_t*)malloc(art.size());
        if (art_px)
            memcpy(art_px, art.data(), art.size());
    }
    if (!art_px)
        return;
    std::lock_guard<std::mutex> hold(g_lock);
    if (g_wanted != zone)
    {
        free(art_px);
        return;
    }
    free(g_done_art.art);
    g_done_art = ZoneMap{ zone, 0, cx, cz, half, NULL, art_px, as, ax, ay };
}
} // namespace

extern "C" void zonemap_init(const char* game_dir, const uint8_t keys[256])
{
    std::lock_guard<std::mutex> hold(g_lock);
    g_game = game_dir ? game_dir : "";
    if (keys)
        memcpy(g_keys, keys, 256), g_have_keys = true;
}

extern "C" void zonemap_want(int zone, float x, float y, float z)
{
    {
        std::lock_guard<std::mutex> hold(g_lock);
        if (zone <= 0 || zone == g_wanted || !g_have_keys || g_game.empty())
            return;
        g_wanted = zone;
    }
    std::thread(make, zone, x, y, z).detach();
}

extern "C" int zonemap_take_art(int zone, ZoneMap* out)
{
    std::lock_guard<std::mutex> hold(g_lock);
    if (!g_done_art.art || g_done_art.zone != zone)
        return 0;
    *out = g_done_art;
    g_done_art.art = NULL;
    return 1;
}

extern "C" int zonemap_take(int zone, ZoneMap* out)
{
    std::lock_guard<std::mutex> hold(g_lock);
    if (!g_done.rgba || g_done.zone != zone)
        return 0;
    *out = g_done;
    g_done.rgba = NULL;
    return 1;
}
