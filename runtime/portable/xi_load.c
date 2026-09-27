/* The host's side of the game as a module (xi_game.h): load it, check it, and bind it. */
#include "xi_game.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

const XiGameModule* xi_game;

int xi_game_load(const char* path, char* err, unsigned errsize)
{
#ifdef _WIN32
    HMODULE h = LoadLibraryA(path);
    if (!h)
        return snprintf(err, errsize, "%s: cannot load it (error %lu)", path, (unsigned long)GetLastError()), 0;
    const XiGameModule* m = (const XiGameModule*)(void*)GetProcAddress(h, XI_GAME_SYMBOL);
#else
    void* h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h)
        return snprintf(err, errsize, "%s", dlerror()), 0;
    const XiGameModule* m = (const XiGameModule*)dlsym(h, XI_GAME_SYMBOL);
#endif
    if (!m)
        return snprintf(err, errsize, "%s: not a game module (no %s)", path, XI_GAME_SYMBOL), 0;
    if (m->abi != XI_GAME_ABI || m->size != sizeof(XiGameModule))
        return snprintf(err, errsize, "%s: made for another version of the host (module ABI %u, host %u); make the game again",
                   path, m->abi, XI_GAME_ABI), 0;
    xi_game = m;
    return 1;
}

void xi_game_bind(void)
{
    *xi_game->guest_base = rt_guest_base;
    rt_lock_contended_ptr = xi_game->lock_contended;
    XiHostFns fns = { rt_call_indirect, rt_fatal, rt_cpuid, rt_safepoint };
    xi_game->set_host(&fns);
}
