/* GLSL generation for the OpenGL back end (gfx_glsl.c, gfx_glsl_shaders.c): the same keys and token
 * streams as gfx_hlsl.h, as GLSL 4.10 (OpenGL 4.1 core: Mesa on Linux, and macOS). */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "gfx.h"

typedef struct Sb
{
    char* s;
    size_t len, cap;
} Sb;

void sb_printf(Sb* b, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

/* The source of both stages for a key pair - the vertex function under VERTEX, the fragment
 * function otherwise; the back end puts the #version line, that define and CLIPZ in front - or
 * NULL when a shader uses something the translator does not know (malloc'd; the caller frees). */
char* gfx_glsl_generate(const GfxVsKey* vk, const GfxFsKey* fk, const uint32_t* vs_tokens, const uint32_t* ps_tokens);

/* vs.1.0/1.1 and ps.1.0-1.3 token streams: the function bodies (after the signature, before the
 * fragment tail, which reads r0). 0 when the shader cannot be translated. */
int gfx_glsl_vs1(Sb* b, const GfxVsKey* k, const uint32_t* tokens);
int gfx_glsl_ps1(Sb* b, const GfxFsKey* k, const uint32_t* tokens);

/* v# for an element: a vec4 read from its stream; texture slot i sampled at coord */
void gfx_glsl_fetch(Sb* b, const GfxVsKey* k, int reg);
void gfx_glsl_sample(Sb* b, const char* dst, int i, const char* coord);

/* The utility functions every device needs (present, the frame-rate overlay), one source: PRESENT
 * or OVERLAY, and VERTEX or not, pick the function. */
extern const char gfx_glsl_util[];
