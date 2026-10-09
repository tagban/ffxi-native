/* The effects of our own a draw can get (gfx_fx.h, d3d8.c fx_classify): GfxFsKey.fx. */
#pragma once

enum
{
    GFX_FX_NONE,
    GFX_FX_MARK,   /* painted magenta: //xi fx mark */
    GFX_FX_CLOUDS, /* the sky's cloud layers */
    GFX_FX_POOL,   /* water: still where it lies flat, falling where it is steep (its slope in the shader) */
    GFX_FX_FALLS,  /* falling water (now within GFX_FX_POOL; kept so the numbers stay) */
    GFX_FX_WET,    /* a zone's ground and walls in the rain: darker, deeper color, drops striking; snow lying */
    GFX_FX_SKY,    /* the sky's dome: more stars at night, now and then a shooting star */
    GFX_FX_COUNT
};
