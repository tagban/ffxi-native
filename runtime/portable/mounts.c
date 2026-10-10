/* MogHouse's own mounts (mounts.h). A mount's file is a creature's model file made over, as retail's
 * are: the tiger mount (102707) holds the tiger's (1608) textures, skeleton, mesh and motions - idl0 as
 * chi0 (ridden, standing), wlk0 as wlk0 and mvb0, run0, dfi0 as dam0 - in a 'moun' folder with the mount's
 * own effects (coff, cdam, vdam) and sounds, and two things more:
 *   - the seats: points 48 to 55 of the skeleton's 128 (a bone, three floats, a place in the bone's frame),
 *     one for each race (Hume male, female, Elvaan male, female, Tarutaru male, female, Mithra, Galka):
 *     where the rider's hips go. The rider sits upright whatever the bone's turn: only the place counts.
 *   - a 'moun' chunk (0x45): 8 bytes (zero on most), then how each race sits, by race as above (6 astride).
 * The mounts (MOUNTS below): each a creature's model, bigger or smaller, moved to a mount's height and
 * turned to face forward where it must be, with a seat for the rider; the effects, sounds and folder from
 * the Crackclaw's (102741, a beetle). The sizes and places were measured from the models' skinned idle
 * (scratch prototypes trybee2.py and ship2.py: a back's top with the wings left out, a deck's planks), and
 * the files checked byte for byte against the prototype's. A rider astride sits on the point; one standing
 * has their hips there, 0.75 yalm over what they stand on (as on the Levitus). */
#include "mounts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plat.h"
#include "vfs.h"

extern void rt_log(const char* fmt, ...);

/* the same files from every compiler and machine: no fused multiply-adds */
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

enum { MOUNT_FILE0 = 102704, CRACKCLAW = 102741, SEATS = 48, RACES = 8 };

/* What a mount is made of, and how. */
typedef struct MountSpec
{
    int mount;              /* its mount id */
    uint32_t model;         /* the creature's model file */
    const char* skeleton;   /* its skeleton's name and bones (checked: another model there is left alone) */
    int bones;
    double scale;           /* times its size */
    int body;               /* the bone that holds it up, and how far down (heights count down) it is moved */
    double lower;
    double turn[4];         /* the whole of it turned (a quaternion; none: 0, 0, 0, 1) */
    int seat_bone;          /* the rider's seat: on this bone, in its frame */
    float seat[3];
    uint8_t sit;            /* how the rider sits: 6 astride, 5 standing */
    const char* motions[5][2]; /* the mount's motions, each from one of the model's */
    uint32_t mmb;           /* else 0: a file whose static model (MMB) and textures are the mount's look, on the
                             * creature's skeleton (its root unturned: the MMB faces forward), all on the body bone */
    double mmb_scale;
} MountSpec;

#define BEE_MOTIONS { { "wlk0", "wlk0" }, { "dam0", "dfi0" }, { "chi0", "idl0" }, { "mvb0", "wlk0" }, { "run0", "run0" } }
#define STILL_MOTIONS { { "wlk0", "idl0" }, { "dam0", "idl0" }, { "chi0", "idl0" }, { "mvb0", "idl0" }, { "run0", "idl0" } }
#define NO_TURN { 0, 0, 0, 1 }
#define QUARTER_TURN { 0, 0.7071067811865475, 0, 0.7071067811865476 } /* about the up axis */

static const MountSpec MOUNTS[] = {
    /* the bee (the Killer Bee), its seat on top of its thorax: small for a Tarutaru, the seat 1.25 yalms up;
     * middling, 1.6; large for a Galka, 1.95 */
    { 40, 1572, "k_be", 30, 2.20, 1, 2.4657, NO_TURN, 2, { -0.0731f, 0.3088f, 0.0836f }, 6, BEE_MOTIONS, 0, 0 },
    { 41, 1572, "k_be", 30, 3.00, 1, 3.4668, NO_TURN, 2, { -0.0997f, 0.4210f, 0.1140f }, 6, BEE_MOTIONS, 0, 0 },
    { 42, 1572, "k_be", 30, 3.80, 1, 4.4680, NO_TURN, 2, { -0.1263f, 0.5333f, 0.1444f }, 6, BEE_MOTIONS, 0, 0 },
    /* the airship: the one that flies into port (31004, 63 yalms long) at 0.12 (7.6), on the sailing ship's
     * skeleton (its hull bone bobs), its keel 0.5 yalm off the ground, the rider standing on the forward deck
     * (2.05 yalms up; their hips 0.75 above it) */
    { 43, 53079, "ship", 6, 0.35, 1, -1.676, NO_TURN, 1, { 1.0f, -1.124f, 0.0f }, 5, STILL_MOTIONS, 31004, 0.12 },
    /* the boat: the sailing ship (53079) a third of its size (3.4 yalms long), turned to sail forward, its keel
     * 0.4 yalm off the ground, the rider standing on the stern deck */
    { 44, 53079, "ship", 6, 0.35, 1, -0.63, QUARTER_TURN, 1, { -1.1496f, -1.0007f, 0.0092f }, 5, STILL_MOTIONS, 0, 0 },
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

static void qmul(const double* a, const double* b, double* o)
{
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

/* --- a static model (MMB, 0x2E, version 4, not encrypted) made skinned meshes ---------------------------
 * The MMB: "MMB" and its version, a name, a box; at 0x40 its piece count and a box; from 0x60 the pieces, each a
 * texture's name (16), a u32 vertex count (its low 16 bits), the vertices (36 bytes: place, normal, color,
 * u, v), a u32 index count and the indices (a strip, joined by repeated indices), padded to 4. The meshes:
 * all the vertices on one bone (bone table [1]), each piece its texture and its strip as triangles (a mount's
 * mesh winds them as the MMB's strip does), as many meshes as it takes: a mesh's whole length, counted in
 * 16-bit words, must fit 16 bits. */
enum { MMB_VERTEX = 36, MESH_BUDGET = 120000, MAX_PIECES = 128 };

typedef struct Piece
{
    const uint8_t* name;  /* the texture's, 16 */
    const uint8_t* verts;
    uint32_t nverts;
    const uint8_t* idx;   /* u16 each */
    uint32_t nidx;
    int* where;           /* each vertex's place in the mesh being filled, or -1 */
} Piece;

static int mmb_pieces(const uint8_t* b, size_t n, Piece* out, int max)
{
    if (n < 0x60 || memcmp(b, "MMB", 3) || b[3] != 4)
        return -1;
    uint32_t count = rd32(b + 0x40);
    size_t o = 0x60;
    int k = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        if (o + 20 > n)
            return -1;
        Piece p = { 0 };
        p.name = b + o;
        p.nverts = rd32(b + o + 16) & 0xffff;
        o += 20;
        if (o + (size_t)MMB_VERTEX * p.nverts + 4 > n)
            return -1;
        p.verts = b + o;
        o += (size_t)MMB_VERTEX * p.nverts;
        p.nidx = rd32(b + o);
        o += 4;
        if (o + 2ull * p.nidx > n)
            return -1;
        p.idx = b + o;
        o = (o + 2ull * p.nidx + 3) & ~(size_t)3;
        int blank = 1;
        for (int c = 0; c < 16; ++c)
            blank &= p.name[c] == ' ';
        if (!blank && p.nverts && k < max)
            out[k++] = p;
    }
    return k;
}

typedef struct MeshOut
{
    const uint8_t** verts; /* the source vertices, in the mesh's order */
    int nverts, cap;
    struct Group { const uint8_t* name; uint16_t (*tris)[3]; int ntris, cap; } groups[256];
    int ngroups, ntris;
} MeshOut;

static int64_t mesh_cost(const MeshOut* mo, int new_verts, int new_tris)
{
    int64_t nt = mo->ntris + new_tris;
    return 28ll * (mo->nverts + new_verts) + 30 * nt + 4 * ((nt + 127) / 128 + mo->ngroups + 1) + 18ll * (mo->ngroups + 1) + 64;
}

static void mesh_reset(MeshOut* mo, Piece* ps, int np)
{
    for (int g = 0; g < mo->ngroups; ++g)
        free(mo->groups[g].tris);
    mo->nverts = mo->ngroups = mo->ntris = 0;
    for (int i = 0; i < np; ++i)
        for (uint32_t k = 0; k < ps[i].nverts; ++k)
            ps[i].where[k] = -1;
}

static void mesh_add(MeshOut* mo, Piece* p, const uint16_t (*tris)[3], int n)
{
    if (!mo->ngroups || memcmp(mo->groups[mo->ngroups - 1].name, p->name, 16))
    {
        struct Group* g = &mo->groups[mo->ngroups++];
        memset(g, 0, sizeof *g);
        g->name = p->name;
    }
    struct Group* g = &mo->groups[mo->ngroups - 1];
    for (int t = 0; t < n; ++t)
    {
        if (g->ntris == g->cap)
            g->cap = g->cap ? g->cap * 2 : 256, g->tris = (uint16_t(*)[3])realloc(g->tris, sizeof *g->tris * g->cap);
        for (int c = 0; c < 3; ++c)
        {
            uint16_t k = tris[t][c];
            if (p->where[k] < 0)
            {
                if (mo->nverts == mo->cap)
                    mo->cap = mo->cap ? mo->cap * 2 : 1024, mo->verts = (const uint8_t**)realloc(mo->verts, sizeof *mo->verts * mo->cap);
                p->where[k] = mo->nverts;
                mo->verts[mo->nverts++] = p->verts + (size_t)MMB_VERTEX * k;
            }
            g->tris[g->ntris][c] = (uint16_t)p->where[k];
        }
        ++g->ntris;
    }
    mo->ntris += n;
}

static void mesh_put(Buf* out, const MeshOut* mo, double s, const uint8_t* head6, int k)
{
    Buf body = { 0 };
    uint8_t zero[0x34] = { 0 };
    put(&body, zero, sizeof zero);
    uint32_t secs[7][2];
    /* the polygons: each group its texture, then its triangles 128 at a time (corners' u, v), then the end */
    secs[0][0] = (uint32_t)(body.n / 2);
    for (int g = 0; g < mo->ngroups; ++g)
    {
        uint16_t op = 0x8000;
        put(&body, &op, 2), put(&body, mo->groups[g].name, 16);
        for (int i = 0; i < mo->groups[g].ntris; i += 128)
        {
            uint16_t hdr[2] = { 0x0054, (uint16_t)(mo->groups[g].ntris - i < 128 ? mo->groups[g].ntris - i : 128) };
            put(&body, hdr, 4);
            for (int t = i; t < i + hdr[1]; ++t)
            {
                put(&body, mo->groups[g].tris[t], 6);
                for (int c = 0; c < 3; ++c)
                    put(&body, mo->verts[mo->groups[g].tris[t][c]] + 28, 8);
            }
        }
    }
    uint16_t end = 0xFFFF, one = 1, counts[2] = { (uint16_t)mo->nverts, 0 }, ref[2] = { 0x4000, 0 };
    put(&body, &end, 2);
    secs[0][1] = (uint32_t)(body.n / 2) - secs[0][0];
    secs[1][0] = (uint32_t)(body.n / 2), put(&body, &one, 2), secs[1][1] = 1;      /* the bone table: [1] */
    secs[2][0] = (uint32_t)(body.n / 2), put(&body, counts, 4), secs[2][1] = 2;    /* one-bone, two-bone vertices */
    secs[3][0] = (uint32_t)(body.n / 2);
    for (int i = 0; i < mo->nverts; ++i)
        put(&body, ref, 4);
    secs[3][1] = 2u * mo->nverts;
    secs[4][0] = (uint32_t)(body.n / 2);
    for (int i = 0; i < mo->nverts; ++i)
    {
        uint8_t v[24];
        for (int c = 0; c < 3; ++c)
            wrf(v + 4 * c, (float)((double)rdf(mo->verts[i] + 4 * c) * s));
        memcpy(v + 12, mo->verts[i] + 12, 12);
        put(&body, v, 24);
    }
    secs[4][1] = 12u * mo->nverts;
    uint32_t words = (uint32_t)(body.n / 2);
    secs[5][0] = words, secs[5][1] = 0, secs[6][0] = 0, secs[6][1] = words;
    memcpy(body.p, head6, 6);
    for (int i = 0; i < 7; ++i)
    {
        uint16_t c = (uint16_t)secs[i][1];
        memcpy(body.p + 6 + 6 * i, &secs[i][0], 4), memcpy(body.p + 10 + 6 * i, &c, 2);
    }
    uint8_t name[4] = { 'a', 'i', 'r', (uint8_t)('0' + k) };
    put_chunk(out, name, 0x2a, body.p, body.n);
    free(body.p);
}

/* The MMB's meshes into out; the box about it (min, max) into lo, hi. 0 if it is not as expected. */
static int mmb_meshes(const uint8_t* mmb, size_t n, double s, const uint8_t* head6, Buf* out, double lo[3], double hi[3])
{
    Piece ps[MAX_PIECES];
    int np = mmb_pieces(mmb, n, ps, MAX_PIECES);
    if (np <= 0)
        return 0;
    double mn[3] = { 1e30, 1e30, 1e30 }, mx[3] = { -1e30, -1e30, -1e30 };
    for (int i = 0; i < np; ++i)
    {
        ps[i].where = (int*)malloc(sizeof(int) * ps[i].nverts);
        for (uint32_t k = 0; k < ps[i].nverts; ++k)
            for (int c = 0; c < 3; ++c)
            {
                double v = rdf(ps[i].verts + (size_t)MMB_VERTEX * k + 4 * c);
                mn[c] = v < mn[c] ? v : mn[c], mx[c] = v > mx[c] ? v : mx[c];
            }
    }
    for (int c = 0; c < 3; ++c)
        lo[c] = mn[c] * s, hi[c] = mx[c] * s;
    MeshOut mo = { 0 };
    int k = 0, ok = 1;
    mesh_reset(&mo, ps, np);
    for (int i = 0; i < np && ok; ++i)
    {
        /* the strip's triangles, joins dropped, every other one turned */
        uint16_t(*tris)[3] = (uint16_t(*)[3])malloc(sizeof *tris * (ps[i].nidx ? ps[i].nidx : 1));
        int nt = 0;
        for (uint32_t j = 0; j + 2 < ps[i].nidx; ++j)
        {
            uint16_t a = rd16(ps[i].idx + 2 * j), b = rd16(ps[i].idx + 2 * j + 2), c = rd16(ps[i].idx + 2 * j + 4);
            if (a == b || b == c || a == c)
                continue;
            if (a >= ps[i].nverts || b >= ps[i].nverts || c >= ps[i].nverts)
            {
                ok = 0;
                break;
            }
            tris[nt][0] = j % 2 ? b : a, tris[nt][1] = j % 2 ? a : b, tris[nt][2] = c;
            ++nt;
        }
        for (int t = 0; ok && t < nt; t += 128)
        {
            int m = nt - t < 128 ? nt - t : 128;
            /* the batch's vertices not yet in the mesh, each once */
            int fresh = 0;
            for (int q = t; q < t + m; ++q)
                for (int c = 0; c < 3; ++c)
                {
                    uint16_t v = tris[q][c];
                    if (ps[i].where[v] == -1)
                        ps[i].where[v] = -2, ++fresh;
                }
            for (int q = t; q < t + m; ++q)
                for (int c = 0; c < 3; ++c)
                    if (ps[i].where[tris[q][c]] == -2)
                        ps[i].where[tris[q][c]] = -1;
            if (mo.ntris && mesh_cost(&mo, fresh, m) > MESH_BUDGET)
            {
                if (k >= 9)
                {
                    ok = 0;
                    break;
                }
                mesh_put(out, &mo, s, head6, k++);
                mesh_reset(&mo, ps, np);
            }
            mesh_add(&mo, &ps[i], (const uint16_t(*)[3])(tris + t), m);
        }
        free(tris);
    }
    if (ok && mo.ntris)
        mesh_put(out, &mo, s, head6, k++);
    mesh_reset(&mo, ps, np);
    free(mo.verts);
    for (int i = 0; i < np; ++i)
        free(ps[i].where);
    return ok;
}

/* A mount's file, from the creature's model and the donor mount's */
static int make_mount(const MountSpec* m, const uint8_t* src, size_t nsrc, const uint8_t* donor, size_t ndonor,
    const uint8_t* look, size_t nlook, Buf* out)
{
    Chunk bc[256], dc[256], lc[256];
    int nb = chunks(src, nsrc, bc, 256), nd = chunks(donor, ndonor, dc, 256), nl = look ? chunks(look, nlook, lc, 256) : 0;
    if (nb < 4 || nd < 4 || dc[0].type != 0x01 || !named(&dc[0], "moun"))
        return 0;
    const Chunk* mmb = NULL;
    for (int i = 0; i < nl; ++i)
        if (lc[i].type == 0x2e)
            mmb = &lc[i];
    if (m->mmb && !mmb)
        return 0;
    const Chunk* skel = NULL;
    for (int i = 0; i < nb; ++i)
        if (bc[i].type == 0x29)
            skel = &bc[i];
    if (!skel || !named(skel, m->skeleton) || skel->size < 20 || rd16(skel->p + 16 + 2) != m->bones)
        return 0;
    double s = m->scale;

    put(out, dc[0].p, dc[0].size); /* the mount's folder */
    for (int i = 0; i < nd; ++i)
        if (dc[i].type == 0x07)
            put(out, dc[i].p, dc[i].size); /* its effects */
    for (int i = 0; i < (mmb ? nl : nb); ++i)
        if ((mmb ? lc : bc)[i].type == 0x20)
            put(out, (mmb ? lc : bc)[i].p, (mmb ? lc : bc)[i].size); /* the textures: the look's, else the creature's */

    /* the skeleton: bigger or smaller, moved, turned, with the seats */
    size_t sn = skel->size - 16;
    uint8_t* sk = (uint8_t*)malloc(sn);
    memcpy(sk, skel->p + 16, sn);
    size_t end = 4 + 30 * (size_t)m->bones;
    if (end + 4 > sn)
    {
        free(sk);
        return 0;
    }
    for (int k = 0; k < m->bones; ++k)
    {
        uint8_t* t = sk + 4 + 30 * k + 18;
        for (int c = 0; c < 3; ++c)
        {
            double v = (double)rdf(t + 4 * c) * s;
            if (k == m->body)
                v += c == 1 ? m->lower : 0.0;
            wrf(t + 4 * c, (float)v);
        }
        if (k == 0 && mmb)
        {
            static const float still[4] = { 0, 0, 0, 1 };
            memcpy(sk + 4 + 30 * k + 2, still, 16);
        }
        else if (k == 0 && m->turn[3] != 1.0)
        {
            /* the root's own turn, then this one */
            uint8_t* q = sk + 4 + 30 * k + 2;
            double was[4], now[4];
            for (int c = 0; c < 4; ++c)
                was[c] = rdf(q + 4 * c);
            qmul(m->turn, was, now);
            for (int c = 0; c < 4; ++c)
                wrf(q + 4 * c, (float)now[c]);
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
        uint16_t bone = (uint16_t)m->seat_bone;
        memcpy(r, &bone, 2);
        memset(r + 2, 0, 12);
        memcpy(r + 14, m->seat, 12);
    }
    for (int i = 0; i < 18; ++i) /* three boxes about it */
        scalef(sk + end + 4 + 26 * npts + 4 * i, s);
    Buf meshes = { 0 };
    if (mmb)
    {
        /* the look's meshes, on the creature's mesh's header; the first two boxes about them (heights - top,
         * bottom - then x, z: max, min) */
        const Chunk* first = NULL;
        for (int i = 0; i < nb && !first; ++i)
            if (bc[i].type == 0x2a && bc[i].size >= 16 + 6)
                first = &bc[i];
        double lo[3], hi[3];
        if (!first || !mmb_meshes(mmb->p + 16, mmb->size - 16, m->mmb_scale, first->p + 16, &meshes, lo, hi))
        {
            free(sk), free(meshes.p);
            return 0;
        }
        float box[6] = { (float)(lo[1] + m->lower), (float)(hi[1] + m->lower), (float)hi[0], (float)lo[0], (float)hi[2], (float)lo[2] };
        for (int b = 0; b < 2; ++b)
            memcpy(sk + end + 4 + 26 * npts + 24 * b, box, 24);
    }
    put_chunk(out, skel->p, 0x29, sk, sn);
    free(sk);
    if (mmb)
        put(out, meshes.p, meshes.n);
    free(meshes.p);

    for (int i = 0; i < nb && !mmb; ++i)
        if (bc[i].type == 0x2a)
        {
            uint8_t* mesh = (uint8_t*)malloc(bc[i].size - 16);
            memcpy(mesh, bc[i].p + 16, bc[i].size - 16);
            scale_mesh(mesh, bc[i].size - 16, s);
            put_chunk(out, bc[i].p, 0x2a, mesh, bc[i].size - 16);
            free(mesh);
        }
    for (int k = 0; k < 5; ++k)
    {
        const Chunk* a = NULL;
        for (int i = 0; i < nb && !a; ++i)
            if (bc[i].type == 0x2b && named(&bc[i], m->motions[k][1]))
                a = &bc[i];
        if (!a)
            return 0;
        uint8_t* body = (uint8_t*)malloc(a->size - 16);
        memcpy(body, a->p + 16, a->size - 16);
        scale_anim(body, a->size - 16, s);
        put_chunk(out, (const uint8_t*)m->motions[k][0], 0x2b, body, a->size - 16);
        free(body);
    }
    for (int i = 0; i < nd; ++i)
        if (dc[i].type == 0x3d)
            put(out, dc[i].p, dc[i].size); /* the mount's sounds */
    for (int i = 0; i < nb; ++i)
        if (bc[i].type == 0x45 && named(&bc[i], "info") && bc[i].size >= 32)
        {
            /* the creature's, its last byte a mount's: every retail mount has one (0x32 to 0x96, 100 the
             * most) where a creature has 0xFF; with 0xFF the game never loaded a chocobo after one of
             * ours was ridden (Tagban's test, 2026-10-10) */
            uint8_t info[256];
            size_t n = bc[i].size < sizeof info ? bc[i].size : sizeof info;
            memcpy(info, bc[i].p, n);
            if (info[16 + 15] == 0xFF)
                info[16 + 15] = 100;
            put(out, info, n);
        }
    uint8_t moun[16] = { 0 };
    memset(moun + 8, m->sit, RACES); /* every race sits so */
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
    size_t ndonor = 0;
    unsigned char* donor = t.vt && t.ft ? read_dat(guest_game, &t, CRACKCLAW, &ndonor) : NULL;

    /* a ROM folder no file of the install's is in, from the last there could be (511) down */
    int folder = 0;
    if (donor)
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
    int missing = 0;
    for (size_t i = 0; folder && i < sizeof MOUNTS / sizeof *MOUNTS && g_nmade < 8; ++i)
    {
        const MountSpec* m = &MOUNTS[i];
        uint32_t file = MOUNT_FILE0 + (uint32_t)m->mount;
        if (file >= t.nvt || 2u * file + 1 >= t.nft || t.vt[file])
            continue; /* the game has its own there (a newer install): leave it */
        size_t nsrc = 0, nlook = 0;
        unsigned char* src = read_dat(guest_game, &t, m->model, &nsrc);
        unsigned char* look = m->mmb ? read_dat(guest_game, &t, m->mmb, &nlook) : NULL;
        Buf b = { 0 };
        if (src && (!m->mmb || look) && make_mount(m, src, nsrc, donor, ndonor, look, nlook, &b))
        {
            snprintf(path, sizeof path, "%s%cROM%c%d%c%d.DAT", out_dir, plat_path_sep, plat_path_sep, folder, plat_path_sep, (int)i);
            if (write_if_changed(path, b.p, b.n))
            {
                g_made[g_nmade].file = file, g_made[g_nmade].place = (uint16_t)(folder << 7 | (int)i);
                ++g_nmade, ++made;
            }
        }
        else
            ++missing;
        free(b.p), free(src), free(look);
    }
    if (made)
        vfs_add_overlay(out_dir, NULL);
    rt_log("[recomp] mounts: %d made (%s)%s%s\n", made, out_dir,
        !t.vt || !t.ft ? ", no file table" : !donor ? ", no donor mount" : !folder ? ", no free folder" : "",
        missing ? ", some models missing or not as expected" : "");
    free(t.vt), free(t.ft), free(donor);
    return made;
}
