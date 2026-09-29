/* The module's side of the game as a module (xi_game.h): compiled with the translation of one
 * build into a shared library that exports xi_game_module and nothing else. */
#include "xi_game.h"
#include "build.h" /* the build this translation is of */

#include <stdlib.h>

#if defined(_WIN32)
#define XI_EXPORT __declspec(dllexport)
#else
#define XI_EXPORT __attribute__((visibility("default")))
#endif

/* the runtime's variables the translation reads (guest.h), kept here */
unsigned char* rt_guest_base;
uint32_t rt_reloc_delta;
volatile uint32_t rt_lock_contended;

static XiHostFns g_host;

void rt_call_indirect(Guest* g, uint32_t target) { g_host.call_indirect(g, target); }
void rt_fatal(Guest* g, uint32_t addr, const char* what)
{
    g_host.fatal(g, addr, what);
    abort(); /* the host's does not return */
}
void rt_cpuid(Guest* g) { g_host.cpuid(g); }
void rt_safepoint(void) { g_host.safepoint(); }
void* rt_setjmp_buf(Guest* g) { return g_host.setjmp_buf(g); }
void* rt_longjmp_regs(Guest* g) { return g_host.longjmp_regs(g); }

static void set_host(const XiHostFns* fns) { g_host = *fns; }

extern const RtModule rt_module_ffxi;
#ifdef FFXI_HOOK_NAMEPLATE_SCALE
extern GuestFn rt_hook_nameplate_scale;
#endif
#ifdef FFXI_HOOK_PACKET_IN
extern GuestFn rt_hook_packet_in;
#endif
#ifdef FFXI_HOOK_PACKET_OUT
extern GuestFn rt_hook_packet_out;
#endif
#ifdef FFXI_HOOK_CHAT_ADD
extern GuestFn rt_hook_chat_add;
#endif

XI_EXPORT const XiGameModule xi_game_module = {
    XI_GAME_ABI,
    sizeof(XiGameModule),
    FFXI_BUILD,
    FFXI_VERSION,
    FFXI_CHARS_PTR,
    FFXI_PRESENT_SITE,
    rt_table,
    &rt_table_count,
    rt_table_patch,
    &rt_image_base,
    &rt_image_timestamp,
    &rt_image_size,
    &rt_image_text_rva,
    &rt_image_text_size,
    &rt_image_pol1_rva,
    &rt_image_pol1_src_len,
    &rt_image_oep,
    &rt_image_reloc_rva,
    &rt_module_ffxi,
#ifdef FFXI_HOOK_NAMEPLATE_SCALE
    &rt_hook_nameplate_scale,
#else
    NULL,
#endif
    &rt_guest_base,
    &rt_reloc_delta,
    &rt_lock_contended,
    set_host,
#ifdef FFXI_HOOK_PACKET_IN
    &rt_hook_packet_in,
#else
    NULL,
#endif
#ifdef FFXI_HOOK_PACKET_OUT
    &rt_hook_packet_out,
#else
    NULL,
#endif
#ifdef FFXI_HOOK_CHAT_ADD
    &rt_hook_chat_add,
#else
    NULL,
#endif
#ifdef FFXI_INPUT_LINE
    FFXI_INPUT_LINE,
#else
    0,
#endif
#ifdef FFXI_ENTITY_MAP
    FFXI_ENTITY_MAP,
#else
    0,
#endif
#ifdef FFXI_MZB_KEYS
    FFXI_MZB_KEYS,
#else
    0,
#endif
};
