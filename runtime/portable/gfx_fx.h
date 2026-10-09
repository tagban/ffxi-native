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
 *   GFX_FX_FALLS   falling water: streaks running down it (GFX_FX_POOL does it too, where water is steep)
 *   GFX_FX_SKY     the sky's dome: at night more stars, twinkling, and now and then a shooting star
 *                  (GfxU.fxp[4]: how many stars, shooting stars on; [5] east)
 *   GFX_FX_WET     a zone's ground and walls in the rain: darker and deeper in color as they soak,
 *                  shiny (the sky mirrored, most at a glancing look and on the ground: the surface's
 *                  tilt from its eye depth's change across the pixel, GfxU.fxp), and drops striking the
 *                  ground - a small dark dot that fades over a second or so. GfxU.params2.w carries the
 *                  wetness (0-1) plus 2 x the rain (0 none, 1 rain, 2 a downpour). And snow lying on
 *                  the ground in patches that fill in (GfxU.fxp[4].x, 0-1), glinting
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
            "  float fxw_flat, fxw_steep;\n"
            "  float2 fx_rip, fxw_fall;\n"
            "  {\n"
            "    float ez = %s.ez;\n"
            /* the point in eye space, from its pixel and depth; and on the world's ground (east and north
             * of the world, from the camera's place: GfxU.fxp[9], [10]), so the waves are the world's,
             * whether the water has a texture or not (the sea's tiles have none) */
            "    float3 p = float3((%s.pos.x - u.fxp[2].x) * u.fxp[0].x * ez, -(%s.pos.y - u.fxp[2].y) * fx_dy * u.fxp[0].y * ez, ez);\n"
            "    float3 up = u.fxp[1].xyz, east = u.fxp[9].xyz, north = u.fxp[10].xyz;\n"
            "    float2 w = float2(u.fxp[9].w + dot(p, east), u.fxp[10].w + dot(p, north));\n"
            "    float spd = u.fxp[4].y, sz = max(u.fxp[4].z, 0.05), dir = u.fxp[4].w;\n"
            /* five long waves, near the direction the player picked (yalms: some ten long at size 1) */
            "    float2 g = float2(0.0, 0.0);\n"
            "    for (int i = 0; i < 5; ++i) {\n"
            "      float fi = float(i);\n"
            "      float a = dir + (fract(fi * 0.618) - 0.5) * 1.4;\n"
            "      float kl = 6.2831853 / (10.0 * sz / (1.0 + fi * 0.9));\n"
            "      float2 kd = float2(cos(a), sin(a));\n"
            "      float ph = dot(kd, w) * kl - fx_t * spd * (1.0 + sqrt(kl * 4.0) * 0.6);\n"
            "      g = g + kd * (cos(ph) / (1.0 + fi * 0.5));\n"
            "    }\n"
            /* and fine ripples drifting over them */
            "    float2 q = w * (0.6 / sz) + float2(fx_t * 0.06, -fx_t * 0.045) * spd;\n"
            "    float n0 = fx_fbm(q, float2(4096.0, 4096.0)), nx = fx_fbm(q + float2(0.12, 0.0), float2(4096.0, 4096.0)),\n"
            "          ny = fx_fbm(q + float2(0.0, 0.12), float2(4096.0, 4096.0));\n"
            "    g = g * 0.5 + float2(nx - n0, ny - n0) * (1.0 / 0.12) * 0.4;\n"
            "    float2 s = g * u.fxp[4].x * 0.25;\n"
            /* the surface's own slope (its triangle, facing the camera): flat water gets the waves, steep
             * water the falls' streaks; seen from below (a sheet over the camera), neither */
            "    float3 dp1 = dfdx(p), dp2 = dfdy(p);\n"
            "    float3 ng = normalize(cross(dp1, dp2) + float3(0.0, 0.0, 1e-9));\n"
            "    ng = dot(ng, p) > 0.0 ? -ng : ng;\n"
            "    float upw = dot(ng, up);\n"
            "    fxw_flat = smoothstep(0.6, 0.85, upw);\n"
            "    fxw_steep = 1.0 - smoothstep(0.35, 0.6, abs(upw));\n"
            "    s = s * fxw_flat;\n"
            "    fxw_n = normalize(up - east * s.x - north * s.y);\n"
            "    fxw_v = normalize(p + float3(0.0, 0.0, 1e-4));\n"
            "    fx_rip = s * (0.012 * fx_k);\n"
            /* where a fall is: along it (across the world) and its height, for the streaks */
            "    fxw_fall = float2((w.x + w.y) * 1.4, dot(p, up) * 0.3);\n"
            "  }\n",
            in, in, in);
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

/* An aurora on the sky (the dome's draws, or the clouds' when the dome was not found): where this point
 * is, seen from the camera - how high (from up) and which way round (from east and up, so it stays put
 * as the camera turns) - curtains in a band low over the horizon all round, waving, their lower edges
 * brightest, fringed above. `row`: strength, r, g, b; east in u.fxp[5]. */
static void gfx_fx_aurora(Sb* b, const char* in, const char* row)
{
    sb_printf(b,
        "  if (%s.x > 0.0) {\n"
        "    float ez = %s.ez;\n"
        "    float3 p = float3((%s.pos.x - u.fxp[2].x) * u.fxp[0].x * ez, -(%s.pos.y - u.fxp[2].y) * fx_dy * u.fxp[0].y * ez, ez);\n"
        "    float3 d = normalize(p + float3(0.0, 0.0, 1e-4)), up = u.fxp[1].xyz, east = u.fxp[5].xyz;\n"
        "    float3 north = cross(up, east);\n"
        "    float e = dot(d, up);\n"
        "    float az = atan2(dot(d, north), dot(d, east)) * 0.15915494 + 0.5;\n"
        "    float sway = fx_fbm(float2(az * 5.0, fx_t * 0.03), float2(5.0, 4096.0));\n"
        "    float mid = 0.2 + 0.14 * sway;\n"
        "    float h = (e - mid) / 0.14;\n"
        "    float band = exp(-h * h) * smoothstep(0.0, 0.06, e);\n"
        "    float rays = fx_fbm(float2(az * 64.0 + sway * 6.0, fx_t * 0.12), float2(64.0, 4096.0));\n"
        "    float a = saturate(smoothstep(0.3, 0.75, rays) * band * (1.2 - 0.6 * saturate(h)) * %s.x);\n"
        "    float3 c = %s.yzw;\n"
        "    c = mix(c, float3(c.z, c.x * 0.4, c.y) * 0.8 + float3(0.2, 0.0, 0.25), saturate(h * 0.6));\n"
        "    cur.rgb = saturate(mix(cur.rgb, c, a) + c * (a * 0.35));\n"
        "    cur.a = max(cur.a, a);\n"
        "  }\n",
        row, in, in, in, row, row);
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
            "  }\n",
            in);
        gfx_fx_aurora(b, in, "u.fxp[4]");
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
            "    float kw = saturate(fx_k) * fxw_flat;\n"
            /* //xi fx mark 9: the water as the effect sees it - flat blue, steep yellow, the rest grey */
            "    if (u.params2.z < 0.0) c = fxw_flat > 0.5 ? float3(0.1, 0.4, 1.0) : fxw_steep > 0.5 ? float3(1.0, 0.9, 0.1) : float3(0.5, 0.5, 0.5), kw = 0.8;\n"
            "    cur.rgb = mix(cur.rgb, saturate(c), kw);\n"
            "    cur.a = mix(cur.a, max(cur.a, saturate(k * 1.2 + sp)), kw);\n"
            /* steep: the falls' streaks running down it */
            "    float st = fx_fbm(float2(fxw_fall.x, fxw_fall.y + fx_t * 1.2), float2(4096.0, 4096.0));\n"
            "    float kf = fx_k * fxw_steep;\n"
            "    cur.rgb = saturate(cur.rgb * mix(1.0, 0.8 + 0.45 * st, kf));\n"
            "    cur.a = cur.a * mix(1.0, 0.75 + 0.5 * st, kf);\n"
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
            /* the point in eye space and its surface (its triangle, facing the camera), as the water's;
             * where it lies on the world's ground (fxp[9], [10]) */
            "    float ez = %s.ez;\n"
            "    float3 p = float3((%s.pos.x - u.fxp[2].x) * u.fxp[0].x * ez, -(%s.pos.y - u.fxp[2].y) * fx_dy * u.fxp[0].y * ez, ez);\n"
            "    float3 up = u.fxp[1].xyz;\n"
            "    float3 ng = normalize(cross(dfdx(p), dfdy(p)) + float3(0.0, 0.0, 1e-9));\n"
            "    ng = dot(ng, p) > 0.0 ? -ng : ng;\n"
            "    float upward = dot(ng, up);\n"
            "    float ground = smoothstep(0.55, 0.85, upward);\n"
            "    float2 w = float2(u.fxp[9].w + dot(p, u.fxp[9].xyz), u.fxp[10].w + dot(p, u.fxp[10].xyz));\n"
            "    float3 c = cur.rgb;\n"
            "    float l = dot(c, float3(0.299, 0.587, 0.114));\n"
            "    if (u.params2.z < 0.0) {\n"
            /* //xi fx mark 9: which way each surface faces, as the effects see it - ground green, walls
             * red, undersides blue */
            "      float3 m = upward > 0.55 ? float3(0.1, 0.9, 0.2) : upward < -0.55 ? float3(0.2, 0.3, 1.0) : float3(0.95, 0.15, 0.1);\n"
            "      cur.rgb = mix(cur.rgb, m, 0.7);\n"
            "    } else {\n"
            /* soaked: deeper color, less light given back */
            "    c = mix(c, saturate(float3(l, l, l) + (c - float3(l, l, l)) * 1.35), wet * 0.7);\n"
            "    c = c * (1.0 - 0.42 * wet);\n"
            /* puddles: on the ground, where the world's own noise is low, spreading as it soaks */
            "    float pn = fx_fbm(w * 0.18, float2(4096.0, 4096.0));\n"
            "    float puddle = smoothstep(0.6 - 0.14 * wet, 0.68 - 0.14 * wet, 1.0 - pn) * ground * smoothstep(0.3, 0.9, wet);\n"
            /* wet and slick: the sky mirrored (the horizon the game's fog color, deeper overhead), far more
             * at a glancing look (Fresnel) and in the puddles, almost a mirror there */
            "    float3 v = normalize(p + float3(0.0, 0.0, 1e-4));\n"
            "    float3 r = v - 2.0 * dot(v, ng) * ng;\n"
            "    float nv = saturate(-dot(v, ng));\n"
            "    float fres = 0.03 + 0.97 * pow(saturate(1.0 - nv), 5.0);\n"
            "    float e = saturate(dot(r, up));\n"
            "    float3 sky = mix(u.fogcolor.rgb, u.fogcolor.rgb * float3(0.7, 0.8, 1.0) + float3(0.05, 0.07, 0.12), sqrt(e));\n"
            "    float refl = saturate(fres * 1.5 * (0.35 + 0.65 * ground) + puddle * (0.55 + 0.4 * fres)) * wet;\n"
            "    c = mix(c, sky, refl);\n"
            /* the sun's (or moon's) glint off it: sharp in the puddles */
            "    float sp = pow(saturate(dot(r, u.fxp[3].xyz)), 60.0 + 400.0 * puddle) * u.fxp[3].w * wet * (0.4 + 0.6 * ground);\n"
            "    c = c + float3(1.0, 0.96, 0.88) * (sp * 2.0);\n"
            /* rings spreading in the puddles where the rain strikes */
            "    float2 rq = w * 0.8;\n"
            "    float2 rc = floor(rq), rf = fract(rq);\n"
            "    float rh = fx_hash(rc);\n"
            "    float rper = 0.9 + 0.8 * rh;\n"
            "    float rk = floor((fx_t + rh * 7.0) / rper);\n"
            "    float rage = fx_t + rh * 7.0 - rk * rper;\n"
            "    float2 rat = float2(fx_hash(rc + float2(rk, 3.1)), fx_hash(rc + float2(5.7, rk))) * 0.6 + 0.2;\n"
            "    float ring = (1.0 - smoothstep(0.0, 0.04, abs(length(rf - rat) - rage * 0.45))) * exp(-rage * 3.0) *\n"
            "                 step(fx_hash(rc + float2(rk * 0.7, rk)), rain * 1.6);\n"
            "    c = c + float3(0.8, 0.85, 0.9) * (ring * puddle * 0.35);\n"
            /* drops: a cell of the texture each, striking now and then somewhere in it, on the ground */
            "    float2 q = %s.t0.xy * 18.0;\n"
            "    float2 cell = floor(q), f = fract(q);\n"
            "    float h = fx_hash(cell);\n"
            "    float period = 1.6 + 2.0 * h;\n"
            "    float k = floor((fx_t + h * 13.0) / period);\n"
            "    float age = (fx_t + h * 13.0) - k * period;\n"
            "    float2 at = float2(fx_hash(cell + float2(k, 1.7)), fx_hash(cell + float2(2.3, k))) * 0.7 + 0.15;\n"
            "    float hit = step(fx_hash(cell + float2(k * 0.37, k * 1.3)), rain);\n"
            "    float drop = (1.0 - smoothstep(0.05, 0.1, length(f - at))) * exp(-age * 2.2) * hit * (0.25 + 0.75 * ground) * (1.0 - puddle);\n"
            "    c = c * (1.0 - 0.38 * drop);\n"
            /* snow lying: on the ground, in patches that grow until it is all white, lit as the ground
             * was (its brightness), glinting here and there */
            "    float snow = u.fxp[4].x;\n"
            "    if (snow > 0.0) {\n"
            "      float2 sq = %s.t0.xy;\n"
            "      float lie = fx_fbm(sq * 3.0, float2(3.0, 3.0));\n"
            "      float cover = smoothstep(1.0 - snow * 1.2, 1.15 - snow * 1.2, lie) * smoothstep(0.35, 0.75, upward);\n"
            "      float3 white = float3(0.9, 0.93, 1.0) * saturate(0.35 + 1.5 * l);\n"
            "      float2 gq = sq * 64.0;\n"
            "      float gh = fx_hash(floor(gq));\n"
            "      float glint = step(0.985, gh) * (1.0 - smoothstep(0.1, 0.3, length(fract(gq) - 0.5))) * (0.5 + 0.5 * sin(fx_t * 3.0 + gh * 40.0));\n"
            "      c = mix(c, white + glint * 0.6, cover);\n"
            "    }\n"
            "    cur.rgb = mix(cur.rgb, saturate(c), saturate(fx_k));\n"
            "    }\n"
            "  }\n",
            in, in, in, in, in);
        break;
    case GFX_FX_SKY:
        sb_printf(b,
            "  {\n"
            /* where on the sky: how high, which way round (east and up, so it stays as the camera turns) */
            "    float ez = %s.ez;\n"
            "    float3 p = float3((%s.pos.x - u.fxp[2].x) * u.fxp[0].x * ez, -(%s.pos.y - u.fxp[2].y) * fx_dy * u.fxp[0].y * ez, ez);\n"
            "    float3 d = normalize(p + float3(0.0, 0.0, 1e-4)), up = u.fxp[1].xyz, east = u.fxp[5].xyz;\n"
            "    float3 north = cross(up, east);\n"
            "    float e = dot(d, up);\n"
            "    float az = atan2(dot(d, north), dot(d, east)) * 0.15915494 + 0.5;\n"
            "    float l = dot(cur.rgb, float3(0.299, 0.587, 0.114));\n"
            "    float night = (1.0 - smoothstep(0.06, 0.22, l)) * smoothstep(0.02, 0.12, e);\n"
            /* stars: a grid over the sky, finer toward the top (so the cells keep their size), one star in
             * some cells, twinkling */
            "    float rows = 180.0, ring = floor(max(cos(asin(saturate(e))) * 360.0, 6.0));\n"
            "    float2 g = float2(az * ring, e * rows);\n"
            "    float2 cell = floor(g), f = fract(g);\n"
            "    float h = fx_hash(cell + float2(ring, 0.0));\n"
            "    float2 at = float2(fx_hash(cell + 1.7), fx_hash(cell + 3.1)) * 0.6 + 0.2;\n"
            "    float star = step(1.0 - 0.08 * u.fxp[4].x, h) * (1.0 - smoothstep(0.02, 0.12, length(f - at)));\n"
            "    star = star * (0.55 + 0.45 * sin(fx_t * (1.5 + h * 4.0) + h * 60.0)) * (0.4 + 0.6 * fx_hash(cell + 7.7));\n"
            "    float3 tint = mix(float3(1.0, 0.85, 0.7), float3(0.75, 0.85, 1.0), fx_hash(cell + 5.3));\n"
            "    float3 add = tint * star;\n"
            /* a shooting star: every few seconds, maybe, a streak across part of the sky with its tail */
            "    float slot = floor(fx_t / 7.0), ago = fx_t - slot * 7.0;\n"
            "    float sh = fx_hash(float2(slot, 9.1));\n"
            "    if (u.fxp[4].y > 0.0 && sh > 0.45 && ago < 1.1) {\n"
            "      float2 a0 = float2(fx_hash(float2(slot, 1.3)), 0.35 + 0.45 * fx_hash(float2(slot, 2.9)));\n"
            "      float ang = fx_hash(float2(slot, 4.4)) * 6.2831853;\n"
            "      float2 dir = float2(cos(ang) * 0.06, -abs(sin(ang)) * 0.12 - 0.03);\n"
            "      float u0 = saturate(ago / 0.8);\n"
            "      float2 head = a0 + dir * u0;\n"
            "      float2 sp = float2(az, e);\n"
            "      float2 dd = sp - head; dd.x = dd.x - floor(dd.x + 0.5);\n"
            "      float along = dot(dd, -normalize(dir)), across = length(dd + normalize(dir) * along);\n"
            "      float tail = (1.0 - smoothstep(0.0, 0.07, along)) * step(0.0, along) * (1.0 - smoothstep(0.0, 0.0015, across));\n"
            "      add = add + float3(1.0, 0.95, 0.85) * tail * (1.0 - smoothstep(0.75, 1.1, ago)) * 1.5;\n"
            "    }\n"
            "    cur.rgb = saturate(cur.rgb + add * night * saturate(fx_k));\n"
            "  }\n",
            in, in, in);
        gfx_fx_aurora(b, in, "u.fxp[3]");
        break;
    default: break;
    }
}
