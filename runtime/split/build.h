/* build.h for a host built without the translation (XI_SPLIT): what the generated build.h says of
 * one build, read from the game module the host loaded (xi_game.h) instead. */
#pragma once
#include "xi_game.h"

#define FFXI_BUILD (xi_game->build)
#define FFXI_VERSION (xi_game->version)
#define FFXI_CHARS_PTR (xi_game->chars_ptr)
#define FFXI_PRESENT_SITE (xi_game->present_site)
/* hooks are known only at run time: host64.c checks xi_game->hook_nameplate_scale */
#define FFXI_HOOK_NAMEPLATE_SCALE_RUNTIME 1
