/* The graphics back end on OpenGL 4.1 core (Linux with Mesa; macOS has the same version, so it
 * runs and is tested there too): what gfx.h asks for, on one GL context made by SDL. The same design
 * as gfx_d3d12.c, in GL's terms:
 *
 *   - The context: SDL_GL on the game's window (gfx_window_flags() asks for SDL_WINDOW_OPENGL), or
 *     on a hidden window of its own for the offscreen tests. Nothing links libGL or OpenGL.framework:
 *     every entry point is loaded through SDL_GL_GetProcAddress. Every call makes the context
 *     current on the calling thread if it is not (the game draws from one thread; the sign-in
 *     screen before it may be another).
 *   - Frames: up to three in flight, each with its own upload ring - vertex, index and uniform bytes
 *     are copied into the ring per draw (into a CPU copy of the chunk, then through one unsynchronized
 *     map before the draw), so the game may rewrite a buffer the moment a draw returns, as D3D lets it.
 *     A fence at Present ends a frame; its ring is reused once the fence has passed. Everything
 *     else is GL's own ordering: textures and buffers are filled with glTex/BufferSubData, and
 *     objects are deleted at once (GL keeps them while the GPU still reads them).
 *   - Binding: one empty vertex array. The vertex streams are R32UI buffer textures (units 8..11)
 *     the vertex function reads by hand, as the HLSL does its raw buffers; a stream's place in its
 *     buffer goes into the uniforms' per-register offsets. The uniforms (GfxU, std140) are a range
 *     of the ring (binding 0); texture stage i is unit i with a cached sampler object.
 *   - Programs come from the generated GLSL (gfx_glsl.c), compiled and linked on first use on this
 *     thread and cached by the vertex and fragment keys. GL has no pipeline objects: blending,
 *     depth-stencil and rasterizer state are set per draw, only where they change.
 *   - D3D's conventions: render targets keep D3D's rows (top first) - the vertex functions draw
 *     upside down in GL's terms, which flips the winding too (D3D's clockwise front is GL's
 *     counterclockwise), and viewports, scissors, clears, copies and reads use D3D's rows as they
 *     are; only the window's framebuffer is the right way up (the present). Clip z is D3D's 0..w
 *     through glClipControl where there is one (GL 4.5, ARB_clip_control: Mesa), else remapped in
 *     the vertex function (macOS).
 *   - Textures: the 16-bit color formats are GL's packed _REV types, A8 / L8 / A8L8 and X8R8G8B8 are
 *     texture swizzles, DXT1-5 are EXT_texture_compression_s3tc. Render targets are textures in
 *     framebuffer objects, one per target / level / depth combination, cached. Clears are glClear
 *     under a scissor; the back buffer reaches the window drawn scaled, with the frame-rate overlay
 *     on top, and SDL_GL_SwapWindow.
 *   - The scene effects are Metal's (gfx_metal.m): here, as on D3D12, they are not run. */
#define GL_GLEXT_PROTOTYPES /* the prototypes are only read for their types (__typeof__); nothing links them */
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "gfx_glsl.h"

/* --- the entry points: pointers loaded at start-up, called by their GL names ---------------------------- */
#define GL_FUNCS(X) \
    X(glActiveTexture) X(glAttachShader) X(glBeginQuery) X(glBindBuffer) X(glBindBufferRange) X(glBindFramebuffer) X(glBindSampler) \
    X(glBindTexture) X(glBindVertexArray) X(glBlendEquation) X(glBlendFuncSeparate) X(glBlitFramebuffer) \
    X(glBufferData) X(glBufferSubData) X(glCheckFramebufferStatus) X(glClear) X(glClearColor) X(glClearDepthf) \
    X(glClearStencil) X(glClientWaitSync) X(glColorMask) X(glCompileShader) X(glCompressedTexImage2D) \
    X(glCompressedTexSubImage2D) X(glCreateProgram) X(glCreateShader) X(glCullFace) X(glDeleteBuffers) \
    X(glDeleteFramebuffers) X(glDeleteProgram) X(glDeleteShader) X(glDeleteSync) \
    X(glDeleteTextures) X(glDepthFunc) X(glDepthMask) X(glDepthRangef) X(glDisable) X(glDrawArrays) X(glDrawBuffer) \
    X(glDrawElements) X(glEnable) X(glEndQuery) X(glFenceSync) X(glFinish) X(glFlush) X(glFramebufferTexture2D) X(glFrontFace) \
    X(glGenBuffers) X(glGenFramebuffers) X(glGenQueries) X(glGenSamplers) X(glGenTextures) X(glGenVertexArrays) \
    X(glGetCompressedTexImage) X(glGetError) X(glGetIntegerv) X(glGetProgramInfoLog) X(glGetProgramiv) \
    X(glGetQueryObjectui64v) X(glGetShaderInfoLog) X(glGetShaderiv) X(glGetString) \
    X(glGetStringi) X(glGetTexImage) X(glGetUniformBlockIndex) X(glGetUniformLocation) X(glLinkProgram) \
    X(glMapBufferRange) X(glPixelStorei) X(glPolygonMode) X(glPolygonOffset) X(glProvokingVertex) \
    X(glReadBuffer) X(glSamplerParameterf) X(glSamplerParameterfv) X(glSamplerParameteri) X(glScissor) \
    X(glShaderSource) X(glStencilFunc) X(glStencilMask) X(glStencilOp) X(glTexBuffer) X(glTexImage2D) \
    X(glTexParameteri) X(glTexParameteriv) X(glTexSubImage2D) X(glUniform1f) X(glUniform1i) X(glUniform1iv) \
    X(glUniform2f) X(glUniform4f) X(glUniformBlockBinding) X(glUnmapBuffer) X(glUseProgram) X(glViewport)
/* newer than 4.1: NULL where the context has none */
#define GL_OPTIONAL(X) X(glClipControl) X(glDebugMessageCallback)

#define GL_POINTER(n) static __typeof__(n)* p_##n;
GL_FUNCS(GL_POINTER)
GL_OPTIONAL(GL_POINTER)
#undef GL_POINTER

#define glActiveTexture p_glActiveTexture
#define glAttachShader p_glAttachShader
#define glBeginQuery p_glBeginQuery
#define glBindBuffer p_glBindBuffer
#define glBindBufferRange p_glBindBufferRange
#define glBindFramebuffer p_glBindFramebuffer
#define glBindSampler p_glBindSampler
#define glBindTexture p_glBindTexture
#define glBindVertexArray p_glBindVertexArray
#define glBlendEquation p_glBlendEquation
#define glBlendFuncSeparate p_glBlendFuncSeparate
#define glBlitFramebuffer p_glBlitFramebuffer
#define glBufferData p_glBufferData
#define glBufferSubData p_glBufferSubData
#define glCheckFramebufferStatus p_glCheckFramebufferStatus
#define glClear p_glClear
#define glClearColor p_glClearColor
#define glClearDepthf p_glClearDepthf
#define glClearStencil p_glClearStencil
#define glClientWaitSync p_glClientWaitSync
#define glColorMask p_glColorMask
#define glCompileShader p_glCompileShader
#define glCompressedTexImage2D p_glCompressedTexImage2D
#define glCompressedTexSubImage2D p_glCompressedTexSubImage2D
#define glCreateProgram p_glCreateProgram
#define glCreateShader p_glCreateShader
#define glCullFace p_glCullFace
#define glDeleteBuffers p_glDeleteBuffers
#define glDeleteFramebuffers p_glDeleteFramebuffers
#define glDeleteProgram p_glDeleteProgram
#define glDeleteShader p_glDeleteShader
#define glDeleteSync p_glDeleteSync
#define glDeleteTextures p_glDeleteTextures
#define glDepthFunc p_glDepthFunc
#define glDepthMask p_glDepthMask
#define glDepthRangef p_glDepthRangef
#define glDisable p_glDisable
#define glDrawArrays p_glDrawArrays
#define glDrawBuffer p_glDrawBuffer
#define glDrawElements p_glDrawElements
#define glEnable p_glEnable
#define glEndQuery p_glEndQuery
#define glFenceSync p_glFenceSync
#define glFinish p_glFinish
#define glFlush p_glFlush
#define glFramebufferTexture2D p_glFramebufferTexture2D
#define glFrontFace p_glFrontFace
#define glGenBuffers p_glGenBuffers
#define glGenFramebuffers p_glGenFramebuffers
#define glGenQueries p_glGenQueries
#define glGenSamplers p_glGenSamplers
#define glGenTextures p_glGenTextures
#define glGenVertexArrays p_glGenVertexArrays
#define glGetCompressedTexImage p_glGetCompressedTexImage
#define glGetError p_glGetError
#define glGetIntegerv p_glGetIntegerv
#define glGetProgramInfoLog p_glGetProgramInfoLog
#define glGetProgramiv p_glGetProgramiv
#define glGetQueryObjectui64v p_glGetQueryObjectui64v
#define glGetShaderInfoLog p_glGetShaderInfoLog
#define glGetShaderiv p_glGetShaderiv
#define glGetString p_glGetString
#define glGetStringi p_glGetStringi
#define glGetTexImage p_glGetTexImage
#define glGetUniformBlockIndex p_glGetUniformBlockIndex
#define glGetUniformLocation p_glGetUniformLocation
#define glLinkProgram p_glLinkProgram
#define glMapBufferRange p_glMapBufferRange
#define glPixelStorei p_glPixelStorei
#define glPolygonMode p_glPolygonMode
#define glPolygonOffset p_glPolygonOffset
#define glProvokingVertex p_glProvokingVertex
#define glReadBuffer p_glReadBuffer
#define glSamplerParameterf p_glSamplerParameterf
#define glSamplerParameterfv p_glSamplerParameterfv
#define glSamplerParameteri p_glSamplerParameteri
#define glScissor p_glScissor
#define glShaderSource p_glShaderSource
#define glStencilFunc p_glStencilFunc
#define glStencilMask p_glStencilMask
#define glStencilOp p_glStencilOp
#define glTexBuffer p_glTexBuffer
#define glTexImage2D p_glTexImage2D
#define glTexParameteri p_glTexParameteri
#define glTexParameteriv p_glTexParameteriv
#define glTexSubImage2D p_glTexSubImage2D
#define glUniform1f p_glUniform1f
#define glUniform1i p_glUniform1i
#define glUniform1iv p_glUniform1iv
#define glUniform2f p_glUniform2f
#define glUniform4f p_glUniform4f
#define glUniformBlockBinding p_glUniformBlockBinding
#define glUnmapBuffer p_glUnmapBuffer
#define glUseProgram p_glUseProgram
#define glViewport p_glViewport
#define glClipControl p_glClipControl
#define glDebugMessageCallback p_glDebugMessageCallback

#define FRAMES 3
#define GFX_PROBES 8 /* reads of one surface per frame that keep their own history */
#define GFX_READBACKS ((FRAMES + 1) * GFX_PROBES)
#define RING_CHUNK (8u << 20)
#define UNIT_STREAM0 8 /* texture units: 0..7 the stages, 8..11 the vertex streams, 15 uploads and reads */
#define UNIT_SCRATCH 15
#define NUNITS 16

/* D3DFORMAT values the back end maps */
enum
{
    F_A8R8G8B8 = 21,
    F_X8R8G8B8 = 22,
    F_R5G6B5 = 23,
    F_X1R5G5B5 = 24,
    F_A1R5G5B5 = 25,
    F_A4R4G4B4 = 26,
    F_A8 = 28,
    F_L8 = 50,
    F_A8L8 = 51,
    F_V8U8 = 60,
    F_D24S8 = 75,
    F_D24X8 = 77,
    F_D16 = 80,
};
#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

struct GfxTex
{
    GLuint id;
    GLenum target; /* GL_TEXTURE_2D or GL_TEXTURE_CUBE_MAP */
    GLenum ifmt, pfmt, ptype; /* internal format; D3D's bytes as GL pixel data (format, type) */
    int type, use;
    uint32_t fmt, w, h, levels; /* D3D's sizes */
    uint32_t faces;
    uint32_t block;       /* bytes per 4x4 block for the compressed formats, else 0 */
    uint32_t bpp;         /* bytes per texel in D3D's layout */
    int has_stencil, x8, renderable;
    /* asynchronous readbacks (gfx_tex_read_async): pixel buffers and the frame each was recorded in */
    GLuint rb[GFX_READBACKS];
    uint32_t rb_size[GFX_READBACKS];
    uint64_t rb_serial[GFX_READBACKS];
    uint32_t rb_face[GFX_READBACKS], rb_level[GFX_READBACKS], rb_index[GFX_READBACKS];
    uint64_t rb_frame; /* the frame the reads below were counted in */
    uint32_t rb_count; /* reads of this surface so far in that frame */
};

struct GfxBuf
{
    GLuint buf, tbo; /* the buffer, and the R32UI buffer texture over it the vertex functions read */
    uint32_t size;
};

typedef struct Chunk
{
    GLuint buf, tbo;
    uint8_t* cpu;          /* the chunk's bytes, written here and flushed before a draw reads them */
    uint32_t used, flushed, size;
} Chunk;

typedef struct Frame
{
    Chunk* chunks;
    uint32_t nchunks, cur;
    GLsync fence;    /* set at the frame's Present */
    uint64_t serial; /* the frame the fence ends */
    GLuint query;    /* GPU time (profile) */
    int timed;
} Frame;

static SDL_GLContext g_ctx;
static SDL_Window* g_window; /* what the context draws to: the game's window, or g_own_window */
static SDL_Window* g_own_window;
static Frame g_frames[FRAMES];
static uint32_t g_frame;       /* index into g_frames */
static uint64_t g_serial = 1;  /* the frame being recorded */
static uint64_t g_completed;   /* the last frame the GPU finished */
static int g_frame_open;
static uint64_t g_gpu_ns;      /* GPU time of finished frames (profile) */
static GLint g_ubo_align = 256;
static int g_clip01;           /* glClipControl: D3D's clip z as it is */
static int g_aniso, g_mirror_once;

static GfxTex* g_rt;
static uint32_t g_rt_face, g_rt_level;
static GfxTex* g_ds;
static int g_targets_bound;
static GLuint g_vao, g_dummy_2d, g_dummy_cube, g_dummy_buf, g_dummy_tbo;
static GLuint g_blit_fbo[2];

/* the window */
static int g_vsync;
static GLuint g_present_prog, g_overlay_prog, g_present_samp;
static GLint g_ov_rect, g_ov_scale, g_ov_n, g_ov_size, g_ov_text;
/* the frame-rate overlay: presents counted over half-second windows */
static int g_overlay = 1;
static double g_fps_since;
static uint32_t g_fps_frames;
static char g_fps_text[32] = "-- FPS";

/* --- small hash maps (key bytes -> pointer) ------------------------------------------------------------ */
typedef struct MapEnt
{
    uint64_t hash;
    void* key;
    size_t klen;
    void* obj;
} MapEnt;

typedef struct Map
{
    MapEnt* e;
    uint32_t cap, n;
} Map;

static uint64_t fnv(const void* p, size_t n)
{
    const uint8_t* b = (const uint8_t*)p;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i)
        h = (h ^ b[i]) * 1099511628211ull;
    return h ? h : 1;
}

static MapEnt* map_find(Map* m, const void* key, size_t klen, uint64_t h)
{
    if (!m->cap)
        return NULL;
    for (uint32_t i = (uint32_t)h & (m->cap - 1);; i = (i + 1) & (m->cap - 1))
    {
        MapEnt* e = &m->e[i];
        if (!e->hash)
            return NULL;
        if (e->hash == h && e->klen == klen && !memcmp(e->key, key, klen))
            return e;
    }
}

static void* map_get(Map* m, const void* key, size_t klen)
{
    MapEnt* e = map_find(m, key, klen, fnv(key, klen));
    return e ? e->obj : NULL;
}

static void map_put(Map* m, const void* key, size_t klen, void* obj)
{
    if ((m->n + 1) * 2 > m->cap)
    {
        Map old = *m;
        m->cap = m->cap ? m->cap * 2 : 256;
        m->e = (MapEnt*)calloc(m->cap, sizeof *m->e);
        m->n = 0;
        for (uint32_t i = 0; i < old.cap; ++i)
            if (old.e[i].hash)
            {
                uint32_t j = (uint32_t)old.e[i].hash & (m->cap - 1);
                while (m->e[j].hash)
                    j = (j + 1) & (m->cap - 1);
                m->e[j] = old.e[i];
                m->n++;
            }
        free(old.e);
    }
    uint64_t h = fnv(key, klen);
    uint32_t j = (uint32_t)h & (m->cap - 1);
    while (m->e[j].hash)
        j = (j + 1) & (m->cap - 1);
    m->e[j].hash = h;
    m->e[j].key = malloc(klen);
    memcpy(m->e[j].key, key, klen);
    m->e[j].klen = klen;
    m->e[j].obj = obj;
    m->n++;
}

static Map g_progs, g_samplers;

/* --- the frame profile ------------------------------------------------------------------------------------ */
int gfx_profiling;

typedef struct Prof
{
    uint64_t frames, draws, bytes, pipelines, front_ns, draw_ns, sem_ns, drawable_ns, present_ns, last_ns, since_ns;
    uint64_t skips[GFX_NSKIPS];
    uint64_t shim_ns, probe_ns;
} Prof;

static Prof g_prof;

uint64_t gfx_now_ns(void) { return SDL_GetTicksNS(); }
void gfx_prof_front(uint64_t ns) { g_prof.front_ns += ns; }
void gfx_prof_skip(int reason) { if (reason >= 0 && reason < GFX_NSKIPS) g_prof.skips[reason]++; }

static SDL_ThreadID g_present_thread;

void gfx_prof_shim(uint64_t ns)
{
    if (SDL_GetCurrentThreadID() == g_present_thread)
        g_prof.shim_ns += ns;
}

/* at each Present: every two seconds, the average frame and where it went (gfx_d3d12.c's format) */
static void prof_frame(uint64_t present_start)
{
    uint64_t now = gfx_now_ns();
    g_prof.present_ns += now - present_start;
    g_prof.frames++;
    if (!g_prof.since_ns)
        g_prof.since_ns = now;
    if (now - g_prof.since_ns < 2000000000ull)
        return;
    double f = (double)g_prof.frames, ms = 1e-6 / f;
    double frame = (double)(now - g_prof.since_ns) * ms;
    double front = (double)g_prof.front_ns * ms, enc = (double)g_prof.draw_ns * ms;
    double sem = (double)g_prof.sem_ns * ms, drw = (double)g_prof.drawable_ns * ms, pres = (double)g_prof.present_ns * ms;
    double shims = (double)g_prof.shim_ns * ms, probe = (double)g_prof.probe_ns * ms;
    double gpu = (double)g_gpu_ns * ms;
    g_gpu_ns = 0;
    if (shims > 0)
        fprintf(stderr,
            "[gfx] %.1f fps: frame %.2f ms = game code %.2f + API calls %.2f (draws %.2f [encode %.2f], probe wait %.2f, "
            "present %.2f [swap %.2f], other %.2f) | GPU %.2f ms | %.0f draws, %.0f KB up, %llu new pipelines\n",
            f / ((double)(now - g_prof.since_ns) * 1e-9), frame, frame - shims, shims, front, enc, probe, pres, drw,
            shims - front - probe - pres, gpu, (double)g_prof.draws / f, (double)g_prof.bytes / f / 1024.0,
            (unsigned long long)g_prof.pipelines);
    else
        fprintf(stderr,
            "[gfx] %.1f fps: frame %.2f ms = game %.2f + d3d %.2f (encode %.2f) + present %.2f (gpu wait %.2f, swap %.2f) | "
            "GPU %.2f ms | %.0f draws, %.0f KB up, %llu new pipelines\n",
            f / ((double)(now - g_prof.since_ns) * 1e-9), frame, frame - front - pres, front, enc, pres, sem, drw, gpu,
            (double)g_prof.draws / f, (double)g_prof.bytes / f / 1024.0, (unsigned long long)g_prof.pipelines);
    static const char* const WHY[GFX_NSKIPS] = { "no shader", "stream>=4", "no position", "no buffer data", "range past buffer",
        "no indices", "pipeline not ready", "no target" };
    int any = 0;
    for (int i = 0; i < GFX_NSKIPS; ++i)
        if (g_prof.skips[i])
            fprintf(stderr, "%s%s %llu", any++ ? ", " : "[gfx]   skipped draws (2 s): ", WHY[i], (unsigned long long)g_prof.skips[i]);
    if (any)
        fprintf(stderr, "\n");
    memset(&g_prof, 0, sizeof g_prof);
    g_prof.since_ns = now;
}

static uint32_t g_failures;

uint32_t gfx_failures(void) { return g_failures; }

/* --- the context on this thread ---------------------------------------------------------------------------- */
static SDL_ThreadID g_gl_thread;

/* whether there is a device; if so its context is current here */
static int ready(void)
{
    if (!g_ctx)
        return 0;
    if (SDL_GL_GetCurrentContext() != g_ctx)
    {
        SDL_ThreadID self = SDL_GetCurrentThreadID();
        if (g_gl_thread && self != g_gl_thread)
            fprintf(stderr, "[recomp] gfx: the GL context moves to thread %llu\n", (unsigned long long)self);
        if (!SDL_GL_MakeCurrent(g_window, g_ctx))
        {
            fprintf(stderr, "[recomp] gfx: SDL_GL_MakeCurrent: %s\n", SDL_GetError());
            return 0;
        }
        g_gl_thread = self;
    }
    return 1;
}

/* --- state GL keeps, set only where it changes ------------------------------------------------------------ */
enum
{
    B_2D,
    B_CUBE,
    B_BUFFER,
    B_TARGETS,
};

static struct
{
    GLuint tex[NUNITS][B_TARGETS], samp[NUNITS];
    int unit;
    GLuint prog, fbo, ebo, copy_buf, pack_buf;
    int blend;
    GLenum bsrc, bdst, bsrca, bdsta, bop;
    uint8_t cmask;
    int ztest, zwrite;
    GLenum zfunc;
    int stencil;
    GLenum sfunc, sfail, szfail, spass;
    GLint sref;
    GLuint sread, swrite;
    int cull;
    GLenum cullface, poly;
    int offset;
    float ofs_factor, ofs_units;
    int scissor;
    GLint vp[4];
    float zmin, zmax;
} g_st;

/* after anything that binds behind the cache's back: everything is bound again */
static void state_forget(void)
{
    memset(&g_st, 0xFF, sizeof g_st);
    g_targets_bound = 0;
}

static GLenum tex_target(int b)
{
    return b == B_2D ? GL_TEXTURE_2D : b == B_CUBE ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_BUFFER;
}

static void bind_tex(int unit, int b, GLuint id)
{
    if (g_st.tex[unit][b] == id)
        return;
    if (g_st.unit != unit)
        glActiveTexture(GL_TEXTURE0 + (GLenum)unit), g_st.unit = unit;
    glBindTexture(tex_target(b), id);
    g_st.tex[unit][b] = id;
}

static void bind_samp(int unit, GLuint s)
{
    if (g_st.samp[unit] != s)
        glBindSampler((GLuint)unit, s), g_st.samp[unit] = s;
}

/* a texture on the scratch unit, which is made the active one: for the calls that act on "the bound
 * texture" (uploads, reads, parameters) */
static void scratch_tex(int b, GLuint id)
{
    bind_tex(UNIT_SCRATCH, b, id);
    if (g_st.unit != UNIT_SCRATCH)
        glActiveTexture(GL_TEXTURE0 + UNIT_SCRATCH), g_st.unit = UNIT_SCRATCH;
}

/* a texture deleted is unbound everywhere: so in the cache */
static void forget_tex(GLuint id)
{
    for (int u = 0; u < NUNITS; ++u)
        for (int b = 0; b < B_TARGETS; ++b)
            if (g_st.tex[u][b] == id)
                g_st.tex[u][b] = 0;
}

static void forget_buf(GLuint id)
{
    if (g_st.ebo == id)
        g_st.ebo = 0;
    if (g_st.copy_buf == id)
        g_st.copy_buf = 0;
    if (g_st.pack_buf == id)
        g_st.pack_buf = 0;
}

static void use_prog(GLuint p)
{
    if (g_st.prog != p)
        glUseProgram(p), g_st.prog = p;
}

static void bind_fbo(GLuint f)
{
    if (g_st.fbo != f)
        glBindFramebuffer(GL_FRAMEBUFFER, f), g_st.fbo = f;
}

static void bind_copy_buf(GLuint b)
{
    if (g_st.copy_buf != b)
        glBindBuffer(GL_COPY_WRITE_BUFFER, b), g_st.copy_buf = b;
}

static void bind_pack_buf(GLuint b)
{
    if (g_st.pack_buf != b)
        glBindBuffer(GL_PIXEL_PACK_BUFFER, b), g_st.pack_buf = b;
}

static void enable(GLenum cap, int* cur, int on)
{
    on = on != 0;
    if (*cur != on)
    {
        if (on)
            glEnable(cap);
        else
            glDisable(cap);
        *cur = on;
    }
}

static void set_scissor(int on) { enable(GL_SCISSOR_TEST, &g_st.scissor, on); }

static void set_color_mask(uint8_t m)
{
    if (g_st.cmask != m)
        glColorMask((m & 1) != 0, (m & 2) != 0, (m & 4) != 0, (m & 8) != 0), g_st.cmask = m;
}

static void set_depth_write(int on)
{
    on = on != 0;
    if (g_st.zwrite != on)
        glDepthMask(on ? GL_TRUE : GL_FALSE), g_st.zwrite = on;
}

static void set_stencil_write(GLuint m)
{
    if (g_st.swrite != m)
        glStencilMask(m), g_st.swrite = m;
}

static void set_viewport_px(GLint x, GLint y, GLint w, GLint h)
{
    if (g_st.vp[0] != x || g_st.vp[1] != y || g_st.vp[2] != w || g_st.vp[3] != h)
    {
        glViewport(x, y, w, h);
        g_st.vp[0] = x, g_st.vp[1] = y, g_st.vp[2] = w, g_st.vp[3] = h;
    }
}

static void set_depth_range(float zmin, float zmax)
{
    if (g_st.zmin != zmin || g_st.zmax != zmax)
        glDepthRangef(zmin, zmax), g_st.zmin = zmin, g_st.zmax = zmax;
}

/* --- frames and the upload ring ---------------------------------------------------------------------------- */
/* the frames whose fences have passed, without waiting */
static uint64_t completed(void)
{
    for (int i = 0; i < FRAMES; ++i)
    {
        Frame* f = &g_frames[i];
        if (f->fence && glClientWaitSync(f->fence, 0, 0) != GL_TIMEOUT_EXPIRED)
        {
            glDeleteSync(f->fence);
            f->fence = NULL;
            if (f->serial > g_completed)
                g_completed = f->serial;
        }
    }
    return g_completed;
}

static void wait_frame(Frame* f)
{
    if (!f->fence)
        return;
    while (glClientWaitSync(f->fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull) == GL_TIMEOUT_EXPIRED)
        ;
    glDeleteSync(f->fence);
    f->fence = NULL;
    if (f->serial > g_completed)
        g_completed = f->serial;
}

static void frame_begin(void)
{
    if (g_frame_open)
        return;
    Frame* f = &g_frames[g_frame];
    uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
    wait_frame(f);
    if (gfx_profiling)
    {
        g_prof.sem_ns += gfx_now_ns() - t0;
        if (f->timed)
        {
            GLuint64 ns = 0;
            glGetQueryObjectui64v(f->query, GL_QUERY_RESULT, &ns);
            g_gpu_ns += ns;
        }
    }
    f->timed = 0;
    for (uint32_t i = 0; i < f->nchunks; ++i)
        f->chunks[i].used = f->chunks[i].flushed = 0;
    f->cur = 0;
    if (gfx_profiling)
    {
        if (!f->query)
            glGenQueries(1, &f->query);
        glBeginQuery(GL_TIME_ELAPSED, f->query);
        f->timed = 1;
    }
    g_frame_open = 1;
}

static GLuint make_tbo(GLuint buf)
{
    GLuint t;
    glGenTextures(1, &t);
    scratch_tex(B_BUFFER, t);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, buf);
    return t;
}

static GLuint make_buffer(uint32_t size, const void* data)
{
    GLuint b;
    glGenBuffers(1, &b);
    bind_copy_buf(b);
    glBufferData(GL_COPY_WRITE_BUFFER, size, data, GL_DYNAMIC_DRAW);
    return b;
}

typedef struct Alloc
{
    uint8_t* cpu;
    GLuint buf, tbo;
    uint32_t off;
} Alloc;

/* allocations too large for the ring: a buffer each, filled at the flush, deleted after the draw */
typedef struct Big
{
    GLuint buf, tbo;
    uint8_t* cpu;
    uint32_t size;
} Big;

static Big g_big[16];
static uint32_t g_nbig;

/* n bytes of this frame's ring */
static Alloc ring(size_t n, size_t align)
{
    frame_begin();
    Alloc a = { 0 };
    g_prof.bytes += n;
    if (n > RING_CHUNK / 2)
    {
        if (g_nbig == sizeof g_big / sizeof g_big[0])
            return a;
        Big* b = &g_big[g_nbig++];
        b->size = (uint32_t)n;
        b->cpu = (uint8_t*)malloc(n);
        b->buf = make_buffer(b->size, NULL);
        b->tbo = make_tbo(b->buf);
        a.cpu = b->cpu, a.buf = b->buf, a.tbo = b->tbo;
        return a;
    }
    Frame* f = &g_frames[g_frame];
    for (;; f->cur++)
    {
        if (f->cur == f->nchunks)
        {
            Chunk c = { 0 };
            c.size = RING_CHUNK;
            c.cpu = (uint8_t*)malloc(RING_CHUNK);
            c.buf = make_buffer(RING_CHUNK, NULL);
            c.tbo = make_tbo(c.buf);
            f->chunks = (Chunk*)realloc(f->chunks, (f->nchunks + 1) * sizeof(Chunk));
            f->chunks[f->nchunks++] = c;
        }
        Chunk* c = &f->chunks[f->cur];
        uint32_t at = (uint32_t)((c->used + align - 1) & ~(align - 1));
        if (at + n <= c->size)
        {
            c->used = at + (uint32_t)n;
            a.cpu = c->cpu + at, a.buf = c->buf, a.tbo = c->tbo, a.off = at;
            return a;
        }
    }
}

/* what the ring's allocations hold, into their buffers: before the draw that reads them */
static void ring_flush(void)
{
    Frame* f = &g_frames[g_frame];
    for (uint32_t i = 0; i < f->nchunks && i <= f->cur; ++i)
    {
        Chunk* c = &f->chunks[i];
        if (c->used > c->flushed)
        {
            /* unsynchronized: the frame's fence says the GPU is done with this part of the chunk. (A
             * glBufferSubData here costs macOS's GL a wait on the GPU per draw.) */
            bind_copy_buf(c->buf);
            void* m = glMapBufferRange(GL_COPY_WRITE_BUFFER, c->flushed, c->used - c->flushed,
                GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
            if (m)
            {
                memcpy(m, c->cpu + c->flushed, c->used - c->flushed);
                glUnmapBuffer(GL_COPY_WRITE_BUFFER);
            }
            c->flushed = c->used;
        }
    }
    for (uint32_t i = 0; i < g_nbig; ++i)
        if (g_big[i].cpu)
        {
            bind_copy_buf(g_big[i].buf);
            glBufferSubData(GL_COPY_WRITE_BUFFER, 0, g_big[i].size, g_big[i].cpu);
            free(g_big[i].cpu);
            g_big[i].cpu = NULL;
        }
}

/* after the draw: the large allocations go (GL keeps them until the GPU is done) */
static void ring_done(void)
{
    for (uint32_t i = 0; i < g_nbig; ++i)
    {
        free(g_big[i].cpu);
        forget_tex(g_big[i].tbo);
        forget_buf(g_big[i].buf);
        glDeleteTextures(1, &g_big[i].tbo);
        glDeleteBuffers(1, &g_big[i].buf);
    }
    g_nbig = 0;
}

/* --- formats ---------------------------------------------------------------------------------------------- */
static void gl_format(GfxTex* t, GLint swz[4])
{
    swz[0] = GL_RED, swz[1] = GL_GREEN, swz[2] = GL_BLUE, swz[3] = GL_ALPHA;
    t->block = 0, t->bpp = 4, t->renderable = 1;
    t->ifmt = GL_RGBA8, t->pfmt = GL_BGRA, t->ptype = GL_UNSIGNED_INT_8_8_8_8_REV;
    if (t->use == GFX_USE_DEPTH)
    {
        if (t->fmt == F_D16)
            t->ifmt = GL_DEPTH_COMPONENT16, t->pfmt = GL_DEPTH_COMPONENT, t->ptype = GL_UNSIGNED_SHORT, t->bpp = 2;
        else
            t->ifmt = GL_DEPTH24_STENCIL8, t->pfmt = GL_DEPTH_STENCIL, t->ptype = GL_UNSIGNED_INT_24_8, t->has_stencil = 1;
        return;
    }
    switch (t->fmt)
    {
    case F_A8R8G8B8: return;
    case F_X8R8G8B8: swz[3] = GL_ONE; return;
    case F_R5G6B5: t->ifmt = GL_RGB565, t->pfmt = GL_RGB, t->ptype = GL_UNSIGNED_SHORT_5_6_5, t->bpp = 2; return;
    case F_X1R5G5B5: swz[3] = GL_ONE; /* fall through */
    case F_A1R5G5B5: t->ifmt = GL_RGB5_A1, t->ptype = GL_UNSIGNED_SHORT_1_5_5_5_REV, t->bpp = 2; return;
    case F_A4R4G4B4: t->ifmt = GL_RGBA4, t->ptype = GL_UNSIGNED_SHORT_4_4_4_4_REV, t->bpp = 2; return;
    case F_A8:
        t->ifmt = GL_R8, t->pfmt = GL_RED, t->ptype = GL_UNSIGNED_BYTE, t->bpp = 1;
        swz[0] = swz[1] = swz[2] = GL_ZERO, swz[3] = GL_RED;
        return;
    case F_L8:
        t->ifmt = GL_R8, t->pfmt = GL_RED, t->ptype = GL_UNSIGNED_BYTE, t->bpp = 1;
        swz[0] = swz[1] = swz[2] = GL_RED, swz[3] = GL_ONE;
        return;
    case F_A8L8:
        t->ifmt = GL_RG8, t->pfmt = GL_RG, t->ptype = GL_UNSIGNED_BYTE, t->bpp = 2;
        swz[0] = swz[1] = swz[2] = GL_RED, swz[3] = GL_GREEN;
        return;
    case F_V8U8: t->ifmt = GL_RG8_SNORM, t->pfmt = GL_RG, t->ptype = GL_BYTE, t->bpp = 2, t->renderable = 0; return;
    }
    t->renderable = 0;
    if (t->fmt == FOURCC('D', 'X', 'T', '1'))
        t->ifmt = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, t->block = 8;
    else if (t->fmt == FOURCC('D', 'X', 'T', '2') || t->fmt == FOURCC('D', 'X', 'T', '3'))
        t->ifmt = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, t->block = 16;
    else if (t->fmt == FOURCC('D', 'X', 'T', '4') || t->fmt == FOURCC('D', 'X', 'T', '5'))
        t->ifmt = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, t->block = 16;
    else
        t->renderable = 1; /* anything else: as A8R8G8B8 */
}

/* the GL target of one face: a cube's faces are GL's, in D3D's order */
static GLenum face_target(const GfxTex* t, uint32_t face)
{
    return t->target == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + face : GL_TEXTURE_2D;
}

static int bslot(const GfxTex* t)
{
    return t->target == GL_TEXTURE_CUBE_MAP ? B_CUBE : B_2D;
}

static void level_size(const GfxTex* t, uint32_t level, uint32_t* w, uint32_t* h)
{
    *w = t->w >> level ? t->w >> level : 1;
    *h = t->h >> level ? t->h >> level : 1;
}

static uint32_t level_bytes(const GfxTex* t, uint32_t w, uint32_t h)
{
    return t->block ? ((w + 3) / 4) * ((h + 3) / 4) * t->block : w * h * t->bpp;
}

/* --- static buffers --------------------------------------------------------------------------------------------- */
GfxBuf* gfx_buf_create(uint32_t size)
{
    if (!ready() || !size)
        return NULL;
    GfxBuf* b = (GfxBuf*)calloc(1, sizeof *b);
    b->size = size;
    b->buf = make_buffer((size + 15) & ~15u, NULL);
    b->tbo = make_tbo(b->buf);
    return b;
}

void gfx_buf_destroy(GfxBuf* b)
{
    if (!b || !ready())
        return;
    forget_tex(b->tbo);
    forget_buf(b->buf);
    glDeleteTextures(1, &b->tbo);
    glDeleteBuffers(1, &b->buf);
    free(b);
}

/* in command order (GL's): draws issued before this read the old contents, draws after the new */
void gfx_buf_upload(GfxBuf* b, const void* data, uint32_t size)
{
    if (!b || !ready())
        return;
    if (size > b->size)
        size = b->size;
    if (!size)
        return;
    g_prof.bytes += size;
    bind_copy_buf(b->buf);
    glBufferSubData(GL_COPY_WRITE_BUFFER, 0, size, data);
}

/* --- textures ------------------------------------------------------------------------------------------------ */
GfxTex* gfx_tex_create(int type, uint32_t fmt, uint32_t w, uint32_t h, uint32_t levels, int use)
{
    if (!ready())
        return NULL;
    GfxTex* t = (GfxTex*)calloc(1, sizeof *t);
    t->type = type, t->use = use, t->fmt = fmt, t->w = w ? w : 1, t->h = h ? h : 1, t->levels = levels ? levels : 1;
    if (type == GFX_TEX_CUBE)
        t->h = t->w;
    t->faces = type == GFX_TEX_CUBE ? 6 : 1;
    t->target = type == GFX_TEX_CUBE ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
    uint32_t most = 1;
    for (uint32_t s = t->w > t->h ? t->w : t->h; s > 1; s >>= 1)
        most++;
    if (t->levels > most)
        t->levels = most;
    GLint swz[4];
    gl_format(t, swz);
    t->x8 = fmt == F_X8R8G8B8;
    while (glGetError() != GL_NO_ERROR)
        ;
    glGenTextures(1, &t->id);
    scratch_tex(bslot(t), t->id);
    for (uint32_t f = 0; f < t->faces; ++f)
        for (uint32_t l = 0; l < t->levels; ++l)
        {
            uint32_t lw, lh;
            level_size(t, l, &lw, &lh);
            if (t->block)
                glCompressedTexImage2D(face_target(t, f), (GLint)l, t->ifmt, (GLsizei)lw, (GLsizei)lh, 0,
                    (GLsizei)level_bytes(t, lw, lh), NULL);
            else
                glTexImage2D(face_target(t, f), (GLint)l, (GLint)t->ifmt, (GLsizei)lw, (GLsizei)lh, 0, t->pfmt, t->ptype, NULL);
        }
    glTexParameteri(t->target, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(t->target, GL_TEXTURE_MAX_LEVEL, (GLint)t->levels - 1);
    if (use != GFX_USE_DEPTH)
        glTexParameteriv(t->target, GL_TEXTURE_SWIZZLE_RGBA, swz);
    GLenum err = glGetError();
    if (err != GL_NO_ERROR)
    {
        fprintf(stderr, "[recomp] gfx: texture %ux%u format %u failed (GL error %04x)\n", w, h, fmt, err);
        forget_tex(t->id);
        glDeleteTextures(1, &t->id);
        free(t);
        return NULL;
    }
    return t;
}

/* framebuffer objects: one per color target level (or face) and depth surface, made on first use */
typedef struct Fbo
{
    GLuint id, color, depth;
    uint32_t face, level;
} Fbo;

static Fbo* g_fbos;
static uint32_t g_nfbos;

static void forget_fbos(GLuint tex)
{
    uint32_t k = 0;
    for (uint32_t i = 0; i < g_nfbos; ++i)
    {
        if (g_fbos[i].color == tex || g_fbos[i].depth == tex)
        {
            if (g_st.fbo == g_fbos[i].id)
                g_st.fbo = (GLuint)-1;
            glDeleteFramebuffers(1, &g_fbos[i].id);
        }
        else
            g_fbos[k++] = g_fbos[i];
    }
    g_nfbos = k;
}

void gfx_tex_destroy(GfxTex* t)
{
    if (!t || !ready())
        return;
    if (g_rt == t)
        g_rt = NULL, g_targets_bound = 0;
    if (g_ds == t)
        g_ds = NULL, g_targets_bound = 0;
    forget_fbos(t->id);
    forget_tex(t->id);
    glDeleteTextures(1, &t->id);
    for (int i = 0; i < GFX_READBACKS; ++i)
        if (t->rb[i])
        {
            forget_buf(t->rb[i]);
            glDeleteBuffers(1, &t->rb[i]);
        }
    free(t);
}

void gfx_tex_upload_rect(GfxTex* t, uint32_t face, uint32_t level, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
    const void* src, uint32_t pitch)
{
    if (!t || !ready() || level >= t->levels || face >= t->faces || !w || !h || t->use == GFX_USE_DEPTH)
        return;
    uint32_t lw, lh;
    level_size(t, level, &lw, &lh);
    if (x >= lw || y >= lh)
        return;
    if (x + w > lw)
        w = lw - x;
    if (y + h > lh)
        h = lh - y;
    scratch_tex(bslot(t), t->id);
    if (t->block)
    {
        /* whole blocks, rows packed (GL 4.1 reads compressed rows back to back) */
        uint32_t cols = (w + 3) / 4, rows = (h + 3) / 4, row = cols * t->block;
        w = x + cols * 4 < lw ? cols * 4 : lw - x;
        h = y + rows * 4 < lh ? rows * 4 : lh - y;
        const uint8_t* s = (const uint8_t*)src;
        uint8_t* packed = NULL;
        if (pitch != row && rows > 1)
        {
            packed = (uint8_t*)malloc((size_t)row * rows);
            for (uint32_t r = 0; r < rows; ++r)
                memcpy(packed + (size_t)r * row, s + (size_t)r * pitch, row);
            s = packed;
        }
        glCompressedTexSubImage2D(face_target(t, face), (GLint)level, (GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h, t->ifmt,
            (GLsizei)(row * rows), s);
        free(packed);
        g_prof.bytes += (uint64_t)row * rows;
        return;
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(pitch / t->bpp));
    glTexSubImage2D(face_target(t, face), (GLint)level, (GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h, t->pfmt, t->ptype, src);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    g_prof.bytes += (uint64_t)h * pitch;
}

void gfx_tex_upload(GfxTex* t, uint32_t face, uint32_t level, const void* src, uint32_t pitch)
{
    if (!t || level >= t->levels)
        return;
    uint32_t w, h;
    level_size(t, level, &w, &h);
    gfx_tex_upload_rect(t, face, level, 0, 0, w, h, src, pitch);
}

/* rows of a level read back tight -> D3D's pitch */
static void copy_out(const GfxTex* t, const uint8_t* s, uint32_t w, uint32_t h, void* dst, uint32_t pitch)
{
    uint32_t row = w * t->bpp;
    for (uint32_t y = 0; y < h; ++y)
        memcpy((uint8_t*)dst + (size_t)y * pitch, s + (size_t)y * row, row);
}

/* the level into the bound pixel pack buffer (offset 0), or into dst without one */
static void read_level(GfxTex* t, uint32_t face, uint32_t level, void* dst)
{
    scratch_tex(bslot(t), t->id);
    glGetTexImage(face_target(t, face), (GLint)level, t->pfmt, t->ptype, dst);
}

void gfx_tex_read(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch)
{
    if (!t || !ready() || level >= t->levels || face >= t->faces || t->use == GFX_USE_DEPTH || t->block)
        return;
    uint32_t w, h;
    level_size(t, level, &w, &h);
    uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
    uint8_t* tmp = (uint8_t*)malloc((size_t)w * h * t->bpp);
    bind_pack_buf(0);
    read_level(t, face, level, tmp); /* GL waits for the draws before it */
    copy_out(t, tmp, w, h, dst, pitch);
    free(tmp);
    if (gfx_profiling)
        g_prof.probe_ns += gfx_now_ns() - t0;
}

/* A surface read several times a frame (the game reuses one 16x16 target for more than one
 * probe: copy a region, lock, read; copy another, lock, read) keeps each read's history apart:
 * the k-th read this frame gets the k-th read of the newest frame the GPU has finished, never
 * another probe's pixels. */
void gfx_tex_read_async(GfxTex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch)
{
    if (!t || !ready() || level >= t->levels || face >= t->faces || t->use == GFX_USE_DEPTH || t->block)
        return;
    uint32_t w, h;
    level_size(t, level, &w, &h);
    uint32_t size = w * h * t->bpp;
    if (t->rb_frame != g_serial)
        t->rb_frame = g_serial, t->rb_count = 0;
    uint32_t index = t->rb_count++;
    if (index >= GFX_PROBES)
    {
        gfx_tex_read(t, face, level, dst, pitch); /* more reads a frame than we keep apart */
        return;
    }
    uint64_t done = completed();
    int best = -1;
    for (int i = 0; i < GFX_READBACKS; ++i)
        if (t->rb[i] && t->rb_serial[i] && t->rb_serial[i] <= done && t->rb_index[i] == index && t->rb_face[i] == face &&
            t->rb_level[i] == level && (best < 0 || t->rb_serial[i] > t->rb_serial[best]))
            best = i;
    if (best < 0)
        gfx_tex_read(t, face, level, dst, pitch); /* nothing finished for this read yet: wait, once */
    else
    {
        bind_pack_buf(t->rb[best]);
        const uint8_t* p = (const uint8_t*)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, size, GL_MAP_READ_BIT);
        if (p)
        {
            copy_out(t, p, w, h, dst, pitch);
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
    }
    /* this read's copy for a later frame: a slot the GPU is done with, not the one just read */
    int slot = -1;
    for (int i = 0; i < GFX_READBACKS && slot < 0; ++i)
        if (i != best && (!t->rb_serial[i] || t->rb_serial[i] <= done))
            slot = i;
    if (slot < 0)
        return;
    if (!t->rb[slot] || t->rb_size[slot] < size)
    {
        if (!t->rb[slot])
            glGenBuffers(1, &t->rb[slot]);
        bind_pack_buf(t->rb[slot]);
        glBufferData(GL_PIXEL_PACK_BUFFER, size, NULL, GL_STREAM_READ);
        t->rb_size[slot] = size;
    }
    bind_pack_buf(t->rb[slot]);
    read_level(t, face, level, NULL);
    bind_pack_buf(0);
    t->rb_serial[slot] = g_serial, t->rb_face[slot] = face, t->rb_level[slot] = level, t->rb_index[slot] = index;
}

/* a level (face) of a texture as the scratch framebuffer's attachment */
static void attach(GLenum fb, GfxTex* t, uint32_t face, uint32_t level)
{
    if (t->use == GFX_USE_DEPTH)
    {
        glFramebufferTexture2D(fb, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        glFramebufferTexture2D(fb, t->has_stencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, t->id, 0);
    }
    else
    {
        glFramebufferTexture2D(fb, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
        glFramebufferTexture2D(fb, GL_COLOR_ATTACHMENT0, face_target(t, face), t->id, (GLint)level);
    }
}

void gfx_copy(GfxTex* src, uint32_t sface, uint32_t slevel, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h, GfxTex* dst,
    uint32_t dface, uint32_t dlevel, uint32_t dx, uint32_t dy)
{
    if (!src || !dst || src == dst || src->ifmt != dst->ifmt || !w || !h || !ready())
        return;
    if (slevel >= src->levels || dlevel >= dst->levels || sface >= src->faces || dface >= dst->faces)
        return;
    if (!src->renderable || !dst->renderable)
    {
        /* formats a framebuffer cannot hold (DXT, V8U8): through the CPU */
        if (src->use == GFX_USE_DEPTH)
            return;
        uint32_t lw, lh;
        level_size(src, slevel, &lw, &lh);
        uint32_t bytes = level_bytes(src, lw, lh);
        uint8_t* tmp = (uint8_t*)malloc(bytes);
        bind_pack_buf(0);
        scratch_tex(bslot(src), src->id);
        if (src->block)
        {
            glGetCompressedTexImage(face_target(src, sface), (GLint)slevel, tmp);
            uint32_t row = (lw + 3) / 4 * src->block;
            gfx_tex_upload_rect(dst, dface, dlevel, dx, dy, w, h, tmp + (size_t)(sy / 4) * row + (size_t)(sx / 4) * src->block, row);
        }
        else
        {
            glGetTexImage(face_target(src, sface), (GLint)slevel, src->pfmt, src->ptype, tmp);
            uint32_t row = lw * src->bpp;
            gfx_tex_upload_rect(dst, dface, dlevel, dx, dy, w, h, tmp + (size_t)sy * row + (size_t)sx * src->bpp, row);
        }
        free(tmp);
        return;
    }
    if (!g_blit_fbo[0])
        glGenFramebuffers(2, g_blit_fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_blit_fbo[0]);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_blit_fbo[1]);
    g_st.fbo = (GLuint)-1, g_targets_bound = 0;
    attach(GL_READ_FRAMEBUFFER, src, sface, slevel);
    attach(GL_DRAW_FRAMEBUFFER, dst, dface, dlevel);
    GLenum color = src->use == GFX_USE_DEPTH ? GL_NONE : GL_COLOR_ATTACHMENT0;
    glReadBuffer(color);
    glDrawBuffer(color);
    set_scissor(0); /* blits are scissored */
    GLbitfield mask = src->use != GFX_USE_DEPTH ? GL_COLOR_BUFFER_BIT : GL_DEPTH_BUFFER_BIT | (src->has_stencil ? GL_STENCIL_BUFFER_BIT : 0);
    glBlitFramebuffer((GLint)sx, (GLint)sy, (GLint)(sx + w), (GLint)(sy + h), (GLint)dx, (GLint)dy, (GLint)(dx + w), (GLint)(dy + h),
        mask, GL_NEAREST);
}

/* --- render targets ---------------------------------------------------------------------------------------------- */
static void color_size(uint32_t* w, uint32_t* h)
{
    level_size(g_rt, g_rt_level, w, h);
}

/* the framebuffer for the current targets, bound. D3D8 lets depth be larger than color; GL draws to
 * the area both cover, which is what D3D does. */
static int bind_targets(void)
{
    if (!g_rt)
        return 0;
    if (g_targets_bound)
        return 1;
    GLuint depth = g_ds ? g_ds->id : 0;
    Fbo* f = NULL;
    for (uint32_t i = 0; i < g_nfbos && !f; ++i)
        if (g_fbos[i].color == g_rt->id && g_fbos[i].depth == depth && g_fbos[i].face == g_rt_face && g_fbos[i].level == g_rt_level)
            f = &g_fbos[i];
    if (!f)
    {
        g_fbos = (Fbo*)realloc(g_fbos, (g_nfbos + 1) * sizeof *g_fbos);
        f = &g_fbos[g_nfbos++];
        f->color = g_rt->id, f->depth = depth, f->face = g_rt_face, f->level = g_rt_level;
        glGenFramebuffers(1, &f->id);
        bind_fbo(f->id);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, face_target(g_rt, g_rt_face), g_rt->id, (GLint)g_rt_level);
        if (g_ds)
            glFramebufferTexture2D(GL_FRAMEBUFFER, g_ds->has_stencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                g_ds->id, 0);
        GLenum s = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (s != GL_FRAMEBUFFER_COMPLETE)
            fprintf(stderr, "[recomp] gfx: framebuffer (format %u, depth %u) incomplete: %04x\n", g_rt->fmt, g_ds ? g_ds->fmt : 0, s);
    }
    bind_fbo(f->id);
    g_targets_bound = 1;
    return 1;
}

void gfx_set_targets(GfxTex* color, uint32_t face, uint32_t level, GfxTex* depth)
{
    if (color == g_rt && face == g_rt_face && level == g_rt_level && depth == g_ds)
        return;
    g_rt = color, g_rt_face = face, g_rt_level = level, g_ds = depth;
    g_targets_bound = 0;
}

/* --- programs ------------------------------------------------------------------------------------------------
 * GL has no pipeline objects: a program per vertex and fragment key, compiled and linked in place on
 * first use (a new effect costs its first frame a few milliseconds); the fixed state is set per draw. */
typedef struct LibKey
{
    GfxVsKey vs;
    GfxFsKey fs;
} LibKey;

#define PROG_FAILED ((void*)1)

static int g_sync_pipelines;

void gfx_set_sync_pipelines(int on) { g_sync_pipelines = on; }

/* the source's line numbers beside it, for a compiler message that names them */
static void print_source(const char* src)
{
    int line = 1;
    for (const char* p = src; *p;)
    {
        const char* e = strchr(p, '\n');
        int n = e ? (int)(e - p) : (int)strlen(p);
        fprintf(stderr, "%4d  %.*s\n", line++, n, p);
        p += n + (e ? 1 : 0);
    }
}

static const char* clipz(void)
{
    return g_clip01 ? "#define CLIPZ(p) ((p).z)\n" : "#define CLIPZ(p) (2.0 * (p).z - (p).w)\n";
}

static GLuint compile_stage(GLenum type, const char* defines, const char* src)
{
    GLuint s = glCreateShader(type);
    const char* parts[4] = { "#version 410 core\n", type == GL_VERTEX_SHADER ? "#define VERTEX\n" : "", defines, src };
    glShaderSource(s, 4, parts, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[4096];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        g_failures++;
        fprintf(stderr, "[recomp] gfx: GLSL compile failed (%s): %s\n", type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        print_source(src);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

/* a program from the source of both stages; 0 on failure */
static GLuint link_program(const char* defines, const char* src)
{
    GLuint vs = compile_stage(GL_VERTEX_SHADER, defines, src);
    GLuint fs = vs ? compile_stage(GL_FRAGMENT_SHADER, defines, src) : 0;
    if (!fs)
    {
        if (vs)
            glDeleteShader(vs);
        return 0;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[4096];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        g_failures++;
        fprintf(stderr, "[recomp] gfx: GLSL link failed: %s\n", log);
        glDeleteProgram(p);
        return 0;
    }
    /* the bindings GLSL 4.10 cannot declare: stage textures on units 0-7, streams on 8-11, uniforms at 0 */
    use_prog(p);
    char name[8];
    for (int i = 0; i < 8; ++i)
    {
        snprintf(name, sizeof name, "tx%d", i);
        GLint loc = glGetUniformLocation(p, name);
        if (loc >= 0)
            glUniform1i(loc, i);
    }
    for (int s = 0; s < GFX_NSTREAMS; ++s)
    {
        snprintf(name, sizeof name, "s%d", s);
        GLint loc = glGetUniformLocation(p, name);
        if (loc >= 0)
            glUniform1i(loc, UNIT_STREAM0 + s);
    }
    GLuint block = glGetUniformBlockIndex(p, "CU");
    if (block != GL_INVALID_INDEX)
        glUniformBlockBinding(p, block, 0);
    return p;
}

static GLuint program(const GfxDraw* d)
{
    LibKey k;
    memset(&k, 0, sizeof k);
    k.vs = d->vs, k.fs = d->fs;
    void* e = map_get(&g_progs, &k, sizeof k);
    if (!e)
    {
        GLuint p = 0;
        char* src = gfx_glsl_generate(&k.vs, &k.fs, d->vs.prog ? d->vs_tokens : NULL, d->fs.prog ? d->ps_tokens : NULL);
        if (src)
        {
            p = link_program(clipz(), src);
            free(src);
        }
        else
            g_failures++;
        e = p ? (void*)(uintptr_t)p : PROG_FAILED;
        map_put(&g_progs, &k, sizeof k, e);
        g_prof.pipelines++;
    }
    return e == PROG_FAILED ? 0 : (GLuint)(uintptr_t)e;
}

static GLenum blend_factor(uint32_t f, int x8, int alpha)
{
    switch (f)
    {
    case 1: return GL_ZERO;
    case 2: return GL_ONE;
    case 3: return alpha ? GL_SRC_ALPHA : GL_SRC_COLOR; /* the alpha factors take no colors */
    case 4: return alpha ? GL_ONE_MINUS_SRC_ALPHA : GL_ONE_MINUS_SRC_COLOR;
    case 5: return GL_SRC_ALPHA;
    case 6: return GL_ONE_MINUS_SRC_ALPHA;
    case 7: return x8 ? GL_ONE : GL_DST_ALPHA;
    case 8: return x8 ? GL_ZERO : GL_ONE_MINUS_DST_ALPHA;
    case 9: return alpha ? GL_DST_ALPHA : GL_DST_COLOR;
    case 10: return alpha ? GL_ONE_MINUS_DST_ALPHA : GL_ONE_MINUS_DST_COLOR;
    case 11: return GL_SRC_ALPHA_SATURATE;
    default: return GL_ONE;
    }
}

static GLenum blend_op(uint32_t op)
{
    switch (op)
    {
    case 2: return GL_FUNC_SUBTRACT;
    case 3: return GL_FUNC_REVERSE_SUBTRACT;
    case 4: return GL_MIN;
    case 5: return GL_MAX;
    default: return GL_FUNC_ADD;
    }
}

static GLenum compare(uint32_t f)
{
    switch (f)
    {
    case 1: return GL_NEVER;
    case 2: return GL_LESS;
    case 3: return GL_EQUAL;
    case 4: return GL_LEQUAL;
    case 5: return GL_GREATER;
    case 6: return GL_NOTEQUAL;
    case 7: return GL_GEQUAL;
    default: return GL_ALWAYS;
    }
}

static GLenum stencil_op(uint32_t op)
{
    switch (op)
    {
    case 2: return GL_ZERO;
    case 3: return GL_REPLACE;
    case 4: return GL_INCR;
    case 5: return GL_DECR;
    case 6: return GL_INVERT;
    case 7: return GL_INCR_WRAP;
    case 8: return GL_DECR_WRAP;
    default: return GL_KEEP;
    }
}

/* blending, depth-stencil and rasterizer state of a draw: gfx_d3d12.c's pipeline description, set */
static void set_fixed_state(const GfxDraw* d)
{
    const GfxPipeKey* pk = &d->pipe;
    set_color_mask(pk->write_mask & 15);
    enable(GL_BLEND, &g_st.blend, pk->blend);
    if (pk->blend)
    {
        uint32_t sf = pk->src, df = pk->dst;
        if (sf == 12) /* BOTHSRCALPHA */
            sf = 5, df = 6;
        else if (sf == 13) /* BOTHINVSRCALPHA */
            sf = 6, df = 5;
        int x8 = g_rt->x8;
        GLenum s = blend_factor(sf, x8, 0), sa = blend_factor(sf, x8, 1), dd = blend_factor(df, x8, 0), da = blend_factor(df, x8, 1);
        if (s != g_st.bsrc || sa != g_st.bsrca || dd != g_st.bdst || da != g_st.bdsta)
        {
            glBlendFuncSeparate(s, dd, sa, da);
            g_st.bsrc = s, g_st.bsrca = sa, g_st.bdst = dd, g_st.bdsta = da;
        }
        GLenum op = blend_op(pk->op);
        if (op != g_st.bop)
            glBlendEquation(op), g_st.bop = op;
    }
    /* D3D's front faces are clockwise on screen: counterclockwise here, drawn upside down */
    int cull = d->cull == 2 || d->cull == 3;
    enable(GL_CULL_FACE, &g_st.cull, cull);
    GLenum face = d->cull == 2 ? GL_FRONT : GL_BACK;
    if (cull && face != g_st.cullface)
        glCullFace(face), g_st.cullface = face;
    GLenum poly = d->fill == 2 ? GL_LINE : GL_FILL;
    if (poly != g_st.poly)
        glPolygonMode(GL_FRONT_AND_BACK, poly), g_st.poly = poly;

    const GfxDepthKey* dk = &d->depth;
    int ds = g_ds != NULL;
    enable(GL_DEPTH_TEST, &g_st.ztest, ds && dk->zenable);
    if (ds && dk->zenable)
    {
        GLenum f = compare(dk->zfunc);
        if (f != g_st.zfunc)
            glDepthFunc(f), g_st.zfunc = f;
        set_depth_write(dk->zwrite);
    }
    int zbias = ds && dk->zenable ? d->zbias : 0;
    enable(GL_POLYGON_OFFSET_FILL, &g_st.offset, zbias != 0);
    if (zbias && (g_st.ofs_factor != -(float)zbias * 0.5f || g_st.ofs_units != -(float)zbias))
    {
        g_st.ofs_factor = -(float)zbias * 0.5f, g_st.ofs_units = -(float)zbias;
        glPolygonOffset(g_st.ofs_factor, g_st.ofs_units);
    }
    int st = ds && g_ds->has_stencil && dk->stencil;
    enable(GL_STENCIL_TEST, &g_st.stencil, st);
    if (st)
    {
        GLenum f = compare(dk->sfunc);
        if (f != g_st.sfunc || (GLint)d->stencil_ref != g_st.sref || dk->sread != g_st.sread)
        {
            glStencilFunc(f, (GLint)d->stencil_ref, dk->sread);
            g_st.sfunc = f, g_st.sref = (GLint)d->stencil_ref, g_st.sread = dk->sread;
        }
        GLenum a = stencil_op(dk->sfail), b = stencil_op(dk->szfail), c = stencil_op(dk->spass);
        if (a != g_st.sfail || b != g_st.szfail || c != g_st.spass)
        {
            glStencilOp(a, b, c);
            g_st.sfail = a, g_st.szfail = b, g_st.spass = c;
        }
        set_stencil_write(dk->swrite);
    }
    set_scissor(0);
}

static GLenum address(uint32_t a)
{
    switch (a)
    {
    case 2: return GL_MIRRORED_REPEAT;
    case 3: return GL_CLAMP_TO_EDGE;
    case 4: return GL_CLAMP_TO_BORDER;
    case 5: return g_mirror_once ? GL_MIRROR_CLAMP_TO_EDGE : GL_MIRRORED_REPEAT;
    default: return GL_REPEAT;
    }
}

/* the sampler object for a key: made on first use, kept */
static GLuint sampler(const GfxSampler* k)
{
    void* s = map_get(&g_samplers, k, sizeof *k);
    if (s)
        return (GLuint)(uintptr_t)s;
    GLuint id;
    glGenSamplers(1, &id);
    int aniso = (k->min == 3 || k->mag == 3) && k->max_aniso > 1 && g_aniso;
    int lin_min = k->min >= 2 || aniso, lin_mag = k->mag >= 2 || aniso;
    GLenum min;
    if (k->mip == 0 && k->max_level == 0) /* no mipmapping: the first level */
        min = lin_min ? GL_LINEAR : GL_NEAREST;
    else if (k->mip == 0) /* ... or the one level MAXMIPLEVEL names (the LOD clamp below) */
        min = lin_min ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
    else if (k->mip >= 2 || aniso)
        min = lin_min ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
    else
        min = lin_min ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
    glSamplerParameteri(id, GL_TEXTURE_MIN_FILTER, (GLint)min);
    glSamplerParameteri(id, GL_TEXTURE_MAG_FILTER, lin_mag ? GL_LINEAR : GL_NEAREST);
    if (aniso)
        glSamplerParameterf(id, GL_TEXTURE_MAX_ANISOTROPY_EXT, (float)(k->max_aniso > 16 ? 16 : k->max_aniso));
    glSamplerParameteri(id, GL_TEXTURE_WRAP_S, (GLint)address(k->addr_u));
    glSamplerParameteri(id, GL_TEXTURE_WRAP_T, (GLint)address(k->addr_v));
    glSamplerParameteri(id, GL_TEXTURE_WRAP_R, (GLint)address(k->addr_w));
    float border[4] = { ((k->border >> 16) & 255) / 255.0f, ((k->border >> 8) & 255) / 255.0f, (k->border & 255) / 255.0f,
        (k->border >> 24) / 255.0f };
    glSamplerParameterfv(id, GL_TEXTURE_BORDER_COLOR, border);
    /* GL clamps the LOD before it chooses between the min and mag filters: a clamp to 0 would make
     * every minified sample a magnified one. The unmipmapped filters read the first level anyway. */
    glSamplerParameterf(id, GL_TEXTURE_MIN_LOD, (float)k->max_level);
    float max_lod = k->mip == 0 && k->max_level ? (float)k->max_level : 1000.0f;
    if (k->lod_cap && max_lod > (float)(k->lod_cap - 1)) /* glyph sheets: not their smallest mips (gfx_metal.m) */
        max_lod = (float)(k->lod_cap - 1);
    glSamplerParameterf(id, GL_TEXTURE_MAX_LOD, max_lod);
    map_put(&g_samplers, k, sizeof *k, (void*)(uintptr_t)id);
    return id;
}

/* --- drawing --------------------------------------------------------------------------------------------------- */
static void set_viewport(const uint32_t vp[6])
{
    float zmin, zmax;
    memcpy(&zmin, &vp[4], 4);
    memcpy(&zmax, &vp[5], 4);
    uint32_t w, h;
    color_size(&w, &h);
    uint32_t x = vp[0], y = vp[1], vw = vp[2], vh = vp[3];
    if (x > w)
        x = w;
    if (y > h)
        y = h;
    if (x + vw > w)
        vw = w - x;
    if (y + vh > h)
        vh = h - y;
    set_viewport_px((GLint)x, (GLint)y, (GLint)vw, (GLint)vh); /* D3D's rows: the targets hold them top first */
    set_depth_range(zmin, zmax);
}

/* vertex count for a D3D primitive count */
static uint32_t vertex_count(uint32_t prim, uint32_t n)
{
    switch (prim)
    {
    case GFX_POINTLIST: return n;
    case GFX_LINELIST: return n * 2;
    case GFX_LINESTRIP: return n + 1;
    case GFX_TRIANGLELIST: return n * 3;
    case GFX_TRIANGLESTRIP:
    case GFX_TRIANGLEFAN: return n + 2;
    default: return 0;
    }
}

static GLenum mode(uint32_t prim)
{
    switch (prim)
    {
    case GFX_POINTLIST: return GL_POINTS;
    case GFX_LINELIST: return GL_LINES;
    case GFX_LINESTRIP: return GL_LINE_STRIP;
    case GFX_TRIANGLESTRIP: return GL_TRIANGLE_STRIP;
    case GFX_TRIANGLEFAN: return GL_TRIANGLE_FAN;
    default: return GL_TRIANGLES;
    }
}

static void bind_ebo(GLuint b)
{
    if (g_st.ebo != b)
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b), g_st.ebo = b;
}

static void draw_encode(const GfxDraw* d);

void gfx_draw(const GfxDraw* d)
{
    if (!d->count || !ready())
        return;
    uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
    draw_encode(d);
    if (gfx_profiling)
        g_prof.draw_ns += gfx_now_ns() - t0, g_prof.draws++;
}

static void draw_encode(const GfxDraw* d)
{
    if (!bind_targets())
    {
        gfx_prof_skip(GFX_SKIP_NO_TARGET);
        return;
    }
    GLuint p = program(d);
    if (!p)
    {
        gfx_prof_skip(GFX_SKIP_PIPELINE); /* failed to build */
        return;
    }
    use_prog(p);
    set_fixed_state(d);
    set_viewport(d->vp);
    for (int i = 0; i < 8; ++i)
    {
        int wanted = d->fs.prog || i < d->fs.nstages ? d->fs.st[i].tex : 0;
        if (!wanted)
            continue;
        GfxTex* t = d->tex[i];
        int cube = wanted == 2;
        GLuint id = cube ? g_dummy_cube : g_dummy_2d;
        if (t && t != g_rt && t->use != GFX_USE_DEPTH && cube == (t->type == GFX_TEX_CUBE))
            id = t->id; /* else unbound, or the target being drawn to: reads as zeros */
        bind_tex(i, cube ? B_CUBE : B_2D, id);
        bind_samp(i, sampler(&d->samp[i]));
    }

    /* the streams: a static buffer where it is, else a copy in the ring; where each starts goes into
     * the offsets of the registers it feeds */
    int32_t base[GFX_NSTREAMS] = { 0 };
    for (int s = 0; s < GFX_NSTREAMS; ++s)
    {
        GLuint tbo = g_dummy_tbo;
        if (d->buf[s])
            tbo = d->buf[s]->tbo, base[s] = (int32_t)d->buf_off[s];
        else if (d->data[s] && d->size[s])
        {
            Alloc v = ring(d->size[s] + 16, 16); /* the slack: a last element read whole */
            if (!v.cpu)
                return;
            memcpy(v.cpu, d->data[s], d->size[s]);
            tbo = v.tbo, base[s] = (int32_t)v.off;
        }
        bind_tex(UNIT_STREAM0 + s, B_BUFFER, tbo);
    }

    /* the uniforms the draw's functions read: the lights only when lit, the vertex shader's constants
     * only for a vertex shader, the pixel shader's only for a pixel shader (the bound range is the
     * whole struct: that is what the functions are compiled against) */
    size_t need = offsetof(GfxU, light) + (size_t)d->vs.nlights * sizeof(GfxLight);
    if (d->vs.prog)
        need = offsetof(GfxU, psc);
    if (d->fs.prog)
        need = sizeof(GfxU);
    Alloc u = ring(sizeof(GfxU), (size_t)g_ubo_align);
    if (!u.cpu)
        return;
    memcpy(u.cpu, &d->u, need);
    GfxU* gu = (GfxU*)u.cpu;
    for (int r = 0; r < GFX_NREGS; ++r)
        if (d->vs.el[r].used && d->vs.el[r].stream < GFX_NSTREAMS)
            gu->offset[r] += base[d->vs.el[r].stream];

    uint32_t n = vertex_count(d->prim, d->count);
    GLenum m = mode(d->prim);
    GLenum itype = d->index_size == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
    Alloc ia = { 0 };
    if (!d->ibuf && d->indices)
    {
        ia = ring((size_t)n * d->index_size, 16);
        if (!ia.cpu)
            return;
        memcpy(ia.cpu, d->indices, (size_t)n * d->index_size);
    }
    ring_flush();
    glBindBufferRange(GL_UNIFORM_BUFFER, 0, u.buf, u.off, sizeof(GfxU));
    if (d->ibuf)
    {
        bind_ebo(d->ibuf->buf);
        glDrawElements(m, (GLsizei)n, itype, (const void*)(uintptr_t)d->ibuf_off);
    }
    else if (d->indices)
    {
        bind_ebo(ia.buf);
        glDrawElements(m, (GLsizei)n, itype, (const void*)(uintptr_t)ia.off);
    }
    else
        glDrawArrays(m, (GLint)d->vertex_start, (GLsizei)n);
    if (g_nbig)
        ring_done();
}

/* --- clears ---------------------------------------------------------------------------------------------------- */
void gfx_clear(uint32_t nrects, const int32_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil, const uint32_t vp[6])
{
    if (!ready() || !g_rt)
        return;
    if (!g_ds)
        flags &= 1;
    if (!flags || !bind_targets())
        return;
    GLbitfield bits = 0;
    if (flags & 1)
    {
        glClearColor(((color >> 16) & 255) / 255.0f, ((color >> 8) & 255) / 255.0f, (color & 255) / 255.0f, (color >> 24) / 255.0f);
        set_color_mask(15);
        bits |= GL_COLOR_BUFFER_BIT;
    }
    if (flags & 2)
    {
        glClearDepthf(z);
        set_depth_write(1);
        bits |= GL_DEPTH_BUFFER_BIT;
    }
    if ((flags & 4) && g_ds->has_stencil)
    {
        glClearStencil((GLint)(stencil & 255));
        set_stencil_write(0xFF);
        bits |= GL_STENCIL_BUFFER_BIT;
    }
    uint32_t w, h;
    color_size(&w, &h);
    if (!nrects && vp[0] == 0 && vp[1] == 0 && vp[2] >= w && vp[3] >= h)
    {
        set_scissor(0);
        glClear(bits);
        return;
    }
    /* the viewport, intersected with each rectangle, as scissors (D3D's rows, as the target's) */
    int32_t vx0 = (int32_t)vp[0], vy0 = (int32_t)vp[1], vx1 = vx0 + (int32_t)vp[2], vy1 = vy0 + (int32_t)vp[3];
    int32_t whole_rect[4] = { vx0, vy0, vx1, vy1 };
    if (!nrects)
        rects = whole_rect, nrects = 1;
    set_scissor(1);
    for (uint32_t i = 0; i < nrects; ++i)
    {
        int32_t x0 = rects[4 * i] > vx0 ? rects[4 * i] : vx0, y0 = rects[4 * i + 1] > vy0 ? rects[4 * i + 1] : vy0;
        int32_t x1 = rects[4 * i + 2] < vx1 ? rects[4 * i + 2] : vx1, y1 = rects[4 * i + 3] < vy1 ? rects[4 * i + 3] : vy1;
        x1 = x1 < (int32_t)w ? x1 : (int32_t)w, y1 = y1 < (int32_t)h ? y1 : (int32_t)h;
        x0 = x0 > 0 ? x0 : 0, y0 = y0 > 0 ? y0 : 0;
        if (x1 > x0 && y1 > y0)
        {
            glScissor(x0, y0, x1 - x0, y1 - y0);
            glClear(bits);
        }
    }
}

/* --- frames ---------------------------------------------------------------------------------------------------- */
static void frame_end(void)
{
    frame_begin();
    Frame* f = &g_frames[g_frame];
    if (f->timed)
        glEndQuery(GL_TIME_ELAPSED);
    f->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    f->serial = g_serial;
    g_frame_open = 0;
    g_frame = (g_frame + 1) % FRAMES;
    g_serial++;
}

/* "60 FPS 16.7ms": the text of the overlay, from the presents of the last half second */
static void fps_tick(void)
{
    double now = (double)gfx_now_ns() * 1e-9;
    if (!g_fps_since)
        g_fps_since = now;
    g_fps_frames++;
    double dt = now - g_fps_since;
    if (dt >= 0.5)
    {
        double fps = g_fps_frames / dt;
        snprintf(g_fps_text, sizeof g_fps_text, "%.0f FPS %.1fms", fps, dt * 1000.0 / g_fps_frames);
        g_fps_since = now, g_fps_frames = 0;
    }
}

static void draw_overlay(uint32_t w, uint32_t h)
{
    GLint text[32];
    int n = 0;
    for (const char* c = g_fps_text; *c && n < 32; ++c)
        text[n++] = *c >= '0' && *c <= '9' ? *c - '0' : *c == 'F' ? 10 : *c == 'P' ? 11 : *c == 'S' ? 12 : *c == 'm' ? 13
            : *c == 's' ? 14 : *c == '.' ? 15 : 16;
    /* in the window's points, as on Metal (whose layer is not high-density): pixels times the density */
    float d = g_window ? SDL_GetWindowPixelDensity(g_window) : 1.0f;
    if (!(d >= 1.0f))
        d = 1.0f;
    float scale = (float)((float)h / d >= 1400.0f ? 3 : 2) * d;
    use_prog(g_overlay_prog);
    glUniform4f(g_ov_rect, 4 * scale, 4 * scale, (float)(n * 6 + 3) * scale, 11 * scale);
    glUniform1f(g_ov_scale, scale);
    glUniform1i(g_ov_n, n);
    glUniform2f(g_ov_size, (float)w, (float)h);
    glUniform1iv(g_ov_text, n, text);
    enable(GL_BLEND, &g_st.blend, 1);
    if (g_st.bsrc != GL_SRC_ALPHA || g_st.bdst != GL_ONE_MINUS_SRC_ALPHA || g_st.bsrca != GL_ONE || g_st.bdsta != GL_ZERO)
    {
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
        g_st.bsrc = GL_SRC_ALPHA, g_st.bdst = GL_ONE_MINUS_SRC_ALPHA, g_st.bsrca = GL_ONE, g_st.bdsta = GL_ZERO;
    }
    if (g_st.bop != GL_FUNC_ADD)
        glBlendEquation(GL_FUNC_ADD), g_st.bop = GL_FUNC_ADD;
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* the scene effects are Metal's so far (gfx_metal.m) */
void gfx_scene_done(GfxTex* color, const GfxScene* s) { (void)color, (void)s; }
void gfx_fx_set(const char* key, float v) { (void)key, (void)v; }
void gfx_show_overlay(int on) { g_overlay = on != 0; }
void gfx_set_grey(float amount) { (void)amount; }
void gfx_trace_dump(const char* path) { (void)path; }
int gfx_has_scene_effects(void) { return 0; }
/* high density: the window's framebuffer in its pixels (macOS scales a lower-density GL surface up
 * without filtering), the back buffer drawn to it filtered, as Metal's layer is */
/* The context and the window's framebuffer. Set before any window is made: on X11 the window's visual
 * is chosen when it is created, from the attributes set then (on macOS the pixel format comes with the
 * context, so the order never showed). */
static void gl_attributes(void)
{
#ifdef __linux__
    /* EGL, not GLX, on X11 as on Wayland: every current driver has it, and GLX cannot make a core
     * context on an X server without direct rendering (DRI3). SDL_VIDEO_FORCE_EGL=0 goes back. */
    SDL_SetHintWithPriority(SDL_HINT_VIDEO_FORCE_EGL, "1", SDL_HINT_DEFAULT);
#endif
    const char* dbg = getenv("FFXI_GL_DEBUG");
    int debug = dbg && dbg[0] == '1';
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG | (debug ? SDL_GL_CONTEXT_DEBUG_FLAG : 0));
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0); /* the window's framebuffer only takes the present */
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
}

uint64_t gfx_window_flags(void)
{
    gl_attributes();
    return SDL_WINDOW_OPENGL | SDL_WINDOW_HIGH_PIXEL_DENSITY;
}

void gfx_present(GfxTex* bb)
{
    if (!ready())
        return;
    uint64_t present_start = gfx_profiling ? gfx_now_ns() : 0;
    g_present_thread = SDL_GetCurrentThreadID();
    int presented = 0;
    int pw = 0, ph = 0;
    if (g_window && g_window != g_own_window)
        SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
    if (bb && pw > 0 && ph > 0 && g_present_prog && bb->use != GFX_USE_DEPTH)
    {
        bind_fbo(0);
        g_targets_bound = 0;
        set_viewport_px(0, 0, pw, ph);
        set_depth_range(0.0f, 1.0f);
        set_scissor(0);
        set_color_mask(15);
        enable(GL_BLEND, &g_st.blend, 0);
        enable(GL_DEPTH_TEST, &g_st.ztest, 0);
        enable(GL_STENCIL_TEST, &g_st.stencil, 0);
        enable(GL_CULL_FACE, &g_st.cull, 0);
        if (g_st.poly != GL_FILL)
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL), g_st.poly = GL_FILL;
        use_prog(g_present_prog);
        bind_tex(0, B_2D, bb->id);
        bind_samp(0, g_present_samp);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        if (g_overlay && g_overlay_prog)
            draw_overlay((uint32_t)pw, (uint32_t)ph);
        presented = 1;
    }
    fps_tick();
    frame_end();
    if (presented)
    {
        uint64_t t0 = gfx_profiling ? gfx_now_ns() : 0;
        SDL_GL_SwapWindow(g_window);
        if (gfx_profiling)
            g_prof.drawable_ns += gfx_now_ns() - t0;
    }
    else
        glFlush();
    if (gfx_profiling)
        prof_frame(present_start);
}

void gfx_finish(void)
{
    if (!ready())
        return;
    glFinish();
    completed();
}

void gfx_resize(uint32_t w, uint32_t h)
{
    (void)w, (void)h; /* the window's framebuffer follows the window */
}

/* --- start-up -------------------------------------------------------------------------------------------------- */
static int has_ext(const char* name)
{
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; ++i)
    {
        const char* e = (const char*)glGetStringi(GL_EXTENSIONS, (GLuint)i);
        if (e && !strcmp(e, name))
            return 1;
    }
    return 0;
}

static void GLAPIENTRY debug_message(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* msg,
    const void* user)
{
    (void)source, (void)id, (void)length, (void)user;
    if (severity != GL_DEBUG_SEVERITY_NOTIFICATION)
        fprintf(stderr, "[gl] %s%s\n", type == GL_DEBUG_TYPE_ERROR ? "error: " : "", msg);
}

static int load_functions(void)
{
    int ok = 1;
#define LOAD_FN(n)                                                                                                    \
    if (!(p_##n = (__typeof__(p_##n))SDL_GL_GetProcAddress(#n)))                                                      \
        fprintf(stderr, "[recomp] gfx: no %s\n", #n), ok = 0;
    GL_FUNCS(LOAD_FN)
#undef LOAD_FN
#define LOAD_OPTIONAL(n) p_##n = (__typeof__(p_##n))SDL_GL_GetProcAddress(#n);
    GL_OPTIONAL(LOAD_OPTIONAL)
#undef LOAD_OPTIONAL
    return ok;
}

static GLuint dummy_texture(GLenum target)
{
    static const uint8_t zero[4] = { 0, 0, 0, 0 };
    GLuint t;
    glGenTextures(1, &t);
    scratch_tex(target == GL_TEXTURE_CUBE_MAP ? B_CUBE : B_2D, t);
    for (int f = 0; f < (target == GL_TEXTURE_CUBE_MAP ? 6 : 1); ++f)
        glTexImage2D(target == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + (GLenum)f : GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0,
            GL_RGBA, GL_UNSIGNED_BYTE, zero);
    glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, 0);
    return t;
}

/* the context and what every frame needs; on the window, or on a hidden one of its own (tests) */
static int make_context(SDL_Window* window)
{
    if (!window)
    {
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
        {
            fprintf(stderr, "[recomp] gfx: no video for an OpenGL context: %s\n", SDL_GetError());
            return 0;
        }
        gl_attributes();
        g_own_window = SDL_CreateWindow("gfx", 16, 16, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        if (!g_own_window)
        {
            fprintf(stderr, "[recomp] gfx: no window for an OpenGL context: %s\n", SDL_GetError());
            return 0;
        }
        window = g_own_window;
    }
    gl_attributes();
    g_ctx = SDL_GL_CreateContext(window);
    if (!g_ctx)
    {
        fprintf(stderr, "[recomp] gfx: no OpenGL 4.1 core context: %s\n", SDL_GetError());
        return 0;
    }
    g_window = window;
    g_gl_thread = SDL_GetCurrentThreadID();
    if (!load_functions())
    {
        SDL_GL_DestroyContext(g_ctx);
        g_ctx = NULL;
        return 0;
    }
    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    fprintf(stderr, "[recomp] gfx: OpenGL %s on %s\n", (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER));
    if (major < 4 || (major == 4 && minor < 1))
    {
        fprintf(stderr, "[recomp] gfx: the back end needs OpenGL 4.1\n");
        SDL_GL_DestroyContext(g_ctx);
        g_ctx = NULL;
        return 0;
    }
    int ver = major * 10 + minor;
    if (!has_ext("GL_EXT_texture_compression_s3tc"))
        fprintf(stderr, "[recomp] gfx: no GL_EXT_texture_compression_s3tc: DXT textures will not draw\n");
    g_clip01 = glClipControl && (ver >= 45 || has_ext("GL_ARB_clip_control"));
    g_aniso = ver >= 46 || has_ext("GL_EXT_texture_filter_anisotropic") || has_ext("GL_ARB_texture_filter_anisotropic");
    g_mirror_once = ver >= 44 || has_ext("GL_ARB_texture_mirror_clamp_to_edge") || has_ext("GL_EXT_texture_mirror_clamp") ||
        has_ext("GL_ATI_texture_mirror_once");
    const char* dbg = getenv("FFXI_GL_DEBUG");
    if (dbg && dbg[0] == '1' && glDebugMessageCallback)
    {
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(debug_message, NULL);
        fprintf(stderr, "[recomp] gfx: GL debug output on\n");
    }
    return 1;
}

int gfx_init(void* window, int vsync)
{
    if (g_ctx)
    {
        /* up already (host64's sign-in screen, on this same window): the game's present interval */
        if (window && window != g_window)
        {
            g_window = (SDL_Window*)window;
            SDL_GL_MakeCurrent(g_window, g_ctx);
        }
        if (ready())
            SDL_GL_SetSwapInterval(vsync ? 1 : 0);
        g_vsync = vsync;
        return 1;
    }
    if (!make_context((SDL_Window*)window))
        return 0;
    state_forget();
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &g_ubo_align);
    if (g_ubo_align < 16)
        g_ubo_align = 16;
    glGenVertexArrays(1, &g_vao);
    glBindVertexArray(g_vao); /* the vertex functions fetch their own: no attributes */
    glProvokingVertex(GL_FIRST_VERTEX_CONVENTION); /* D3D's flat shading takes the first vertex's color */
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glFrontFace(GL_CCW);
    glDisable(GL_DITHER); /* on by default in GL: D3D's DITHERENABLE, which Metal and D3D12 do not do either */
    if (g_clip01)
        glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    g_dummy_2d = dummy_texture(GL_TEXTURE_2D);
    g_dummy_cube = dummy_texture(GL_TEXTURE_CUBE_MAP);
    g_dummy_buf = make_buffer(256, NULL);
    g_dummy_tbo = make_tbo(g_dummy_buf);
    GfxSampler linear;
    memset(&linear, 0, sizeof linear);
    linear.addr_u = linear.addr_v = linear.addr_w = 3, linear.mag = linear.min = 2;
    g_present_samp = sampler(&linear);

    const char* prof = getenv("FFXI_PROFILE");
    gfx_profiling = prof && prof[0] && prof[0] != '0';
    const char* show = getenv("FFXI_FPS");
    g_overlay = !(show && show[0] == '0');
    g_present_prog = link_program("#define PRESENT\n", gfx_glsl_util);
    if (g_present_prog)
        glUniform1i(glGetUniformLocation(g_present_prog, "tex"), 0);
    g_overlay_prog = link_program("", gfx_glsl_util);
    if (g_overlay_prog)
    {
        g_ov_rect = glGetUniformLocation(g_overlay_prog, "rect");
        g_ov_scale = glGetUniformLocation(g_overlay_prog, "scale");
        g_ov_n = glGetUniformLocation(g_overlay_prog, "n");
        g_ov_size = glGetUniformLocation(g_overlay_prog, "size");
        g_ov_text = glGetUniformLocation(g_overlay_prog, "text");
    }
    g_vsync = vsync;
    SDL_GL_SetSwapInterval(vsync ? 1 : 0);
    GLenum err = glGetError();
    if (err != GL_NO_ERROR)
        fprintf(stderr, "[recomp] gfx: GL error %04x at start-up\n", err);
    return 1;
}
