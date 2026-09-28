/* The game as a module: the translation of one build of FFXiMain.dll and FFXi.dll, compiled on the
 * player's machine into a shared library (runtime/xi_module.c + generated/), and loaded by a host
 * that ships prebuilt (runtime/portable/xi_load.c, built with XI_SPLIT).
 *
 * The translation needs little of the runtime: six functions (given to it through XiHostFns) and
 * three variables, which the module keeps itself so its hot paths read them directly: the guest
 * window's base and the relocation delta (set once, before any guest code runs) and the guest
 * lock's contention flag (which the host's threads write through the pointer here). Everything
 * else crosses in the host's direction: the tables, the image's pinned values, and what build.h
 * says of the build, all in XiGameModule. */
#pragma once

#include "runtime.h"

#define XI_GAME_ABI 3

typedef struct XiHostFns
{
    void (*call_indirect)(Guest* g, uint32_t target);
    void (*fatal)(Guest* g, uint32_t addr, const char* what);
    void (*cpuid)(Guest* g);
    void (*safepoint)(void);
    void* (*setjmp_buf)(Guest* g);
    void* (*longjmp_regs)(Guest* g);
} XiHostFns;

typedef struct XiGameModule
{
    uint32_t abi, size; /* XI_GAME_ABI, sizeof (XiGameModule) */
    const char* build;   /* build.h FFXI_BUILD: meta/builds.json's label */
    const char* version; /* FFXI_VERSION: the version string patch.ver carries */
    uint32_t chars_ptr;  /* FFXI_CHARS_PTR */
    uint32_t present_site;
    /* FFXiMain's translation (table.c) */
    const RtEntry* table;
    const unsigned* table_count;
    const unsigned char* table_patch;
    const uint32_t *image_base, *image_timestamp, *image_size, *image_text_rva, *image_text_size, *image_pol1_rva,
        *image_pol1_src_len, *image_oep, *image_reloc_rva;
    /* FFXi.dll's (recomp.py --module ffxi) */
    const RtModule* ffxi;
    /* host hook points (meta/builds.json "hooks"), NULL when the build has none */
    GuestFn* hook_nameplate_scale;
    /* the module's own copies of the runtime's variables */
    unsigned char** guest_base;
    uint32_t* reloc_delta;
    volatile uint32_t* lock_contended;
    void (*set_host)(const XiHostFns* fns);
    /* appended (size tells whether a module has it): the hook at the end of the game's decrypt and
     * decompress of an incoming packet (meta/builds.json "packet_in"), NULL when its build has none */
    GuestFn* hook_packet_in;
    /* the entry of the game's encrypt of an outgoing packet ("packet_out"): the player's own chat */
    GuestFn* hook_packet_out;
    /* the entry of the game's add-a-line-to-the-chat-log ("chat_add"): every line its log shows */
    GuestFn* hook_chat_add;
    /* the game's parser of a typed line ("addresses" input_line; 0 when unknown): cdecl, the line
     * and how it came (1: typed). Its own menus run their commands through it too. */
    uint32_t input_line;
} XiGameModule;

/* The one symbol a module exports. */
#define XI_GAME_SYMBOL "xi_game_module"

#ifdef XI_SPLIT
/* the host: the module it loaded (xi_load.c) */
extern const XiGameModule* xi_game;
/* Loads the module at path (a .dylib, .so or .dll) and checks it; 0 with a message on failure. */
int xi_game_load(const char* path, char* err, unsigned errsize);
/* Once the guest window is reserved: hands the module the host's functions and state. */
void xi_game_bind(void);
#endif
