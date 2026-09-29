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
ZoneMap g_done = {}; /* the last one made, until taken */

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
    std::lock_guard<std::mutex> hold(g_lock);
    if (g_wanted != zone)
    {
        free(rgba); /* zoned again while it was being made */
        return;
    }
    free(g_done.rgba);
    g_done = ZoneMap{ zone, SIZE, cx, cz, half, rgba };
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

extern "C" int zonemap_take(int zone, ZoneMap* out)
{
    std::lock_guard<std::mutex> hold(g_lock);
    if (!g_done.rgba || g_done.zone != zone)
        return 0;
    *out = g_done;
    g_done.rgba = NULL;
    return 1;
}
