/* Effects of our own on some of the game's draws, the same for every back end (the generators for
 * Metal, GLSL and HLSL each include this after their Sb). d3d8.c (fx_classify) picks the draws and
 * sets GfxFsKey.fx; GfxU.params2 carries y: seconds (wrapping), z: how strong (0 = the game's own).
 *
 *   GFX_FX_MARK    painted magenta: //xi fx mark, to see which draws a rule takes
 *   GFX_FX_CLOUDS  the sky's cloud layers: shapes broken up by moving noise, lit tops, darker undersides
 *   GFX_FX_POOL    still water: waves (whole waves to the texture's repeat, so they meet where its
 *                  pieces do, and fine ripples), the sky mirrored by them - far more at a glancing look
 *                  (Fresnel) - and the sun's or moon's glint; the game's water seen through them, tinted
 *                  as the player likes (GfxU.fxp[4], [5])
 *   GFX_FX_FALLS   falling water: streaks running down it
 *   GFX_FX_WET     a zone's ground and walls in the rain: darker and deeper in color as they soak,
 *                  shiny (the sky mirrored, most at a glancing look and on the ground: the surface's
 *                  tilt from its eye depth's change across the pixel, GfxU.fxp), and drops striking the
 *                  ground - a small dark dot that fades over a second or so. GfxU.params2.w carries the
 *                  wetness (0-1) plus 2 x the rain (0 none, 1 rain, 2 a downpour)
 *
 * GFX_FX_CLOUDS also carries an aurora when one is asked for (GfxU.fxp[4], [5]: MogHouse's !skyfx or the
 * player's): curtains of light low over the horizon all around, waving, their lower edges brightest.
 *
 * And the weather on every fogged draw (gfx_fx_weather_*): its fog, nearer (GfxU.fxp[0].w), and the
 * heat's shimmer, the textures read a pixel or two aside in rising bands, the more the farther (.z).
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
    /* fx_dy: down the screen is +1 (Metal's and Direct3D's pixel rows run down; OpenGL's run up) */
    if (lang == GFX_FX_GLSL)
        sb_printf(b, "#define float2 vec2\n#define float3 vec3\n#define float4 vec4\n#define dfdx dFdx\n#define dfdy dFdy\n"
                     "#define fx_dy (-1.0)\n#define atan2 atan\n");
    else if (lang == GFX_FX_HLSL)
        sb_printf(b, "#define fract frac\n#define mix lerp\n#define dfdx ddx\n#define dfdy ddy\n#define fx_dy 1.0\n");
    else
        sb_printf(b, "#define fx_dy 1.0\n");
    if (fx < GFX_FX_CLOUDS)
        return;
    sb_printf(b,
        "float fx_hash(float2 p) { p = fract(p * float2(123.34, 456.21)); p += dot(p, p + 45.32); return fract(p.x * p.y); }\n"
        /* noise that repeats every `per` (whole numbers): the game's texture coordinates jump by whole
         * repeats where its meshes' pieces meet (the sky's panels), and the noise has to meet there */
        "float2 fx_wrap(float2 i, float2 per) { return i - per * floor(i / per); }\n"
        "float fx_noise(float2 p, float2 per) {\n"
        "  float2 i = floor(p), f = fract(p);\n"
        "  f = f * f * (3.0 - 2.0 * f);\n"
        "  return mix(mix(fx_hash(fx_wrap(i, per)), fx_hash(fx_wrap(i + float2(1.0, 0.0), per)), f.x),\n"
        "             mix(fx_hash(fx_wrap(i + float2(0.0, 1.0), per)), fx_hash(fx_wrap(i + float2(1.0, 1.0), per)), f.x), f.y);\n"
        "}\n"
        "float fx_fbm(float2 p, float2 per) {\n"
        "  float s = 0.0, a = 0.5;\n"
        "  for (int k = 0; k < 5; ++k) { s += a * fx_noise(p, per); p = p * 2.0 + float2(17.0, 9.0); per = per * 2.0; a *= 0.5; }\n"
        "  return s;\n"
        "}\n");
}

/* At the top of the fragment function: the time and strength, and for still water its waves: the
 * surface's normal (up, tilted by them) and the look at it, and the ripple that moves where the first
 * texture is read (gfx_fx_coord). `in` is the inputs' name. */
static void gfx_fx_begin(Sb* b, int fx, const char* in)
{
    if (fx < GFX_FX_CLOUDS)
        return;
    sb_printf(b, "  float fx_t = u.params2.y, fx_k = u.params2.z;\n");
    if (fx == GFX_FX_POOL)
        sb_printf(b,
            "  float3 fxw_n, fxw_v;\n"
            "  float2 fx_rip;\n"
            "  {\n"
            "    float2 uv = %s.t0.xy;\n"
            "    float ez = %s.ez;\n"
            /* the point in eye space, from its pixel and depth */
            "    float3 p = float3((%s.pos.x - u.fxp[2].x) * u.fxp[0].x * ez, -(%s.pos.y - u.fxp[2].y) * fx_dy * u.fxp[0].y * ez, ez);\n"
            "    float3 up = u.fxp[1].xyz;\n"
            /* which ways the texture runs across the water (its cotangent frame) */
            "    float3 dp1 = dfdx(p), dp2 = dfdy(p);\n"
            "    float2 du1 = dfdx(uv), du2 = dfdy(uv);\n"
            "    float3 c2 = cross(dp2, up), c1 = cross(up, dp1);\n"
            "    float3 T = c2 * du1.x + c1 * du2.x, B = c2 * du1.y + c1 * du2.y;\n"
            "    float im = 1.0 / sqrt(max(max(dot(T, T), dot(B, B)), 1e-20));\n"
            "    T = T * im; B = B * im;\n"
            "    float spd = u.fxp[4].y, sz = max(u.fxp[4].z, 0.05), dir = u.fxp[4].w;\n"
            /* five long waves, near the direction the player picked: whole waves to the texture's repeat */
            "    float2 g = float2(0.0, 0.0);\n"
            "    for (int i = 0; i < 5; ++i) {\n"
            "      float fi = float(i);\n"
            "      float a = dir + (fract(fi * 0.618) - 0.5) * 1.4;\n"
            "      float2 K = floor(float2(cos(a), sin(a)) * ((3.0 / sz) * (1.0 + fi * 0.9)) + 0.5);\n"
            "      float kl = max(length(K), 1e-3);\n"
            "      float ph = dot(K, uv) * 6.2831853 - fx_t * spd * (1.0 + sqrt(kl) * 0.6);\n"
            "      g = g + (K / kl) * (cos(ph) * step(0.5, kl) / (1.0 + fi * 0.5));\n"
            "    }\n"
            /* and fine ripples drifting over them */
            "    float per = max(floor(12.0 / sz + 0.5), 1.0);\n"
            "    float2 q = uv * per + float2(fx_t * 0.06, -fx_t * 0.045) * spd;\n"
            "    float n0 = fx_fbm(q, float2(per, per)), nx = fx_fbm(q + float2(0.12, 0.0), float2(per, per)),\n"
            "          ny = fx_fbm(q + float2(0.0, 0.12), float2(per, per));\n"
            "    g = g * 0.5 + float2(nx - n0, ny - n0) * (1.0 / 0.12) * 0.4;\n"
            "    float2 s = g * u.fxp[4].x * 0.25;\n"
            "    fxw_n = normalize(up - T * s.x - B * s.y);\n"
            "    fxw_v = normalize(p + float3(0.0, 0.0, 1e-4));\n"
            "    fx_rip = s * (0.012 * fx_k);\n"
            "  }\n",
            in, in, in, in);
}

/* The first stage's texture coordinate, rippled for still water. */
static const char* gfx_fx_coord(int fx, int stage)
{
    return fx == GFX_FX_POOL && stage == 0 ? " + fx_rip" : "";
}

/* --- the weather, on every fogged draw ------------------------------------------------------------------- */
/* Before the fragment function: reading a texture a few pixels aside (the heat's shimmer). */
static void gfx_fx_weather_functions(Sb* b, int fog, int lang)
{
    if (!fog)
        return;
    if (lang == GFX_FX_GLSL)
        sb_printf(b, "vec2 fxw_uv(vec2 c, vec2 s) { return c + dFdx(c) * s.x + dFdy(c) * s.y; }\n");
    else if (lang == GFX_FX_HLSL)
        sb_printf(b, "float2 fxw_uv(float2 c, float2 s) { return c + ddx(c) * s.x + ddy(c) * s.y; }\n");
    else
        sb_printf(b, "float2 fxw_uv(float2 c, float2 s) { return c + dfdx(c) * s.x + dfdy(c) * s.y; }\n");
}

/* At the top: how far aside, in pixels (0 without heat): rising bands, wavering, more far away. */
static void gfx_fx_weather_begin(Sb* b, int fog, int lang, const char* in)
{
    if (!fog)
        return;
    const char* v2 = lang == GFX_FX_GLSL ? "vec2" : "float2";
    sb_printf(b,
        "  %s fxw_p = %s.pos.xy / max(u.fxp[2].z, 0.25);\n"
        "  float fxw_d = smoothstep(10.0, 55.0, abs(%s.ez)) * u.fxp[0].z * max(u.fxp[2].z, 0.25);\n"
        "  %s fxw_s = %s(sin(fxw_p.y * 0.23 + u.fxp[1].w * 6.0 + sin(fxw_p.x * 0.031 + u.fxp[1].w * 1.3) * 2.5) * 1.5,\n"
        "                 sin(fxw_p.y * 0.11 + u.fxp[1].w * 4.0 + fxw_p.x * 0.047) * 0.8) * fxw_d;\n",
        v2, in, in, v2, v2);
}

/* A texture coordinate (an expression) read where the heat moves it: into out. */
static const char* gfx_fx_weather_uv(char* out, size_t n, int fog, const char* coord)
{
    if (fog)
        snprintf(out, n, "fxw_uv(%s, fxw_s)", coord);
    else
        snprintf(out, n, "%s", coord);
    return out;
}

/* A color filter (MogHouse's !skyfx, or the player's), on everything but the interface: the color
 * plus each row's change (GfxU.fxp[6..8]: all 0 none), on the color the draw would leave. */
static void gfx_fx_filter(Sb* b, const char* col, int lang)
{
    sb_printf(b, "  %s.rgb = clamp(%s.rgb + %s(dot(%s.rgb, u.fxp[6].xyz) + u.fxp[6].w, dot(%s.rgb, u.fxp[7].xyz) + u.fxp[7].w,\n"
                 "      dot(%s.rgb, u.fxp[8].xyz) + u.fxp[8].w), 0.0, 1.0);\n",
        col, col, lang == GFX_FX_GLSL ? "vec3" : "float3", col, col, col);
}

/* The fog factor f (1 clear) thinned by the weather's fog, by the eye depth: after the game's own. */
static void gfx_fx_weather_fog(Sb* b, const char* in)
{
    sb_printf(b, "  f = f * exp(-abs(%s.ez) * u.fxp[0].w);\n", in);
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
            "    float n = fx_fbm(q * 5.0 + float2(fx_t * 0.006, fx_t * 0.002), float2(5.0, 5.0));\n"
            "    float d = fx_fbm(q * 13.0 - float2(fx_t * 0.011, 0.0), float2(13.0, 13.0));\n"
            "    float c = saturate(n * 0.7 + d * 0.3);\n"
            "    cur.a = cur.a * mix(1.0, smoothstep(0.3, 0.72, c) * 1.25, fx_k);\n"
            "    cur.rgb = saturate(cur.rgb * mix(1.0, 0.78 + 0.34 * smoothstep(0.35, 0.8, c), fx_k));\n"
            /* in a fog (the weather's), the clouds all but lost in it */
            "    cur.rgb = mix(cur.rgb, u.fogcolor.rgb, saturate(u.fxp[0].w * 15.0));\n"
            "  }\n"
            /* an aurora: where this point of the dome is, seen from the camera - how high (from up) and
             * which way round (from east and up, so it stays put as the camera turns) */
            "  if (u.fxp[4].x > 0.0) {\n"
            "    float ez = %s.ez;\n"
            "    float3 p = float3((%s.pos.x - u.fxp[2].x) * u.fxp[0].x * ez, -(%s.pos.y - u.fxp[2].y) * fx_dy * u.fxp[0].y * ez, ez);\n"
            "    float3 d = normalize(p + float3(0.0, 0.0, 1e-4)), up = u.fxp[1].xyz, east = u.fxp[5].xyz;\n"
            "    float3 north = cross(up, east);\n"
            "    float e = dot(d, up);\n"
            "    float az = atan2(dot(d, north), dot(d, east)) * 0.15915494 + 0.5;\n"
            "    float sway = fx_fbm(float2(az * 5.0, fx_t * 0.03), float2(5.0, 4096.0));\n"
            "    float mid = 0.22 + 0.16 * sway;\n"
            "    float h = (e - mid) / 0.11;\n"
            "    float band = exp(-h * h) * smoothstep(0.0, 0.08, e);\n"
            "    float rays = fx_fbm(float2(az * 64.0 + sway * 6.0, fx_t * 0.12), float2(64.0, 4096.0));\n"
            "    float a = saturate(smoothstep(0.3, 0.75, rays) * band * (1.2 - 0.6 * saturate(h)) * u.fxp[4].x);\n"
            "    float3 c = u.fxp[4].yzw;\n"
            "    c = mix(c, float3(c.z, c.x * 0.4, c.y) * 0.8 + float3(0.2, 0.0, 0.25), saturate(h * 0.6));\n"
            "    cur.rgb = saturate(mix(cur.rgb, c, a) + c * (a * 0.35));\n"
            "    cur.a = max(cur.a, a);\n"
            "  }\n",
            in, in, in, in);
        break;
    case GFX_FX_POOL:
        sb_printf(b,
            "  {\n"
            "    float3 n = fxw_n, v = fxw_v, up = u.fxp[1].xyz;\n"
            "    float nv = saturate(-dot(v, n));\n"
            "    float fres = 0.02 + 0.98 * pow(saturate(1.0 - nv), 5.0);\n"
            "    float3 r = v - 2.0 * dot(v, n) * n;\n"
            /* the sky it mirrors: the horizon is the game's fog color, deeper and bluer overhead */
            "    float e = saturate(dot(r, up));\n"
            "    float3 hor = u.fogcolor.rgb;\n"
            "    float3 sky = mix(hor, hor * float3(0.62, 0.76, 0.98) + float3(0.02, 0.05, 0.12), sqrt(e));\n"
            "    float tint = u.fxp[5].x;\n"
            "    float3 c = cur.rgb * u.fxp[5].y;\n"
            "    float l = dot(c, float3(0.299, 0.587, 0.114));\n"
            "    float3 hue = tint < 0.0 ? float3(0.35, 0.68, 1.2) : float3(0.3, 1.05, 0.78);\n"
            "    c = mix(c, hue * (l * 1.3), abs(tint) * 0.7);\n"
            "    float k = saturate(fres * u.fxp[5].z);\n"
            "    c = mix(c, sky, k);\n"
            /* the sun's (or moon's) glint off the waves */
            "    float sp = pow(saturate(dot(r, u.fxp[3].xyz)), 500.0 / max(u.fxp[2].w, 0.1)) * u.fxp[5].w * u.fxp[3].w;\n"
            "    c = c + float3(1.0, 0.95, 0.85) * (sp * 4.0);\n"
            "    cur.rgb = mix(cur.rgb, saturate(c), saturate(fx_k));\n"
            "    cur.a = mix(cur.a, max(cur.a, saturate(k * 1.2 + sp)), saturate(fx_k));\n"
            "  }\n");
        break;
    case GFX_FX_FALLS:
        sb_printf(b,
            "  {\n"
            "    float2 q = %s.t0.xy;\n"
            "    float s = fx_fbm(float2(q.x * 24.0, q.y * 3.0 - fx_t * 1.2), float2(24.0, 3.0));\n"
            "    cur.rgb = saturate(cur.rgb * mix(1.0, 0.8 + 0.45 * s, fx_k));\n"
            "    cur.a = cur.a * mix(1.0, 0.75 + 0.5 * s, fx_k);\n"
            "  }\n",
            in);
        break;
    case GFX_FX_WET:
        sb_printf(b,
            "  {\n"
            "    float lvl = floor(u.params2.w * 0.5), wet = u.params2.w - 2.0 * lvl, rain = lvl * 0.5;\n"
            /* the surface's tilt, from how its eye depth changes across the pixel: n faces the camera */
            "    float ez = %s.ez, ezx = dfdx(ez), ezy = dfdy(ez) * fx_dy;\n"
            "    float fa = u.fxp[0].x, fb = u.fxp[0].y;\n"
            "    float3 n = normalize(float3(fb * ezx, -fa * ezy, -fa * fb * ez) + float3(0.0, 0.0, -1e-6));\n"
            "    float upward = saturate(dot(n, u.fxp[1].xyz));\n"
            "    float ground = smoothstep(0.55, 0.85, upward);\n"
            "    float3 c = cur.rgb;\n"
            "    float l = dot(c, float3(0.299, 0.587, 0.114));\n"
            /* soaked: deeper color, less light given back */
            "    c = mix(c, saturate(float3(l, l, l) + (c - float3(l, l, l)) * 1.25), wet * 0.6);\n"
            "    c = c * (1.0 - 0.32 * wet);\n"
            /* wet and shiny: the sky (the game's fog color is its horizon) mirrored, far more at a glancing
             * look across the surface (Fresnel); the ground most, walls and things standing a little */
            "    float glance = 1.0 - saturate(-n.z);\n"
            "    float fres = 0.04 + 0.96 * glance * glance * glance * glance * glance;\n"
            "    float3 sky = mix(u.fogcolor.rgb, float3(1.0, 1.0, 1.0), 0.15);\n"
            "    c = mix(c, sky, saturate(fres * 1.6) * wet * (0.3 + 0.7 * ground) * 0.85);\n"
            /* drops: a cell of the texture each, striking now and then somewhere in it, on the ground */
            "    float2 q = %s.t0.xy * 18.0;\n"
            "    float2 cell = floor(q), f = fract(q);\n"
            "    float h = fx_hash(cell);\n"
            "    float period = 1.6 + 2.0 * h;\n"
            "    float k = floor((fx_t + h * 13.0) / period);\n"
            "    float age = (fx_t + h * 13.0) - k * period;\n"
            "    float2 at = float2(fx_hash(cell + float2(k, 1.7)), fx_hash(cell + float2(2.3, k))) * 0.7 + 0.15;\n"
            "    float hit = step(fx_hash(cell + float2(k * 0.37, k * 1.3)), rain);\n"
            "    float drop = (1.0 - smoothstep(0.05, 0.1, length(f - at))) * exp(-age * 2.2) * hit * (0.25 + 0.75 * ground);\n"
            "    c = c * (1.0 - 0.38 * drop);\n"
            "    cur.rgb = mix(cur.rgb, c, saturate(fx_k));\n"
            "  }\n",
            in, in);
        break;
    default: break;
    }
}
