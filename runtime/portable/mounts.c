/* MogHouse's own mounts (mounts.h). A mount's file is a creature's model file made over, as retail's
 * are: the tiger mount (102707) holds the tiger's (1608) textures, skeleton, mesh and motions - idl0 as
 * chi0 (ridden, standing), wlk0 as wlk0 and mvb0, run0, dfi0 as dam0 - in a 'moun' folder with the mount's
 * own effects (coff, cdam, vdam) and sounds, and two things more:
 *   - the seats: points 48 to 55 of the skeleton's 128 (a bone, three floats, a place in the bone's frame),
 *     one for each race (Hume male, female, Elvaan male, female, Tarutaru male, female, Mithra, Galka):
 *     where the rider's hips go. The rider sits upright whatever the bone's turn: only the place counts.
 *   - a 'moun' chunk (0x45): 8 bytes (zero on most), then how each race sits, by race as above (6 astride).
 * A bee is made so from the Killer Bee's model (1572), bigger, lowered from its own hover to sit a rider at
 * a mount's height, its seat on top of its thorax; the effects, sounds and folder from the Crackclaw's
 * (102741, a beetle). The sizes and places were measured from the models' skinned idle (ffxi-scratch,
 * choco/trybee2.py: the back's top, the wings left out), and the files checked byte for byte against it. */
#include "mounts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plat.h"
#include "vfs.h"

extern void rt_log(const char* fmt, ...);

enum { MOUNT_FILE0 = 102704, BEE_FILE = 1572, BEE_DONOR = 102741, BEE_BONES = 30, SEATS = 48, RACES = 8 };

static const struct
{
    int mount;    /* its mount id */
    double scale; /* times the bee's size */
    double lower; /* down from its hover (heights count down: added to the body bone's place) */
    float seat[3]; /* on the thorax (bone 2), in its frame */
} BEES[] = {
    { 40, 2.20, 2.4657, { -0.0731f, 0.3088f, 0.0836f } }, /* small: a Tarutaru's, the seat 1.25 yalms up */
    { 41, 3.00, 3.4668, { -0.0997f, 0.4210f, 0.1140f } }, /* middling: 1.6 */
    { 42, 3.80, 4.4680, { -0.1263f, 0.5333f, 0.1444f } }, /* large: a Galka's, 1.95 */
};

/* --- the made mounts' entries in the file table ------------------------------------------------------- */
typedef struct Made
{
    uint32_t file;   /* its file id */
    uint16_t place;  /* FTABLE's: ROM folder << 7 | file */
} Made;
static Made g_made[8];
static int g_nmade;
static char g_vtable[1100], g_ftable[1100]; /* the install's table files (host paths) */

static const void* g_tracked[16];
static int g_tracked_kind[16]; /* 1 VTABLE, 2 FTABLE */

void mounts_track(const void* file, const char* host_path)
{
    int kind = !g_nmade ? 0 : !strcmp(host_path, g_vtable) ? 1 : !strcmp(host_path, g_ftable) ? 2 : 0;
    if (!kind)
        return;
    for (int i = 0; i < 16; ++i)
        if (!g_tracked[i])
        {
            g_tracked[i] = file, g_tracked_kind[i] = kind;
            return;
        }
}

void mounts_untrack(const void* file)
{
    for (int i = 0; i < 16; ++i)
        if (g_tracked[i] == file)
            g_tracked[i] = NULL;
}

int mounts_tracked(const void* file)
{
    for (int i = 0; i < 16; ++i)
        if (g_tracked[i] && g_tracked[i] == file)
            return g_tracked_kind[i];
    return 0;
}

void mounts_patch(const void* file, uint64_t at, uint8_t* buf, size_t n)
{
    int kind = mounts_tracked(file);
    if (!kind)
        return;
    for (int i = 0; i < g_nmade; ++i)
    {
        /* VTABLE: a byte each, the ROM folder set the file is in (1: ROM\); FTABLE: two, its place there */
        uint64_t o = kind == 1 ? g_made[i].file : 2ull * g_made[i].file;
        uint8_t v[2] = { kind == 1 ? 1 : (uint8_t)g_made[i].place, (uint8_t)(g_made[i].place >> 8) };
        for (int k = 0; k < kind; ++k)
            if (o + k >= at && o + k < at + n)
                buf[o + k - at] = v[k];
    }
}

/* --- reading the install ------------------------------------------------------------------------------ */
static uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
static float rdf(const uint8_t* p) { float v; memcpy(&v, p, 4); return v; }
static void wrf(uint8_t* p, float v) { memcpy(p, &v, 4); }
static void scalef(uint8_t* p, double s) { wrf(p, (float)((double)rdf(p) * s)); }

typedef struct Table
{
    unsigned char *vt, *ft;
    size_t nvt, nft;
} Table;

/* A file of the install by id, read whole (through the DAT overlays, as the game would read it) */
static unsigned char* read_dat(const char* guest_game, const Table* t, uint32_t id, size_t* n)
{
    if (id >= t->nvt || 2u * id + 1 >= t->nft || !t->vt[id])
        return NULL;
    uint16_t f = rd16(t->ft + 2u * id);
    char guest[900], host[1400];
    if (t->vt[id] == 1)
        snprintf(guest, sizeof guest, "%s\\ROM\\%u\\%u.DAT", guest_game, f >> 7, f & 0x7f);
    else
        snprintf(guest, sizeof guest, "%s\\ROM%u\\%u\\%u.DAT", guest_game, t->vt[id], f >> 7, f & 0x7f);
    return vfs_host_path(guest, host, sizeof host) ? plat_read_file(host, n) : NULL;
}

/* --- a file's chunks ------------------------------------------------------------------------------------ */
typedef struct Chunk
{
    const uint8_t* p; /* its 16-byte header, then its body */
    uint32_t size, type;
} Chunk;

/* The file's chunks, in order (at most max); their number */
static int chunks(const uint8_t* d, size_t n, Chunk* out, int max)
{
    int k = 0;
    for (size_t o = 0; o + 16 <= n && k < max;)
    {
        uint32_t w = rd32(d + o + 4), size = ((w >> 7) & 0x7ffff) * 16;
        if (!size || o + size > n)
            break;
        out[k].p = d + o, out[k].size = size, out[k].type = w & 0x7f;
        ++k, o += size;
    }
    return k;
}

static int named(const Chunk* c, const char* name) { return !memcmp(c->p, name, 4); }

typedef struct Buf
{
    uint8_t* p;
    size_t n, cap;
} Buf;

static void put(Buf* b, const void* p, size_t n)
{
    if (b->n + n > b->cap)
    {
        b->cap = (b->n + n) * 2;
        b->p = (uint8_t*)realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
}

/* A chunk: its header (the name, the type and its size in 16-byte units, 8 bytes of zero), the body,
 * padded to 16 */
static void put_chunk(Buf* b, const uint8_t* name, uint32_t type, const uint8_t* body, size_t n)
{
    size_t total = (16 + n + 15) & ~(size_t)15;
    uint8_t h[16] = { 0 };
    uint32_t w = type | (uint32_t)(total / 16) << 7;
    memcpy(h, name, 4), memcpy(h + 4, &w, 4);
    put(b, h, 16), put(b, body, n);
    static const uint8_t zero[16];
    put(b, zero, total - 16 - n);
}

/* A mesh (0x2A) body bigger: its vertices' places (one-bone: 3 of 6 floats; two-bone: 6 of 14, each a
 * bone's part, weighted) times s, normals as they were */
static void scale_mesh(uint8_t* b, size_t n, double s)
{
    if (n < 0x34)
        return;
    uint32_t counts = rd32(b + 6 + 6 * 2) * 2u, verts = rd32(b + 6 + 6 * 4) * 2u;
    if (counts + 4 > n)
        return;
    uint32_t n1 = rd16(b + counts), n2 = rd16(b + counts + 2);
    if (verts + 24ull * n1 + 56ull * n2 > n)
        return;
    for (uint32_t i = 0; i < n1; ++i)
        for (int k = 0; k < 3; ++k)
            scalef(b + verts + 24 * i + 4 * k, s);
    for (uint32_t i = 0; i < n2; ++i)
        for (int k = 0; k < 6; ++k)
            scalef(b + verts + 24 * n1 + 56 * i + 4 * k, s);
}

/* A motion (0x2B) body bigger: each bone's moves (its keyed translations, else the default) times s */
static void scale_anim(uint8_t* b, size_t n, double s)
{
    if (n < 10)
        return;
    uint16_t nb = rd16(b + 2), nf = rd16(b + 4);
    uint32_t done[256];
    int ndone = 0;
    for (int k = 0; k < nb && 10u + 84u * (k + 1) <= n; ++k)
    {
        uint8_t* e = b + 10 + 84 * k;
        for (int c = 0; c < 3; ++c)
        {
            uint32_t o = rd32(e + 36 + 4 * c);
            if (o && !(o & 0x80000000u))
            {
                int seen = 0;
                for (int i = 0; i < ndone; ++i)
                    seen |= done[i] == o;
                if (seen || 10ull + 4ull * (o + nf) > n)
                    continue;
                if (ndone < 256)
                    done[ndone++] = o;
                for (int f = 0; f < nf; ++f)
                    scalef(b + 10 + 4 * (o + f), s);
            }
            else
                scalef(e + 48 + 4 * c, s);
        }
    }
}

/* A bee mount's file, from the bee's and the donor's */
static int make_bee(const uint8_t* bee, size_t nbee, const uint8_t* donor, size_t ndonor, int which, Buf* out)
{
    Chunk bc[256], dc[256];
    int nb = chunks(bee, nbee, bc, 256), nd = chunks(donor, ndonor, dc, 256);
    if (nb < 4 || nd < 4 || dc[0].type != 0x01 || !named(&dc[0], "moun"))
        return 0;
    const Chunk* skel = NULL;
    for (int i = 0; i < nb; ++i)
        if (bc[i].type == 0x29)
            skel = &bc[i];
    if (!skel || !named(skel, "k_be") || skel->size < 20 || rd16(skel->p + 16 + 2) != BEE_BONES)
        return 0;
    double s = BEES[which].scale;

    put(out, dc[0].p, dc[0].size); /* the mount's folder */
    for (int i = 0; i < nd; ++i)
        if (dc[i].type == 0x07)
            put(out, dc[i].p, dc[i].size); /* its effects */
    for (int i = 0; i < nb; ++i)
        if (bc[i].type == 0x20)
            put(out, bc[i].p, bc[i].size); /* the bee's textures */

    /* the skeleton: bigger, lowered, with the seats */
    size_t sn = skel->size - 16;
    uint8_t* sk = (uint8_t*)malloc(sn);
    memcpy(sk, skel->p + 16, sn);
    size_t end = 4 + 30 * BEE_BONES;
    if (end + 4 > sn)
    {
        free(sk);
        return 0;
    }
    for (int k = 0; k < BEE_BONES; ++k)
    {
        uint8_t* t = sk + 4 + 30 * k + 18;
        for (int c = 0; c < 3; ++c)
        {
            double v = (double)rdf(t + 4 * c) * s;
            if (k == 1 && c == 1)
                v += BEES[which].lower;
            wrf(t + 4 * c, (float)v);
        }
    }
    uint16_t npts = rd16(sk + end);
    if (npts < SEATS + RACES || end + 4 + 26ull * npts + 72 > sn)
    {
        free(sk);
        return 0;
    }
    for (int k = 0; k < npts; ++k)
        for (int c = 0; c < 3; ++c)
            scalef(sk + end + 4 + 26 * k + 14 + 4 * c, s);
    for (int k = 0; k < RACES; ++k)
    {
        uint8_t* r = sk + end + 4 + 26 * (SEATS + k);
        uint16_t bone = 2; /* the thorax */
        memcpy(r, &bone, 2);
        memset(r + 2, 0, 12);
        memcpy(r + 14, BEES[which].seat, 12);
    }
    for (int i = 0; i < 18; ++i) /* three boxes about it */
        scalef(sk + end + 4 + 26 * npts + 4 * i, s);
    put_chunk(out, skel->p, 0x29, sk, sn);
    free(sk);

    for (int i = 0; i < nb; ++i)
        if (bc[i].type == 0x2a)
        {
            uint8_t* m = (uint8_t*)malloc(bc[i].size - 16);
            memcpy(m, bc[i].p + 16, bc[i].size - 16);
            scale_mesh(m, bc[i].size - 16, s);
            put_chunk(out, bc[i].p, 0x2a, m, bc[i].size - 16);
            free(m);
        }
    static const char* const MOTIONS[][2] = { { "wlk0", "wlk0" }, { "dam0", "dfi0" }, { "chi0", "idl0" }, { "mvb0", "wlk0" }, { "run0", "run0" } };
    for (size_t m = 0; m < sizeof MOTIONS / sizeof *MOTIONS; ++m)
    {
        const Chunk* a = NULL;
        for (int i = 0; i < nb && !a; ++i)
            if (bc[i].type == 0x2b && named(&bc[i], MOTIONS[m][1]))
                a = &bc[i];
        if (!a)
            return 0;
        uint8_t* body = (uint8_t*)malloc(a->size - 16);
        memcpy(body, a->p + 16, a->size - 16);
        scale_anim(body, a->size - 16, s);
        put_chunk(out, (const uint8_t*)MOTIONS[m][0], 0x2b, body, a->size - 16);
        free(body);
    }
    for (int i = 0; i < nd; ++i)
        if (dc[i].type == 0x3d)
            put(out, dc[i].p, dc[i].size); /* the mount's sounds */
    for (int i = 0; i < nb; ++i)
        if (bc[i].type == 0x45 && named(&bc[i], "info"))
            put(out, bc[i].p, bc[i].size);
    uint8_t moun[16] = { 0 };
    memset(moun + 8, 6, RACES); /* every race astride */
    put_chunk(out, (const uint8_t*)"moun", 0x45, moun, sizeof moun);
    put(out, dc[nd - 1].p, 16); /* the end of the folder */
    return 1;
}

/* --- making them --------------------------------------------------------------------------------------- */
static int write_if_changed(const char* path, const uint8_t* p, size_t n)
{
    size_t had = 0;
    unsigned char* old = plat_read_file(path, &had);
    int same = old && had == n && !memcmp(old, p, n);
    free(old);
    if (same)
        return 1;
    FILE* f = fopen(path, "wb");
    if (!f)
        return 0;
    int ok = fwrite(p, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

int mounts_build(const char* guest_game, const char* out_dir)
{
    char guest[900];
    Table t = { 0 };
    snprintf(guest, sizeof guest, "%s\\VTABLE.DAT", guest_game);
    if (vfs_host_path(guest, g_vtable, sizeof g_vtable))
        t.vt = plat_read_file(g_vtable, &t.nvt);
    snprintf(guest, sizeof guest, "%s\\FTABLE.DAT", guest_game);
    if (vfs_host_path(guest, g_ftable, sizeof g_ftable))
        t.ft = plat_read_file(g_ftable, &t.nft);
    int made = 0;
    size_t nbee = 0, ndonor = 0;
    unsigned char* bee = t.vt && t.ft ? read_dat(guest_game, &t, BEE_FILE, &nbee) : NULL;
    unsigned char* donor = t.vt && t.ft ? read_dat(guest_game, &t, BEE_DONOR, &ndonor) : NULL;

    /* a ROM folder no file of the install's is in, from the last there could be (511) down */
    int folder = 0;
    if (bee && donor)
    {
        static uint8_t used[512];
        memset(used, 0, sizeof used);
        for (size_t i = 0; i < t.nvt && 2 * i + 1 < t.nft; ++i)
            if (t.vt[i] == 1)
                used[rd16(t.ft + 2 * i) >> 7] = 1;
        for (int d = 511; d > 400 && !folder; --d)
            if (!used[d])
                folder = d;
    }
    char path[1300];
    if (folder)
    {
        plat_mkdir(out_dir);
        snprintf(path, sizeof path, "%s%cROM", out_dir, plat_path_sep);
        plat_mkdir(path);
        snprintf(path, sizeof path, "%s%cROM%c%d", out_dir, plat_path_sep, plat_path_sep, folder);
        plat_mkdir(path);
    }
    g_nmade = 0;
    for (size_t i = 0; folder && i < sizeof BEES / sizeof *BEES; ++i)
    {
        uint32_t file = MOUNT_FILE0 + (uint32_t)BEES[i].mount;
        if (file >= t.nvt || 2u * file + 1 >= t.nft || t.vt[file])
            continue; /* the game has its own there (a newer install): leave it */
        Buf b = { 0 };
        if (make_bee(bee, nbee, donor, ndonor, (int)i, &b))
        {
            snprintf(path, sizeof path, "%s%cROM%c%d%c%d.DAT", out_dir, plat_path_sep, plat_path_sep, folder, plat_path_sep, (int)i);
            if (write_if_changed(path, b.p, b.n))
            {
                g_made[g_nmade].file = file, g_made[g_nmade].place = (uint16_t)(folder << 7 | (int)i);
                ++g_nmade, ++made;
            }
        }
        free(b.p);
    }
    if (made)
        vfs_add_overlay(out_dir, NULL);
    rt_log("[recomp] mounts: %d made (%s)%s\n", made, out_dir,
        !t.vt || !t.ft ? ", no file table" : !bee || !donor ? ", the bee's or donor's model missing" : !folder ? ", no free folder" : "");
    free(t.vt), free(t.ft), free(bee), free(donor);
    return made;
}
