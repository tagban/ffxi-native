/* The effects of our own a draw can get (gfx_fx.h, d3d8.c fx_classify): GfxFsKey.fx. */
#pragma once

enum
{
    GFX_FX_NONE,
    GFX_FX_MARK,   /* painted magenta: //xi fx mark */
    GFX_FX_CLOUDS, /* the sky's cloud layers */
    GFX_FX_POOL,   /* still water */
    GFX_FX_FALLS,  /* falling water */
    GFX_FX_COUNT
};
