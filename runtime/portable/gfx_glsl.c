/* GLSL 4.10 for a draw's keys (gfx.h): the OpenGL back end's counterpart of gfx_hlsl.c, with the
 * same fixed-function pipeline - transform, lighting, fog, texture coordinate generation and
 * transforms, the texture stage cascade, alpha test - as one vertex and one fragment function per
 * key pair, and vs.1.x / ps.1.x shaders translated (gfx_glsl_shaders.c). Pure C: the GL side
 * (gfx_gl.c) compiles the text twice (VERTEX defined, then not) and caches the program by key.
 *
 * Conventions the functions follow (the bindings are gfx_gl.c's):
 *   - matrices are D3D's bytes in a std140 (column-major) mat4, so `m * v` is D3D's v * M;
 *   - D3D8 puts pixel centers on integer coordinates and GL on half-integers: every clip-space
 *     position moves by half a pixel (the "position fixup"), as on Metal and D3D12;
 *   - render targets hold D3D's rows top first, so the vertex function flips y (the image is drawn
 *     upside down in GL's terms and sampled the right way up), and D3D's clip z (0..w) becomes
 *     GL's (-w..w) through CLIPZ unless the back end has glClipControl;
 *   - vertex fetch is by hand from the stream buffers (R32UI buffer textures s0..s3), stride and
 *     per-register offset in the uniforms, so one function serves any offsets and strides;
 *   - the uniforms are the block CU in both stages; texture stage i samples tx<i>, a sampler2D or
 *     samplerCube as the key says, on texture unit i. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "gfx_glsl.h"

_Static_assert(sizeof(GfxU) == 3536, "GfxU must match the GLSL block CU");

void sb_printf(Sb* b, const char* fmt, ...)
{
    va_list ap;
    for (;;)
    {
        size_t room = b->cap - b->len;
        va_start(ap, fmt);
        int n = b->cap ? vsnprintf(b->s + b->len, room, fmt, ap) : -1;
        va_end(ap);
        if (n >= 0 && (size_t)n < room)
        {
            b->len += (size_t)n;
            return;
        }
        b->cap = b->cap ? b->cap * 2 : 8192;
        if (n >= 0 && b->cap < b->len + (size_t)n + 1)
            b->cap = b->len + (size_t)n + 1;
        b->s = (char*)realloc(b->s, b->cap);
    }
}

static const char PRELUDE[] =
    "#define INFINITY uintBitsToFloat(0x7F800000u)\n"
    "float saturate(float x) { return clamp(x, 0.0, 1.0); }\n"
    "vec2 saturate(vec2 x) { return clamp(x, 0.0, 1.0); }\n"
    "vec3 saturate(vec3 x) { return clamp(x, 0.0, 1.0); }\n"
    "vec4 saturate(vec4 x) { return clamp(x, 0.0, 1.0); }\n"
    "struct Light { vec4 diffuse, specular, ambient, pos, dir, att, spot; };\n"
    "layout(std140) uniform CU {\n"
    "  mat4 wvp, wv, wvit;\n"
    "  mat4 texm[8];\n"
    "  vec4 mat_d, mat_a, mat_s, mat_e, params, ambient, tfactor, fogcolor, params2, vp;\n"
    "  ivec4 vofs, stride;\n"
    "  ivec4 offset[5];\n"
    "  Light light[8];\n"
    "  vec4 vsc[96];\n"
    "  vec4 psc[8];\n"
    "} u;\n"
    "#ifdef VERTEX\n"
    "uniform usamplerBuffer s0, s1, s2, s3;\n"
    "uint ld(usamplerBuffer b, int a) { return texelFetch(b, a >> 2).x; }\n"
    "float ldf(usamplerBuffer b, int a) { return uintBitsToFloat(ld(b, a)); }\n"
    "vec4 ld_float1(usamplerBuffer b, int a) { return vec4(ldf(b, a), 0.0, 0.0, 1.0); }\n"
    "vec4 ld_float2(usamplerBuffer b, int a) { return vec4(ldf(b, a), ldf(b, a + 4), 0.0, 1.0); }\n"
    "vec4 ld_float3(usamplerBuffer b, int a) { return vec4(ldf(b, a), ldf(b, a + 4), ldf(b, a + 8), 1.0); }\n"
    "vec4 ld_float4(usamplerBuffer b, int a) { return vec4(ldf(b, a), ldf(b, a + 4), ldf(b, a + 8), ldf(b, a + 12)); }\n"
    "vec4 ld_color(usamplerBuffer b, int a) { uint c = ld(b, a); return vec4((c >> 16) & 255u, (c >> 8) & 255u, c & 255u, c >> 24) / 255.0; }\n"
    "vec4 ld_ubyte4(usamplerBuffer b, int a) { uint c = ld(b, a); return vec4(c & 255u, (c >> 8) & 255u, (c >> 16) & 255u, c >> 24); }\n"
    "vec4 ld_short2(usamplerBuffer b, int a) { int c = int(ld(b, a)); return vec4((c << 16) >> 16, c >> 16, 0, 1); }\n"
    "vec4 ld_short4(usamplerBuffer b, int a) { int x = int(ld(b, a)), y = int(ld(b, a + 4)); return vec4((x << 16) >> 16, x >> 16, (y << 16) >> 16, y >> 16); }\n"
    "#define VARY out\n"
    "#else\n"
    "#define VARY in\n"
    "#endif\n";

/* the vertex function's output, what the fragment function reads: a struct in both functions (as
 * HLSL's VOut), and the varyings between them */
static void emit_vout(Sb* b, int ntex, int flat)
{
    const char* fl = flat ? "flat " : "";
    sb_printf(b, "struct VOut {\n  vec4 pos;\n  vec4 d;\n  vec4 s;\n");
    for (int i = 0; i < ntex; ++i)
        sb_printf(b, "  vec4 t%d;\n", i);
    sb_printf(b, "  float fog;\n  float ez;\n};\n");
    sb_printf(b, "%sVARY vec4 io_d;\n%sVARY vec4 io_s;\n", fl, fl);
    for (int i = 0; i < ntex; ++i)
        sb_printf(b, "VARY vec4 io_t%d;\n", i);
    sb_printf(b, "VARY float io_fog;\nVARY float io_ez;\n");
}

/* the stages' textures: tx<i> on unit i, of the type the key samples */
static void emit_samplers(Sb* b, const GfxFsKey* k)
{
    sb_printf(b, "#ifndef VERTEX\n");
    for (int i = 0; i < 8; ++i)
        if (k->st[i].tex)
            sb_printf(b, "uniform %s tx%d;\n", k->st[i].tex == 2 ? "samplerCube" : "sampler2D", i);
    sb_printf(b, "#endif\n");
}

/* main() of each stage around the generated vs_main / fs_main */
static void emit_vs_main(Sb* b, int ntex)
{
    sb_printf(b, "void main() {\n  VOut o = vs_main();\n  gl_Position = vec4(o.pos.x, -o.pos.y, CLIPZ(o.pos), o.pos.w);\n"
                 "  io_d = o.d;\n  io_s = o.s;\n");
    for (int i = 0; i < ntex; ++i)
        sb_printf(b, "  io_t%d = o.t%d;\n", i, i);
    sb_printf(b, "  io_fog = o.fog;\n  io_ez = o.ez;\n}\n");
}

static void emit_fs_main(Sb* b, int ntex)
{
    sb_printf(b, "layout(location = 0) out vec4 o_color;\n"
                 "void main() {\n  VOut vin;\n  vin.pos = gl_FragCoord;\n  vin.d = io_d;\n  vin.s = io_s;\n");
    for (int i = 0; i < ntex; ++i)
        sb_printf(b, "  vin.t%d = io_t%d;\n", i, i);
    sb_printf(b, "  vin.fog = io_fog;\n  vin.ez = io_ez;\n  o_color = fs_main(vin);\n}\n");
}

void gfx_glsl_fetch(Sb* b, const GfxVsKey* k, int reg)
{
    const GfxElem* e = &k->el[reg];
    if (!e->used)
    {
        sb_printf(b, "  vec4 v%d = vec4(0, 0, 0, 1);\n", reg);
        return;
    }
    sb_printf(b, "  int ad%d = vi * u.stride[%d] + u.offset[%d][%d];\n", reg, e->stream, reg >> 2, reg & 3);
    static const char* const LD[] = { "ld_float1", "ld_float2", "ld_float3", "ld_float4", "ld_color", "ld_ubyte4", "ld_short2",
        "ld_short4" };
    if (e->type < sizeof LD / sizeof LD[0])
        sb_printf(b, "  vec4 v%d = %s(s%d, ad%d);\n", reg, LD[e->type], e->stream, reg);
    else
        sb_printf(b, "  vec4 v%d = vec4(0, 0, 0, 1);\n", reg);
}

static int elem_components(uint8_t type)
{
    switch (type)
    {
    case GFX_FLOAT1: return 1;
    case GFX_FLOAT2:
    case GFX_SHORT2: return 2;
    case GFX_FLOAT3: return 3;
    default: return 4;
    }
}

/* the struct's zero, as HLSL's (VOut)0 */
static void emit_vs_signature(Sb* b, int ntex)
{
    sb_printf(b, "VOut vs_main() {\n  VOut o;\n  o.pos = vec4(0);\n  o.d = vec4(0);\n  o.s = vec4(0);\n");
    for (int i = 0; i < ntex; ++i)
        sb_printf(b, "  o.t%d = vec4(0);\n", i);
    sb_printf(b, "  o.fog = 0.0;\n  o.ez = 0.0;\n  int vi = gl_VertexID + u.vofs.x;\n");
}

/* clip-space position fixup: D3D8's pixel centers onto GL's */
static void emit_fixup(Sb* b)
{
    sb_printf(b, "  o.pos.x += o.pos.w / u.vp.z;\n  o.pos.y -= o.pos.w / u.vp.w;\n");
}

/* a D3DMATERIALCOLORSOURCE: the vertex color when the vertex has it, else the material's */
static const char* mcs(const GfxVsKey* k, int src, const char* mat)
{
    return src == 1 && k->el[GFX_R_DIFFUSE].used ? "v5" : src == 2 && k->el[GFX_R_SPECULAR].used ? "v6" : mat;
}

static void emit_fog_factor(Sb* b, const char* dst, int mode, const char* dist)
{
    switch (mode)
    {
    case 1: sb_printf(b, "  %s = saturate(exp(-u.params2.x * %s));\n", dst, dist); break;
    case 2: sb_printf(b, "  %s = saturate(exp(-(u.params2.x * %s) * (u.params2.x * %s)));\n", dst, dist, dist); break;
    case 3: sb_printf(b, "  %s = saturate((u.params.w - %s) / (u.params.w - u.params.z));\n", dst, dist); break;
    default: sb_printf(b, "  %s = 1.0;\n", dst); break;
    }
}

static void emit_ff_vs(Sb* b, const GfxVsKey* k)
{
    emit_vs_signature(b, k->ntex);
    for (int r = 0; r < GFX_NREGS; ++r) /* unused registers are constants the compiler drops */
        gfx_glsl_fetch(b, k, r);
    if (k->rhw)
    {
        sb_printf(b,
            "  float w = v0.w != 0.0 ? 1.0 / v0.w : 1.0;\n"
            "  vec2 ndc = vec2((v0.x - u.vp.x) / u.vp.z * 2.0 - 1.0, 1.0 - (v0.y - u.vp.y) / u.vp.w * 2.0);\n"
            "  o.pos = vec4(ndc * w, v0.z * w, w);\n"
            "  vec3 pe = vec3(0.0, 0.0, w);\n"
            "  o.ez = w;\n");
    }
    else
    {
        sb_printf(b,
            "  vec4 P = vec4(v0.xyz, 1.0);\n"
            "  o.pos = u.wvp * P;\n"
            "  vec3 pe = (u.wv * P).xyz;\n"
            "  o.ez = pe.z;\n");
    }
    emit_fixup(b);
    int need_normal = k->lighting;
    for (int i = 0; i < k->ntex; ++i)
        need_normal |= (k->tci[i] >> 4) == 1 || (k->tci[i] >> 4) == 3;
    if (need_normal && !k->rhw)
    {
        sb_printf(b, "  vec3 N = (u.wvit * vec4(v3.xyz, 0.0)).xyz;\n");
        if (k->normalize)
            sb_printf(b, "  N = normalize(N);\n");
    }
    else
        sb_printf(b, "  vec3 N = vec3(0.0, 0.0, 1.0);\n");

    const char* dif_in = k->el[GFX_R_DIFFUSE].used ? "v5" : "vec4(1.0)";
    const char* spe_in = k->el[GFX_R_SPECULAR].used ? "v6" : "vec4(0.0)";
    if (k->lighting && !k->rhw)
    {
        sb_printf(b, "  vec4 cd = %s, ca = %s, cs = %s, ce = %s;\n", mcs(k, k->src_diffuse, "u.mat_d"),
            mcs(k, k->src_ambient, "u.mat_a"), mcs(k, k->src_specular, "u.mat_s"), mcs(k, k->src_emissive, "u.mat_e"));
        sb_printf(b, "  vec3 amb = u.ambient.rgb, dif = vec3(0.0), spc = vec3(0.0);\n");
        if (k->specular)
            sb_printf(b, "  vec3 V = %s;\n", k->localviewer ? "normalize(-pe)" : "vec3(0.0, 0.0, -1.0)");
        for (int i = 0; i < k->nlights; ++i)
        {
            int t = k->light_type[i];
            sb_printf(b, "  {\n    Light L = u.light[%d];\n", i);
            if (t == 3)
                sb_printf(b, "    vec3 l = L.dir.xyz;\n    float a = 1.0;\n");
            else
            {
                sb_printf(b,
                    "    vec3 lv = L.pos.xyz - pe;\n    float d = length(lv);\n    vec3 l = lv / max(d, 1e-20);\n"
                    "    float a = d > L.pos.w ? 0.0 : 1.0 / max(L.att.x + L.att.y * d + L.att.z * d * d, 1e-20);\n");
                if (t == 2)
                    sb_printf(b,
                        "    float rho = dot(-l, L.dir.xyz);\n"
                        "    a *= rho > L.spot.x ? 1.0 : rho <= L.spot.y ? 0.0 : pow(saturate((rho - L.spot.y) / (L.spot.x - L.spot.y)), L.dir.w);\n");
            }
            sb_printf(b, "    amb += L.ambient.rgb * a;\n    float ndl = max(dot(N, l), 0.0);\n    dif += L.diffuse.rgb * (ndl * a);\n");
            if (k->specular)
                sb_printf(b, "    if (ndl > 0.0) spc += L.specular.rgb * (pow(max(dot(N, normalize(V + l)), 0.0), u.params.x) * a);\n");
            sb_printf(b, "  }\n");
        }
        sb_printf(b,
            "  o.d = saturate(vec4(ce.rgb + ca.rgb * amb + cd.rgb * dif, cd.a));\n"
            "  o.s = saturate(vec4(cs.rgb * spc, cs.a));\n");
    }
    else
        sb_printf(b, "  o.d = %s;\n  o.s = %s;\n", dif_in, spe_in);

    if (k->fog_vertex && !k->rhw)
    {
        sb_printf(b, "  float fd = %s;\n", k->range_fog ? "length(pe)" : "abs(pe.z)");
        emit_fog_factor(b, "o.fog", k->fog_vertex, "fd");
    }
    else
        sb_printf(b, "  o.fog = %s.a;\n", k->el[GFX_R_SPECULAR].used ? "v6" : "vec4(1.0)");

    for (int i = 0; i < k->ntex; ++i)
    {
        int idx = k->tci[i] & 7, gen = k->tci[i] >> 4, count = k->ttf[i] & 7;
        if (k->rhw)
            gen = 0;
        switch (gen)
        {
        case 1: sb_printf(b, "  vec4 c%d = vec4(N, 1.0);\n", i); break;
        case 2: sb_printf(b, "  vec4 c%d = vec4(pe, 1.0);\n", i); break;
        case 3: sb_printf(b, "  vec4 c%d = vec4(reflect(%s, N), 1.0);\n", i, k->localviewer ? "normalize(pe)" : "vec3(0.0, 0.0, 1.0)"); break;
        default:
        {
            int reg = GFX_R_TEXCOORD0 + idx;
            if (!k->el[reg].used)
                sb_printf(b, "  vec4 c%d = vec4(0, 0, 0, 1);\n", i);
            else if (count)
            {
                /* D3D fills the component after the last one given with 1, which is how a 2D
                 * texture matrix carries its translation in the third row */
                int n = elem_components(k->el[reg].type);
                char y[16] = "1.0", z[16] = "1.0", w[16] = "1.0";
                if (n > 1)
                    snprintf(y, sizeof y, "v%d.y", reg);
                if (n > 2)
                    snprintf(z, sizeof z, "v%d.z", reg);
                if (n > 3)
                    snprintf(w, sizeof w, "v%d.w", reg);
                sb_printf(b, "  vec4 c%d = vec4(v%d.x, %s, %s, %s);\n", i, reg, y, z, w);
            }
            else
                sb_printf(b, "  vec4 c%d = v%d;\n", i, reg);
            break;
        }
        }
        if (count && !k->rhw)
            sb_printf(b, "  c%d = u.texm[%d] * c%d;\n", i, i, i);
        sb_printf(b, "  o.t%d = c%d;\n", i, i);
    }
    sb_printf(b, "  return o;\n}\n");
}

/* --- fragment: the texture stage cascade ------------------------------------------------------------ */
static void arg_expr(char* out, size_t n, int a)
{
    const char* base;
    switch (a & 0xF)
    {
    case 0: base = "vin.d"; break;
    case 1: base = "cur"; break;
    case 2: base = "tex"; break;
    case 3: base = "u.tfactor"; break;
    case 4: base = "vin.s"; break;
    case 5: base = "tmp"; break;
    default: base = "cur"; break;
    }
    char v[64];
    if (a & 0x20)
        snprintf(v, sizeof v, "%s.aaaa", base);
    else
        snprintf(v, sizeof v, "%s", base);
    if (a & 0x10)
        snprintf(out, n, "(1.0 - %s)", v);
    else
        snprintf(out, n, "%s", v);
}

/* D3DTEXTUREOP on vec4 arguments; the caller keeps .rgb or .a */
static void op_expr(char* out, size_t n, int op, const char* a1, const char* a2, const char* a0)
{
    switch (op)
    {
    case 2: snprintf(out, n, "%s", a1); break;
    case 3: snprintf(out, n, "%s", a2); break;
    case 4: snprintf(out, n, "(%s * %s)", a1, a2); break;
    case 5: snprintf(out, n, "(%s * %s * 2.0)", a1, a2); break;
    case 6: snprintf(out, n, "(%s * %s * 4.0)", a1, a2); break;
    case 7: snprintf(out, n, "(%s + %s)", a1, a2); break;
    case 8: snprintf(out, n, "(%s + %s - 0.5)", a1, a2); break;
    case 9: snprintf(out, n, "((%s + %s - 0.5) * 2.0)", a1, a2); break;
    case 10: snprintf(out, n, "(%s - %s)", a1, a2); break;
    case 11: snprintf(out, n, "(%s + %s - %s * %s)", a1, a2, a1, a2); break;
    case 12: snprintf(out, n, "mix(%s, %s, vin.d.a)", a2, a1); break;
    case 13: snprintf(out, n, "mix(%s, %s, tex.a)", a2, a1); break;
    case 14: snprintf(out, n, "mix(%s, %s, u.tfactor.a)", a2, a1); break;
    case 15: snprintf(out, n, "(%s + %s * (1.0 - tex.a))", a1, a2); break;
    case 16: snprintf(out, n, "mix(%s, %s, cur.a)", a2, a1); break;
    case 17: snprintf(out, n, "%s", a1); break; /* PREMODULATE: the next stage's texture is not known here */
    case 18: snprintf(out, n, "(%s + %s.a * %s)", a1, a1, a2); break;
    case 19: snprintf(out, n, "(%s * %s + %s.a)", a1, a2, a1); break;
    case 20: snprintf(out, n, "((1.0 - %s.a) * %s + %s)", a1, a2, a1); break;
    case 21: snprintf(out, n, "((1.0 - %s) * %s + %s.a)", a1, a2, a1); break;
    case 24: snprintf(out, n, "vec4(saturate(dot((%s.rgb - 0.5) * 2.0, (%s.rgb - 0.5) * 2.0)))", a1, a2); break;
    case 25: snprintf(out, n, "(%s + %s * %s)", a0, a1, a2); break;
    case 26: snprintf(out, n, "(%s * %s + (1.0 - %s) * %s)", a0, a1, a0, a2); break;
    default: snprintf(out, n, "%s", a1); break; /* the bump ops: the environment map is not emulated */
    }
}

void gfx_glsl_sample(Sb* b, const char* dst, int i, const char* coord)
{
    sb_printf(b, "  %s = texture(tx%d, %s);\n", dst, i, coord);
}

static void emit_fs_signature(Sb* b)
{
    sb_printf(b, "vec4 fs_main(VOut vin) {\n");
}

static void emit_fs_tail(Sb* b, const GfxFsKey* k, const char* col)
{
    if (k->alpha_func && k->alpha_func != 8)
    {
        static const char* const cmp[] = { "", "false", "<", "==", "<=", ">", "!=", ">=", "true" };
        if (k->alpha_func == 1)
            sb_printf(b, "  discard;\n");
        else if (k->alpha_func < 8)
            sb_printf(b, "  if (!(roundEven(saturate(%s.a) * 255.0) %s u.params.y)) discard;\n", col, cmp[k->alpha_func]);
    }
    if (k->fog)
    {
        if (k->fog == 4)
            sb_printf(b, "  float f = saturate(vin.fog);\n");
        else
        {
            sb_printf(b, "  float f;\n  float fd = abs(vin.ez);\n");
            emit_fog_factor(b, "f", k->fog, "fd");
        }
        sb_printf(b, "  %s.rgb = mix(u.fogcolor.rgb, %s.rgb, f);\n", col, col);
    }
    sb_printf(b, "  return %s;\n}\n", col);
}

static void emit_ff_fs(Sb* b, const GfxFsKey* k)
{
    emit_fs_signature(b);
    sb_printf(b, "  vec4 cur = vin.d, tmp = vec4(0.0), tex = vec4(1.0);\n");
    for (int i = 0; i < k->nstages; ++i)
    {
        const GfxStage* s = &k->st[i];
        char coord[64];
        if (s->tex == 1)
        {
            if (s->projected)
            {
                const char* w = s->ncoord == 3 ? "z" : s->ncoord == 4 ? "w" : "y";
                snprintf(coord, sizeof coord, "vin.t%d.xy / vin.t%d.%s", i, i, w);
            }
            else
                snprintf(coord, sizeof coord, "vin.t%d.xy", i);
            gfx_glsl_sample(b, "tex", i, coord);
        }
        else if (s->tex == 2)
        {
            snprintf(coord, sizeof coord, "vin.t%d.xyz", i);
            gfx_glsl_sample(b, "tex", i, coord);
        }
        else
            sb_printf(b, "  tex = vec4(1.0);\n");
        char a1[64], a2[64], a0[64], ce[512], ae[512];
        arg_expr(a1, sizeof a1, s->ca1);
        arg_expr(a2, sizeof a2, s->ca2);
        arg_expr(a0, sizeof a0, s->ca0);
        op_expr(ce, sizeof ce, s->cop, a1, a2, a0);
        const char* dst = s->result == 5 ? "tmp" : "cur";
        if (s->cop == 24) /* DOTPRODUCT3 goes to every channel, alpha too */
        {
            sb_printf(b, "  %s = saturate(%s);\n", dst, ce);
            continue;
        }
        if (s->aop > 1)
        {
            arg_expr(a1, sizeof a1, s->aa1);
            arg_expr(a2, sizeof a2, s->aa2);
            arg_expr(a0, sizeof a0, s->aa0);
            op_expr(ae, sizeof ae, s->aop, a1, a2, a0);
            sb_printf(b, "  { vec3 c = saturate(%s.rgb); float a = saturate(%s.a); %s = vec4(c, a); }\n", ce, ae, dst);
        }
        else /* alpha disabled: the alpha carries on unchanged */
            sb_printf(b, "  { vec3 c = saturate(%s.rgb); %s = vec4(c, %s.a); }\n", ce, dst, i ? "cur" : "vin.d");
    }
    if (k->specular_add)
        sb_printf(b, "  cur.rgb = saturate(cur.rgb + vin.s.rgb);\n");
    emit_fs_tail(b, k, "cur");
}

char* gfx_glsl_generate(const GfxVsKey* vk, const GfxFsKey* fk, const uint32_t* vs_tokens, const uint32_t* ps_tokens)
{
    Sb b = { 0 };
    sb_printf(&b, "%s", PRELUDE);
    emit_vout(&b, vk->ntex, vk->flat);
    emit_samplers(&b, fk);
    sb_printf(&b, "#ifdef VERTEX\n");
    if (vk->prog)
    {
        if (!gfx_glsl_vs1(&b, vk, vs_tokens))
            goto fail;
    }
    else
        emit_ff_vs(&b, vk);
    emit_vs_main(&b, vk->ntex);
    sb_printf(&b, "#else\n");
    if (fk->prog)
    {
        emit_fs_signature(&b);
        if (!gfx_glsl_ps1(&b, fk, ps_tokens))
            goto fail;
        emit_fs_tail(&b, fk, "r0");
    }
    else
        emit_ff_fs(&b, fk);
    emit_fs_main(&b, vk->ntex);
    sb_printf(&b, "#endif\n");
    return b.s;
fail:
    free(b.s);
    return NULL;
}

/* --- the device's own functions: the back buffer to the window, and the frame-rate overlay --------------
 * Both draw to the window's framebuffer, whose rows GL keeps bottom first: no flip there. */
const char gfx_glsl_util[] =
    "#ifdef PRESENT\n"
    "#ifdef VERTEX\n"
    "out vec2 uv;\n"
    "void main() {\n"
    "  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
    "  gl_Position = vec4(p * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0); uv = p;\n"
    "}\n"
    "#else\n"
    "uniform sampler2D tex;\n"
    "in vec2 uv;\n"
    "layout(location = 0) out vec4 o_color;\n"
    "void main() { o_color = vec4(texture(tex, uv).rgb, 1.0); }\n"
    "#endif\n"
    "#else\n"
    /* the frame-rate overlay: a 5x7 bitmap font drawn per pixel, no texture */
    "uniform vec4 rect;\n"
    "uniform float scale;\n"
    "uniform int n;\n"
    "uniform vec2 size;\n"
    "#ifdef VERTEX\n"
    "void main() {\n"
    "  vec2 c = rect.xy + vec2((gl_VertexID & 1) != 0 ? rect.z : 0.0, (gl_VertexID & 2) != 0 ? rect.w : 0.0);\n"
    "  gl_Position = vec4(c.x / size.x * 2.0 - 1.0, 1.0 - c.y / size.y * 2.0, 0.0, 1.0);\n"
    "}\n"
    "#else\n"
    "uniform int text[32];\n"
    "const int FONT[17 * 7] = int[](\n"
    "  0x0E,0x11,0x13,0x15,0x19,0x11,0x0E, 0x04,0x0C,0x04,0x04,0x04,0x04,0x0E, 0x0E,0x11,0x01,0x02,0x04,0x08,0x1F,\n"
    "  0x1F,0x02,0x04,0x02,0x01,0x11,0x0E, 0x02,0x06,0x0A,0x12,0x1F,0x02,0x02, 0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E,\n"
    "  0x06,0x08,0x10,0x1E,0x11,0x11,0x0E, 0x1F,0x01,0x02,0x04,0x08,0x08,0x08, 0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E,\n"
    "  0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C, 0x1F,0x10,0x10,0x1E,0x10,0x10,0x10, 0x1E,0x11,0x11,0x1E,0x10,0x10,0x10,\n"
    "  0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E, 0x00,0x00,0x1A,0x15,0x15,0x11,0x11, 0x00,0x00,0x0E,0x10,0x0E,0x01,0x1E,\n"
    "  0x00,0x00,0x00,0x00,0x00,0x0C,0x0C, 0x00,0x00,0x00,0x00,0x00,0x00,0x00);\n"
    "layout(location = 0) out vec4 o_color;\n"
    "void main() {\n"
    "  vec2 pos = vec2(gl_FragCoord.x, size.y - gl_FragCoord.y); /* from the top, as D3D's */\n"
    "  vec2 p = (pos - rect.xy) / scale - 2.0;\n"
    "  int cell = int(floor(p.x / 6.0)), gx = int(floor(p.x)) - cell * 6, gy = int(floor(p.y));\n"
    "  o_color = vec4(0.0, 0.0, 0.0, 0.55);\n"
    "  if (p.x >= 0.0 && cell < n && gx < 5 && gy >= 0 && gy < 7 && ((FONT[text[cell] * 7 + gy] >> (4 - gx)) & 1) != 0)\n"
    "    o_color = vec4(1.0, 0.85, 0.2, 1.0);\n"
    "}\n"
    "#endif\n"
    "#endif\n";
