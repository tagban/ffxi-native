/* Effects of our own on some of the game's draws, the same for every back end (the generators for
 * Metal, GLSL and HLSL each include this after their Sb). d3d8.c (fx_classify) picks the draws and
 * sets GfxFsKey.fx; GfxU.params2 carries y: seconds (wrapping), z: how strong (0 = the game's own).
 *
 *   GFX_FX_MARK    painted magenta: //xi fx mark, to see which draws a rule takes
 *   GFX_FX_CLOUDS  the sky's cloud layers: shapes broken up by moving noise, lit tops, darker undersides
 *   GFX_FX_POOL    still water: the texture rippled, the light glinting off it
 *   GFX_FX_FALLS   falling water: streaks running down it
 *
 * The code is written once, in the shared subset of the three languages (float2..float4, fract, mix,
 * saturate, smoothstep), with a few #defines for GLSL and HLSL. */
#pragma once

#include "gfx_fx_ids.h"

enum
{
    GFX_FX_MSL,
    GFX_FX_GLSL,
    GFX_FX_HLSL
};

/* Before the fragment function: the language's names, and the noise the effects are made of. */
static void gfx_fx_functions(Sb* b, int fx, int lang)
{
    if (!fx)
        return;
    if (lang == GFX_FX_GLSL)
        sb_printf(b, "#define float2 vec2\n#define float3 vec3\n#define float4 vec4\n");
    else if (lang == GFX_FX_HLSL)
        sb_printf(b, "#define fract frac\n#define mix lerp\n");
    if (fx < GFX_FX_CLOUDS)
        return;
    sb_printf(b,
        "float fx_hash(float2 p) { p = fract(p * float2(123.34, 456.21)); p += dot(p, p + 45.32); return fract(p.x * p.y); }\n"
        "float fx_noise(float2 p) {\n"
        "  float2 i = floor(p), f = fract(p);\n"
        "  f = f * f * (3.0 - 2.0 * f);\n"
        "  return mix(mix(fx_hash(i), fx_hash(i + float2(1.0, 0.0)), f.x),\n"
        "             mix(fx_hash(i + float2(0.0, 1.0)), fx_hash(i + float2(1.0, 1.0)), f.x), f.y);\n"
        "}\n"
        "float fx_fbm(float2 p) {\n"
        "  float s = 0.0, a = 0.5;\n"
        "  for (int k = 0; k < 5; ++k) { s += a * fx_noise(p); p = p * 2.03 + float2(17.1, 9.2); a *= 0.5; }\n"
        "  return s;\n"
        "}\n");
}

/* At the top of the fragment function: the time and strength, and for still water the ripple that
 * moves where the first texture is read (gfx_fx_coord). `in` is the inputs' name. */
static void gfx_fx_begin(Sb* b, int fx, const char* in)
{
    if (fx < GFX_FX_CLOUDS)
        return;
    sb_printf(b, "  float fx_t = u.params2.y, fx_k = u.params2.z;\n");
    if (fx == GFX_FX_POOL)
        sb_printf(b,
            "  float2 fx_rip = (float2(fx_fbm(%s.t0.xy * 7.0 + float2(fx_t * 0.07, 0.0)),\n"
            "                         fx_fbm(%s.t0.xy * 7.0 + float2(5.2, 1.3 - fx_t * 0.06))) - 0.5) * (0.03 * fx_k);\n",
            in, in);
}

/* The first stage's texture coordinate, rippled for still water. */
static const char* gfx_fx_coord(int fx, int stage)
{
    return fx == GFX_FX_POOL && stage == 0 ? " + fx_rip" : "";
}

/* After the texture stages, on `cur` (the color the game's stages made). */
static void gfx_fx_end(Sb* b, int fx, const char* in)
{
    switch (fx)
    {
    case GFX_FX_MARK: sb_printf(b, "  cur.rgb = float3(1.0, 0.0, 1.0);\n"); break;
    case GFX_FX_CLOUDS:
        sb_printf(b,
            "  {\n"
            "    float2 q = %s.t0.xy;\n"
            "    float n = fx_fbm(q * 5.0 + float2(fx_t * 0.006, fx_t * 0.002));\n"
            "    float d = fx_fbm(q * 13.0 - float2(fx_t * 0.011, 0.0));\n"
            "    float c = saturate(n * 0.7 + d * 0.3);\n"
            "    cur.a = cur.a * mix(1.0, smoothstep(0.3, 0.72, c) * 1.25, fx_k);\n"
            "    cur.rgb = saturate(cur.rgb * mix(1.0, 0.78 + 0.34 * smoothstep(0.35, 0.8, c), fx_k));\n"
            "  }\n",
            in);
        break;
    case GFX_FX_POOL:
        sb_printf(b,
            "  {\n"
            "    float w = fx_fbm(%s.t0.xy * 11.0 + float2(fx_t * 0.09, -fx_t * 0.07));\n"
            "    float g = pow(saturate(w * 1.8 - 0.95), 3.0);\n"
            "    cur.rgb = saturate(cur.rgb * mix(1.0, 0.85 + 0.25 * w, fx_k) + g * (0.6 * fx_k));\n"
            "  }\n",
            in);
        break;
    case GFX_FX_FALLS:
        sb_printf(b,
            "  {\n"
            "    float2 q = %s.t0.xy;\n"
            "    float s = fx_fbm(float2(q.x * 24.0, q.y * 2.5 - fx_t * 1.2));\n"
            "    cur.rgb = saturate(cur.rgb * mix(1.0, 0.8 + 0.45 * s, fx_k));\n"
            "    cur.a = cur.a * mix(1.0, 0.75 + 0.5 * s, fx_k);\n"
            "  }\n",
            in);
        break;
    default: break;
    }
}
