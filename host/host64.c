/* The game host for 64-bit machines (R3: Windows x64 now, arm64 macOS next).
 *
 * What pol.exe and COM do for retail FFXI on Windows, with no x86 anywhere: the retail
 * FFXiMain.dll and FFXi.dll are mapped into the guest window (FFXiMain at its preferred
 * 0x10000000, FFXi.dll - which prefers the same base - relocated), both CRTs start, FFXi.dll's
 * FFXiEntry is created through its class factory, and IFFXiEntry::GameStart runs the game with
 * our own polcore (runtime/portable/polcore.c) in place of PlayOnline's.
 *
 * usage: host64 --game <FINAL FANTASY XI folder> [--reg <file.reg>]... [--reg-overlay <file.reg>]
 *               [--reg-final <file.reg>]...   loaded after the overlay: a launcher's settings
 *               [--data-dir <folder>]   where host64 writes its own files (default: beside it)
 *               [--server <name or a.b.c.d>]
 *               [--session <V: 16 characters, or 32 hex digits>]                   PlayOnline servers
 *               [--user <name> [--pass <password>] [--otp <code>] [--login-token <t>]   LandSandBoat servers
 *                [--authport 54231] [--dataport 54230] [--viewport 54001]]
 *               [--dats <folder>]...   DAT overlays, as XIPivot: the first folder given wins
 *               [--textures <folder>]...   texture packs: high-resolution replacements for the
 *                                      game's textures (tools/make_texpack.py); default <data dir>/textures
 *               [--user-dir <folder>]  the game's USER folder (settings, macros) there instead of
 *                                      in the install, for installs that cannot be written (UWP)
 *
 * --server is where the game's servers are: "ffxi00.pol.com" (the lobby) and every other
 * "*.pol.com" name resolve to it instead of through DNS. Default 127.0.0.1 (this
 * machine); --pol-server and --lobby are older names for it.
 *
 * --fps-divisor: FFXI's frames are 60 / divisor per second; 1 (60 fps) here, 2 (30) as shipped.
 *
 * --aspect auto|off|<w:h>: the 3D scene's aspect ratio. auto (the default) follows the window's
 * shape, so a widescreen window shows more to the sides instead of a 4:3 view stretched across it.
 *
 * --ui-aspect <w:h>: the interface keeps this shape (16:9, say) centered in a wider window, instead
 * of being stretched across it; the mouse is mapped to match. Off by default; an app bundle's
 * FFXIUIAspect key is the default.
 *
 * --nameplates fix|off: the names over characters' heads keep the shape they have in a 4:3 window
 * (fix, the default) or widen with the window as the game draws them (off).
 * --nameplate-scale <s>|<sx>x<sy>: their size, 1 as the game draws them (1.25; 1x1.2 for taller).
 * An app bundle's FFXINameplates and FFXINameplateScale keys are the defaults for both.
 *
 * --viewer <folder>: the PlayOnlineViewer folder, when it is not beside FINAL FANTASY XI.
 * --version-dir <folder>: another version of the game over the install (the launcher's xi-vault
 *   overlay: FFXiMain.dll, FFXi.dll and every file that version has the install does not).
 *
 * --live <file>: settings the launcher changes while the game runs (live_reload): lines of
 * key = value, looked at twice a second. With it, the host also answers the launcher's hotkey
 * (Cmd+, on macOS, Ctrl+F12 elsewhere) and reports the lobby's errors, as "@launcher" lines on
 * stdout.
 *
 * Two ways in:
 *   - a server with PlayOnline behind it: the session value V its lobby checks
 *     (pol_accounts.session_value). --session gives it; else the sign-in screen gets it.
 *   - a LandSandBoat server with none (xiloader's path, host/lsb_login.c): --user signs in on the
 *     server's auth port first. The password comes from --pass, else FFXI_PASSWORD, else the
 *     sign-in screen; --otp is the two-factor code, if the account has one. --login-token is a
 *     launch token from the server's own launcher, in place of the password and code.
 *
 * With neither, the sign-in screen (host/signin.c) comes first, in the game's own UI art: either
 * method, picked in its Settings, and the server; it remembers them in <data dir>/signin.cfg
 * (--data-dir, else the user's app data), the password in the keychain, and writes display
 * defaults to <data dir>/settings.reg, loaded when no --reg-final is given. Its window becomes the
 * game's. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gthread.h"
#include "gwin.h"
#include "k32.h"
#include "pe.h"
#include "polcore.h"
#include "polcore_config.h"
#include "lsb_login.h"
#include "signin.h"
#include "appdefaults.h"
#if defined(_WIN32)
#include "sampler.h"
#endif
#include "thunk.h"
#include "user32.h"
#include "d3d8.h"
#include "gfx.h"
#include "dsound.h"
#include "overlay.h"
#include "gamestate.h"
#include "dinput.h"
#include "ws2.h"
#include "plat.h"
#include "vfs.h"
#include "build.h" /* FFXI_VERSION */
#include <SDL3/SDL_keycode.h> /* the launcher's hotkey */

#ifndef XI_SPLIT
extern const RtModule rt_module_ffxi; /* recomp.py --module ffxi */
#endif

#define FFXI_BASE 0x0F000000u /* where FFXi.dll goes: free, below FFXiMain */
#define DEFAULT_POL_SERVER 0x7F000001u /* 127.0.0.1: a server on this machine */

/* FFXiEntry {989D790D-6236-11D4-80E9-00105A81E890}, IFFXiEntry {989D790C-...} */
static const uint8_t CLSID_FFXiEntry[16] = { 0x0D, 0x79, 0x9D, 0x98, 0x36, 0x62, 0xD4, 0x11,
                                             0x80, 0xE9, 0x00, 0x10, 0x5A, 0x81, 0xE8, 0x90 };
static const uint8_t IID_IFFXiEntry[16] = { 0x0C, 0x79, 0x9D, 0x98, 0x36, 0x62, 0xD4, 0x11,
                                            0x80, 0xE9, 0x00, 0x10, 0x5A, 0x81, 0xE8, 0x90 };
/* GameMain {1027DC46-750D-4B1F-8834-1D25B8BEBAB8} (FFXiMain), FxFileManager
 * {0DF0E951-D03C-4A94-90EF-40AE60668F5F} (FFXi.dll): the classes FFXi.dll creates */
static const uint8_t CLSID_GameMain[16] = { 0x46, 0xDC, 0x27, 0x10, 0x0D, 0x75, 0x1F, 0x4B,
                                            0x88, 0x34, 0x1D, 0x25, 0xB8, 0xBE, 0xBA, 0xB8 };
static const uint8_t CLSID_FxFileManager[16] = { 0x51, 0xE9, 0xF0, 0x0D, 0x3C, 0xD0, 0x94, 0x4A,
                                                 0x90, 0xEF, 0x40, 0xAE, 0x60, 0x66, 0x8F, 0x5F };
static const uint8_t IID_IClassFactory[16] = { 0x01, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46 };

static uint32_t guest_bytes(const void* p, uint32_t n)
{
    uint32_t a = gheap_alloc(n, 1);
    memcpy(GUEST_PTR(a), p, n);
    return a;
}

static uint32_t com_call(uint32_t obj, unsigned slot, unsigned nargs, const uint32_t* args)
{
    uint32_t all[8] = { obj };
    for (unsigned i = 0; i < nargs && i < 7; ++i)
        all[i + 1] = args[i];
    return guest_call(rd32(rd32(obj) + 4u * slot), nargs + 1, all);
}

/* --- the frame rate ---------------------------------------------------------------------------------
 * FFXiMain paces its frames by a divisor of 60: 2 (30 fps) as shipped, 1 for 60. It is a field
 * of an object FFXiMain creates at startup; Ashita's fps addon finds it the same way: the code
 * `sub esp, 0x100; cmp eax, ecx; je +0x21; mov ecx, [global]` names the global that points at
 * the object, and the divisor is at +0x30. The game may reset it (zoning), so every frame puts
 * it back. */
static uint32_t g_fps_divisor = 1, g_fps_global;
static char g_live_file[1024]; /* --live: settings while the game runs (live_reload) */

static void find_fps_global(void)
{
    static const uint8_t PAT[] = { 0x81, 0xEC, 0x00, 0x01, 0x00, 0x00, 0x3B, 0xC1, 0x74, 0x21, 0x8B, 0x0D };
    uint32_t start = rt_image_base + rt_image_text_rva, end = start + rt_image_text_size;
    for (uint32_t a = start; a + sizeof PAT + 4 <= end; ++a)
        if (!memcmp(GUEST_PTR(a), PAT, sizeof PAT))
        {
            g_fps_global = rd32(a + sizeof PAT);
            rt_log("[recomp] frame-rate divisor: global %08x (code at %08x), set to %u (%u fps)\n", g_fps_global, a,
                g_fps_divisor, 60 / g_fps_divisor);
            return;
        }
    rt_log("[recomp] frame-rate divisor: not found; the game keeps its own frame rate\n");
    g_fps_global = 0xFFFFFFFFu;
}

/* --- the aspect ratio ---------------------------------------------------------------------------------
 * FFXiMain's projection takes its shape from a float at +0x2F0 of its camera object, which the game
 * sets from the configured resolution as h / (w * 0.25 * 3): 1 for 4:3, and a window of another
 * shape stretches the scene. Ashita's aspect addon finds the setter by its code,
 * `mov eax, [global]; test eax, eax; je; fld [esp+4]; fmul [0.25]; fmul [3.0]`, where the global
 * points at the object; every frame puts the value back from the shape the frame is shown at. */
static float g_aspect;          /* width / height to project for; 0 follows the window, < 0 off */
static uint32_t g_aspect_global; /* 0 not looked for yet, 0xFFFFFFFF not found */

static void find_aspect_global(void)
{
    static const uint8_t PAT[] = { 0xA1, 0, 0, 0, 0, 0x85, 0xC0, 0x74, 0, 0xD9, 0x44, 0x24, 0x04, 0xD8, 0x0D,
                                   0, 0, 0, 0, 0xD8, 0x0D };
    static const uint8_t ANY[] = { 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0 };
    uint32_t start = rt_image_base + rt_image_text_rva, end = start + rt_image_text_size;
    for (uint32_t a = start; a + sizeof PAT <= end; ++a)
    {
        const uint8_t* p = GUEST_PTR(a);
        size_t i = 0;
        while (i < sizeof PAT && (ANY[i] || p[i] == PAT[i]))
            ++i;
        if (i == sizeof PAT)
        {
            g_aspect_global = rd32(a + 1);
            rt_log("[recomp] aspect ratio: global %08x (code at %08x), %s\n", g_aspect_global, a,
                g_aspect > 0 ? "fixed" : "following the window");
            return;
        }
    }
    rt_log("[recomp] aspect ratio: not found; the scene keeps the game's shape\n");
    g_aspect_global = 0xFFFFFFFFu;
}

static void fix_aspect(void)
{
    if (g_aspect < 0)
        return;
    if (!g_aspect_global)
        find_aspect_global();
    if (g_aspect_global == 0xFFFFFFFFu)
        return;
    uint32_t object = rd32(g_aspect_global);
    if (!object)
        return;
    float ratio = g_aspect;
    if (!(ratio > 0))
    {
        uint32_t w = 0, h = 0;
        d3d8_screen_size(&w, &h);
        if (!w || !h)
            return;
        ratio = (float)w / (float)h;
    }
    float v = 4.0f / 3.0f / ratio; /* the game's h / (w * 0.75) */
    uint32_t bits;
    memcpy(&bits, &v, 4);
    if (rd32(object + 0x2F0) != bits)
        wr32(object + 0x2F0, bits);
}

/* w:h (16:9), wxh, w/h, or a ratio (1.778); 0 if it is none of those */
static double parse_shape(const char* s)
{
    char* end;
    double a = strtod(s, &end), b = 1.0;
    if (*end == ':' || *end == 'x' || *end == '/')
        b = strtod(end + 1, &end);
    if (*end || !(a > 0) || !(b > 0) || a / b < 0.5 || a / b > 8)
        return 0;
    return a / b;
}

/* --- nameplates ----------------------------------------------------------------------------------------
 * FFXiMain draws the names over characters' heads (2026-09-03: 0x10086210, given a point in the
 * world, the text and a size) as screen-space quads into the 3D scene's render target: every glyph
 * corner is its offset from the projected point times [esp+0x4c] across and [esp+0x50] down. It
 * sets those from the target's width and height, which the target's stretch to the window turns
 * into the window's, so a name keeps the shape it was drawn for only in a 4:3 window and widens
 * with the window (1.8 times at 3440x1440). The hook point (meta/builds.json "nameplate_scale")
 * is just after both are set: the across factor is brought back to a 4:3 window's, and both take
 * the size asked for. Text stays centred on the point, since offsets are from it. */
#if defined(FFXI_HOOK_NAMEPLATE_SCALE)
extern GuestFn rt_hook_nameplate_scale;
#define NAMEPLATE_HOOK (&rt_hook_nameplate_scale)
#elif defined(XI_SPLIT)
#define NAMEPLATE_HOOK (xi_game->hook_nameplate_scale) /* NULL when the module's build has none */
#else
#define NAMEPLATE_HOOK ((GuestFn*)NULL)
#endif
static int g_nameplate_fix = 1;                     /* --nameplates: 1 the 4:3 shape, 0 as the game draws */
static float g_nameplate_sx = 1.0f, g_nameplate_sy = 1.0f; /* --nameplate-scale */

static void nameplate_scale(Guest* g)
{
    /* The overlay may draw the names itself (its own font, size, outline): the name is handed to it
     * as the game placed it (the point in the 3D frame's pixels at +0x30, top-left origin; the
     * text, size and color are the routine's 2nd to 4th arguments), and the game's own glyphs are
     * drawn at no size at all. */
    if (overlay_nameplates_wanted())
    {
        uint32_t text = rd32(g->esp + 0x6dc), color = rd32(g->esp + 0x6e4), vx, vy, vw, vh;
        /* the point is in the viewport the game projected it into (its own units, which are not
         * the frame's): handed on as a fraction of it */
        d3d8_viewport(&vx, &vy, &vw, &vh);
        if (text && gwin_is_committed(text) && vw && vh)
            overlay_nameplate((rdf32(g->esp + 0x30) - (float)vx) / (float)vw, (rdf32(g->esp + 0x34) - (float)vy) / (float)vh,
                rdf32(g->esp + 0x38) /* its depth, 0-1 as the projection gives it */, (const char*)GUEST_PTR(text), color);
        wrf32(g->esp + 0x4c, 0.0f);
        wrf32(g->esp + 0x50, 0.0f);
        return;
    }
    float kx = g_nameplate_sx, ky = g_nameplate_sy;
    if (g_nameplate_fix)
    {
        uint32_t w = 0, h = 0;
        d3d8_screen_size(&w, &h);
        if (w && h)
            kx *= (4.0f / 3.0f) * (float)h / (float)w;
    }
    wrf32(g->esp + 0x4c, rdf32(g->esp + 0x4c) * kx);
    wrf32(g->esp + 0x50, rdf32(g->esp + 0x50) * ky);
}

static void setup_nameplates(void)
{
    /* with --live they can change while the game runs, so the hook is always in */
    int wanted = g_live_file[0] || g_nameplate_fix || g_nameplate_sx != 1.0f || g_nameplate_sy != 1.0f;
    GuestFn* hook = NAMEPLATE_HOOK;
    if (hook)
    {
        if (wanted)
            *hook = nameplate_scale;
        overlay_set_nameplates_available(wanted);
        rt_log("[recomp] nameplates: %s, scale %gx%g\n", g_nameplate_fix ? "4:3 shape" : "as the game draws them",
            g_nameplate_sx, g_nameplate_sy);
    }
    else if (wanted)
        rt_log("[recomp] nameplates: build %s has no nameplate hook; they stay as the game draws them\n", FFXI_BUILD);
}

/* The game's incoming packets, for the overlay (docs/OVERLAY.md): the hook sits on the success
 * return of the game's own decrypt-and-decompress (meta/builds.json "packet_in"), where eax is the
 * length it made and the first argument its buffer. Read only. */
#if defined(FFXI_HOOK_PACKET_IN)
extern GuestFn rt_hook_packet_in;
#define PACKET_HOOK (&rt_hook_packet_in)
#elif defined(XI_SPLIT)
/* a module from before the field was appended has none */
#define PACKET_HOOK (xi_game->size >= offsetof(XiGameModule, hook_packet_in) + sizeof(GuestFn*) ? xi_game->hook_packet_in : NULL)
#else
#define PACKET_HOOK ((GuestFn*)NULL)
#endif

static void packet_in(Guest* g)
{
    uint32_t len = g->eax, buf = rd32(g->esp + 4);
    static int told;
    if (told < 4)
    {
        /* the first few, to the log: what the hook sees */
        ++told;
        char hex[3 * 40 + 1] = "";
        for (int i = 0; buf && (int32_t)len > 0 && i < 40 && (uint32_t)i < len; ++i)
            snprintf(hex + 3 * i, 4, "%02x ", GUEST_PTR(buf)[i]);
        rt_log("[recomp] packets: the hook ran: length %d, buffer %08x: %s\n", (int)len, buf, hex);
    }
    if ((int32_t)len > 28 && len < 0x10000 && buf)
        gamestate_feed(GUEST_PTR(buf), len);
}

#if defined(FFXI_HOOK_PACKET_OUT)
extern GuestFn rt_hook_packet_out;
#define PACKET_OUT_HOOK (&rt_hook_packet_out)
#elif defined(XI_SPLIT)
#define PACKET_OUT_HOOK (xi_game->size >= offsetof(XiGameModule, hook_packet_out) + sizeof(GuestFn*) ? xi_game->hook_packet_out : NULL)
#else
#define PACKET_OUT_HOOK ((GuestFn*)NULL)
#endif

/* the entry of the game's encrypt of an outgoing packet: its 4th argument the packet in the clear
 * (the header, then the client's packets), its 5th the length. Read only. */
static void packet_out(Guest* g)
{
    uint32_t buf = rd32(g->esp + 0x10), len = rd32(g->esp + 0x14);
    if (len > 28 && len < 0x10000 && buf)
        gamestate_feed_out(GUEST_PTR(buf), len);
}

#if defined(FFXI_HOOK_CHAT_ADD)
extern GuestFn rt_hook_chat_add;
#define CHAT_ADD_HOOK (&rt_hook_chat_add)
#elif defined(XI_SPLIT)
#define CHAT_ADD_HOOK (xi_game->size >= offsetof(XiGameModule, hook_chat_add) + sizeof(GuestFn*) ? xi_game->hook_chat_add : NULL)
#else
#define CHAT_ADD_HOOK ((GuestFn*)NULL)
#endif

/* The entry of the game's add-a-line-to-its-chat-log (a method: ecx the chat manager, five
 * arguments): the first is the line as the log shows it (with the game's colour and control codes),
 * the second a header before it whose first word is taken as its mode. Every line: chat, NPCs,
 * the system, battle. Read only. */
static void chat_add(Guest* g)
{
    uint32_t text = rd32(g->esp + 4), head = rd32(g->esp + 8);
    if (!text || text >= 0xF0000000u)
        return;
    uint32_t mode = head && head < 0xF0000000u ? rd32(head) : 0xFFFFFFFFu;
    static int told;
    if (told < 0)
    {
        /* the header's bytes, to learn the modes (raise the limit to see them) */
        ++told;
        const uint8_t* h = head && head < 0xF0000000u ? GUEST_PTR(head) : NULL;
        rt_log("[recomp] chat: mode %08x head %02x %02x %02x %02x %02x %02x %02x %02x\n", mode, h ? h[0] : 0,
            h ? h[1] : 0, h ? h[2] : 0, h ? h[3] : 0, h ? h[4] : 0, h ? h[5] : 0, h ? h[6] : 0, h ? h[7] : 0);
    }
    gamestate_chat_line(mode, GUEST_PTR(text));
}

#if defined(FFXI_INPUT_LINE)
#define INPUT_LINE FFXI_INPUT_LINE
#elif defined(XI_SPLIT)
#define INPUT_LINE (xi_game->size >= offsetof(XiGameModule, input_line) + sizeof(uint32_t) ? xi_game->input_line : 0u)
#else
#define INPUT_LINE 0u
#endif

#if defined(FFXI_ENTITY_MAP)
#define ENTITY_MAP FFXI_ENTITY_MAP
#elif defined(XI_SPLIT)
#define ENTITY_MAP (xi_game->size >= offsetof(XiGameModule, entity_map) + sizeof(uint32_t) ? xi_game->entity_map : 0u)
#else
#define ENTITY_MAP 0u
#endif

#if defined(FFXI_MZB_KEYS)
#define MZB_KEYS FFXI_MZB_KEYS
#elif defined(XI_SPLIT)
#define MZB_KEYS (xi_game->size >= offsetof(XiGameModule, mzb_keys) + sizeof(uint32_t) ? xi_game->mzb_keys : 0u)
#else
#define MZB_KEYS 0u
#endif

#if defined(FFXI_MENU_MGR)
#define MENU_MGR FFXI_MENU_MGR
#define MENU_FIND FFXI_MENU_FIND
#elif defined(XI_SPLIT)
#define MENU_MGR (xi_game->size >= offsetof(XiGameModule, menu_find) + sizeof(uint32_t) ? xi_game->menu_mgr : 0u)
#define MENU_FIND (xi_game->size >= offsetof(XiGameModule, menu_find) + sizeof(uint32_t) ? xi_game->menu_find : 0u)
#else
#define MENU_MGR 0u
#define MENU_FIND 0u
#endif
#if defined(FFXI_TARGET_PTR)
#define TARGET_PTR FFXI_TARGET_PTR
#elif defined(XI_SPLIT)
#define TARGET_PTR (xi_game->size >= offsetof(XiGameModule, target_ptr) + sizeof(uint32_t) ? xi_game->target_ptr : 0u)
#else
#define TARGET_PTR 0u
#endif

/* --- the game's own windows, where the overlay's stand in for them --------------------------------
 * The game's window manager ("menu_mgr") keeps its windows in a list: nodes of (next, ..., the window
 * at +0x10, removed at +0x14), the first at the manager's +0. A window's rectangle is x, y, width,
 * height at +0x3A (shorts); its name ("menu    logwindo") is in the description it points to at +4,
 * at +0x46. Hidden, a window is only moved off the screen, so the game goes on running it exactly
 * as before (its log keeps every line), and put back where it was when the overlay's own is turned
 * off or hidden. */
typedef struct
{
    const char* name;
    int group; /* 0 the chat log, 1 the party list, 2 the target box, 3 the log's typing line */
    uint32_t win;
    int16_t x, y, x1, y1; /* where the game had it */
    int16_t sx, sy;       /* where it was last put (the log: under the overlay's chat) */
    int moved;
    int dropped; /* its draws dropped (a window the game lays out every frame cannot be moved) */
} GameWindow;

/* where the overlay's chat is (fractions of the screen; x0 < 0 none): the game's log, hidden, is kept
 * there and what it draws dropped, so the game's questions, which it puts above its log, come just
 * above the overlay's chat (as they do above the log). With no chat, it goes off the screen. */
static float g_log_at[4] = { -1, -1, -1, -1 };
static void place_game_log(float x0, float y0, float x1, float y1)
{
    g_log_at[0] = x0, g_log_at[1] = y0, g_log_at[2] = x1, g_log_at[3] = y1;
}
static GameWindow g_game_windows[] = {
    { "logwindo", 0 }, { "logwin2 ", 0 }, { "inline  ", 3 }, /* the log, and the game's own typing line */
    { "partywin", 1 }, { "ptw0    ", 1 }, { "ptw1    ", 1 }, { "ptw2    ", 1 },
    { "targetwi", 2 },
};

/* a window's name (the 8 characters after "menu    "), or NULL */
static const char* game_window_name(uint32_t win)
{
    if (!win || !gwin_is_committed(win) || !gwin_is_committed(win + 0x80))
        return NULL;
    uint32_t desc = rd32(win + 4);
    if (!desc || !gwin_is_committed(desc) || !gwin_is_committed(desc + 0x56))
        return NULL;
    const char* n = (const char*)GUEST_PTR(desc + 0x46);
    return !memcmp(n, "menu    ", 8) ? n + 8 : NULL;
}

/* the windows the game has now, by the manager's list; each tracked one found by its name */
static void find_game_windows(void)
{
    static int told;
    for (size_t i = 0; i < sizeof g_game_windows / sizeof *g_game_windows; ++i)
        g_game_windows[i].win = 0;
    if (!MENU_MGR || !gwin_is_committed(MENU_MGR))
        return;
    int n = 0;
    char all[1200] = "";
    for (uint32_t node = rd32(MENU_MGR); node && gwin_is_committed(node) && n < 400; node = rd32(node), ++n)
    {
        if (rd8(node + 0x14))
            continue; /* removed */
        uint32_t win = rd32(node + 0x10);
        const char* name = game_window_name(win);
        if (!name)
            continue;
        if (told < 30 && strlen(all) + 12 < sizeof all)
            snprintf(all + strlen(all), sizeof all - strlen(all), " %.8s", name);
        for (size_t i = 0; i < sizeof g_game_windows / sizeof *g_game_windows; ++i)
            if (!memcmp(name, g_game_windows[i].name, 8))
                g_game_windows[i].win = win;
    }
    static char last[1200];
    if (told < 30 && all[0] && strcmp(all, last))
    {
        ++told;
        snprintf(last, sizeof last, "%s", all);
        rt_log("[recomp] the game's windows:%s\n", all);
    }
}

#if defined(FFXI_MENU_CLOSE)
#define MENU_CLOSE FFXI_MENU_CLOSE
#elif defined(XI_SPLIT)
#define MENU_CLOSE (xi_game->size >= offsetof(XiGameModule, menu_close) + sizeof(uint32_t) ? xi_game->menu_close : 0u)
#else
#define MENU_CLOSE 0u
#endif

static void open_launcher_settings(void);

/* the game's own close of one of its windows, by name (its input line, when the overlay's chat box
 * takes the typing): on the game's thread */
static int close_game_window(const char* name8)
{
    static uint32_t buf;
    if (!MENU_MGR || !MENU_CLOSE || (!buf && !(buf = gheap_alloc(32, 1))))
        return 0;
    memcpy(GUEST_PTR(buf), "menu    ", 8);
    memcpy(GUEST_PTR(buf + 8), name8, 8);
    GUEST_PTR(buf)[16] = 0;
    uint32_t arg = buf;
    guest_thiscall(MENU_CLOSE, MENU_MGR, 1, &arg);
    return 1;
}

#if defined(FFXI_PARTY_DISPLAY)
#define PARTY_DISPLAY FFXI_PARTY_DISPLAY
#elif defined(XI_SPLIT)
#define PARTY_DISPLAY (xi_game->size >= offsetof(XiGameModule, party_display) + sizeof(uint32_t) ? xi_game->party_display : 0u)
#else
#define PARTY_DISPLAY 0u
#endif

/* The party display (the bars at the bottom right, the player's alone when not in a party) is not the
 * partywin window, which only holds a place, but an object of its own ("party_display", a pointer),
 * whose place (+0x3C, +0x3E) the window manager sets every frame in the same pass as it goes over
 * its windows: moved off the screen there (menu_draw, after), while the overlay's Party stands in. */
static int g_party_hidden;

static void party_display_off_screen(void)
{
    uint32_t at = PARTY_DISPLAY;
    if (!g_party_hidden || !at || !gwin_is_committed(at))
        return;
    uint32_t obj = rd32(at);
    if (!obj || !gwin_is_committed(obj + 0x40))
        return;
    int16_t* pos = (int16_t*)GUEST_PTR(obj + 0x3C);
    pos[0] = -8000, pos[1] = -8000;
}

#if defined(FFXI_HOOK_MENU_DRAW)
extern GuestFn rt_hook_menu_draw, rt_hook_menu_drawn;
#define MENU_DRAW_HOOK (&rt_hook_menu_draw)
#define MENU_DRAWN_HOOK (&rt_hook_menu_drawn)
#elif defined(XI_SPLIT)
#define MENU_DRAW_HOOK (xi_game->size >= offsetof(XiGameModule, hook_menu_drawn) + sizeof(GuestFn*) ? xi_game->hook_menu_draw : NULL)
#define MENU_DRAWN_HOOK (xi_game->size >= offsetof(XiGameModule, hook_menu_drawn) + sizeof(GuestFn*) ? xi_game->hook_menu_drawn : NULL)
#else
#define MENU_DRAW_HOOK ((GuestFn*)NULL)
#define MENU_DRAWN_HOOK ((GuestFn*)NULL)
#endif

/* the window manager's draw of one window (eax the window) and its end: a window whose draws are
 * dropped draws nothing */
static uint32_t g_dropping; /* the window between menu_draw and menu_drawn, when it is one hidden */

/* a hidden window's place, off the screen: set on both sides of the manager's call, since the game
 * lays some out again in it (the party list, every frame) and draws them after */
static void off_screen(uint32_t win)
{
    if (gwin_is_committed(win + 0x3A))
    {
        int16_t* pos = (int16_t*)GUEST_PTR(win + 0x3A);
        pos[0] = -8000, pos[1] = -8000;
    }
}

static void placed_again(uint32_t win);

/* The interface drawn in a window's rectangle (+0x3A), dropped: the party list's and target box's
 * bars and text are not all drawn by the window itself, so moving it off the screen is not enough.
 * Rectangle 0 the party list's, 1 the target box's. The four shorts read as corners (the party
 * list's: 1792,1030 to 1904,1064) when they can be, else as a place and a size. */
static int drop_in_window(int slot, uint32_t win, const char* name)
{
    if (!gwin_is_committed(win + 0x42))
        return 0;
    const int16_t* r = (const int16_t*)GUEST_PTR(win + 0x3A);
    if (r[0] <= -4000 || r[2] <= 0 || r[3] <= 0)
        return 0;
    float x1 = r[2] > r[0] && r[3] > r[1] ? r[2] : r[0] + r[2], y1 = r[2] > r[0] && r[3] > r[1] ? r[3] : r[1] + r[3];
    d3d8_drop_rect(slot, 1, (float)r[0] - 4, (float)r[1] - 4, x1 + 4, y1 + 4);
    static int told[4];
    if (!told[slot & 3]++)
        rt_log("[recomp] game window %.8s: its rectangle %d,%d %d,%d, the interface drawn in it dropped\n", name, r[0], r[1], r[2], r[3]);
    return 1;
}

static void menu_draw(Guest* g)
{
    g_dropping = 0;
    party_display_off_screen();
    placed_again(g->eax);
    {
        /* which windows the manager's pass reaches, once each (the first few dozen) */
        static uint32_t seen[48];
        static int nseen;
        int known = 0;
        for (int k = 0; k < nseen && !known; ++k)
            known = seen[k] == g->eax;
        if (!known && nseen < 48)
        {
            seen[nseen++] = g->eax;
            const char* n = game_window_name(g->eax);
            rt_log("[recomp] the manager's pass draws %.8s\n", n ? n : "(unnamed)");
        }
    }
    for (size_t i = 0; i < sizeof g_game_windows / sizeof *g_game_windows; ++i)
        if (g_game_windows[i].dropped && g_game_windows[i].win == g->eax)
        {
            g_dropping = g->eax;
            /* the party list or target box, as the game has just laid it out */
            if (g_game_windows[i].group == 1 || g_game_windows[i].group == 2)
                drop_in_window(g_game_windows[i].group - 1, g_dropping, g_game_windows[i].name);
            static int seen[16];
            if (!seen[i]++)
                rt_log("[recomp] game window %.8s: the manager's pass seen, moved off the screen there\n", g_game_windows[i].name);
            off_screen(g_dropping);
            d3d8_drop_draws(1);
            return;
        }
}

static void menu_drawn(Guest* g)
{
    (void)g;
    if (g_dropping)
        off_screen(g_dropping);
    g_dropping = 0;
    d3d8_drop_draws(0);
}

/* the game's window with the keyboard now (the manager's +0x54): its name, "" for none */
static const char* game_focus(void)
{
    if (!MENU_MGR || !gwin_is_committed(MENU_MGR + 0x54))
        return "";
    uint32_t win = rd32(MENU_MGR + 0x54);
    const char* name = win ? game_window_name(win) : NULL;
    return name ? name : "";
}

/* Learning a window that asks (an NPC's choices, "query"): while it has the keyboard, its object and
 * what three of its pointers lead to (+0x14, +0x18, +0x88) are compared four times a second, and what
 * changed is logged (the cursor moving shows where the choice is kept), with the text found in them
 * once (where its choices' words are). A few hundred lines at most, for working out an overlay
 * window in its place. */
static void study_asking(uint32_t win, const char* name)
{
    static uint32_t last_win;
    static uint8_t before[4][0x400];
    static int lines;
    static uint64_t next;
    if (lines > 400 || !win)
        return;
    uint64_t now = rt_monotonic_ns();
    if (win == last_win && now < next)
        return;
    next = now + 250000000ull;
    uint32_t at[4] = { win, rd32(win + 0x14), rd32(win + 0x18), rd32(win + 0x88) };
    for (int k = 0; k < 4; ++k)
    {
        uint8_t cur[0x400];
        if (!at[k] || !gwin_is_committed(at[k]) || !gwin_is_committed(at[k] + 0x3FF))
        {
            memset(before[k], 0, sizeof before[k]);
            continue;
        }
        memcpy(cur, GUEST_PTR(at[k]), sizeof cur);
        if (win != last_win)
        {
            /* the words in it: runs of 4 or more printable characters */
            char out[900];
            int o = 0;
            for (int i = 0; i < 0x400 && o < 800;)
            {
                int j = i;
                while (j < 0x400 && cur[j] >= 0x20 && cur[j] < 0x7F)
                    ++j;
                if (j - i >= 4)
                    o += snprintf(out + o, sizeof out - (size_t)o, " +%03x \"%.*s\"", i, j - i, (const char*)cur + i);
                i = j + 1;
            }
            rt_log("[recomp] study %.8s [%d] at %08x:%s\n", name, k, at[k], o ? out : " (no text)");
            ++lines;
        }
        else
        {
            char out[900];
            int o = 0, n = 0;
            for (int i = 0; i + 4 <= 0x400 && o < 800; i += 4)
                if (memcmp(cur + i, before[k] + i, 4))
                {
                    uint32_t a, b;
                    memcpy(&a, before[k] + i, 4), memcpy(&b, cur + i, 4);
                    o += snprintf(out + o, sizeof out - (size_t)o, " +%03x %x>%x", i, a, b), ++n;
                }
            if (n && n < 40)
                rt_log("[recomp] study %.8s [%d] changed:%s\n", name, k, out), ++lines;
        }
        memcpy(before[k], cur, sizeof cur);
    }
    last_win = win;
}

/* frames left of the kept rectangle (set while a window asks; hide_game_windows lets it go) */
static int g_keep_frames;

/* While a window asks, what the dropped rectangles take (a second at a time, for its first few
 * seconds): a question lying in one of them is not seen. Logged for each window once. */
static void asking_dropped(const char* name, const int16_t* r)
{
    static char last[9];
    static uint64_t next;
    static int left, lines;
    uint64_t now = rt_monotonic_ns();
    if (strncmp(last, name, 8))
    {
        static char seen[64][8];
        static int nseen;
        int known = 0;
        for (int k = 0; k < nseen && !known; ++k)
            known = !memcmp(seen[k], name, 8);
        if (!known && nseen < 64)
            memcpy(seen[nseen++], name, 8);
        snprintf(last, sizeof last, "%.8s", name);
        left = known ? 0 : 4, next = now + 1000000000ull;
        for (int i = 0; i < 4; ++i)
        {
            int on;
            uint32_t n;
            float q[4], b[4];
            d3d8_drop_rect_seen(i, &on, q, &n, b); /* counted from here */
        }
        d3d8_kept_draws();
        return;
    }
    if (!left || now < next || lines > 200)
        return;
    --left, next = now + 1000000000ull;
    char out[600];
    int o = snprintf(out, sizeof out, "[recomp] asking %.8s at %d,%d %d,%d: kept %u;", name, r[0], r[1], r[2], r[3], d3d8_kept_draws());
    for (int i = 0; i < 4; ++i)
    {
        int on;
        uint32_t n;
        float q[4], b[4];
        d3d8_drop_rect_seen(i, &on, q, &n, b);
        if (on || n)
            o += snprintf(out + o, sizeof out - (size_t)o, " rect %d %.0f,%.0f %.0f,%.0f dropped %u%s", i, q[0], q[1], q[2], q[3], n,
                n ? "" : ";");
        if (n && o < (int)sizeof out)
            o += snprintf(out + o, sizeof out - (size_t)o, " in %.0f,%.0f %.0f,%.0f;", b[0], b[1], b[2], b[3]);
    }
    rt_log("%s\n", out);
    ++lines;
}

/* where that window is: its rectangle (+0x3A: left, top, right, bottom, shorts) in the game's own
 * units (its back buffer's pixels), as fractions of the screen; 0 when none has the keyboard */
static int game_focus_rect(float* x, float* y, float* w, float* h)
{
    uint32_t bw = 0, bh = 0;
    d3d8_backbuffer_size(&bw, &bh);
    if (!MENU_MGR || !bw || !bh || !gwin_is_committed(MENU_MGR + 0x54))
        return 0;
    uint32_t win = rd32(MENU_MGR + 0x54);
    if (!game_window_name(win))
        return 0;
    const int16_t* r = (const int16_t*)GUEST_PTR(win + 0x3A);
    study_asking(win, game_window_name(win));
    if (r[2] <= r[0] || r[3] <= r[1])
        return 0;
    *x = (float)r[0] / bw, *y = (float)r[1] / bh, *w = (float)(r[2] - r[0]) / bw, *h = (float)(r[3] - r[1]) / bh;
    /* what it draws is never dropped, though it lie where the game's log is hidden (the bottom left:
     * the "Return to home point" question vanished with the log) */
    d3d8_keep_rect(1, (float)r[0] - 4, (float)r[1] - 4, (float)r[2] + 4, (float)r[3] + 4);
    g_keep_frames = 3;
    asking_dropped(game_window_name(win), r);
    {
        /* each window that takes the keyboard, once: its rectangle and the start of it (what its
         * list is made of, for the overlay's own in its place later) */
        static char seen[64][8];
        static int nseen;
        const char* name = game_window_name(win);
        int known = 0;
        for (int k = 0; k < nseen && !known; ++k)
            known = !memcmp(seen[k], name, 8);
        if (!known && nseen < 64 && gwin_is_committed(win + 0x200))
        {
            memcpy(seen[nseen++], name, 8);
            rt_log("[recomp] game window %.8s: at %d,%d to %d,%d; its object:\n", name, r[0], r[1], r[2], r[3]);
            for (int o = 0; o < 0x200; o += 32)
            {
                char line[200];
                int n = snprintf(line, sizeof line, "  +%03x", o);
                for (int k = 0; k < 32; k += 4)
                    n += snprintf(line + n, sizeof line - (size_t)n, " %08x", rd32(win + (uint32_t)(o + k)));
                rt_log("%s\n", line);
            }
        }
    }
    return 1;
}

/* The game's question, placed where the overlay wants it (on top of its chat): the window with the
 * keyboard moved there, and kept there in the window manager's pass too (menu_draw), since some
 * are laid out again every frame. x, y: its top left, as fractions of the screen; < 0 lets go. */
static uint32_t g_place_win, g_place_last;
static int16_t g_place_x, g_place_y, g_place_w, g_place_h;

static void place_game_focus(float x, float y)
{
    uint32_t bw = 0, bh = 0;
    d3d8_backbuffer_size(&bw, &bh);
    g_place_win = 0;
    if (x < 0 || !MENU_MGR || !bw || !gwin_is_committed(MENU_MGR + 0x54))
        return;
    uint32_t win = rd32(MENU_MGR + 0x54);
    if (!game_window_name(win))
        return;
    int16_t* pos = (int16_t*)GUEST_PTR(win + 0x3A);
    if (win != g_place_last || pos[2] - pos[0] > 0)
        g_place_w = (int16_t)(pos[2] - pos[0]), g_place_h = (int16_t)(pos[3] - pos[1]);
    g_place_last = win;
    g_place_win = win, g_place_x = (int16_t)(x * bw), g_place_y = (int16_t)(y * bh);
    /* moved whole: both corners, so it keeps its size */
    pos[0] = g_place_x, pos[1] = g_place_y, pos[2] = (int16_t)(g_place_x + g_place_w), pos[3] = (int16_t)(g_place_y + g_place_h);
}

static void placed_again(uint32_t win)
{
    if (win && win == g_place_win && game_window_name(win))
    {
        int16_t* pos = (int16_t*)GUEST_PTR(win + 0x3A);
        pos[0] = g_place_x, pos[1] = g_place_y, pos[2] = (int16_t)(g_place_x + g_place_w), pos[3] = (int16_t)(g_place_y + g_place_h);
    }
}

/* each frame (the overlay's): which of the game's windows the overlay stands in for now */
static void hide_game_windows(int log, int party, int target)
{
    static unsigned frame;
    static int any_moved;
    g_party_hidden = party;
    if (g_keep_frames && !--g_keep_frames)
        d3d8_keep_rect(0, 0, 0, 0, 0);
    if (!party)
        d3d8_drop_rect(0, 0, 0, 0, 0, 0);
    {
        /* the target box: its rectangle wherever the game has it now (not every build draws it in
         * the manager's pass), none when there is no target box */
        GameWindow* t = NULL;
        for (size_t i = 0; i < sizeof g_game_windows / sizeof *g_game_windows; ++i)
            if (g_game_windows[i].group == 2)
                t = &g_game_windows[i];
        if (!(target && t && game_window_name(t->win) && drop_in_window(1, t->win, t->name)) && !(target && t && t->win))
            d3d8_drop_rect(1, 0, 0, 0, 0, 0);
    }
    {
        static unsigned frames;
        if (party && ++frames == 600)
            rt_log("[recomp] the party list: %u interface draws dropped in its rectangle in 600 frames\n", d3d8_dropped_rect_draws());
    }
    if (!log && !party && !target && !any_moved)
        return;
    if (frame++ % 10 == 0) /* the game makes and remakes its windows (the target box on each target) */
    {
        int16_t keep_x[16], keep_y[16];
        int keep_moved[16];
        uint32_t keep_win[16];
        size_t n = sizeof g_game_windows / sizeof *g_game_windows;
        for (size_t i = 0; i < n; ++i)
            keep_win[i] = g_game_windows[i].win, keep_x[i] = g_game_windows[i].x, keep_y[i] = g_game_windows[i].y,
            keep_moved[i] = g_game_windows[i].moved;
        find_game_windows();
        for (size_t i = 0; i < n; ++i)
            if (g_game_windows[i].win != keep_win[i])
                g_game_windows[i].moved = 0; /* a new window, where the game put it */
            else
                g_game_windows[i].x = keep_x[i], g_game_windows[i].y = keep_y[i], g_game_windows[i].moved = keep_moved[i];
    }
    any_moved = 0;
    for (size_t i = 0; i < sizeof g_game_windows / sizeof *g_game_windows; ++i)
    {
        GameWindow* w = &g_game_windows[i];
        if (!game_window_name(w->win))
            continue;
        int hide = w->group == 0 || w->group == 3 ? log : w->group == 1 ? party : target;
        int16_t* pos = (int16_t*)GUEST_PTR(w->win + 0x3A);
        if (w->group >= 1 && MENU_DRAW_HOOK && MENU_DRAWN_HOOK)
        {
            /* the party list and target box: laid out again every frame, so they are moved off
             * the screen in the manager's own pass (menu_draw), and their draws dropped there */
            if (w->dropped != hide)
                rt_log("[recomp] game window %.8s: %s\n", w->name, hide ? "not drawn" : "drawn again");
            w->dropped = hide;
            any_moved |= w->dropped;
            continue;
        }
        if (hide)
        {
            static int held[16], undone[16], told[16];
            if (!w->moved || pos[0] != w->sx || pos[1] != w->sy) /* where the game has it now (it may have laid it out again) */
            {
                if (w->moved)
                    ++undone[i]; /* the game put it back since the last frame */
                w->x = pos[0], w->y = pos[1], w->x1 = pos[2], w->y1 = pos[3];
            }
            if (++held[i] == 600 && !told[i])
            {
                /* after ten seconds, whether it stays moved: if the game lays it out every frame,
                 * moving it cannot hide it */
                told[i] = 1;
                rt_log("[recomp] game window %.8s: hidden from %d,%d; the game put it back %d times in 600 frames\n", w->name, w->x,
                    w->y, undone[i]);
            }
            w->moved = 1;
            uint32_t bw = 0, bh = 0;
            d3d8_backbuffer_size(&bw, &bh);
            if (w->group == 0 && g_log_at[0] >= 0 && bw && bh)
            {
                /* under the overlay's chat, its size, and nothing it draws there shown */
                float r[4] = { g_log_at[0] * bw, g_log_at[1] * bh, g_log_at[2] * bw, g_log_at[3] * bh };
                /* and wherever the game laid it out this frame: some states of the log (typing,
                 * its wide form) are laid out again every frame, so the move above does not stay,
                 * and the log drew at its own place, wider than the chat. Its draws are dropped in
                 * both (one rectangle around the two). */
                float d[4] = { r[0], r[1], r[2], r[3] };
                if (w->x1 > w->x && w->y1 > w->y && w->x > -4000)
                {
                    d[0] = fminf(d[0], w->x), d[1] = fminf(d[1], w->y);
                    d[2] = fmaxf(d[2], w->x1), d[3] = fmaxf(d[3], w->y1);
                }
                pos[0] = (int16_t)r[0], pos[1] = (int16_t)r[1], pos[2] = (int16_t)r[2], pos[3] = (int16_t)r[3];
                d3d8_drop_rect(2, 1, d[0] - 2, d[1] - 2, d[2] + 2, d[3] + 2);
                static int said;
                if (!said++)
                    rt_log("[recomp] game window %.8s: kept under the overlay's chat, at %d,%d to %d,%d\n", w->name, pos[0], pos[1], pos[2], pos[3]);
            }
            else
            {
                pos[0] = -8000, pos[1] = -8000;
                if (w->group == 0)
                    d3d8_drop_rect(2, 0, 0, 0, 0, 0);
            }
            w->sx = pos[0], w->sy = pos[1];
        }
        else if (w->moved)
        {
            pos[0] = w->x, pos[1] = w->y;
            if (w->x1 > w->x && w->y1 > w->y)
                pos[2] = w->x1, pos[3] = w->y1;
            if (w->group == 0)
                d3d8_drop_rect(2, 0, 0, 0, 0, 0);
            w->moved = 0;
            rt_log("[recomp] game window %.8s: back at %d,%d\n", w->name, w->x, w->y);
        }
        any_moved |= w->moved;
    }
}

/* A line from the overlay (its chat box, or a window's button, on the player's click): through the
 * game's own parser of a typed line, as if typed in its input line: its /commands, or chat. The
 * game's own menus run their commands the same way. On the game's thread (the overlay's frame). */
static int run_line(const char* line)
{
    static uint32_t buf;
    uint32_t fn = INPUT_LINE;
    if (!fn || !line || !line[0])
        return 0;
    if (!buf && !(buf = gheap_alloc(512, 1)))
        return 0;
    size_t n = strlen(line);
    if (n > 255)
        n = 255;
    memcpy(GUEST_PTR(buf), line, n);
    GUEST_PTR(buf)[n] = 0;
    uint32_t args[2] = { buf, 1 /* typed */ };
    guest_call(fn, 2, args);
    return 1;
}

static void setup_packets(void)
{
    if (INPUT_LINE)
        overlay_set_line_runner(run_line);
    gamestate_set_entity_map(ENTITY_MAP);
    gamestate_set_target_ptr(TARGET_PTR);
    if (MENU_MGR)
        overlay_set_game_windows(hide_game_windows, game_focus);
    if (MENU_MGR && MENU_CLOSE)
        overlay_set_game_window_closer(close_game_window);
    if (MENU_MGR)
        overlay_set_focus_rect(game_focus_rect, place_game_focus);
    overlay_set_settings_opener(open_launcher_settings);
    overlay_set_log_placer(place_game_log);
    if (MENU_DRAW_HOOK && MENU_DRAWN_HOOK)
        *MENU_DRAW_HOOK = menu_draw, *MENU_DRAWN_HOOK = menu_drawn;
    GuestFn* ca = CHAT_ADD_HOOK;
    if (ca)
        *ca = chat_add;
    GuestFn* out = PACKET_OUT_HOOK;
    if (out)
        *out = packet_out;
    GuestFn* hook = PACKET_HOOK;
    if (hook)
    {
        *hook = packet_in;
        rt_log("[recomp] packets: hooked (build %s)\n", FFXI_BUILD);
    }
    else
        rt_log("[recomp] packets: build %s has no packet hook; the overlay shows no game state\n", FFXI_BUILD);
}

/* s, or sx x sy (1.25, 1x1.2); 0 if it is neither */
static int parse_scale(const char* s, float* sx, float* sy)
{
    char* end;
    double a = strtod(s, &end), b = a;
    if (*end == 'x' || *end == ':' || *end == ',')
        b = strtod(end + 1, &end);
    if (*end || !(a >= 0.25 && a <= 4) || !(b >= 0.25 && b <= 4))
        return 0;
    *sx = (float)a, *sy = (float)b;
    return 1;
}

/* w:h or a ratio (16:9, 1.778) as width / height, or off (0); 0 if it is none of these */
static int parse_aspect(const char* s, float* aspect)
{
    if (!strcmp(s, "off"))
        return *aspect = 0.0f, 1;
    char* end;
    double a = strtod(s, &end), b = 1.0;
    if (*end == ':' || *end == 'x' || *end == '/')
        b = strtod(end + 1, &end);
    if (*end || !(a > 0) || !(b > 0) || a / b < 0.5 || a / b > 8)
        return 0;
    *aspect = (float)(a / b);
    return 1;
}

/* --- settings while the game runs ------------------------------------------------------------------
 * --live <file>: the launcher rewrites it as its settings change; Present looks at it twice a second
 * and what it says holds from the next frame. The scene effects (gfx_metal.m) read the same file
 * through FFXI_FX_FILE; each side skips the other's keys.
 *   fps_divisor = 1..60      aspect = auto | off | w:h       ui_aspect = off | w:h
 *   nameplates = fix | off   nameplate_scale = s | sx x sy   fps_overlay = 0 | 1
 *   hotkey = f9..f12 | pause | scrolllock | none: the key that brings up the launcher's settings
 *   window_mode = 0..3 (the game's own: 0 and 3 full screen, 1 and 2 a window), applied when it
 *   changes, so the player's own switches (Alt+Enter) hold until the launcher's setting moves */
static uint64_t g_live_mtime, g_live_looked;
static int g_live_window_mode = -1; /* the file's window_mode as last read */
static int g_settings_key = SDL_SCANCODE_F12; /* hotkey: the launcher's settings, 0 none */

static int key_by_name(const char* v)
{
    static const struct { const char* name; int sc; } KEYS[] = {
        { "f9", SDL_SCANCODE_F9 }, { "f10", SDL_SCANCODE_F10 }, { "f11", SDL_SCANCODE_F11 }, { "f12", SDL_SCANCODE_F12 },
        { "pause", SDL_SCANCODE_PAUSE }, { "scrolllock", SDL_SCANCODE_SCROLLLOCK }, { "none", 0 },
    };
    for (size_t i = 0; i < sizeof KEYS / sizeof KEYS[0]; ++i)
        if (!strcmp(v, KEYS[i].name))
            return KEYS[i].sc;
    return -1;
}

static int live_set(const char* key, const char* v)
{
    float a;
    if (!strcmp(key, "fps_divisor"))
    {
        long d = strtol(v, NULL, 10);
        if (d < 1 || d > 60)
            return 0;
        g_fps_divisor = (uint32_t)d;
    }
    else if (!strcmp(key, "aspect"))
    {
        double r = !strcmp(v, "auto") ? 0.0 : !strcmp(v, "off") ? -1.0 : parse_shape(v);
        if (r == 0.0 && strcmp(v, "auto"))
            return 0;
        g_aspect = (float)r;
    }
    else if (!strcmp(key, "volume") || !strcmp(key, "title_volume"))
    {
        /* percent: in the world, and on the title and login screens before it */
        static float world = 100.0f, title = 35.0f;
        char* end;
        float pct = strtof(v, &end);
        if (end == v || pct < 0.0f || pct > 100.0f)
            return 0;
        *(!strcmp(key, "volume") ? &world : &title) = pct;
        dsound_set_volume(world / 100.0f, title / 100.0f);
    }
    else if (!strcmp(key, "ui_aspect"))
    {
        if (!parse_aspect(v, &a))
            return 0;
        user32_set_ui_aspect(a);
    }
    else if (!strcmp(key, "nameplates"))
        g_nameplate_fix = strcmp(v, "off") != 0;
    else if (!strcmp(key, "nameplate_scale"))
        return parse_scale(v, &g_nameplate_sx, &g_nameplate_sy);
    else if (!strcmp(key, "fps_overlay"))
        gfx_show_overlay(atoi(v) != 0);
    else if (!strcmp(key, "hotkey"))
    {
        int sc = key_by_name(v);
        if (sc < 0)
            return 0;
        g_settings_key = sc;
    }
    else if (!strcmp(key, "window_mode"))
    {
        int m = atoi(v);
        if (m < 0 || m > 3 || m == g_live_window_mode)
            return 0;
        if (g_live_window_mode >= 0) /* the first read is what the game started with */
            user32_request_display(m == 0 || m == 3);
        g_live_window_mode = m;
    }
    else
        return 0;
    return 1;
}

static void live_reload(void)
{
    uint64_t now = rt_monotonic_ns();
    if (!g_live_file[0] || now - g_live_looked < 500000000ull)
        return;
    g_live_looked = now;
    PlatStat st;
    if (!plat_stat(g_live_file, &st) || st.mtime_ms == g_live_mtime)
        return;
    g_live_mtime = st.mtime_ms;
    FILE* f = fopen(g_live_file, "r");
    if (!f)
        return;
    char line[256], key[64], v[128];
    while (fgets(line, sizeof line, f))
        if (sscanf(line, " %63[a-z_0-9] = %127s", key, v) == 2 && live_set(key, v))
            rt_log("[recomp] live: %s = %s\n", key, v);
    fclose(f);
}

/* The host's own keys, before the game sees them: Alt+Enter (and Ctrl+Cmd+F on macOS) switch between
 * a window and full screen; with the launcher (--live), its settings key (hotkey, F12 unless it says
 * otherwise; pressed alone, so Shift+F12 and the like still reach the game) and on macOS Cmd+, bring
 * up its settings over the game. */
static int host_key(int scancode, int mods, int down)
{
    const int any_mod = SDL_KMOD_SHIFT | SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI;
    int display = scancode == SDL_SCANCODE_RETURN && (mods & SDL_KMOD_ALT) && !(mods & (SDL_KMOD_CTRL | SDL_KMOD_GUI));
    int settings = g_settings_key && scancode == g_settings_key && !(mods & any_mod);
#ifdef __APPLE__
    display |= scancode == SDL_SCANCODE_F && (mods & SDL_KMOD_GUI) && (mods & SDL_KMOD_CTRL);
    settings |= scancode == SDL_SCANCODE_COMMA && (mods & SDL_KMOD_GUI);
#endif
    settings &= g_live_file[0] != 0;
    if (display && down)
        user32_request_display(2);
    if (settings && down)
    {
        printf("@launcher settings\n");
        fflush(stdout);
    }
    return display || settings;
}

/* the launcher's settings, from the overlay's bar */
static void open_launcher_settings(void)
{
    if (!g_live_file[0])
        return;
    printf("@launcher settings\n");
    fflush(stdout);
}

/* the lobby turned the game away: the launcher says why (331: the server wants another version) */
static void launcher_lobby_error(unsigned code)
{
    printf("@launcher lobby-error %u\n", code);
    fflush(stdout);
}

static int g_profile_shims;

/* Closing the window in the world: the game's own /shutdown first, as if typed (the player's one
 * close, one command), so the server hears the player leave. Quitting at once leaves the character
 * in the world until the server gives up on it (LandSandBoat: a minute), and signing in again before
 * then is refused ("same character already logged in"). The game ends the run itself once logged
 * out; a close while it counts down ends it at once. The close button and Cmd+Q can arrive as two
 * events for one close: those within a second are one. */
static volatile int g_logout; /* 1 asked, 2 sent */
static uint64_t g_logout_at;

static int close_asked(void)
{
    uint64_t now = rt_monotonic_ns();
    if (g_logout)
        return now - g_logout_at < 1000000000ull;
    if (!INPUT_LINE || !dsound_in_world())
        return 0;
    g_logout_at = now;
    g_logout = 1;
    rt_log("[recomp] quit: logging out first (/shutdown); close again to quit now\n");
    return 1;
}

static void present_hook(void)
{
    if (g_logout == 1)
    {
        g_logout = 2;
        run_line("/shutdown");
    }
    live_reload();
    if (g_profile_shims)
    {
        static uint64_t last;
        uint64_t now = rt_monotonic_ns();
        thunk_prof_thread = plat_thread_id();
        if (!last)
            last = now;
        if (now - last >= 2000000000ull)
            thunk_prof_report(), last = now;
    }
    fix_aspect();
    if (!g_fps_global)
        find_fps_global();
    if (g_fps_global == 0xFFFFFFFFu)
        return;
    uint32_t object = rd32(g_fps_global);
    if (object && rd32(object + 0x30) != g_fps_divisor)
        wr32(object + 0x30, g_fps_divisor);
}

static int parse_session(const char* s, uint8_t v[16])
{
    size_t n = strlen(s);
    if (n == 16)
    {
        memcpy(v, s, 16);
        return 1;
    }
    if (n != 32)
        return 0;
    for (int i = 0; i < 16; ++i)
    {
        unsigned b;
        if (sscanf(s + 2 * i, "%2x", &b) != 1)
            return 0;
        v[i] = (uint8_t)b;
    }
    return 1;
}

/* playonline.reg, the base registry the launcher passes (--reg): beside host64, in an app bundle's
 * Resources, or at the top of the source tree host64 was built in (build/host64). */
static int bundled_registry(const char* argv0, char* out, size_t n)
{
    const char *end = argv0, *p;
    for (p = argv0; *p; ++p)
        if (*p == '/' || *p == '\\')
            end = p + 1;
    static const char* const WHERE[] = { "playonline.reg", "../Resources/playonline.reg", "../playonline.reg" };
    for (size_t i = 0; i < sizeof WHERE / sizeof *WHERE; ++i)
    {
        PlatStat st;
        snprintf(out, n, "%.*s%s", (int)(end - argv0), argv0, WHERE[i]);
        if (plat_stat(out, &st))
            return 1;
    }
    return 0;
}

static void report_overlay(const char* name, unsigned files)
{
    rt_log("[recomp] dats: %s, %u files\n", name, files);
}

#ifdef __APPLE__
#include <SDL3/SDL.h>
#include <unistd.h>
#endif
#ifndef _WIN32
#include <signal.h>
#endif

/* A file of the game (rel: Windows-style, under the install) as the game sees it: from a DAT or
 * version overlay if one has it, else the install's. */
static void game_file(const char* game, const char* host_game, const char* rel, char* out, size_t n)
{
    char g[1024];
    snprintf(g, sizeof g, "%s\\%s", game, rel);
    if (vfs_host_path(g, out, n))
        return;
    snprintf(out, n, "%s%c%s", host_game, plat_path_sep, rel);
    for (char* p = out + strlen(host_game); *p; ++p)
        if (*p == '\\' || *p == '/')
            *p = plat_path_sep;
}

int main(int argc, char** argv)
{
#ifndef _WIN32
    /* The launcher reads our output through a pipe; if it quits while the game runs, a write to
     * that pipe is an error to ignore, not a signal that ends the game. */
    signal(SIGPIPE, SIG_IGN);
#endif
#ifdef __APPLE__
    /* started from Finder or the Dock (an app bundle, no terminal): the log goes to host64.log
     * beside the sign-in screen's files */
    if (app_bundled() && !isatty(2))
    {
        char* pref = SDL_GetPrefPath("FFXIRecompile", "FFXI");
        if (pref)
        {
            char log[1100];
            snprintf(log, sizeof log, "%shost64.log", pref);
            if (freopen(log, "w", stderr))
                setvbuf(stderr, NULL, _IOLBF, 0);
            freopen(log, "a", stdout);
            setvbuf(stdout, NULL, _IOLBF, 0);
            SDL_free(pref);
        }
    }
#endif
    static char app_game[1024], app_server[256], app_bg[1100], app_val[64];
    const char* game = NULL;
    const char* regs[8];
    unsigned nregs = 0;
    const char* overlay = NULL;
    const char* data_dir = NULL;
    const char* finals[8];
    unsigned nfinals = 0;
    const char* dats[8];
    unsigned ndats = 0;
    const char* packs[8];
    unsigned npacks = 0;
    const char* user_dir = NULL;
    uint32_t pol_server = DEFAULT_POL_SERVER;
    LsbLogin lsb = { 0, 54231, 54230, 54001, NULL, NULL, "", NULL };
    int have_session = 0;
    static char base_reg[1100];
    const char* server_name = NULL; /* --server as given, for the sign-in screen */
    const char* viewer_dir = NULL;  /* --viewer: PlayOnlineViewer, when not beside the game */
    const char* version_dir = NULL; /* --version-dir: another version's files over the install */
#ifdef XI_SPLIT
    const char* module_path = NULL; /* --module: the game itself */
#endif
    int nameplates_given = 0, nameplate_scale_given = 0, ui_aspect_given = 0;
    float ui_aspect = 0.0f;
    for (int i = 1; i + 1 < argc; i += 2)
    {
#ifdef XI_SPLIT
        if (!strcmp(argv[i], "--module"))
            module_path = argv[i + 1];
        else
#endif
        if (!strcmp(argv[i], "--live"))
            snprintf(g_live_file, sizeof g_live_file, "%s", argv[i + 1]);
        else if (!strcmp(argv[i], "--viewer"))
            viewer_dir = argv[i + 1];
        else if (!strcmp(argv[i], "--version-dir"))
            version_dir = argv[i + 1];
        else if (!strcmp(argv[i], "--game"))
            game = argv[i + 1];
        else if (!strcmp(argv[i], "--reg") && nregs < 8)
            regs[nregs++] = argv[i + 1];
        else if (!strcmp(argv[i], "--reg-overlay"))
            overlay = argv[i + 1];
        else if (!strcmp(argv[i], "--reg-final") && nfinals < 8)
            finals[nfinals++] = argv[i + 1];
        else if (!strcmp(argv[i], "--data-dir"))
        {
            data_dir = argv[i + 1];
            /* where the overlay's windows are kept */
            static char ini[1024];
            snprintf(ini, sizeof ini, "%s/overlay.ini", data_dir);
            overlay_set_ini(ini);
        }
        else if (!strcmp(argv[i], "--dats") && ndats < 8)
            dats[ndats++] = argv[i + 1];
        else if (!strcmp(argv[i], "--textures") && npacks < 8)
            packs[npacks++] = argv[i + 1];
        else if (!strcmp(argv[i], "--user-dir"))
            user_dir = argv[i + 1];
        else if (!strcmp(argv[i], "--session"))
        {
            uint8_t v[16];
            if (!parse_session(argv[i + 1], v))
            {
                fprintf(stderr, "--session: 16 characters or 32 hex digits\n");
                return 2;
            }
            polcore_set_session(v);
            have_session = 1;
        }
        else if (!strcmp(argv[i], "--server") || !strcmp(argv[i], "--pol-server") || !strcmp(argv[i], "--lobby"))
        {
            server_name = argv[i + 1];
            if (!net_resolve_ipv4(argv[i + 1], &pol_server))
            {
                fprintf(stderr, "%s: cannot resolve %s\n", argv[i], argv[i + 1]);
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--user"))
            lsb.user = argv[i + 1];
        else if (!strcmp(argv[i], "--pass"))
            lsb.password = argv[i + 1];
        else if (!strcmp(argv[i], "--otp"))
            lsb.otp = argv[i + 1];
        else if (!strcmp(argv[i], "--login-token"))
            lsb.login_token = argv[i + 1];
        else if (!strcmp(argv[i], "--fps-divisor"))
        {
            long d = strtol(argv[i + 1], NULL, 10);
            if (d < 1 || d > 60)
            {
                fprintf(stderr, "--fps-divisor: 1 (60 fps), 2 (30 fps, as shipped), ...\n");
                return 2;
            }
            g_fps_divisor = (uint32_t)d;
        }
        else if (!strcmp(argv[i], "--ui-aspect"))
        {
            if (!parse_aspect(argv[i + 1], &ui_aspect))
            {
                fprintf(stderr, "--ui-aspect: a shape as w:h (16:9), a ratio (1.778), or off\n");
                return 2;
            }
            ui_aspect_given = 1;
        }
        else if (!strcmp(argv[i], "--nameplates"))
        {
            if (strcmp(argv[i + 1], "fix") && strcmp(argv[i + 1], "off"))
            {
                fprintf(stderr, "--nameplates: fix (their 4:3 shape in any window) or off (as the game draws them)\n");
                return 2;
            }
            g_nameplate_fix = !strcmp(argv[i + 1], "fix");
            nameplates_given = 1;
        }
        else if (!strcmp(argv[i], "--nameplate-scale"))
        {
            if (!parse_scale(argv[i + 1], &g_nameplate_sx, &g_nameplate_sy))
            {
                fprintf(stderr, "--nameplate-scale: a size from 0.25 to 4 (1.25), or across x down (1x1.2)\n");
                return 2;
            }
            nameplate_scale_given = 1;
        }
        else if (!strcmp(argv[i], "--authport") || !strcmp(argv[i], "--dataport") || !strcmp(argv[i], "--viewport"))
        {
            long port = strtol(argv[i + 1], NULL, 10);
            if (port <= 0 || port > 65535)
            {
                fprintf(stderr, "%s: a port number\n", argv[i]);
                return 2;
            }
            *(argv[i][2] == 'a' ? &lsb.auth_port : argv[i][2] == 'd' ? &lsb.data_port : &lsb.view_port) = (uint16_t)port;
        }
        else if (!strcmp(argv[i], "--window-size"))
        {
            /* the size the player last gave the window (the launcher remembers it) */
            int ww = 0, wh = 0;
            if (sscanf(argv[i + 1], "%dx%d", &ww, &wh) == 2)
                user32_set_window_size(ww, wh);
        }
        else if (!strcmp(argv[i], "--loader"))
        {
            /* the xiloader protocol the server named (2.2.0); else negotiated */
            if (sscanf(argv[i + 1], "%d.%d.%d", &lsb.loader[0], &lsb.loader[1], &lsb.loader[2]) < 2)
            {
                fprintf(stderr, "--loader: a version like 2.2.0\n");
                return 2;
            }
        }
    }
    if (!game && app_default("FFXIGameFolder", app_game, sizeof app_game))
        game = app_game;
    if (!ui_aspect_given && app_default("FFXIUIAspect", app_val, sizeof app_val) && !parse_aspect(app_val, &ui_aspect))
        ui_aspect = 0.0f;
    user32_set_ui_aspect(ui_aspect);
    if (!nameplates_given && app_default("FFXINameplates", app_val, sizeof app_val))
        g_nameplate_fix = strcmp(app_val, "off") != 0;
    if (!nameplate_scale_given && app_default("FFXINameplateScale", app_val, sizeof app_val)
        && !parse_scale(app_val, &g_nameplate_sx, &g_nameplate_sy))
        g_nameplate_sx = g_nameplate_sy = 1.0f;
    if (!game)
    {
        fprintf(stderr, "usage: host64 --game <FINAL FANTASY XI folder> [--reg f.reg]... [--reg-overlay f.reg] [--reg-final f.reg]... [--data-dir folder] "
                        "[--server name] [--session V | --user name [--pass p] [--otp code] [--authport n] "
                        "[--dataport n] [--viewport n]] [--dats folder]... [--nameplates fix|off] [--nameplate-scale s]\n");
        return 2;
    }
    if (!lsb.password)
        lsb.password = getenv("FFXI_PASSWORD");
    if (!have_session && !(lsb.user && (lsb.password || lsb.login_token)))
    {
        /* Nothing on the command line signs in: the sign-in screen, in the game's own art. Its
         * window becomes the game's; a PlayOnline sign-in sets the session value itself. */
        SigninSetup su = { game, data_dir, lsb.user ? SIGNIN_LSB : 0, server_name, lsb.user,
                           lsb.password, lsb.otp, lsb.auth_port != 54231 ? lsb.auth_port : 0,
                           lsb.data_port != 54230 ? lsb.data_port : 0, lsb.view_port != 54001 ? lsb.view_port : 0 };
        /* an app bundle's first-run defaults (appdefaults.h) */
        su.default_mode = -1, su.default_space = -1;
        if (app_default("FFXIFullscreenSpace", app_val, sizeof app_val))
            su.default_space = atoi(app_val) != 0;
        if (app_default("FFXISignInMethod", app_val, sizeof app_val))
            su.default_method = !strcmp(app_val, "pol") ? SIGNIN_POL : SIGNIN_LSB;
        if (app_default("FFXIServer", app_server, sizeof app_server))
            su.default_server = app_server;
        if (app_default("FFXIWindowMode", app_val, sizeof app_val))
            su.default_mode = atoi(app_val);
        if (app_default("FFXIResolution", app_val, sizeof app_val))
            sscanf(app_val, "%dx%d", &su.default_w, &su.default_h);
        if (app_default("FFXIMenuResolution", app_val, sizeof app_val))
            sscanf(app_val, "%dx%d", &su.default_menu_w, &su.default_menu_h);
        if (app_default("FFXIBackground", app_val, sizeof app_val) && app_resource(app_val, app_bg, sizeof app_bg))
            su.default_background = app_bg;
        SigninResult sr;
        int r = signin_run(&su, &sr);
        if (r == 0)
            return 0;
        if (r > 0)
        {
            pol_server = sr.server;
            user32_adopt_window(sr.window);
            if (!nfinals)
                finals[nfinals++] = strdup(sr.settings_reg);
            /* run as the launcher does: its folder for host64's files and the game's saved
             * settings, the bundled PlayOnline registry under them */
            if (!data_dir)
                data_dir = strdup(sr.data_dir);
            if (!overlay)
            {
                char o[1100];
                snprintf(o, sizeof o, "%s%ssaved.reg", data_dir,
                    data_dir[0] && data_dir[strlen(data_dir) - 1] == '/' ? "" : "/");
                overlay = strdup(o);
            }
            if (!nregs && bundled_registry(argv[0], base_reg, sizeof base_reg))
                regs[nregs++] = base_reg;
            lsb.user = NULL; /* signed in */
        }
        else if (!lsb.user)
        {
            fprintf(stderr, "no sign-in screen here: --session V, or --user name for a LandSandBoat server\n");
            return 2;
        }
    }
    if (lsb.user)
    {
        /* a LandSandBoat server: sign in before anything is loaded, so a refusal costs nothing */
        char pw[256], err[512];
        if (!lsb.password && lsb.login_token)
            lsb.password = ""; /* the token replaces it */
        if (!lsb.password)
        {
            if (!read_secret("Password: ", pw, sizeof pw))
            {
                fprintf(stderr, "--user: no password (--pass, FFXI_PASSWORD, or a terminal to ask on)\n");
                return 2;
            }
            lsb.password = pw;
        }
        lsb.server = pol_server;
        int ok = lsb_login(&lsb, err, sizeof err);
        memset(pw, 0, sizeof pw);
        if (!ok)
        {
            fprintf(stderr, "sign-in failed: %s\n", err);
            return 1;
        }
    }

#ifdef XI_SPLIT
    {
        /* the game itself: this build's translation, made on this machine (xi_game.h) */
        char err[1200];
        if (!module_path)
        {
            fprintf(stderr, "--module <game module>: the translation of this install's build, made by the launcher\n");
            return 2;
        }
        if (!xi_game_load(module_path, err, sizeof err))
        {
            fprintf(stderr, "cannot load the game module: %s\n", err);
            return 1;
        }
        rt_log("[recomp] game module: build %s (version %s) from %s\n", FFXI_BUILD, FFXI_VERSION, module_path);
    }
#endif
    if (!gwin_init())
    {
        fprintf(stderr, "cannot reserve the guest window\n");
        return 1;
    }
#ifdef XI_SPLIT
    xi_game_bind();
#endif
    gt_init();
    rt_set_native_handler(thunk_dispatch);
    /* The game sees Windows paths. On Windows they are the host's own; elsewhere the install
     * (FINAL FANTASY XI and PlayOnlineViewer, side by side as retail installs them) is mounted at
     * C:\PlayOnline\SquareEnix. */
    char exe[700], path[700], guest_game[700], host_game[700];
    snprintf(host_game, sizeof host_game, "%s", game);
    for (size_t n = strlen(host_game); n > 1 && (host_game[n - 1] == '/' || host_game[n - 1] == '\\'); --n)
        host_game[n - 1] = 0;
    if (plat_path_sep == '\\')
        snprintf(guest_game, sizeof guest_game, "%s", host_game);
    else
    {
        char host_viewer[760];
        snprintf(guest_game, sizeof guest_game, "C:\\PlayOnline\\SquareEnix\\FINAL FANTASY XI");
        if (viewer_dir)
            snprintf(host_viewer, sizeof host_viewer, "%s", viewer_dir);
        else
            snprintf(host_viewer, sizeof host_viewer, "%s/../PlayOnlineViewer", host_game);
        vfs_mount(guest_game, host_game);
        vfs_mount("C:\\PlayOnline\\SquareEnix\\PlayOnlineViewer", host_viewer);
    }
    game = guest_game;
    snprintf(exe, sizeof exe, "%s\\..\\PlayOnlineViewer\\pol.exe", game);
    k32_init(game, exe);
    k32_io_init();
    k32_misc_init();
    ole_init();
    user32_init();
    d3d8_init();
    dsound_init();
    dinput_init();
    ws2_init();
    /* PlayOnline's hosts: the lobby through polcore's resolver, every other *.pol.com through
     * gethostbyname */
    ws2_set_pol_server(pol_server);
    polcore_set_lobby(pol_server);
    rt_log("[recomp] *.pol.com -> %u.%u.%u.%u\n", pol_server >> 24, (pol_server >> 16) & 255, (pol_server >> 8) & 255,
        pol_server & 255);
    reg_init(regs, nregs, overlay);
    for (unsigned i = 0; i < nfinals; ++i)
        reg_load_final(finals[i]);
    {
        /* The install folders are where the game is now, whatever the imported registry says (it
         * comes from another machine or folder); the game checks them (FFXI-9001). Retail writes
         * 0001 with a trailing backslash and 1000 without. */
        char p[760];
        snprintf(p, sizeof p, "%s\\", game);
        reg_set_string("HKEY_LOCAL_MACHINE\\SOFTWARE\\PlayOnlineUS\\InstallFolder", "0001", p);
        snprintf(p, sizeof p, "%s\\..\\PlayOnlineViewer", game);
        char viewer[760];
        if (vfs_full_path(p, viewer, sizeof viewer))
            snprintf(p, sizeof p, "%s", viewer);
        reg_set_string("HKEY_LOCAL_MACHINE\\SOFTWARE\\PlayOnlineUS\\InstallFolder", "1000", p);
    }
    vfs_init(game);
    if (user_dir)
    {
        char guest_user[760];
        snprintf(guest_user, sizeof guest_user, "%s\\USER", game);
        vfs_mount(guest_user, user_dir);
        rt_log("[recomp] USER -> %s\n", user_dir);
    }
    for (unsigned i = 0; i < ndats; ++i)
    {
        /* DAT overlays: files under their ROM*\ and sound*\ folders replace the install's */
        if (!vfs_add_overlay(dats[i], report_overlay))
            rt_log("[recomp] dats: no ROM or sound files in %s\n", dats[i]);
    }
    if (version_dir)
        rt_log("[recomp] version: %s, %u files over the install\n", version_dir, vfs_set_version(game, version_dir));
    {
        /* texture packs: --textures, else <data dir>/textures if there is one */
        char def[1100];
        PlatStat st;
        if (!npacks && data_dir)
        {
            snprintf(def, sizeof def, "%s%ctextures", data_dir, plat_path_sep);
            if (plat_stat(def, &st))
                packs[npacks++] = def;
        }
        for (unsigned i = 0; i < npacks; ++i)
            d3d8_texture_pack(packs[i]);
    }
    {
        /* The game reads patch.ver from its folder and will not start without it; the lobby sees
         * the version inside. Installs launched without the PlayOnline Viewer (private servers'
         * xiloader) ship none: then one is made for this build's version, in --data-dir (else next
         * to the host), and mounted over the game's path. The install itself is never written. */
        char pv[760];
        PlatStat st;
        game_file(game, host_game, "patch.ver", pv, sizeof pv);
        if (!plat_stat(pv, &st))
        {
            uint8_t file[0x120];
            if (data_dir)
                snprintf(pv, sizeof pv, "%s%cpatch.%s.ver", data_dir, plat_path_sep, FFXI_VERSION);
            else
            {
                const char *dir_end = argv[0], *p;
                for (p = argv[0]; *p; ++p)
                    if (*p == '/' || *p == plat_path_sep)
                        dir_end = p + 1;
                snprintf(pv, sizeof pv, "%.*spatch.%s.ver", (int)(dir_end - argv[0]), argv[0], FFXI_VERSION);
            }
            PlatFile* f = polcore_make_patch_ver(FFXI_VERSION, file) ? plat_file_open(pv, PLAT_WRITE | PLAT_CREATE | PLAT_TRUNCATE) : NULL;
            if (!f || plat_file_write(f, file, sizeof file) != sizeof file)
            {
                fprintf(stderr, "cannot write %s\n", pv);
                return 1;
            }
            plat_file_close(f);
            char guest_pv[760];
            snprintf(guest_pv, sizeof guest_pv, "%s\\patch.ver", game);
            vfs_mount(guest_pv, pv);
            printf("[recomp] no patch.ver in the install: version %s from %s\n", FFXI_VERSION, pv);
        }
    }
    polcore_slots_init();
    polcore_files_init();
    polcore_polpro_init();
    {
        char viewer[700];
        snprintf(viewer, sizeof viewer, "%s\\..\\PlayOnlineViewer", game);
        char full[700];
        if (vfs_full_path(viewer, full, sizeof full))
            polcore_set_root(full);
    }
    if (data_dir)
    {
        /* Our name dictionary, <data dir>/dic/entryz.dic, written the first time (and editable):
         * the words a new character's name may not contain. The game loads it where PlayOnline keeps
         * its dictionaries; without one it refuses every name ("already in use", 3322). The server
         * checks names as well. */
        static const char DEFAULT_WORDS[] =
            "#xi-name-dic\n"
            "# Character names containing one of these words (any letter case) are refused before\n"
            "# they are sent to the server, which checks names with its own filter as well.\n"
            "# One word a line.\n"
            "fuck\n"
            "nazi\n";
        char dir[1100], file[1200];
        snprintf(dir, sizeof dir, "%s%cdic", data_dir, plat_path_sep);
        snprintf(file, sizeof file, "%s%centryz.dic", dir, plat_path_sep);
        PlatStat st;
        plat_mkdir(dir);
        if (!plat_stat(file, &st))
        {
            FILE* f = fopen(file, "wb");
            if (f)
            {
                fwrite(DEFAULT_WORDS, 1, sizeof DEFAULT_WORDS - 1, f);
                fclose(f);
            }
        }
        if (plat_path_sep == '\\')
            polcore_set_dic_dir(dir);
        else
        {
            vfs_mount("C:\\PlayOnline\\SquareEnix\\XiDic", dir);
            polcore_set_dic_dir("C:\\PlayOnline\\SquareEnix\\XiDic");
        }
    }

    /* the images first, before the heap spreads through the low window */
    game_file(game, host_game, "FFXiMain.dll", path, sizeof path);
    if (!pe_load(path))
        return 1;
    game_file(game, host_game, "FFXi.dll", path, sizeof path);
    if (!pe_load_module(path, &rt_module_ffxi, FFXI_BASE))
        return 1;
    k32_add_module("FFXi.dll", FFXI_BASE);
    ole_register_class(CLSID_GameMain, rt_image_base);
    ole_register_class(CLSID_FxFileManager, FFXI_BASE);
    ole_register_class(CLSID_FFXiEntry, FFXI_BASE);
    polcore_init();
    d3d8_setup();
    d3d8_set_present_hook(present_hook);
    user32_set_close_handler(close_asked);
    setup_nameplates();
    setup_packets();
    {
        /* the auto-translate phrases, for the overlay's chat */
        char at[1024];
        game_file(game, host_game, "ROM\\76\\23.DAT", at, sizeof at);
        int n = gamestate_load_autotranslate(at);
        rt_log("[recomp] auto-translate: %d phrases\n", n);
        gamestate_set_zone_files(host_game, MZB_KEYS); /* the overlay's zone maps */
    }
    user32_key_hook = host_key;
    if (g_live_file[0])
        ws2_lobby_error = launcher_lobby_error;
    if (getenv("FFXI_PROFILE") && getenv("FFXI_PROFILE")[0] && getenv("FFXI_PROFILE")[0] != '0')
        thunk_timer = gfx_prof_shim, g_profile_shims = 1; /* the profile splits the game's time into its code and our API calls */
    dsound_setup();
    dinput_setup();
    if (getenv("FFXI_RECOMP_MISSING"))
        thunk_report_missing();

    /* both CRTs, as the Windows loader would run them */
    uint32_t attach[3] = { rt_image_base, 1, 0 };
    if (!guest_call(rt_image_oep, 3, attach))
    {
        fprintf(stderr, "FFXiMain's DllMain failed\n");
        return 1;
    }
    uint32_t attach_ffxi[3] = { FFXI_BASE, 1, 0 };
    if (!guest_call(rt_module_ffxi.oep + *rt_module_ffxi.delta, 3, attach_ffxi))
    {
        fprintf(stderr, "FFXi.dll's DllMain failed\n");
        return 1;
    }

    /* FFXiEntry, through FFXi.dll's class factory */
    gt_lock();
    uint32_t clsid = guest_bytes(CLSID_FFXiEntry, 16), iid_cf = guest_bytes(IID_IClassFactory, 16),
             iid_entry = guest_bytes(IID_IFFXiEntry, 16), out = gheap_alloc(4, 1);
    gt_unlock();
    uint32_t gco[3] = { clsid, iid_cf, out };
    uint32_t hr = guest_call(pe_export_at(FFXI_BASE, "DllGetClassObject"), 3, gco);
    uint32_t cf = rd32(out);
    if (hr || !cf)
    {
        fprintf(stderr, "FFXi.dll DllGetClassObject(FFXiEntry): %08x\n", hr);
        return 1;
    }
    uint32_t ci[3] = { 0, iid_entry, out };
    hr = com_call(cf, 3, 3, ci);
    uint32_t entry = rd32(out);
    if (hr || !entry)
    {
        fprintf(stderr, "IClassFactory::CreateInstance(IFFXiEntry): %08x\n", hr);
        return 1;
    }

    /* IFFXiEntry::GameStart(pPol, &pFFXiMessage): the game runs inside this call */
    rt_log("[recomp] GameStart\n");
#if defined(_WIN32)
    /* FFXI_SAMPLE=<file>: where this thread's time goes, sampled (tools/sample_report.py) */
    if (getenv("FFXI_SAMPLE") && getenv("FFXI_SAMPLE")[0])
        rt_log("[recomp] sampling the game thread to %s: %s\n", getenv("FFXI_SAMPLE"),
            sampler_start(getenv("FFXI_SAMPLE")) ? "on" : "cannot write it");
#endif
    uint32_t start[2] = { polcore_object(), out };
    hr = com_call(entry, 3, 2, start);
    const char* message = NULL;
    int32_t code = polcore_exit_code(&message);
    rt_log("[recomp] GameStart returned %08x; exit code %d%s%s\n", hr, code, message && *message ? ", message: " : "",
        message ? message : "");
    return hr ? 1 : 0;
}
