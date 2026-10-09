/* The overlay's windows (overlay.h, docs/OVERLAY.md), with Dear ImGui (third_party/imgui). The back
 * end draws what overlay_build_frame makes (overlay_metal.mm). Display only. */
#include "overlay.h"
#include "gamestate.h"
#include "zonemap.h"
#include "itemdat.h"

#include <algorithm>
#include <float.h>
#include <stdlib.h>
#include <string>
#include <unordered_map>
#include <math.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>

#include "imgui.h"
#include "imgui_internal.h" /* ImGuiSettingsHandler: the chat's settings in overlay.ini */
#include "backends/imgui_impl_sdl3.h"
#include <SDL3/SDL.h>
#include "fonts/roboto_medium.h"

extern "C" int dsound_in_world(void);
extern "C" void d3d8_set_weather_effects(int rain, int fog, int heat, int lightning, int snow);
extern "C" void d3d8_set_sky(float stars, int shooting, float bright);
extern "C" void d3d8_falling(float* rain, float* snow);
extern "C" void d3d8_set_water(int on, const float* v);
extern "C" void d3d8_set_water_body(const float* v);
extern "C" void d3d8_set_look(int sky, const float* aurora, int world, int filter, const float* tint);
extern "C" void d3d8_set_entity_xforms(int n, const float (*pos)[4], const float (*m)[16]);
extern "C" void d3d8_set_entity_others(int n, const float (*pos)[3]);
extern "C" void d3d8_set_creatures(float birds, float fish, int flying_fish);
extern "C" int d3d8_creatures_known(int which);
extern "C" int d3d8_world_up_sign(void);
extern "C" void d3d8_set_world_rock(int on, const float* m);
extern "C" int d3d8_on_ship(const float* pos, float* m);
extern "C" int d3d8_cam_command(const char* text);
extern "C" int d3d8_cam_playing(void);
extern "C" int d3d8_cam_hides_ui(void);
extern "C" void d3d8_cam_settings(float* pace, float* smooth, int* hide_ui, int* places, int* recording);

/* Fonts: Roboto is built in; the others are the player's own system's, loaded from where each
 * system keeps them if they are there (nothing of theirs is shipped). Kept in overlay.ini by name. */
static const struct
{
    const char* name;
    const char* paths[3];
} FONT_FILES[] = {
    { "Arial", { "/System/Library/Fonts/Supplemental/Arial.ttf", "C:\\Windows\\Fonts\\arial.ttf", "/usr/share/fonts/truetype/msttcorefonts/Arial.ttf" } },
    { "Arial Bold", { "/System/Library/Fonts/Supplemental/Arial Bold.ttf", "C:\\Windows\\Fonts\\arialbd.ttf", "/usr/share/fonts/truetype/msttcorefonts/Arial_Bold.ttf" } },
    { "Verdana", { "/System/Library/Fonts/Supplemental/Verdana.ttf", "C:\\Windows\\Fonts\\verdana.ttf", NULL } },
    { "Verdana Bold", { "/System/Library/Fonts/Supplemental/Verdana Bold.ttf", "C:\\Windows\\Fonts\\verdanab.ttf", NULL } },
    { "Trebuchet", { "/System/Library/Fonts/Supplemental/Trebuchet MS.ttf", "C:\\Windows\\Fonts\\trebuc.ttf", NULL } },
    { "Trebuchet Bold", { "/System/Library/Fonts/Supplemental/Trebuchet MS Bold.ttf", "C:\\Windows\\Fonts\\trebucbd.ttf", NULL } },
    { "Tahoma", { "/System/Library/Fonts/Supplemental/Tahoma.ttf", "C:\\Windows\\Fonts\\tahoma.ttf", NULL } },
    { "Tahoma Bold", { "/System/Library/Fonts/Supplemental/Tahoma Bold.ttf", "C:\\Windows\\Fonts\\tahomabd.ttf", NULL } },
    { "Georgia", { "/System/Library/Fonts/Supplemental/Georgia.ttf", "C:\\Windows\\Fonts\\georgia.ttf", NULL } },
    { "Georgia Bold", { "/System/Library/Fonts/Supplemental/Georgia Bold.ttf", "C:\\Windows\\Fonts\\georgiab.ttf", NULL } },
    { "Segoe UI", { NULL, "C:\\Windows\\Fonts\\segoeui.ttf", NULL } },
    { "Segoe UI Bold", { NULL, "C:\\Windows\\Fonts\\segoeuib.ttf", NULL } },
    { "DejaVu Sans", { NULL, NULL, "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf" } },
    { "DejaVu Sans Bold", { NULL, NULL, "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf" } },
};
static struct
{
    const char* name;
    ImFont* font;
} g_fonts[1 + sizeof FONT_FILES / sizeof *FONT_FILES];
static int g_nfonts;

static ImFont* font_named(const char* name)
{
    for (int i = 0; i < g_nfonts; ++i)
        if (!strcmp(g_fonts[i].name, name))
            return g_fonts[i].font;
    return g_nfonts ? g_fonts[0].font : NULL;
}

/* a combo of the fonts there are; true when the choice changed */
static bool font_combo(const char* label, char* name, size_t size)
{
    bool changed = false;
    if (ImGui::BeginCombo(label, name[0] ? name : "Roboto"))
    {
        for (int i = 0; i < g_nfonts; ++i)
        {
            ImGui::PushFont(g_fonts[i].font, 0.0f);
            if (ImGui::Selectable(g_fonts[i].name, !strcmp(name, g_fonts[i].name)))
                snprintf(name, size, "%s", g_fonts[i].name), changed = true;
            ImGui::PopFont();
        }
        ImGui::EndCombo();
    }
    return changed;
}

static bool g_ready, g_shown = true; /* the modern interface is on from the start (Cmd+U, Ctrl+Shift+U elsewhere, hides it) */
/* and shows once a character is on its way into the world (its zone's connection open), not over the
 * title, the login or the character list */
static bool ui_on(void)
{
    return g_shown && dsound_in_world();
}
/* Edit GUI (the Overlay window): the windows can be moved and sized, with their names on them; else
 * they stay where they were put, with no title bars and nothing to close by accident */
static bool g_edit;
static void chat_register(void);
static void overlay_register(void);
static int (*g_run_line)(const char* line); /* host64: the game's parser of a typed line */
static void (*g_hide_game)(int log, int party, int target); /* host64: the game's own windows hidden */
static const char* (*g_game_focus)(void);         /* host64: the game's window with the keyboard */
static int (*g_close_game)(const char* name8);    /* host64: the game's own close of one of its windows */
static void (*g_open_settings)(void);             /* host64: the launcher's settings */
static int (*g_focus_rect)(float* x, float* y, float* w, float* h); /* host64: where the game's window with the keyboard is */
static void (*g_place_focus)(float x, float y);   /* host64: and moves it */
static bool g_question;                            /* the one asking is a question (yes/no, a choice): it joins the chat */
static bool g_asking;                              /* one of the game's own windows is asking something */
static char g_asker[9];                            /* its name */
static ImVec2 g_ask0, g_ask1;                      /* where, on the screen (the overlay's units) */
static int g_send_open;                           /* the box asked to open: 1 empty, 2 with "/", 3 with "!" */
static bool g_swallow_text;                        /* the key's own character, not to be typed */
static SDL_Scancode g_swallow_up = SDL_SCANCODE_UNKNOWN; /* and its release (the game opens its own line on /'s) */
/* knocked out (ko_window): the screen's state */
static struct
{
    bool on;          /* the screen up (dead, and the setting on) */
    bool waiting; /* Wait for a Raise: the strip */
    float grey;
    int step;    /* Return to Home Point: 1 open the game's menu, 2 its Yes/No coming, 3 at it */
    double step_at;
    const char* note; /* why Return did nothing, for a while */
    double note_at;
} g_ko = { false, false, 0.0f, 0, 0.0, NULL, 0.0 };

/* The names over heads this frame, as the game placed them (host64's nameplate hook) */
static bool g_plates_available;
static struct Plate
{
    float x, y, z;
    ImU32 color;
    char text[40];
} g_plates[256], g_last_plates[256];
static int g_nplates, g_nlast_plates;
/* names behind walls: the last frames' plates the back end was asked about (a few in flight), and
 * which names were found hidden */
static struct
{
    int token, n;
    char text[256][40];
    float z[256];
} g_plate_asked[4];
static int g_plate_token;
static bool g_plates_depth_wrong; /* the depths turned out not as expected: nothing is hidden */
/* name -> whether it is hidden now, and how many readings running have said otherwise: hidden after
 * two, seen again only after eight (a gap between bricks, a building's edge, is not a way through) */
struct PlateSeen
{
    bool hidden;
    int against;
};
static std::unordered_map<std::string, PlateSeen> g_plate_hidden;
/* each name's depth is read at five points: where it is, and a little to each side, above and below;
 * it is behind something when most of them are */
static const float PLATE_TAPS[5][2] = { { 0, 0 }, { -0.012f, 0 }, { 0.012f, 0 }, { 0, -0.008f }, { 0, 0.008f } };

/* The player's choices, kept in overlay.ini ([Overlay][Settings]) */
static struct
{
    bool chat = true, party = true, map = true, status = false, target = true;
    bool hide_game_log = false, hide_game_party = false, hide_game_target = false; /* the game's own, where ours stand in */
    bool plates = false;       /* the names over heads drawn by the overlay */
    char ui_font[32] = "Roboto", plate_font[32] = "Arial Bold";
    float plate_size = 15.0f;
    bool plate_outline = true;
    bool plates_occlude = true; /* names behind walls hidden, as the game's depth says */
    float ui_size = 15.0f;   /* the windows' text */
    float chat_size = 15.0f; /* the chat's lines */
    float map_range = 50.0f; /* yalms from the middle to the edge */
    bool map_north_up = false, map_names = false;
    bool map_art = false; /* the game's own map under the radar, where there is one (a choice: its
                           * placement is not right in every zone yet) */
    bool bar = true, settings_open = false; /* the bar of icons; the Overlay window */
    bool chat_pinned = true; /* the chat held to the bottom right corner */
    bool equip = false, items = false; /* the equipment and item windows */
    float map_alpha = 1.0f; /* the map's opacity (its right-click menu) */
    /* the weather's effects (d3d8.c): rain on the ground, its fog, the heat's shimmer */
    bool wx_rain = true, wx_fog = true, wx_heat = true, wx_lightning = true, wx_snow = true;
    bool wx_falling = true; /* rain and snow falling over the picture (ours: some zones' have none) */
    float sky_stars = 1.0f; /* the night sky: stars more (0 none) */
    float sky_bright = 1.0f; /* and how bright they are */
    bool sky_shooting = true;
    /* still water (d3d8.c g_water): wave height, speed, size, direction (degrees), blue to green, brightness,
     * sky reflection, glint, glint size */
    bool water = true;
    float water_v[9] = { 1.0f, 1.0f, 1.0f, 35.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f };
    float water_body[4] = { 0.75f, 0.03f, 0.10f, 0.14f }; /* how opaque, the deep color */
    /* fun (d3d8_set_look): the server's events (!skyfx) over the player's own look, or not; an aurora
     * (r, g, b, strength), wireframe (0 off, 1 the zone, 2 everything), a filter (0 none, 1 grey, 2 sepia,
     * 3 a color, 4 inverted, 5 night vision; r, g, b, amount) */
    bool fun_server = true, fun_aurora = false;
    float fun_aurora_c[4] = { 0.24f, 1.0f, 0.55f, 0.8f };
    int fun_world = 0, fun_filter = 0;
    float fun_filter_c[4] = { 1.0f, 0.78f, 0.31f, 0.6f };
    float fun_birds = 1.0f, fun_fish = 1.0f; /* how many of the zone's birds and fish (1 as the game has them) */
    bool fun_ships = true;                    /* ships rock at the dock */
    bool fun_flying_fish = false;             /* the fish's copies through the air, as the birds' */
    bool fun_aboard = true;                   /* aboard a ship, the world rocks (the deck with you) */
    float cam_pace = 0.5f, cam_smooth = 1.0f; /* the cinematic camera: how fast it flies the path, smoothing (seconds) */
    bool cam_hide = true, cam_bars = true;    /* the interfaces gone while it flies; black bars, as a film */
    float fun_ships_k = 1.0f;
    /* the windows' background and the chat's: a color and how solid (the player's) */
    float win_bg[4] = { 0.102f, 0.118f, 0.180f, 0.88f }, chat_bg[4] = { 0.094f, 0.118f, 0.188f, 0.80f }; /* the interface skin's slate */
    /* the chat, as the game's log: down to a few lines when nothing has come for a while */
    bool chat_shrink = true;
    int chat_quiet_lines = 4;
    float chat_quiet_secs = 12.0f;
    bool death_screen = true; /* knocked out: the screen grey, the time, the buttons */
    bool space_jumps = true;  /* J: /jump (the setting's old name kept, for overlay.ini) */
} g_set;
static char g_ini[1024];
static struct
{
    int frame_w, frame_h, screen_w, screen_h, metalfx;
    float fps;
} g_present;

/* The show/hide key: Cmd+U on macOS (the game uses no Command key; macOS keeps F11 for Show
 * Desktop), Ctrl+Shift+U elsewhere (the game's macros are Ctrl or Alt with a digit). */
static bool is_toggle(const SDL_KeyboardEvent& k)
{
    if (k.key == SDLK_F12)
        return !(k.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI | SDL_KMOD_SHIFT)); /* F12, alone */
    if (k.key != SDLK_U)
        return false;
#if defined(__APPLE__)
    return (k.mod & SDL_KMOD_GUI) && !(k.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT));
#else
    return (k.mod & SDL_KMOD_CTRL) && (k.mod & SDL_KMOD_SHIFT) && !(k.mod & SDL_KMOD_ALT);
#endif
}
#if defined(__APPLE__)
static const char* const TOGGLE_NAME = "F12 (or Cmd+U)";
#else
static const char* const TOGGLE_NAME = "F12 (or Ctrl+Shift+U)";
#endif

extern "C" void overlay_set_ini(const char* path)
{
    snprintf(g_ini, sizeof g_ini, "%s", path ? path : "");
}

/* The SDL back end stops the window's text input when an overlay text box lets go of it, but the
 * game's own typing (user32.c) needs it on all the time: it is turned straight back on. */
static void (*g_ime_backend)(ImGuiContext*, ImGuiViewport*, ImGuiPlatformImeData*);
static SDL_Window* g_window;
static void ime_keep_text_input(ImGuiContext* ctx, ImGuiViewport* vp, ImGuiPlatformImeData* data)
{
    g_ime_backend(ctx, vp, data);
    if (!data->WantVisible && !data->WantTextInput && !SDL_TextInputActive(g_window))
        SDL_StartTextInput(g_window);
}

extern "C" void overlay_init(SDL_Window* window)
{
    if (g_ready || !window)
        return;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    chat_register(); /* before overlay.ini is read */
    overlay_register();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = g_ini[0] ? g_ini : NULL; /* where its windows were, kept between sessions */
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange; /* the game's cursor stays the game's */
    ImGui_ImplSDL3_InitForMetal(window);
    g_window = window;
    g_ime_backend = ImGui::GetPlatformIO().Platform_SetImeDataFn;
    ImGui::GetPlatformIO().Platform_SetImeDataFn = ime_keep_text_input;
    /* Roboto, at a size for the screen: ImGui 1.92 renders it at the framebuffer's scale itself */
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    g_fonts[g_nfonts].name = "Roboto";
    g_fonts[g_nfonts++].font = io.Fonts->AddFontFromMemoryCompressedTTF(roboto_medium_compressed_data, (int)roboto_medium_compressed_size, 16.0f, &cfg);
#if defined(__APPLE__)
    const int os = 0;
#elif defined(_WIN32)
    const int os = 1;
#else
    const int os = 2;
#endif
    for (const auto& f : FONT_FILES)
    {
        const char* path = f.paths[os];
        FILE* there = path ? fopen(path, "rb") : NULL;
        if (!there)
            continue;
        fclose(there);
        ImFontConfig fc;
        if (ImFont* font = io.Fonts->AddFontFromFileTTF(path, 16.0f, &fc))
            g_fonts[g_nfonts].name = f.name, g_fonts[g_nfonts++].font = font;
    }
    io.FontDefault = g_fonts[0].font;
    ImGuiStyle& st = ImGui::GetStyle();
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 6.0f;
    st.FrameRounding = 4.0f;
    st.WindowBorderSize = 1.0f;
    st.Colors[ImGuiCol_WindowBg].w = 0.82f; /* the world shows through a little */
    g_ready = true;
}

extern "C" void overlay_set_nameplates_available(int yes)
{
    g_plates_available = yes != 0;
}

extern "C" int overlay_nameplates_wanted(void)
{
    return g_ready && ui_on() && g_plates_available && g_set.plates;
}

extern "C" int overlay_plate_points(float* xy, int max, int* token)
{
    if (!g_set.plates || !g_set.plates_occlude || !g_nlast_plates)
        return 0;
    int n = g_nlast_plates < max / 5 ? g_nlast_plates : max / 5;
    int t = ++g_plate_token;
    auto& a = g_plate_asked[t & 3];
    a.token = t, a.n = n;
    for (int i = 0; i < n; ++i)
    {
        for (int k = 0; k < 5; ++k)
            xy[10 * i + 2 * k] = g_last_plates[i].x + PLATE_TAPS[k][0], xy[10 * i + 2 * k + 1] = g_last_plates[i].y + PLATE_TAPS[k][1];
        memcpy(a.text[i], g_last_plates[i].text, sizeof a.text[i]);
        a.z[i] = g_last_plates[i].z;
    }
    *token = t;
    return 5 * n;
}

extern "C" void overlay_plate_depths(int token, const float* depths, int n, float p10, float p14)
{
    auto& a = g_plate_asked[token & 3];
    n /= 5; /* five readings a name */
    if (a.token != token || n > a.n)
        return;
    /* a depth to a distance: the projection gives z = p10 + p14 / distance (p10 signed by gfx) */
    auto dist = [&](float z) { return z - p10 != 0.0f ? p14 / (z - p10) : 1e9f; };
    static int told;
    for (int i = 0; i < n; ++i)
    {
        float name = dist(a.z[i]), scene = dist(depths[5 * i]);
        int behind = 0;
        for (int k = 0; k < 5; ++k)
        {
            float d = depths[5 * i + k];
            behind += d > 0.0f && d < 1.0f && dist(d) < name - 0.75f;
        }
        bool hidden = behind >= 3;
        if (told < 12)
        {
            ++told;
            extern void rt_log(const char* fmt, ...);
            rt_log("[recomp] nameplate %s: %.1f away, the scene's %.1f there, %d of 5 points in front: %s\n", a.text[i], name, scene, behind,
                hidden ? "hidden" : "seen");
        }
        PlateSeen& h = g_plate_hidden[a.text[i]];
        if (hidden == h.hidden)
            h.against = 0;
        else if (++h.against >= (hidden ? 2 : 8))
            h.hidden = hidden, h.against = 0;
        /* if nearly every name reads as hidden, the depths are not what they are taken to be: the
         * hiding stops (and says so) rather than taking every name away */
        static int seen_n, hidden_n;
        if (seen_n + hidden_n < 150)
        {
            (hidden ? hidden_n : seen_n)++;
            if (seen_n + hidden_n == 150 && hidden_n > 135)
            {
                g_plates_depth_wrong = true;
                extern void rt_log(const char* fmt, ...);
                rt_log("[recomp] nameplates: %d of 150 read as behind something: the depths are not as expected, so none are hidden\n", hidden_n);
            }
        }
    }
}

extern "C" void overlay_nameplate(float x, float y, float z, const char* text, uint32_t color) /* x, y: 0-1 across the view */
{
    if (g_nplates >= (int)(sizeof g_plates / sizeof *g_plates))
        return;
    Plate& p = g_plates[g_nplates++];
    p.x = x, p.y = y, p.z = z;
    /* the game's colors count 0x80 as full */
    auto c = [](uint32_t v) { return (unsigned)(v * 2 > 255 ? 255 : v * 2); };
    p.color = IM_COL32(c(color >> 16 & 255), c(color >> 8 & 255), c(color & 255), 255);
    /* its text: the name only. The game's own marks (GM, mentor...) are codes in it, one byte
     * (0x80 and up) or two (a Shift-JIS lead, 0x81-0x9F or 0xE0-0xFC, and the byte after): all
     * dropped, the overlay draws its own marks from the player's flags. */
    size_t o = 0;
    bool odd = false;
    for (const unsigned char* t = (const unsigned char*)text; *t && o + 1 < sizeof p.text; ++t)
    {
        if (*t >= 0x20 && *t < 0x7F)
            p.text[o++] = (char)*t;
        else
        {
            odd = true;
            if (((*t >= 0x81 && *t <= 0x9F) || (*t >= 0xE0 && *t <= 0xFC)) && t[1])
                ++t;
        }
    }
    while (o && p.text[o - 1] == ' ')
        --o; /* a space the game left before a mark */
    p.text[o] = 0;
    static int told;
    if (odd && told < 4)
    {
        ++told;
        char hex[3 * 24 + 1] = "";
        for (int i = 0; i < 24 && text[i]; ++i)
            snprintf(hex + 3 * i, 4, "%02x ", (unsigned char)text[i]);
        extern void rt_log(const char* fmt, ...);
        rt_log("[recomp] nameplate with the game's codes: %s-> \"%s\"\n", hex, p.text);
    }
}

/* --- the marks by a player's name, drawn sharp at any size ----------------------------------------- */
/* Before the name: GM (a badge), mentor (a star), new adventurer (a leaf), seeking a party (a flag),
 * away (a moon), bazaar (a coin); after it, their linkshell's pearl in its color. */
static const int LEFT_MARKS = MARK_GM | MARK_MENTOR | MARK_NEW | MARK_LFG | MARK_AWAY | MARK_BAZAAR;

static float marks_width(uint16_t marks, float h, bool left)
{
    int n = 0;
    for (int b = 1; b <= MARK_LS; b <<= 1)
        if ((marks & b) && ((b & LEFT_MARKS) != 0) == left && b != MARK_ANON)
            ++n;
    return n ? n * (h * 0.95f) + h * 0.15f : 0.0f;
}

static void star(ImDrawList* dl, ImVec2 c, float r, ImU32 fill, ImU32 edge)
{
    ImVec2 pts[10];
    for (int i = 0; i < 10; ++i)
    {
        float a = -IM_PI / 2 + i * IM_PI / 5, rr = i & 1 ? r * 0.45f : r;
        pts[i] = ImVec2(c.x + cosf(a) * rr, c.y + sinf(a) * rr);
    }
    for (int i = 0; i < 5; ++i) /* a convex fill per point, then the middle */
        dl->AddTriangleFilled(pts[(2 * i + 9) % 10], pts[2 * i], pts[2 * i + 1], fill);
    ImVec2 mid[5] = { pts[1], pts[3], pts[5], pts[7], pts[9] };
    dl->AddConvexPolyFilled(mid, 5, fill);
    dl->AddPolyline(pts, 10, edge, ImDrawFlags_Closed, 1.0f);
}

/* one mark, in a square of side h at a (its top-left) */
static void mark_icon(ImDrawList* dl, ImFont* font, int mark, uint8_t gm, uint32_t ls, ImVec2 a, float h)
{
    ImVec2 c(a.x + h * 0.5f, a.y + h * 0.5f);
    float r = h * 0.42f;
    const ImU32 ink = IM_COL32(20, 15, 10, 230);
    switch (mark)
    {
    case MARK_GM:
    {
        /* a badge: dark red, a gold rim, GM in white (higher GM levels, a brighter gold) */
        ImU32 gold = gm >= 4 ? IM_COL32(255, 225, 90, 255) : IM_COL32(220, 175, 60, 255);
        ImVec2 p0(a.x + h * 0.02f, a.y + h * 0.16f), p1(a.x + h * 0.98f, a.y + h * 0.84f);
        dl->AddRectFilled(p0, p1, IM_COL32(150, 25, 25, 255), h * 0.18f);
        dl->AddRect(p0, p1, gold, h * 0.18f, 0, ImMax(1.0f, h * 0.08f));
        float ts = h * 0.52f;
        ImVec2 t = font->CalcTextSizeA(ts, FLT_MAX, 0.0f, "GM");
        dl->AddText(font, ts, ImVec2(c.x - t.x * 0.5f, c.y - t.y * 0.5f), IM_COL32(255, 245, 220, 255), "GM");
        break;
    }
    case MARK_MENTOR: star(dl, c, r * 1.1f, IM_COL32(255, 205, 60, 255), ink); break;
    case MARK_NEW:
    {
        /* a leaf: two arcs meeting at the ends, and its vein */
        ImVec2 tip(c.x + r * 0.8f, c.y - r * 0.8f), stem(c.x - r * 0.8f, c.y + r * 0.8f);
        dl->PathLineTo(stem);
        dl->PathBezierQuadraticCurveTo(ImVec2(c.x - r * 0.9f, c.y - r * 0.9f), tip);
        dl->PathBezierQuadraticCurveTo(ImVec2(c.x + r * 0.9f, c.y + r * 0.9f), stem);
        dl->PathFillConcave(IM_COL32(95, 190, 80, 255));
        dl->AddLine(stem, tip, IM_COL32(40, 110, 40, 255), ImMax(1.0f, h * 0.06f));
        break;
    }
    case MARK_LFG:
    {
        /* a flag on a pole */
        float x0 = a.x + h * 0.22f;
        dl->AddLine(ImVec2(x0, a.y + h * 0.1f), ImVec2(x0, a.y + h * 0.92f), IM_COL32(230, 230, 230, 255), ImMax(1.0f, h * 0.08f));
        dl->AddTriangleFilled(ImVec2(x0, a.y + h * 0.12f), ImVec2(a.x + h * 0.92f, a.y + h * 0.3f), ImVec2(x0, a.y + h * 0.5f),
            IM_COL32(70, 140, 240, 255));
        break;
    }
    case MARK_AWAY:
    {
        /* a crescent moon: an arc, thick */
        dl->PathArcTo(c, r * 0.75f, IM_PI * 0.35f, IM_PI * 1.65f, 16);
        dl->PathStroke(IM_COL32(200, 205, 225, 255), 0, ImMax(1.5f, r * 0.45f));
        break;
    }
    case MARK_BAZAAR:
        dl->AddCircleFilled(c, r, IM_COL32(230, 185, 60, 255), 20);
        dl->AddCircle(c, r * 0.62f, IM_COL32(150, 105, 25, 255), 20, ImMax(1.0f, h * 0.07f));
        dl->AddCircle(c, r, ink, 20, 1.0f);
        break;
    case MARK_LS:
    {
        /* a pearl in the linkshell's color: a highlight up and left, a dark rim */
        ImU32 col = IM_COL32(ls >> 16 & 255, ls >> 8 & 255, ls & 255, 255);
        dl->AddCircleFilled(c, r * 0.9f, col, 24);
        dl->AddCircleFilled(ImVec2(c.x - r * 0.3f, c.y - r * 0.3f), r * 0.3f, IM_COL32(255, 255, 255, 150), 12);
        dl->AddCircle(c, r * 0.9f, ink, 24, 1.0f);
        break;
    }
    default: break;
    }
}

/* the marks on one side of a name, from x (left to right), middle at y; returns the width used */
static float draw_marks(ImDrawList* dl, ImFont* font, uint16_t marks, uint8_t gm, uint32_t ls, float x, float y, float h, bool left)
{
    float at = x;
    for (int b = 1; b <= MARK_LS; b <<= 1)
        if ((marks & b) && ((b & LEFT_MARKS) != 0) == left && b != MARK_ANON)
        {
            mark_icon(dl, font, b, gm, ls, ImVec2(at, y - h * 0.5f), h);
            at += h * 0.95f;
        }
    return at - x;
}

/* a name as an item, with the player's marks either side of it */
static void name_with_marks(const char* name, ImU32 color)
{
    uint16_t marks = 0;
    uint8_t gm = 0;
    uint32_t ls = 0;
    gamestate_marks(name, &marks, &gm, &ls);
    float h = ImGui::GetTextLineHeight();
    ImVec2 a = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float lw = marks ? draw_marks(dl, ImGui::GetFont(), marks, gm, ls, a.x, a.y + h * 0.5f, h, true) : 0.0f;
    if (lw > 0)
        ImGui::Dummy(ImVec2(lw, h)), ImGui::SameLine(0, 2);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(name);
    ImGui::PopStyleColor();
    if (marks & MARK_LS)
    {
        ImGui::SameLine(0, 3);
        ImVec2 b = ImGui::GetCursorScreenPos();
        float rw = draw_marks(dl, ImGui::GetFont(), marks, gm, ls, b.x, b.y + h * 0.5f, h, false);
        ImGui::Dummy(ImVec2(rw, h));
    }
}

/* The names, drawn under every window, where the game put them (its 3D frame to the window) */
static void draw_nameplates(void)
{
    if (!g_nplates)
        return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImVec2 disp = ImGui::GetIO().DisplaySize;
    float kx = disp.x, ky = disp.y; /* the 3D view fills the window */
    ImFont* font = font_named(g_set.plate_font);
    float size = g_set.plate_size;
    memcpy(g_last_plates, g_plates, sizeof(Plate) * (size_t)g_nplates);
    g_nlast_plates = g_nplates;
    for (int i = 0; i < g_nplates; ++i)
    {
        const Plate& p = g_plates[i];
        if (g_set.plates_occlude && !g_plates_depth_wrong)
        {
            auto h = g_plate_hidden.find(p.text);
            if (h != g_plate_hidden.end() && h->second.hidden)
                continue; /* behind something the game drew */
        }
        ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, p.text);
        uint16_t marks = 0;
        uint8_t gm = 0;
        uint32_t ls = 0;
        gamestate_marks(p.text, &marks, &gm, &ls);
        float ih = size * 1.05f, lw = marks_width(marks, ih, true), rw = marks_width(marks, ih, false);
        float left = p.x * kx - (lw + ts.x + rw) * 0.5f;
        ImVec2 at(left + lw, p.y * ky - ts.y * 0.5f);
        if (marks)
        {
            draw_marks(dl, font, marks, gm, ls, left, at.y + ts.y * 0.5f, ih, true);
            draw_marks(dl, font, marks, gm, ls, at.x + ts.x + ih * 0.15f, at.y + ts.y * 0.5f, ih, false);
        }
        if (g_set.plate_outline)
            for (int k = 0; k < 8; ++k)
            {
                static const float OX[8] = { -1, 0, 1, -1, 1, -1, 0, 1 }, OY[8] = { -1, -1, -1, 0, 0, 1, 1, 1 };
                dl->AddText(font, size, ImVec2(at.x + OX[k], at.y + OY[k]), IM_COL32(0, 0, 0, 200), p.text);
            }
        else
            dl->AddText(font, size, ImVec2(at.x + 1, at.y + 1), IM_COL32(0, 0, 0, 160), p.text);
        dl->AddText(font, size, at, p.color, p.text);
    }
    g_nplates = 0; /* the next frame's come as the game draws them */
}

extern "C" void overlay_set_game_window_closer(int (*close)(const char* name8))
{
    g_close_game = close;
}

extern "C" void overlay_set_focus_rect(int (*rect)(float* x, float* y, float* w, float* h), void (*place)(float x, float y))
{
    g_focus_rect = rect;
    g_place_focus = place;
}

static void (*g_place_log)(float x0, float y0, float x1, float y1);
extern "C" void overlay_set_log_placer(void (*place)(float x0, float y0, float x1, float y1)) { g_place_log = place; }

extern "C" void overlay_set_settings_opener(void (*open)(void))
{
    g_open_settings = open;
}

extern "C" void overlay_set_game_windows(void (*hide)(int log, int party, int target), const char* (*focus)(void))
{
    g_hide_game = hide;
    g_game_focus = focus;
}

/* Typing, when the game's log is hidden: the keys that open the game's own input line (Space, and
 * "/") open the overlay's chat box instead, while no menu of the game's has the keyboard. */
static bool typing_is_ours(const SDL_KeyboardEvent& k)
{
    if (!g_shown || !g_set.chat || !g_set.hide_game_log || !g_run_line || !dsound_in_world() || k.repeat)
        return false;
    if (k.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI))
        return false;
    bool bang = k.key == SDLK_EXCLAIM || (k.key == SDLK_1 && (k.mod & SDL_KMOD_SHIFT));
    bool enter = k.key == SDLK_RETURN || k.key == SDLK_KP_ENTER;
    if (k.key != SDLK_SPACE && k.key != SDLK_SLASH && !bang && !enter)
        return false;
    const char* focus = g_game_focus ? g_game_focus() : "";
    /* knocked out, the game's death menu has the keyboard; the screen's buttons stand in for it */
    bool ko_menu = g_ko.on && !strncmp(focus, "dead", 4);
    if (focus[0] && strncmp(focus, "logwin", 6) && strncmp(focus, "fulllog", 7) && !ko_menu)
        return false;
    /* Enter only with nothing targeted: with a target it is the game's (talk, attack, confirm) */
    return !enter || !gamestate_targeting();
}

extern "C" void overlay_set_line_runner(int (*run)(const char* line))
{
    g_run_line = run;
}

/* flying (gamestate_flying): Space and X held, for the host's climb and sink; never while typing, nor
 * while one of the game's windows has the keyboard */
static bool g_fly_up, g_fly_down;
static bool fly_keys_free(void)
{
    if (!gamestate_flying() || !dsound_in_world() || (g_shown && ImGui::GetIO().WantTextInput))
        return false;
    const char* focus = g_game_focus ? g_game_focus() : "";
    return !focus[0] || !strncmp(focus, "logwin", 6) || !strncmp(focus, "fulllog", 7);
}

extern "C" void overlay_fly_keys(int* up, int* down)
{
    bool free = fly_keys_free();
    *up = free && g_fly_up, *down = free && g_fly_down;
}

extern "C" int overlay_shown(void)
{
    return g_ready && ui_on();
}

extern "C" void overlay_note_present(int frame_w, int frame_h, int screen_w, int screen_h, int metalfx, float fps)
{
    g_present.frame_w = frame_w, g_present.frame_h = frame_h;
    g_present.screen_w = screen_w, g_present.screen_h = screen_h;
    g_present.metalfx = metalfx, g_present.fps = fps;
}

extern "C" int overlay_event(const SDL_Event* e)
{
    if (!g_ready)
        return 0;
    if (e->type == SDL_EVENT_KEY_DOWN && e->key.key == SDLK_ESCAPE && d3d8_cam_playing())
        return d3d8_cam_command("cam stop"), 1; /* a flight ends; the game's Esc waits */
    if (e->type == SDL_EVENT_KEY_DOWN && is_toggle(e->key))
    {
        if (!e->key.repeat)
            g_shown = !g_shown;
        return 1;
    }
    if (e->type == SDL_EVENT_KEY_UP && ((e->key.key == SDLK_U && (e->key.mod & (SDL_KMOD_GUI | SDL_KMOD_CTRL))) || e->key.key == SDLK_F12))
        return 1;
    if (e->type == SDL_EVENT_KEY_UP && g_swallow_up != SDL_SCANCODE_UNKNOWN && e->key.scancode == g_swallow_up)
    {
        g_swallow_up = SDL_SCANCODE_UNKNOWN; /* the release of a key that opened the chat box */
        return 1;
    }
    if (e->type == SDL_EVENT_TEXT_INPUT && g_swallow_text)
    {
        g_swallow_text = false; /* the Space or / that opened the box */
        return 1;
    }
    if (e->type == SDL_EVENT_TEXT_INPUT && (g_fly_up || g_fly_down) &&
        (!strcmp(e->text.text, " ") || !strcmp(e->text.text, "x") || !strcmp(e->text.text, "X")))
        return 1; /* a held Space or X repeats its text: the game would open its typing line */
    if ((e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_KEY_UP) && (e->key.key == SDLK_SPACE || e->key.key == SDLK_X) &&
        !(e->key.mod & (SDL_KMOD_SHIFT | SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)))
    {
        /* flying: Space rises, X sinks, held; the game never sees them (Space would open its typing
         * line). A release always lets go, wherever it goes. */
        bool down = e->type == SDL_EVENT_KEY_DOWN;
        if (!down)
            (e->key.key == SDLK_SPACE ? g_fly_up : g_fly_down) = false;
        {
            /* while learning it: why a press was (or was not) taken for flying */
            static int told;
            if (down && !e->key.repeat && told < 30 && (gamestate_flying() || told < 3))
            {
                ++told;
                const char* f = g_game_focus ? g_game_focus() : "";
                extern void rt_log(const char* fmt, ...);
                rt_log("[recomp] fly key %s: flying %d, in the world %d, typing %d, the game's keyboard \"%.8s\" -> %s\n",
                    e->key.key == SDLK_SPACE ? "Space" : "X", gamestate_flying(), dsound_in_world(),
                    g_shown && ImGui::GetIO().WantTextInput, f, fly_keys_free() ? "flying" : "passed on");
            }
        }
        if (fly_keys_free())
        {
            if (down)
                (e->key.key == SDLK_SPACE ? g_fly_up : g_fly_down) = true;
            if (down && !e->key.repeat)
                g_swallow_text = true;
            return 1;
        }
    }
    if (e->type == SDL_EVENT_KEY_DOWN && e->key.key == SDLK_J && g_set.space_jumps && !(g_shown && ImGui::GetIO().WantTextInput) &&
        !(e->key.mod & (SDL_KMOD_SHIFT | SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) && dsound_in_world())
    {
        /* J: a jump (the game's /jump), while none of the game's windows has the keyboard (its own
         * typing line included), the overlay shown or not. The game never sees the key: its text and
         * release go nowhere. (Shift+Space first, a player's suggestion; Shift is the game's too.) */
        const char* focus = g_game_focus ? g_game_focus() : "";
        bool free = !focus[0] || !strncmp(focus, "logwin", 6) || !strncmp(focus, "fulllog", 7);
        if (free)
        {
            if (!e->key.repeat && !g_ko.on && g_run_line)
                g_run_line("/jump");
            g_swallow_text = true, g_swallow_up = e->key.scancode;
            return 1;
        }
    }
    if (e->type == SDL_EVENT_KEY_DOWN && !ImGui::GetIO().WantTextInput && typing_is_ours(e->key))
    {
        SDL_Keycode key = e->key.key;
        g_send_open = key == SDLK_SLASH ? 2 : key == SDLK_EXCLAIM || key == SDLK_1 ? 3 : 1;
        g_swallow_text = key != SDLK_RETURN && key != SDLK_KP_ENTER; /* Enter types nothing */
        g_swallow_up = e->key.scancode;
        return 1;
    }
    if ((e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_KEY_UP) && e->key.key == SDLK_TAB && !ImGui::GetIO().WantTextInput)
        return 0; /* Tab is the game's (the next target), not a way into the overlay's boxes */
    ImGui_ImplSDL3_ProcessEvent(e);
    if (!ui_on())
        return 0;
    if (g_send_open && (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_TEXT_INPUT))
        return 1; /* typed before the box has opened: the box's */
    const ImGuiIO& io = ImGui::GetIO();
    switch (e->type)
    {
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_WHEEL:
        return io.WantCaptureMouse;
    case SDL_EVENT_MOUSE_BUTTON_UP:
    {
        /* a release goes where its press went: the game's, pressed outside the overlay, to the game */
        int b = e->button.button == SDL_BUTTON_LEFT ? 0 : e->button.button == SDL_BUTTON_RIGHT ? 1 : e->button.button == SDL_BUTTON_MIDDLE ? 2 : -1;
        return b >= 0 ? io.MouseDownOwned[b] : io.WantCaptureMouse;
    }
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_TEXT_INPUT:
        return io.WantTextInput;
    default: return 0;
    }
}

/* the map's colors (map_colors_window) */
enum MapColor
{
    MC_GROUND, MC_BACK, MC_RING, MC_COMPASS, MC_NORTH, MC_SELF, MC_PARTY, MC_PC, MC_NPC, MC_MOB, MC_CLAIMED, MC_TARGET, MC_COUNT
};
static const char* const MC_NAME[MC_COUNT] = { "Ground", "Background", "Rim", "Compass", "North", "You", "Party", "Players",
                                               "NPCs", "Monsters", "Claimed", "Target ring" };
static const char* const MC_KEY[MC_COUNT] = { "ground", "back", "ring", "compass", "north", "self", "party", "pc", "npc", "mob", "claimed", "target" };
static const ImU32 MC_DEFAULT[MC_COUNT] = {
    IM_COL32(232, 214, 170, 255), /* ground: papyrus */
    IM_COL32(58, 44, 30, 215),    /* background: dark umber */
    IM_COL32(120, 90, 55, 255),   /* rim */
    IM_COL32(95, 70, 40, 255),    /* compass letters */
    IM_COL32(170, 40, 30, 255),   /* north */
    IM_COL32(60, 35, 15, 255),    /* you */
    IM_COL32(20, 140, 170, 255),  /* party */
    IM_COL32(40, 80, 190, 255),   /* other players */
    IM_COL32(30, 130, 55, 255),   /* NPCs */
    IM_COL32(200, 135, 20, 255),  /* monsters */
    IM_COL32(190, 35, 30, 255),   /* claimed */
    IM_COL32(20, 20, 20, 255),    /* the target's ring */
};
static ImU32 g_map_col[MC_COUNT];
static bool g_map_colors_open;

/* [Overlay][Settings] in overlay.ini: which windows, the text sizes, the map's */
static void* overlay_ini_open(ImGuiContext*, ImGuiSettingsHandler*, const char*) { return (void*)1; }

static void overlay_ini_line(ImGuiContext*, ImGuiSettingsHandler*, void*, const char* line)
{
    int v;
    float f;
    if (sscanf(line, "chat=%d", &v) == 1) g_set.chat = v != 0;
    else if (sscanf(line, "party=%d", &v) == 1) g_set.party = v != 0;
    else if (sscanf(line, "map=%d", &v) == 1) g_set.map = v != 0;
    else if (sscanf(line, "status=%d", &v) == 1) g_set.status = v != 0;
    else if (sscanf(line, "target=%d", &v) == 1) g_set.target = v != 0;
    else if (sscanf(line, "hide_game_log=%d", &v) == 1) g_set.hide_game_log = v != 0;
    else if (sscanf(line, "hide_game_party=%d", &v) == 1) g_set.hide_game_party = v != 0;
    else if (sscanf(line, "hide_game_target=%d", &v) == 1) g_set.hide_game_target = v != 0;
    else if (sscanf(line, "plates=%d", &v) == 1) g_set.plates = v != 0;
    else if (sscanf(line, "plate_size=%f", &f) == 1 && f >= 8 && f <= 40) g_set.plate_size = f;
    else if (sscanf(line, "plate_outline=%d", &v) == 1) g_set.plate_outline = v != 0;
    else if (sscanf(line, "plates_occlude=%d", &v) == 1) g_set.plates_occlude = v != 0;
    else if (!strncmp(line, "ui_font=", 8)) snprintf(g_set.ui_font, sizeof g_set.ui_font, "%s", line + 8);
    else if (!strncmp(line, "mapcol.", 7))
    {
        char key[24];
        unsigned argb;
        if (sscanf(line + 7, "%23[a-z]=%x", key, &argb) == 2)
            for (int k = 0; k < MC_COUNT; ++k)
                if (!strcmp(key, MC_KEY[k]))
                    g_map_col[k] = IM_COL32(argb >> 16 & 255, argb >> 8 & 255, argb & 255, argb >> 24 & 255);
    }
    else if (!strncmp(line, "plate_font=", 11)) snprintf(g_set.plate_font, sizeof g_set.plate_font, "%s", line + 11);
    else if (sscanf(line, "chat_shrink=%d", &v) == 1) g_set.chat_shrink = v != 0;
    else if (sscanf(line, "death_screen=%d", &v) == 1) g_set.death_screen = v != 0;
    else if (sscanf(line, "space_jumps=%d", &v) == 1) g_set.space_jumps = v != 0;
    else if (sscanf(line, "chat_quiet_lines=%d", &v) == 1 && v >= 1 && v <= 20) g_set.chat_quiet_lines = v;
    else if (sscanf(line, "chat_quiet_secs=%f", &f) == 1 && f >= 2 && f <= 120) g_set.chat_quiet_secs = f;
    else if (!strncmp(line, "win_bg=", 7) || !strncmp(line, "chat_bg=", 8))
    {
        float c[4];
        if (sscanf(strchr(line, '=') + 1, "%f,%f,%f,%f", &c[0], &c[1], &c[2], &c[3]) == 4)
            memcpy(line[0] == 'w' ? g_set.win_bg : g_set.chat_bg, c, sizeof c);
    }
    else if (sscanf(line, "ui_size=%f", &f) == 1 && f >= 10 && f <= 32) g_set.ui_size = f;
    else if (sscanf(line, "chat_size=%f", &f) == 1 && f >= 10 && f <= 32) g_set.chat_size = f;
    else if (sscanf(line, "map_range=%f", &f) == 1 && f >= 10 && f <= 250) g_set.map_range = f;
    else if (sscanf(line, "map_north_up=%d", &v) == 1) g_set.map_north_up = v != 0;
    else if (sscanf(line, "map_names=%d", &v) == 1) g_set.map_names = v != 0;
    else if (sscanf(line, "map_art2=%d", &v) == 1) g_set.map_art = v != 0;
    else if (sscanf(line, "bar=%d", &v) == 1) g_set.bar = v != 0;
    else if (sscanf(line, "chat_pinned=%d", &v) == 1) g_set.chat_pinned = v != 0;
    else if (sscanf(line, "equip=%d", &v) == 1) g_set.equip = v != 0;
    else if (sscanf(line, "items=%d", &v) == 1) g_set.items = v != 0;
    else if (sscanf(line, "map_alpha=%f", &f) == 1 && f >= 0.1f && f <= 1.0f) g_set.map_alpha = f;
    else if (sscanf(line, "wx_rain=%d", &v) == 1) g_set.wx_rain = v != 0;
    else if (sscanf(line, "wx_fog=%d", &v) == 1) g_set.wx_fog = v != 0;
    else if (sscanf(line, "wx_heat=%d", &v) == 1) g_set.wx_heat = v != 0;
    else if (sscanf(line, "wx_lightning=%d", &v) == 1) g_set.wx_lightning = v != 0;
    else if (sscanf(line, "wx_snow=%d", &v) == 1) g_set.wx_snow = v != 0;
    else if (sscanf(line, "wx_falling=%d", &v) == 1) g_set.wx_falling = v != 0;
    else if (sscanf(line, "sky_stars=%f", &f) == 1 && f >= 0 && f <= 3) g_set.sky_stars = f;
    else if (sscanf(line, "sky_bright=%f", &f) == 1 && f >= 0 && f <= 4) g_set.sky_bright = f;
    else if (sscanf(line, "sky_shooting=%d", &v) == 1) g_set.sky_shooting = v != 0;
    else if (sscanf(line, "water=%d", &v) == 1) g_set.water = v != 0;
    else if (sscanf(line, "fun_server=%d", &v) == 1) g_set.fun_server = v != 0;
    else if (sscanf(line, "fun_aurora=%d", &v) == 1) g_set.fun_aurora = v != 0;
    else if (sscanf(line, "fun_world=%d", &v) == 1 && v >= 0 && v <= 2) g_set.fun_world = v;
    else if (sscanf(line, "fun_filter=%d", &v) == 1 && v >= 0 && v <= 5) g_set.fun_filter = v;
    else if (sscanf(line, "fun_birds=%f", &f) == 1 && f >= 1 && f <= 2000) g_set.fun_birds = f;
    else if (sscanf(line, "fun_fish=%f", &f) == 1 && f >= 1 && f <= 2000) g_set.fun_fish = f;
    else if (sscanf(line, "fun_ships=%d", &v) == 1) g_set.fun_ships = v != 0;
    else if (sscanf(line, "fun_flying_fish=%d", &v) == 1) g_set.fun_flying_fish = v != 0;
    else if (sscanf(line, "fun_aboard=%d", &v) == 1) g_set.fun_aboard = v != 0;
    else if (sscanf(line, "cam_pace=%f", &f) == 1 && f >= 0.05f && f <= 4) g_set.cam_pace = f;
    else if (sscanf(line, "cam_smooth=%f", &f) == 1 && f >= 0 && f <= 5) g_set.cam_smooth = f;
    else if (sscanf(line, "cam_hide=%d", &v) == 1) g_set.cam_hide = v != 0;
    else if (sscanf(line, "cam_bars=%d", &v) == 1) g_set.cam_bars = v != 0;
    else if (sscanf(line, "fun_ships_k=%f", &f) == 1 && f >= 0 && f <= 3) g_set.fun_ships_k = f;
    else if (!strncmp(line, "fun_aurora_c=", 13) || !strncmp(line, "fun_filter_c=", 13))
    {
        float c[4];
        if (sscanf(line + 13, "%f,%f,%f,%f", &c[0], &c[1], &c[2], &c[3]) == 4)
            memcpy(line[4] == 'a' ? g_set.fun_aurora_c : g_set.fun_filter_c, c, sizeof c);
    }
    else if (!strncmp(line, "water_body=", 11))
    {
        float w[4];
        if (sscanf(line + 11, "%f,%f,%f,%f", &w[0], &w[1], &w[2], &w[3]) == 4)
            memcpy(g_set.water_body, w, sizeof w);
    }
    else if (!strncmp(line, "water_v=", 8))
    {
        float w[9];
        if (sscanf(line + 8, "%f,%f,%f,%f,%f,%f,%f,%f,%f", &w[0], &w[1], &w[2], &w[3], &w[4], &w[5], &w[6], &w[7], &w[8]) == 9)
            memcpy(g_set.water_v, w, sizeof w);
    }
}

static void overlay_ini_write(ImGuiContext*, ImGuiSettingsHandler* h, ImGuiTextBuffer* out)
{
    out->appendf("[%s][Settings]\n", h->TypeName);
    out->appendf("chat=%d\nparty=%d\nmap=%d\nstatus=%d\n", g_set.chat, g_set.party, g_set.map, g_set.status);
    out->appendf("ui_size=%g\nchat_size=%g\nmap_range=%g\n", g_set.ui_size, g_set.chat_size, g_set.map_range);
    out->appendf("map_north_up=%d\nmap_names=%d\nmap_art2=%d\nbar=%d\n", g_set.map_north_up, g_set.map_names, g_set.map_art, g_set.bar);
    out->appendf("equip=%d\nitems=%d\nchat_pinned=%d\n", g_set.equip, g_set.items, g_set.chat_pinned);
    out->appendf("target=%d\nhide_game_log=%d\nhide_game_party=%d\nhide_game_target=%d\n", g_set.target, g_set.hide_game_log,
        g_set.hide_game_party, g_set.hide_game_target);
    out->appendf("plates=%d\nplate_size=%g\nplate_outline=%d\nplates_occlude=%d\n", g_set.plates, g_set.plate_size, g_set.plate_outline,
        g_set.plates_occlude);
    out->appendf("ui_font=%s\nplate_font=%s\n", g_set.ui_font, g_set.plate_font);
    out->appendf("chat_shrink=%d\nchat_quiet_lines=%d\nchat_quiet_secs=%g\n", g_set.chat_shrink, g_set.chat_quiet_lines,
        g_set.chat_quiet_secs);
    out->appendf("death_screen=%d\nspace_jumps=%d\n", g_set.death_screen, g_set.space_jumps);
    out->appendf("wx_falling=%d\nsky_bright=%g\n", g_set.wx_falling, g_set.sky_bright);
    out->appendf("wx_lightning=%d\nwx_snow=%d\nsky_stars=%g\nsky_shooting=%d\n", g_set.wx_lightning, g_set.wx_snow, g_set.sky_stars,
        g_set.sky_shooting);
    out->appendf("map_alpha=%.2f\nwx_rain=%d\nwx_fog=%d\nwx_heat=%d\nwater=%d\n", g_set.map_alpha, g_set.wx_rain, g_set.wx_fog,
        g_set.wx_heat, g_set.water);
    out->appendf("fun_server=%d\nfun_aurora=%d\nfun_world=%d\nfun_filter=%d\n", g_set.fun_server, g_set.fun_aurora, g_set.fun_world,
        g_set.fun_filter);
    out->appendf("cam_pace=%g\ncam_smooth=%g\ncam_hide=%d\ncam_bars=%d\n", g_set.cam_pace, g_set.cam_smooth, g_set.cam_hide, g_set.cam_bars);
    out->appendf("fun_flying_fish=%d\nfun_aboard=%d\n", g_set.fun_flying_fish, g_set.fun_aboard);
    out->appendf("fun_birds=%g\nfun_fish=%g\nfun_ships=%d\nfun_ships_k=%g\n", g_set.fun_birds, g_set.fun_fish, g_set.fun_ships,
        g_set.fun_ships_k);
    const float *fa = g_set.fun_aurora_c, *ff = g_set.fun_filter_c;
    out->appendf("fun_aurora_c=%.3f,%.3f,%.3f,%.3f\nfun_filter_c=%.3f,%.3f,%.3f,%.3f\n", fa[0], fa[1], fa[2], fa[3], ff[0], ff[1], ff[2],
        ff[3]);
    out->appendf("water_body=%.3f,%.3f,%.3f,%.3f\n", g_set.water_body[0], g_set.water_body[1], g_set.water_body[2], g_set.water_body[3]);
    const float* w = g_set.water_v;
    out->appendf("water_v=%g,%g,%g,%g,%g,%g,%g,%g,%g\n", w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8]);
    out->appendf("win_bg=%.3f,%.3f,%.3f,%.3f\nchat_bg=%.3f,%.3f,%.3f,%.3f\n", g_set.win_bg[0], g_set.win_bg[1], g_set.win_bg[2],
        g_set.win_bg[3], g_set.chat_bg[0], g_set.chat_bg[1], g_set.chat_bg[2], g_set.chat_bg[3]);
    for (int k = 0; k < MC_COUNT; ++k)
    {
        ImU32 c = g_map_col[k] ? g_map_col[k] : MC_DEFAULT[k];
        out->appendf("mapcol.%s=%02x%02x%02x%02x\n", MC_KEY[k], c >> 24 & 255, c & 255, c >> 8 & 255, c >> 16 & 255);
    }
    out->append("\n");
}

static void overlay_register(void)
{
    ImGuiSettingsHandler h;
    h.TypeName = "Overlay";
    h.TypeHash = ImHashStr("Overlay");
    h.ReadOpenFn = overlay_ini_open;
    h.ReadLineFn = overlay_ini_line;
    h.WriteAllFn = overlay_ini_write;
    ImGui::AddSettingsHandler(&h);
}

/* --- Edit GUI ------------------------------------------------------------------------------------------ */
/* A window that stays put: its title bar only while editing, moved and sized only then */
static ImGuiWindowFlags lockable(ImGuiWindowFlags f)
{
    return g_edit ? f & ~ImGuiWindowFlags_NoTitleBar : f | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
}

/* while editing, each window that can be moved outlined, so the layout is easy to see */
static void edit_outline(void)
{
    if (!g_edit)
        return;
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    ImGui::GetForegroundDrawList()->AddRect(w->Pos, ImVec2(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y), IM_COL32(255, 200, 80, 220), 4.0f, 0, 2.0f);
}

/* the banner while editing: what to do, and Lock */
static void edit_banner(void)
{
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.22f), ImGuiCond_Always, ImVec2(0.5f, 0));
    ImGui::SetNextWindowBgAlpha(0.92f);
    if (ImGui::Begin("##editgui", NULL,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse))
    {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), "Editing the GUI");
        ImGui::TextUnformatted("Drag the windows by their title bars, size them by their edges.");
        if (ImGui::Button("Lock the GUI", ImVec2(-FLT_MIN, 0)))
            g_edit = false, ImGui::MarkIniSettingsDirty();
    }
    ImGui::End();
}

/* --- rain and snow falling ------------------------------------------------------------------------------
 * Over the game's picture, under the windows, by our weather (d3d8_falling): rain as slanted streaks,
 * thick in a downpour; snow as flakes drifting down, swaying. Sized to the screen. */
static void precipitation(void)
{
    enum { MOST = 900 };
    static struct
    {
        float x, y, v, len, phase;
    } p[MOST];
    static bool made;
    static float shown_rain, shown_snow;
    float rain = 0.0f, snow = 0.0f;
    if (g_set.wx_falling && dsound_in_world())
        d3d8_falling(&rain, &snow);
    float dt = ImMin(ImGui::GetIO().DeltaTime, 0.1f);
    /* in and out over a few seconds, not all at once */
    shown_rain += (rain - shown_rain) * ImMin(1.0f, dt * 0.5f);
    shown_snow += (snow - shown_snow) * ImMin(1.0f, dt * 0.5f);
    if (shown_rain < 0.01f && shown_snow < 0.01f)
        return;
    ImVec2 d = ImGui::GetIO().DisplaySize;
    float k = d.y / 1080.0f;
    if (!made)
    {
        for (int i = 0; i < MOST; ++i)
            p[i].x = (float)rand() / RAND_MAX, p[i].y = (float)rand() / RAND_MAX, p[i].v = 0.7f + 0.6f * (float)rand() / RAND_MAX,
            p[i].len = 0.6f + 0.8f * (float)rand() / RAND_MAX, p[i].phase = 6.28f * (float)rand() / RAND_MAX;
        made = true;
    }
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    float t = (float)ImGui::GetTime();
    bool snowing = shown_snow > shown_rain;
    int n = (int)((snowing ? shown_snow * 600.0f : shown_rain * 800.0f));
    n = n > MOST ? MOST : n;
    for (int i = 0; i < n; ++i)
    {
        if (snowing)
        {
            /* flakes: slow, swaying, a little wind */
            p[i].y += dt * p[i].v * (0.06f + 0.05f * shown_snow);
            p[i].x += dt * (0.01f + 0.03f * shown_snow) + dt * 0.02f * sinf(t * 1.3f + p[i].phase);
        }
        else
        {
            p[i].y += dt * p[i].v * 1.6f;
            p[i].x += dt * p[i].v * 0.25f;
        }
        if (p[i].y > 1.05f)
            p[i].y -= 1.1f, p[i].x = (float)rand() / RAND_MAX;
        if (p[i].x > 1.02f)
            p[i].x -= 1.04f;
        ImVec2 a(p[i].x * d.x, p[i].y * d.y);
        if (snowing)
            dl->AddCircleFilled(a, (1.2f + 1.8f * p[i].len) * k, IM_COL32(245, 248, 255, (int)(150 + 70 * p[i].len)), 8);
        else
        {
            float len = (18.0f + 26.0f * p[i].len) * k * (0.8f + 0.4f * shown_rain);
            dl->AddLine(a, ImVec2(a.x - len * 0.16f, a.y - len), IM_COL32(205, 215, 235, (int)(40 + 50 * p[i].len)), 1.2f * k);
        }
    }
}

/* --- Graphics: our own effects on the game's picture ---------------------------------------------------- */
static bool g_gfx_open;

/* who has been made bigger or smaller (this session: by their id), drawn so about their feet */
static struct
{
    uint32_t id;
    float scale;
    char name[24];
} g_sizes[8];

static float* size_of(uint32_t id, bool make)
{
    for (auto& z : g_sizes)
        if (z.id == id)
            return &z.scale;
    if (!make)
        return NULL;
    for (auto& z : g_sizes)
        if (!z.id || z.scale == 1.0f)
            return z.id = id, z.scale = 1.0f, &z.scale;
    return NULL;
}

/* each frame: the sized and the ships (rocking at the dock: mostly down from where they float, a little
 * roll and pitch, so the pier's walkway never has to move), as changes to their draws */
static void entity_looks(void)
{
    static GameEntity ents[257];
    int n = dsound_in_world() ? gamestate_entities(ents, 256) : 0;
    {
        /* the player too (not among the others): sized like anyone */
        float x, y, z, f;
        uint32_t me = gamestate_self_id();
        if (n && me && gamestate_self(&x, &y, &z, &f))
        {
            GameEntity& e = ents[n++];
            memset(&e, 0, sizeof e);
            e.id = me, e.x = x, e.y = y, e.z = z;
        }
    }
    float pos[16][4], m[16][16];
    int k = 0;
    float t = (float)ImGui::GetTime();
    for (int i = 0; i < n && k < 16; ++i)
    {
        const GameEntity& e = ents[i];
        float* sc = size_of(e.id, false);
        float px = e.x, py = e.y, pz = e.z;
        if (sc && *sc != 1.0f)
        {
            float s = *sc, reach = e.hitbox > 0.0f ? ImClamp(e.hitbox * 2.0f, 2.5f, 15.0f) : 3.0f;
            const float mm[16] = { s, 0, 0, 0, 0, s, 0, 0, 0, 0, s, 0, px * (1 - s), py * (1 - s), pz * (1 - s), 1 };
            pos[k][0] = px, pos[k][1] = py, pos[k][2] = pz, pos[k][3] = reach;
            memcpy(m[k++], mm, sizeof mm);
        }
        else if (e.ship && g_set.fun_ships && g_set.fun_ships_k > 0.0f)
        {
            /* down, in the drawn world: against its up (the camera's to say) */
            float ph = (float)(e.id % 97) * 0.37f, A = 0.22f * g_set.fun_ships_k;
            float wave = 0.45f + 0.4f * sinf(t * 0.9f + ph) + 0.15f * sinf(t * 1.7f + ph * 2.0f); /* -0.1 .. 1 */
            float down = A * wave;
            float roll = 0.010f * g_set.fun_ships_k * sinf(t * 0.7f + ph), pitch = 0.006f * g_set.fun_ships_k * sinf(t * 0.55f + ph * 1.3f);
            float cr = cosf(roll), sr = sinf(roll), cp = cosf(pitch), sp = sinf(pitch);
            /* R = Rx(roll) Rz(pitch), about the ship's place, then down */
            float R[9] = { cp, sp, 0, -sp * cr, cp * cr, sr, sp * sr, -cp * sr, cr };
            float mm[16] = { R[0], R[1], R[2], 0, R[3], R[4], R[5], 0, R[6], R[7], R[8], 0, 0, 0, 0, 1 };
            /* translation: p - p R + (0, down, 0) */
            mm[12] = px - (px * R[0] + py * R[3] + pz * R[6]);
            mm[13] = py - (px * R[1] + py * R[4] + pz * R[7]) - down * (float)d3d8_world_up_sign();
            mm[14] = pz - (px * R[2] + py * R[5] + pz * R[8]);
            pos[k][0] = px, pos[k][1] = py, pos[k][2] = pz, pos[k][3] = -30.0f; /* its parts within 30 yalms too */
            memcpy(m[k++], mm, sizeof mm);
        }
    }
    {
        /* aboard a ship: the whole world rocked gently about the player, so the deck moves with them
         * (the ferries between Selbina and Mhaura, with and without pirates; the Manaclipper; the barge) */
        uint16_t z = gamestate_zone();
        bool aboard = z == 220 || z == 221 || z == 227 || z == 228 || z == 3 || z == 1;
        float x, y, zz, f;
        if (aboard && g_set.fun_aboard && g_set.fun_ships_k > 0.0f && dsound_in_world() && gamestate_self(&x, &y, &zz, &f))
        {
            float k = g_set.fun_ships_k;
            float roll = 0.018f * k * sinf(t * 0.62f), pitch = 0.009f * k * sinf(t * 0.47f + 1.3f);
            float heave = 0.12f * k * sinf(t * 0.8f + 0.4f) * (float)d3d8_world_up_sign();
            float cr = cosf(roll), sr = sinf(roll), cp = cosf(pitch), sp = sinf(pitch);
            float R[9] = { cp, sp, 0, -sp * cr, cp * cr, sr, sp * sr, -cp * sr, cr };
            float mm[16] = { R[0], R[1], R[2], 0, R[3], R[4], R[5], 0, R[6], R[7], R[8], 0, 0, 0, 0, 1 };
            mm[12] = x - (x * R[0] + y * R[3] + zz * R[6]);
            mm[13] = y - (x * R[1] + y * R[4] + zz * R[7]) + heave;
            mm[14] = zz - (x * R[2] + y * R[5] + zz * R[8]);
            d3d8_set_world_rock(1, mm);
        }
        else
            d3d8_set_world_rock(0, NULL);
    }
    /* aboard a rocking ship at the dock (on its deck): rocked with it, the player too */
    static uint32_t riding[64];
    int nriding = 0;
    if (g_set.fun_ships && g_set.fun_ships_k > 0.0f)
        for (int i = 0; i < n && k < 16; ++i)
        {
            const GameEntity& e = ents[i];
            float* sc = size_of(e.id, false);
            float at[3] = { e.x, e.y, e.z };
            if (e.ship || (sc && *sc != 1.0f) || !d3d8_on_ship(at, m[k]))
                continue;
            pos[k][0] = e.x, pos[k][1] = e.y, pos[k][2] = e.z, pos[k][3] = 3.0f;
            ++k;
            if (nriding < 64)
                riding[nriding++] = e.id;
        }
    auto rides = [&](uint32_t id) {
        for (int r = 0; r < nriding; ++r)
            if (riding[r] == id)
                return true;
        return false;
    };
    d3d8_set_entity_xforms(k, pos, m);
    /* everyone not sized, the player first: a body drawn in world space is the nearest one's, so the
     * player's armor beside a sized prop stays the player's */
    {
        static float others[128][3];
        int no = 0;
        if (n && ents[n - 1].id == gamestate_self_id() && !rides(ents[n - 1].id))
            others[no][0] = ents[n - 1].x, others[no][1] = ents[n - 1].y, others[no][2] = ents[n - 1].z, ++no;
        for (int i = 0; i < n && no < 128; ++i)
        {
            float* sc = size_of(ents[i].id, false);
            if ((sc && *sc != 1.0f) || ents[i].id == gamestate_self_id() || rides(ents[i].id))
                continue;
            others[no][0] = ents[i].x, others[no][1] = ents[i].y, others[no][2] = ents[i].z, ++no;
        }
        d3d8_set_entity_others(no, others);
    }
    if (dsound_in_world())
        d3d8_set_creatures(g_set.fun_birds, g_set.fun_fish, g_set.fun_flying_fish);
    else
        d3d8_set_creatures(1.0f, 1.0f, 0); /* the title, the login and the character list: as the game has them */
}

static void graphics_window(void)
{
    ImGui::SetNextWindowPos(ImVec2(320, 24), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400, 0), ImGuiCond_FirstUseEver);
    /* as tall as it needs, never wider than this: a long name or line wraps, the window stays put */
    float em = ImGui::GetFontSize();
    ImGui::SetNextWindowSizeConstraints(ImVec2(em * 26.0f, 0), ImVec2(em * 30.0f, FLT_MAX));
    if (!ImGui::Begin("Graphics", &g_gfx_open, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    }
    ImGui::PushTextWrapPos(0.0f);
    bool dirty = false;
    if (ImGui::BeginTabBar("gfx"))
    {
        if (ImGui::BeginTabItem("Water"))
        {
            dirty |= ImGui::Checkbox("Our water (still water: ponds, pools, the sea's calm)", &g_set.water);
            ImGui::TextDisabled("Waves, the sky mirrored in them, the sun's and moon's glint.");
            ImGui::BeginDisabled(!g_set.water);
            float* w = g_set.water_v;
            ImGui::PushItemWidth(-130);
            dirty |= ImGui::SliderFloat("Opacity", &g_set.water_body[0], 0.0f, 1.0f, g_set.water_body[0] <= 0.0f ? "the game's" : "%.2f");
            ImGui::SameLine();
            dirty |= ImGui::ColorEdit3("Deep color", &g_set.water_body[1], ImGuiColorEditFlags_NoInputs);
            dirty |= ImGui::SliderFloat("Blue to green", &w[4], -1.0f, 1.0f, w[4] == 0.0f ? "the game's" : "%.2f");
            dirty |= ImGui::SliderFloat("Brightness", &w[5], 0.5f, 2.0f, "%.2f");
            dirty |= ImGui::SliderFloat("Wave height", &w[0], 0.0f, 3.0f, "%.2f");
            dirty |= ImGui::SliderFloat("Wave speed", &w[1], 0.0f, 3.0f, "%.2f");
            dirty |= ImGui::SliderFloat("Wave size", &w[2], 0.25f, 4.0f, "%.2f");
            dirty |= ImGui::SliderFloat("Wave direction", &w[3], 0.0f, 360.0f, "%.0f deg");
            dirty |= ImGui::SliderFloat("Sky reflection", &w[6], 0.0f, 2.0f, "%.2f");
            dirty |= ImGui::SliderFloat("Sun and moon glint", &w[7], 0.0f, 3.0f, "%.2f");
            dirty |= ImGui::SliderFloat("Glint size", &w[8], 0.25f, 4.0f, "%.2f");
            ImGui::PopItemWidth();
            if (ImGui::SmallButton("Defaults##water"))
            {
                const float d[9] = { 1.0f, 1.0f, 1.0f, 35.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f }, b[4] = { 0.75f, 0.03f, 0.10f, 0.14f };
                memcpy(w, d, sizeof d), memcpy(g_set.water_body, b, sizeof b), dirty = true;
            }
            ImGui::EndDisabled();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Weather"))
        {
            ImGui::TextDisabled("With the zone's weather, as the server sends it:");
            dirty |= ImGui::Checkbox("Rain: the ground soaks, shines and takes the drops", &g_set.wx_rain);
            dirty |= ImGui::Checkbox("Fog, storms, snow: a fog nearer than the game's", &g_set.wx_fog);
            dirty |= ImGui::Checkbox("Hot spells, heat waves: the air shimmers far off", &g_set.wx_heat);
            dirty |= ImGui::Checkbox("Thunder: lightning lights up the world", &g_set.wx_lightning);
            dirty |= ImGui::Checkbox("Snow, blizzards: snow lies on the ground, and melts after", &g_set.wx_snow);
            dirty |= ImGui::Checkbox("Rain and snow falling (ours: some zones have none of their own)", &g_set.wx_falling);
            ImGui::Separator();
            ImGui::TextDisabled("The night sky");
            ImGui::SetNextItemWidth(-90);
            dirty |= ImGui::SliderFloat("Stars", &g_set.sky_stars, 0.0f, 3.0f, g_set.sky_stars <= 0.0f ? "the game's" : "x%.1f");
            ImGui::SetNextItemWidth(-90);
            dirty |= ImGui::SliderFloat("Star brightness", &g_set.sky_bright, 0.2f, 4.0f, "%.1f");
            ImGui::TextDisabled("Stars and the aurora come out as the sky darkens, at night.");
            dirty |= ImGui::Checkbox("Shooting stars", &g_set.sky_shooting);
            ImGui::TextDisabled("//xi fx weather <0-19> tries one out; -1 back to the server's.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Fun"))
        {
            uint8_t sl[12];
            bool from_server = gamestate_look(sl) != 0;
            dirty |= ImGui::Checkbox("Let the server's events change the picture (!skyfx)", &g_set.fun_server);
            if (from_server)
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), g_set.fun_server ? "This zone has a look from the server: yours waits until it ends."
                                                                                       : "This zone has a look from the server (not shown: yours instead).");
            ImGui::Separator();
            const ImGuiColorEditFlags cf = ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoAlpha;
            dirty |= ImGui::Checkbox("Aurora", &g_set.fun_aurora);
            ImGui::SameLine(110);
            dirty |= ImGui::ColorEdit3("##auroracol", g_set.fun_aurora_c, cf);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1);
            dirty |= ImGui::SliderFloat("##aurorastr", &g_set.fun_aurora_c[3], 0.1f, 2.5f, "brightness %.2f");
            ImGui::TextDisabled("Curtains of light in the northern sky, at night.");
            const char* worlds[] = { "Off", "The zone (people and monsters as they are)", "Everything" };
            ImGui::SetNextItemWidth(-90);
            dirty |= ImGui::Combo("Wireframe", &g_set.fun_world, worlds, 3);
            const char* filters[] = { "None", "Gray", "Sepia", "A color", "Inverted", "Night vision" };
            ImGui::SetNextItemWidth(-90);
            dirty |= ImGui::Combo("Filter", &g_set.fun_filter, filters, 6);
            if (g_set.fun_filter)
            {
                if (g_set.fun_filter == 3)
                {
                    dirty |= ImGui::ColorEdit3("##filtercol", g_set.fun_filter_c, cf);
                    ImGui::SameLine();
                }
                ImGui::SetNextItemWidth(-90);
                dirty |= ImGui::SliderFloat("Amount", &g_set.fun_filter_c[3], 0.05f, 1.0f, "%.2f");
            }
            ImGui::Separator();
            ImGui::TextDisabled("Size: whatever you target, drawn bigger or smaller (yours alone)");
            {
                GameEntity te;
                int self = 0;
                if (gamestate_target(&te, &self) && te.id)
                {
                    float* sc = size_of(te.id, true);
                    if (sc)
                    {
                        for (auto& z : g_sizes)
                            if (z.id == te.id)
                                snprintf(z.name, sizeof z.name, "%s", te.name);
                        ImGui::SetNextItemWidth(-90);
                        ImGui::SliderFloat(te.name[0] ? te.name : "Target", sc, 0.25f, 5.0f, "x%.2f", ImGuiSliderFlags_Logarithmic);
                    }
                    else
                        ImGui::TextDisabled("Eight are sized already: reset one first.");
                }
                else
                    ImGui::TextDisabled("Target something to size it, or pick from who is around:");
                /* who is around, nearest first: the untargetable too (NPCs of the scenery, ships, props) */
                static GameEntity near[256];
                static uint32_t picked;
                int nn = gamestate_entities(near, 256);
                float mx, my, mz, mf;
                if (gamestate_self(&mx, &my, &mz, &mf))
                {
                    std::sort(near, near + nn, [&](const GameEntity& a, const GameEntity& b) {
                        float da = (a.x - mx) * (a.x - mx) + (a.z - mz) * (a.z - mz), db = (b.x - mx) * (b.x - mx) + (b.z - mz) * (b.z - mz);
                        return da < db;
                    });
                    const GameEntity* sel = NULL;
                    char label[64] = "Pick someone or something nearby";
                    for (int i = 0; i < nn; ++i)
                        if (near[i].id == picked)
                            sel = &near[i];
                    if (sel)
                        snprintf(label, sizeof label, "%s", sel->name[0] ? sel->name : "(no name)");
                    ImGui::SetNextItemWidth(-90);
                    if (ImGui::BeginCombo("Nearby", label))
                    {
                        for (int i = 0; i < nn && i < 80; ++i)
                        {
                            const GameEntity& e = near[i];
                            float dist = sqrtf((e.x - mx) * (e.x - mx) + (e.z - mz) * (e.z - mz));
                            if (dist > 60.0f)
                                break;
                            char who[40], row[96];
                            if (e.name[0])
                                snprintf(who, sizeof who, "%s", e.name);
                            else
                                snprintf(who, sizeof who, "(no name) #%u", e.index);
                            snprintf(row, sizeof row, "%s  %.0f yalms%s##%u", who, dist, e.ship ? "  (ship)" : e.hidden ? "  (not targetable)" : "", e.id);
                            if (ImGui::Selectable(row, e.id == picked))
                                picked = e.id;
                        }
                        ImGui::EndCombo();
                    }
                    if (sel)
                    {
                        float* sc = size_of(sel->id, true);
                        if (sc)
                        {
                            for (auto& z : g_sizes)
                                if (z.id == sel->id)
                                    snprintf(z.name, sizeof z.name, "%s", sel->name[0] ? sel->name : "(no name)");
                            ImGui::SetNextItemWidth(-90);
                            ImGui::SliderFloat("Its size", sc, 0.25f, 5.0f, "x%.2f", ImGuiSliderFlags_Logarithmic);
                        }
                    }
                }
                for (auto& z : g_sizes)
                    if (z.id && z.scale != 1.0f)
                    {
                        ImGui::PushID((int)z.id);
                        ImGui::BulletText("%s x%.2f", z.name[0] ? z.name : "(someone)", z.scale);
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Reset"))
                            z.scale = 1.0f;
                        ImGui::PopID();
                    }
            }
            ImGui::Separator();
            dirty |= ImGui::Checkbox("Ships rock at the dock", &g_set.fun_ships);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1);
            dirty |= ImGui::SliderFloat("##shipk", &g_set.fun_ships_k, 0.2f, 3.0f, "%.1f");
            dirty |= ImGui::Checkbox("Aboard a ship, the sea rocks it (the deck with you)", &g_set.fun_aboard);
            ImGui::SetNextItemWidth(-90);
            dirty |= ImGui::SliderFloat("Birds", &g_set.fun_birds, 1.0f, 2000.0f, g_set.fun_birds >= 300.0f ? "x%.0f (insanity)" : "x%.0f",
                ImGuiSliderFlags_Logarithmic);
            ImGui::SetNextItemWidth(-90);
            dirty |= ImGui::SliderFloat("Fish", &g_set.fun_fish, 1.0f, 2000.0f, g_set.fun_fish >= 300.0f ? "x%.0f (insanity)" : "x%.0f",
                ImGuiSliderFlags_Logarithmic);
            dirty |= ImGui::Checkbox("Flying fish (the fish's copies take to the air)", &g_set.fun_flying_fish);
            ImGui::TextDisabled("The zone's own creatures (birds, butterflies, fish): those above the camera count as birds, those below as fish.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Camera"))
        {
            int places = 0, recording = 0;
            d3d8_cam_settings(NULL, NULL, NULL, &places, &recording);
            ImGui::TextDisabled("A flight through the zone, in place of the game's camera.");
            if (ImGui::Button(recording ? "Stop recording" : "Record", ImVec2(130, 0)))
                d3d8_cam_command(recording ? "cam stop" : "cam record");
            ImGui::SameLine();
            if (ImGui::Button("Add this view", ImVec2(130, 0)))
                d3d8_cam_command("cam add");
            ImGui::TextDisabled(recording ? "Recording: walk or fly (!fly) the way; the camera's way is kept."
                                          : "Record keeps the camera's way as you move; Add, one view at a time.");
            ImGui::Text("%d places on the path", places);
            ImGui::BeginDisabled(places < 2 || recording);
            if (ImGui::Button("Play", ImVec2(84, 0)))
                d3d8_cam_command("cam play");
            ImGui::SameLine();
            if (ImGui::Button("Loop", ImVec2(84, 0)))
                d3d8_cam_command("cam loop");
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Clear", ImVec2(84, 0)))
                d3d8_cam_command("cam clear");
            if (ImGui::Button("Tour round what you look at", ImVec2(-1, 0)))
                d3d8_cam_command("cam tour");
            ImGui::SetNextItemWidth(-110);
            dirty |= ImGui::SliderFloat("Pace", &g_set.cam_pace, 0.1f, 2.0f, "%.2f");
            ImGui::SetNextItemWidth(-110);
            dirty |= ImGui::SliderFloat("Smoothing", &g_set.cam_smooth, 0.0f, 4.0f, "%.1f s");
            dirty |= ImGui::Checkbox("Hide the interfaces while flying", &g_set.cam_hide);
            dirty |= ImGui::Checkbox("Black bars, as a film", &g_set.cam_bars);
            static char name[48] = "zitah";
            ImGui::SetNextItemWidth(140);
            ImGui::InputText("##camname", name, sizeof name, ImGuiInputTextFlags_CharsNoBlank);
            ImGui::SameLine();
            char cmd[96];
            if (ImGui::Button("Save"))
                snprintf(cmd, sizeof cmd, "cam save %s", name), d3d8_cam_command(cmd);
            ImGui::SameLine();
            if (ImGui::Button("Load"))
                snprintf(cmd, sizeof cmd, "cam load %s", name), d3d8_cam_command(cmd);
            ImGui::TextDisabled("Esc ends a flight. Also //xi cam record | stop | add | play | loop | tour | save <name> | load <name>.");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    if (dirty)
        ImGui::MarkIniSettingsDirty();
    ImGui::PopTextWrapPos();
    ImGui::End();
}

/* The overlay's own window: which windows show, text sizes, and what the host knows */
static void overlay_window(void)
{
    ImGui::SetNextWindowPos(ImVec2(24, 24), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(280, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Overlay", &g_set.settings_open, ImGuiWindowFlags_AlwaysAutoResize))
    {
        bool dirty = false;
        if (ImGui::Button(g_edit ? "Lock the GUI" : "Edit GUI", ImVec2(120, 0)))
            g_edit = !g_edit, ImGui::MarkIniSettingsDirty();
        ImGui::SameLine();
        if (ImGui::Button("Graphics...", ImVec2(120, 0)))
            g_gfx_open = true;
        ImGui::TextDisabled(g_edit ? "Move and size the windows; then lock them" : "The windows are locked where they are");
        ImGui::Separator();
        ImGui::TextDisabled("Windows");
        dirty |= ImGui::Checkbox("Chat", &g_set.chat);
        ImGui::SameLine(100);
        dirty |= ImGui::Checkbox("Party", &g_set.party);
        ImGui::SameLine(190);
        dirty |= ImGui::Checkbox("Map", &g_set.map);
        dirty |= ImGui::Checkbox("Target", &g_set.target);
        ImGui::SameLine(100);
        dirty |= ImGui::Checkbox("Equipment", &g_set.equip);
        ImGui::SameLine(190);
        dirty |= ImGui::Checkbox("Items", &g_set.items);
        dirty |= ImGui::Checkbox("Performance", &g_set.status);
        if (g_plates_available)
        {
            ImGui::Separator();
            ImGui::TextDisabled("Names over heads");
            dirty |= ImGui::Checkbox("Draw them in the overlay's font", &g_set.plates);
            if (g_set.plates)
            {
                ImGui::SetNextItemWidth(-60);
                dirty |= ImGui::SliderFloat("Size##plates", &g_set.plate_size, 9, 32, "%.0f");
                ImGui::SetNextItemWidth(-60);
                dirty |= font_combo("Font##plates", g_set.plate_font, sizeof g_set.plate_font);
                dirty |= ImGui::Checkbox("Outline", &g_set.plate_outline);
                dirty |= ImGui::Checkbox("Hidden behind walls", &g_set.plates_occlude);
            }
        }
        if (g_hide_game)
        {
            ImGui::Separator();
            ImGui::TextDisabled("The game's own");
            dirty |= ImGui::Checkbox("Hide its chat log (while Chat is on)", &g_set.hide_game_log);
            dirty |= ImGui::Checkbox("Hide its party list (while Party is on)", &g_set.hide_game_party);
            dirty |= ImGui::Checkbox("Hide its target box (while Target is on)", &g_set.hide_game_target);
        }
        ImGui::Separator();
        ImGui::TextDisabled("Text size");
        ImGui::SetNextItemWidth(-60);
        dirty |= ImGui::SliderFloat("Windows##size", &g_set.ui_size, 11, 24, "%.0f");
        ImGui::SetNextItemWidth(-60);
        dirty |= ImGui::SliderFloat("Chat##size", &g_set.chat_size, 10, 28, "%.0f");
        ImGui::SetNextItemWidth(-60);
        dirty |= font_combo("Font##ui", g_set.ui_font, sizeof g_set.ui_font);
        ImGui::Separator();
        ImGui::TextDisabled("Chat");
        dirty |= ImGui::Checkbox("Shrinks when quiet, as the game's log", &g_set.chat_shrink);
        if (g_set.chat_shrink)
        {
            ImGui::SetNextItemWidth(-60);
            dirty |= ImGui::SliderInt("Lines##quiet", &g_set.chat_quiet_lines, 1, 12);
            ImGui::SetNextItemWidth(-60);
            dirty |= ImGui::SliderFloat("After##quiet", &g_set.chat_quiet_secs, 3, 60, "%.0f seconds");
        }
        dirty |= ImGui::Checkbox("J jumps (/jump)", &g_set.space_jumps);
        ImGui::Separator();
        ImGui::TextDisabled("Knocked out");
        dirty |= ImGui::Checkbox("The screen goes grey, with the time and Return to Home Point", &g_set.death_screen);
        ImGui::Separator();
        ImGui::TextDisabled("Background (color and how solid)");
        const ImGuiColorEditFlags cf = ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf | ImGuiColorEditFlags_NoInputs;
        dirty |= ImGui::ColorEdit4("Windows##bg", g_set.win_bg, cf);
        ImGui::SameLine(190);
        dirty |= ImGui::ColorEdit4("Chat##bg", g_set.chat_bg, cf);
        if (ImGui::SmallButton("Defaults##bg"))
        {
            const float w[4] = { 0.102f, 0.118f, 0.180f, 0.88f }, c[4] = { 0.094f, 0.118f, 0.188f, 0.80f };
            memcpy(g_set.win_bg, w, sizeof w), memcpy(g_set.chat_bg, c, sizeof c), dirty = true;
        }
        ImGui::Separator();
        ImGui::TextDisabled("%s shows and hides the overlay", TOGGLE_NAME);
        if (dirty)
            ImGui::MarkIniSettingsDirty();
    }
    ImGui::End();
}

/* --- the bar: an icon for each window, and the launcher's settings ----------------------------------- */
enum BarIcon { BI_ITEMS, BI_EQUIP, BI_CHAT, BI_PARTY, BI_MAP, BI_TARGET, BI_QUESTS, BI_SETTINGS, BI_OVERLAY, BI_COUNT };

static void bar_icon(ImDrawList* dl, int kind, ImVec2 a, float h, ImU32 c)
{
    ImVec2 m(a.x + h * 0.5f, a.y + h * 0.5f);
    float t = ImMax(1.5f, h * 0.08f), r = h * 0.36f;
    switch (kind)
    {
    case BI_ITEMS: /* a bag: its body and a handle */
        dl->AddRectFilled(ImVec2(a.x + h * 0.2f, a.y + h * 0.38f), ImVec2(a.x + h * 0.8f, a.y + h * 0.86f), c, h * 0.12f);
        dl->PathArcTo(ImVec2(m.x, a.y + h * 0.38f), h * 0.17f, IM_PI, IM_PI * 2, 10);
        dl->PathStroke(c, 0, t);
        break;
    case BI_EQUIP: /* a sword: blade, guard, grip */
        dl->AddLine(ImVec2(a.x + h * 0.78f, a.y + h * 0.18f), ImVec2(a.x + h * 0.36f, a.y + h * 0.6f), c, t * 1.6f);
        dl->AddLine(ImVec2(a.x + h * 0.25f, a.y + h * 0.5f), ImVec2(a.x + h * 0.46f, a.y + h * 0.71f), c, t * 1.3f);
        dl->AddLine(ImVec2(a.x + h * 0.36f, a.y + h * 0.6f), ImVec2(a.x + h * 0.2f, a.y + h * 0.78f), c, t * 1.3f);
        break;
    case BI_CHAT: /* a speech bubble */
        dl->AddRectFilled(ImVec2(a.x + h * 0.15f, a.y + h * 0.2f), ImVec2(a.x + h * 0.85f, a.y + h * 0.66f), c, h * 0.14f);
        dl->AddTriangleFilled(ImVec2(a.x + h * 0.3f, a.y + h * 0.64f), ImVec2(a.x + h * 0.48f, a.y + h * 0.64f), ImVec2(a.x + h * 0.26f, a.y + h * 0.84f), c);
        break;
    case BI_PARTY: /* two people */
        for (int k = 0; k < 2; ++k)
        {
            float x = a.x + h * (k ? 0.64f : 0.36f);
            dl->AddCircleFilled(ImVec2(x, a.y + h * 0.34f), h * 0.12f, c, 12);
            dl->PathArcTo(ImVec2(x, a.y + h * 0.8f), h * 0.2f, IM_PI, IM_PI * 2, 10);
            dl->PathFillConvex(c);
        }
        break;
    case BI_MAP: /* a compass: its ring and needle */
        dl->AddCircle(m, r, c, 20, t);
        dl->AddTriangleFilled(ImVec2(m.x, m.y - r * 0.8f), ImVec2(m.x - r * 0.28f, m.y), ImVec2(m.x + r * 0.28f, m.y), IM_COL32(230, 80, 70, 255));
        dl->AddTriangleFilled(ImVec2(m.x, m.y + r * 0.8f), ImVec2(m.x - r * 0.28f, m.y), ImVec2(m.x + r * 0.28f, m.y), c);
        break;
    case BI_TARGET: /* a crosshair */
        dl->AddCircle(m, r * 0.75f, c, 20, t);
        dl->AddLine(ImVec2(m.x - r, m.y), ImVec2(m.x - r * 0.35f, m.y), c, t);
        dl->AddLine(ImVec2(m.x + r * 0.35f, m.y), ImVec2(m.x + r, m.y), c, t);
        dl->AddLine(ImVec2(m.x, m.y - r), ImVec2(m.x, m.y - r * 0.35f), c, t);
        dl->AddLine(ImVec2(m.x, m.y + r * 0.35f), ImVec2(m.x, m.y + r), c, t);
        break;
    case BI_QUESTS: /* a scroll */
        dl->AddRectFilled(ImVec2(a.x + h * 0.26f, a.y + h * 0.2f), ImVec2(a.x + h * 0.74f, a.y + h * 0.8f), c, h * 0.04f);
        dl->AddCircleFilled(ImVec2(a.x + h * 0.26f, a.y + h * 0.24f), h * 0.08f, c, 10);
        dl->AddCircleFilled(ImVec2(a.x + h * 0.74f, a.y + h * 0.76f), h * 0.08f, c, 10);
        for (int k = 0; k < 3; ++k)
            dl->AddLine(ImVec2(a.x + h * 0.34f, a.y + h * (0.36f + 0.13f * k)), ImVec2(a.x + h * 0.66f, a.y + h * (0.36f + 0.13f * k)),
                IM_COL32(40, 30, 20, 200), ImMax(1.0f, t * 0.7f));
        break;
    case BI_SETTINGS: /* a gear */
        for (int k = 0; k < 8; ++k)
        {
            float g = k * IM_PI / 4;
            dl->AddLine(ImVec2(m.x + cosf(g) * r * 0.55f, m.y + sinf(g) * r * 0.55f), ImVec2(m.x + cosf(g) * r * 1.0f, m.y + sinf(g) * r * 1.0f), c,
                t * 2.0f);
        }
        dl->AddCircleFilled(m, r * 0.68f, c, 20);
        dl->AddCircleFilled(m, r * 0.3f, IM_COL32(30, 30, 38, 255), 14);
        break;
    case BI_OVERLAY: /* sliders */
        for (int k = 0; k < 3; ++k)
        {
            float y = a.y + h * (0.28f + 0.22f * k), x = a.x + h * (k == 1 ? 0.62f : k ? 0.4f : 0.3f);
            dl->AddLine(ImVec2(a.x + h * 0.18f, y), ImVec2(a.x + h * 0.82f, y), c, t);
            dl->AddCircleFilled(ImVec2(x, y), h * 0.08f, c, 10);
        }
        break;
    default: break;
    }
}

static void bar_window(void)
{
    struct
    {
        int icon;
        const char* tip;
        bool* open; /* the window it shows and hides (NULL: an action) */
    } items[] = {
        { BI_ITEMS, "Items", &g_set.items },          { BI_EQUIP, "Equipment", &g_set.equip },
        { BI_CHAT, "Chat", &g_set.chat },             { BI_PARTY, "Party", &g_set.party },
        { BI_MAP, "Map", &g_set.map },                { BI_TARGET, "Target", &g_set.target },
        { BI_QUESTS, "Quests and missions (to come)", NULL }, { BI_SETTINGS, "Graphics settings", NULL },
        { BI_OVERLAY, "Overlay settings", &g_set.settings_open },
    };
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, 8), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0));
    ImGui::SetNextWindowBgAlpha(g_set.win_bg[3] * 0.8f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 4));
    if (ImGui::Begin("Bar", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar |
                                      (g_edit ? 0 : ImGuiWindowFlags_NoMove)))
    {
        edit_outline();
        float h = ImGui::GetFontSize() * 1.9f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (int i = 0; i < (int)(sizeof items / sizeof *items); ++i)
        {
            if (i)
                ImGui::SameLine(0, 4);
            ImGui::PushID(i);
            ImVec2 a = ImGui::GetCursorScreenPos();
            bool clicked = ImGui::InvisibleButton("icon", ImVec2(h, h));
            bool on = items[i].open && *items[i].open, hover = ImGui::IsItemHovered();
            bool future = items[i].icon == BI_QUESTS;
            dl->AddRectFilled(a, ImVec2(a.x + h, a.y + h), on ? IM_COL32(70, 110, 160, 200) : hover ? IM_COL32(70, 70, 85, 200) : IM_COL32(35, 35, 45, 160), 6);
            bar_icon(dl, items[i].icon, ImVec2(a.x + h * 0.08f, a.y + h * 0.08f), h * 0.84f,
                future ? IM_COL32(120, 120, 120, 200) : IM_COL32(235, 225, 200, 255));
            if (hover)
                ImGui::SetTooltip("%s", items[i].tip);
            if (clicked)
            {
                if (items[i].open)
                    *items[i].open = !*items[i].open, ImGui::MarkIniSettingsDirty();
                else if (items[i].icon == BI_SETTINGS && g_open_settings)
                    g_open_settings();
            }
            ImGui::PopID();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

/* What the host knows now: the frame rate and sizes */
static void status_window(void)
{
    ImGui::SetNextWindowPos(ImVec2(24, 240), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(240, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Performance", &g_set.status, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("%.0f FPS", g_present.fps);
        ImGui::Text("Frame  %d x %d", g_present.frame_w, g_present.frame_h);
        ImGui::Text("Screen %d x %d", g_present.screen_w, g_present.screen_h);
        ImGui::Text("MetalFX %s", g_present.metalfx ? "upscaling" : "off");
        ImGui::Text("%s", dsound_in_world() ? "In the world" : "Title and login screens");
    }
    ImGui::End();
}

/* --- the chat ------------------------------------------------------------------------------------ */
/* Lines from the game's own log (gamestate.c), by kind: the game's chat mode sorted into what a
 * player means by say, tell, NPC... Colors default to the game's; tabs show the kinds they choose,
 * and may ask for words. Both are kept in overlay.ini. */
enum Kind
{
    SAY, SHOUT, YELL, TELL, PARTY, LS1, LS2, EMOTE, NPC, BATTLE, SYSTEM, KINDS
};
static const char* const KIND_NAME[KINDS] = { "Say", "Shout", "Yell", "Tell", "Party", "Linkshell 1", "Linkshell 2",
                                              "Emote", "NPC", "Battle", "System" };
static const char* const KIND_KEY[KINDS] = { "say", "shout", "yell", "tell", "party", "ls1", "ls2", "emote", "npc", "battle", "system" };
/* the game's default chat colors */
static const ImU32 KIND_DEFAULT[KINDS] = {
    IM_COL32(255, 255, 255, 255), /* say */
    IM_COL32(255, 155, 100, 255), /* shout */
    IM_COL32(255, 120, 150, 255), /* yell */
    IM_COL32(255, 135, 215, 255), /* tell */
    IM_COL32(110, 215, 255, 255), /* party */
    IM_COL32(150, 255, 140, 255), /* linkshell 1 */
    IM_COL32(200, 255, 110, 255), /* linkshell 2 */
    IM_COL32(210, 180, 255, 255), /* emote */
    IM_COL32(215, 240, 215, 255), /* NPC: the say color, a little green, to stand apart */
    IM_COL32(230, 225, 180, 255), /* battle */
    IM_COL32(200, 200, 200, 255), /* system */
};
static ImU32 g_kind_col[KINDS];

/* the game's chat modes by kind: the player's own and others' of each (1-15), the linkshell 2 and
 * unity pairs, the NPC ones, and the battle range; the rest the system's */
static Kind kind_of(int mode)
{
    switch (mode)
    {
    case 1: case 9: return SAY;
    case 2: case 10: return SHOUT;
    case 3: case 11: return YELL;
    case 4: case 12: return TELL;
    case 5: case 13: return PARTY;
    case 6: case 14: return LS1;
    case 7: case 15: return EMOTE;
    case 213: case 214: return LS2;
    case 142: case 144: case 150: case 151: case 152: return NPC;
    default: return mode >= 20 && mode <= 120 ? BATTLE : SYSTEM;
    }
}

struct Tab
{
    char name[32];
    unsigned kinds;   /* a bit per Kind */
    char words[64];   /* shown only when the line has these (any case); empty: every line */
};
static const unsigned ALL_KINDS = (1u << KINDS) - 1;
static Tab g_tabs[16];
static int g_ntabs, g_tab_edit = -1;
static bool g_chat_defaults_done, g_chat_colors;

static void chat_defaults(void)
{
    if (g_chat_defaults_done)
        return;
    g_chat_defaults_done = true;
    for (int k = 0; k < KINDS; ++k)
        if (!g_kind_col[k])
            g_kind_col[k] = KIND_DEFAULT[k];
    if (g_ntabs)
        return;
    const struct { const char* name; unsigned kinds; } d[] = {
        { "All", ALL_KINDS },
        { "Chat", 1u << SAY | 1u << SHOUT | 1u << YELL | 1u << TELL | 1u << PARTY | 1u << LS1 | 1u << LS2 | 1u << EMOTE },
        { "NPC", 1u << NPC },
        { "Battle", 1u << BATTLE },
        { "System", 1u << SYSTEM },
    };
    for (const auto& t : d)
    {
        snprintf(g_tabs[g_ntabs].name, sizeof g_tabs[g_ntabs].name, "%s", t.name);
        g_tabs[g_ntabs].kinds = t.kinds;
        g_tabs[g_ntabs].words[0] = 0;
        ++g_ntabs;
    }
}

/* overlay.ini: [Chat][Settings] with color.<kind>=RRGGBB and tab=kinds|words|name lines */
static void* chat_ini_open(ImGuiContext*, ImGuiSettingsHandler*, const char*)
{
    g_ntabs = 0;
    return (void*)1;
}

static void chat_ini_line(ImGuiContext*, ImGuiSettingsHandler*, void*, const char* line)
{
    unsigned rgb, kinds;
    char key[32];
    if (sscanf(line, "color.%31[a-z0-9]=%x", key, &rgb) == 2 ||
        sscanf(line, "color.%31[a-z0-9]=%x", key, &rgb) == 2) /* the first test builds wrote color. */
    {
        for (int k = 0; k < KINDS; ++k)
            if (!strcmp(key, KIND_KEY[k]))
                g_kind_col[k] = IM_COL32(rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255, 255);
        if (!strcmp(key, "npc") && rgb == 0xFFFFFF)
            g_kind_col[NPC] = 0; /* the first builds' default (the say color): the new default */
    }
    else if (sscanf(line, "tab=%x|", &kinds) == 1 && g_ntabs < 16)
    {
        const char* w = strchr(line, '|') + 1;
        const char* n = strchr(w, '|');
        if (!n)
            return;
        Tab& t = g_tabs[g_ntabs++];
        t.kinds = kinds & ALL_KINDS;
        snprintf(t.words, sizeof t.words, "%.*s", (int)(n - w), w);
        snprintf(t.name, sizeof t.name, "%s", n + 1);
    }
}

static void chat_ini_write(ImGuiContext*, ImGuiSettingsHandler* h, ImGuiTextBuffer* out)
{
    chat_defaults();
    out->appendf("[%s][Settings]\n", h->TypeName);
    for (int k = 0; k < KINDS; ++k)
    {
        ImU32 c = g_kind_col[k];
        out->appendf("color.%s=%02x%02x%02x\n", KIND_KEY[k], c & 255, c >> 8 & 255, c >> 16 & 255);
    }
    for (int i = 0; i < g_ntabs; ++i)
        out->appendf("tab=%x|%s|%s\n", g_tabs[i].kinds, g_tabs[i].words, g_tabs[i].name);
    out->append("\n");
}

static void chat_register(void)
{
    ImGuiSettingsHandler h;
    h.TypeName = "Chat";
    h.TypeHash = ImHashStr("Chat");
    h.ReadOpenFn = chat_ini_open;
    h.ReadLineFn = chat_ini_line;
    h.WriteAllFn = chat_ini_write;
    ImGui::AddSettingsHandler(&h);
}

static bool has_words(const char* text, const char* words)
{
    if (!words[0])
        return true;
    /* any of the words, space-separated, in any case */
    char w[64];
    snprintf(w, sizeof w, "%s", words);
    for (char* tok = strtok(w, " "); tok; tok = strtok(NULL, " "))
        for (const char* p = text; *p; ++p)
            if (!strncasecmp(p, tok, strlen(tok)))
                return true;
    return false;
}

static void tab_editor(void)
{
    if (g_tab_edit < 0)
        return;
    ImGui::OpenPopup("Tab");
    if (ImGui::BeginPopupModal("Tab", NULL, ImGuiWindowFlags_AlwaysAutoResize))
    {
        Tab& t = g_tabs[g_tab_edit];
        ImGui::InputText("Name", t.name, sizeof t.name);
        ImGui::TextDisabled("Shows");
        for (int k = 0; k < KINDS; ++k)
        {
            bool on = t.kinds >> k & 1;
            if (k % 3)
                ImGui::SameLine(150.0f * (float)(k % 3));
            if (ImGui::Checkbox(KIND_NAME[k], &on))
                t.kinds = on ? t.kinds | 1u << k : t.kinds & ~(1u << k);
        }
        ImGui::InputText("Words", t.words, sizeof t.words);
        ImGui::TextDisabled("Only lines with one of these words (empty: every line)");
        ImGui::Separator();
        if (ImGui::Button("Done"))
        {
            g_tab_edit = -1;
            ImGui::MarkIniSettingsDirty();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (g_ntabs > 1 && ImGui::Button("Remove tab"))
        {
            memmove(g_tabs + g_tab_edit, g_tabs + g_tab_edit + 1, sizeof(Tab) * (size_t)(g_ntabs - g_tab_edit - 1));
            --g_ntabs;
            g_tab_edit = -1;
            ImGui::MarkIniSettingsDirty();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

static void color_editor(void)
{
    if (!g_chat_colors)
        return;
    ImGui::SetNextWindowSize(ImVec2(300, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Chat colors", &g_chat_colors))
    {
        for (int k = 0; k < KINDS; ++k)
        {
            ImVec4 c = ImGui::ColorConvertU32ToFloat4(g_kind_col[k]);
            if (ImGui::ColorEdit3(KIND_NAME[k], &c.x, ImGuiColorEditFlags_NoInputs))
            {
                g_kind_col[k] = ImGui::ColorConvertFloat4ToU32(c);
                ImGui::MarkIniSettingsDirty();
            }
        }
        if (ImGui::Button("The game's colors"))
        {
            for (int k = 0; k < KINDS; ++k)
                g_kind_col[k] = KIND_DEFAULT[k];
            ImGui::MarkIniSettingsDirty();
        }
    }
    ImGui::End();
}

/* A chat line, wrapped, with its auto-translate phrases' braces as the game shows them: the opening
 * one red, the closing one green */
static void phrase_line(const char* s)
{
    ImFont* f = ImGui::GetFont();
    float fs = ImGui::GetFontSize(), lh = ImGui::GetTextLineHeight();
    float width = ImMax(40.0f, ImGui::GetContentRegionAvail().x);
    ImVec2 o = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 base = ImGui::GetColorU32(ImGuiCol_Text), open = ImGui::GetColorU32(ImVec4(0.95f, 0.3f, 0.25f, 1)),
          close = ImGui::GetColorU32(ImVec4(0.35f, 0.85f, 0.35f, 1));
    float x = 0, y = 0;
    for (const char* p = s; *p;)
    {
        const char* w = p; /* a word and the spaces after it */
        while (*p && *p != ' ')
            ++p;
        while (*p == ' ')
            ++p;
        const char* end = p;
        while (end > w && end[-1] == ' ')
            --end;
        if (x > 0 && x + f->CalcTextSizeA(fs, FLT_MAX, 0, w, end).x > width)
            x = 0, y += lh;
        for (const char* q = w; q < p;)
        {
            const char* e = q + 1;
            ImU32 c = *q == '{' ? open : *q == '}' ? close : base;
            if (*q != '{' && *q != '}')
                while (e < p && *e != '{' && *e != '}')
                    ++e;
            dl->AddText(f, fs, ImVec2(o.x + x, o.y + y), c, q, e);
            x += f->CalcTextSizeA(fs, FLT_MAX, 0, q, e).x;
            q = e;
        }
    }
    ImGui::Dummy(ImVec2(width, y + lh));
}

static void chat_lines(const Tab& t)
{
    float box = g_run_line ? ImGui::GetFrameHeightWithSpacing() : 0.0f;
    ImGui::BeginChild("lines", ImVec2(0, -box));
    ImGui::PushFont(NULL, g_set.chat_size / ImGui::GetStyle().FontScaleMain); /* its own size, whatever the windows' */
    int mode, n = 0;
    const char *sender, *text;
    while (gamestate_chat(n, &mode, &sender, &text))
        ++n;
    for (int i = n - 1; i >= 0; --i)
        if (gamestate_chat(i, &mode, &sender, &text))
        {
            Kind k = kind_of(mode);
            if (!(t.kinds >> k & 1) || !has_words(text, t.words))
                continue;
            if (k == NPC)
            {
                /* a small speech bubble before an NPC's line */
                float h = ImGui::GetTextLineHeight();
                ImVec2 a = ImGui::GetCursorScreenPos();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImU32 c = IM_COL32(110, 220, 120, 230);
                ImVec2 b0(a.x + 1, a.y + h * 0.18f), b1(a.x + h * 0.95f, a.y + h * 0.72f);
                dl->AddRectFilled(b0, b1, c, h * 0.2f);
                dl->AddTriangleFilled(ImVec2(b0.x + h * 0.2f, b1.y - 1), ImVec2(b0.x + h * 0.45f, b1.y - 1), ImVec2(b0.x + h * 0.15f, a.y + h * 0.95f), c);
                ImGui::Dummy(ImVec2(h * 1.05f, h));
                ImGui::SameLine(0, 2);
            }
            ImGui::PushStyleColor(ImGuiCol_Text, g_kind_col[k]);
            if (strchr(text, '{'))
            {
                char line[1200];
                snprintf(line, sizeof line, "%s%s%s", sender, sender[0] ? ": " : "", text);
                phrase_line(line);
            }
            else if (sender[0])
                ImGui::TextWrapped("%s: %s", sender, text);
            else
                ImGui::TextWrapped("%s", text);
            ImGui::PopStyleColor();
        }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f)
        ImGui::SetScrollHereY(1.0f);
    ImGui::PopFont();
    ImGui::EndChild();
}

/* The chat box: a line to the game, as if typed in its input line. Chat goes to the channel chosen
 * beside it; a line that starts with / is the game's command as it is (/tell, /equip, /ma...). */
static const struct
{
    const char *name, *prefix;
} SEND_TO[] = {
    { "Say", "/s " }, { "Party", "/p " }, { "Linkshell 1", "/l " }, { "Linkshell 2", "/l2 " },
    { "Shout", "/sh " }, { "Yell", "/yell " }, { "Tell", "/t " }, { "Emote", "/em " },
};
static int g_send_to;
static char g_send[256];

/* "/cm l", "/chatmode ls2": the game's chat mode, which the box's own follows (the line still goes to
 * the game, so its mode is the same). The mode's names as the game takes them. */
static void follow_chat_mode(const char* t)
{
    if (strncmp(t, "/cm ", 4) && strncmp(t, "/chatmode ", 10))
        return;
    t = strchr(t, ' ');
    while (*t == ' ')
        ++t;
    char m[16] = "";
    size_t n = 0;
    for (; t[n] && t[n] != ' ' && n + 1 < sizeof m; ++n)
        m[n] = (char)tolower((unsigned char)t[n]);
    m[n] = 0;
    static const struct
    {
        const char* name;
        int to;
    } MODES[] = {
        { "s", 0 }, { "say", 0 }, { "p", 1 }, { "party", 1 }, { "l", 2 }, { "l1", 2 }, { "ls", 2 }, { "ls1", 2 },
        { "linkshell", 2 }, { "linkshell1", 2 }, { "l2", 3 }, { "ls2", 3 }, { "linkshell2", 3 }, { "sh", 4 },
        { "shout", 4 }, { "y", 5 }, { "yell", 5 },
    };
    for (const auto& e : MODES)
        if (!strcmp(m, e.name))
            g_send_to = e.to;
}
static bool g_send_refocus, g_send_fresh;

/* Auto-translate, as the game's Tab: the word before the cursor, and the dictionary's phrases for it
 * (those starting with it first) in a list over the box; Tab or the arrows move in it, Enter or a
 * click takes one, typing or Esc leaves it. A phrase taken shows in the box in braces ("{Lower}")
 * and goes to the game as its key between two 0xFD bytes, as the game's own does. */
static struct
{
    bool open;
    int n, sel, from, to; /* the matches; the chosen one; the word's place in the box */
    bool moved;           /* the chosen one changed: kept in view */
    uint32_t keys[200];
    const char* texts[200];
} g_at;
/* the phrases in the box: their text and key, so a brace pair goes out as the phrase chosen */
static struct
{
    char text[64];
    uint32_t key;
} g_at_used[16];
static int g_at_nused;

/* What was sent, newest last: Up in the box brings back the one before, Down the one after (and at
 * the end, what was being typed). Each keeps its phrases, so their braces still go as their keys. */
static struct
{
    char text[256];
    int nused;
    struct
    {
        char text[64];
        uint32_t key;
    } used[16];
} g_sent[32];
static int g_nsent, g_sent_at = -1; /* -1: not looking back */
static char g_draft[256];

static void remember_sent(const char* text)
{
    if (g_nsent && !strcmp(g_sent[g_nsent - 1].text, text))
        return; /* the same line again: once */
    if (g_nsent == (int)(sizeof g_sent / sizeof *g_sent))
        memmove(g_sent, g_sent + 1, sizeof g_sent - sizeof *g_sent), --g_nsent;
    snprintf(g_sent[g_nsent].text, sizeof g_sent[0].text, "%s", text);
    g_sent[g_nsent].nused = g_at_nused;
    memcpy(g_sent[g_nsent].used, g_at_used, sizeof g_at_used);
    ++g_nsent;
}

static void recall_sent(ImGuiInputTextCallbackData* d, bool older)
{
    if (!g_nsent)
        return;
    int at = g_sent_at;
    if (older)
    {
        if (at < 0)
            snprintf(g_draft, sizeof g_draft, "%s", d->Buf), at = g_nsent - 1;
        else if (at > 0)
            --at;
        else
            return;
    }
    else
    {
        if (at < 0)
            return;
        at = at + 1 < g_nsent ? at + 1 : -1;
    }
    g_sent_at = at;
    const char* text = at < 0 ? g_draft : g_sent[at].text;
    if (at >= 0)
    {
        g_at_nused = g_sent[at].nused;
        memcpy(g_at_used, g_sent[at].used, sizeof g_at_used);
    }
    d->DeleteChars(0, d->BufTextLen);
    d->InsertChars(0, text);
}

static int chat_box_callback(ImGuiInputTextCallbackData* d)
{
    if (d->EventFlag == ImGuiInputTextFlags_CallbackAlways)
    {
        /* the cursor at the end of what is there (a "/"), not all of it selected */
        if (g_send_fresh)
            d->CursorPos = d->SelectionStart = d->SelectionEnd = d->BufTextLen, g_send_fresh = false;
    }
    else if (d->EventFlag == ImGuiInputTextFlags_CallbackEdit)
        g_at.open = false;
    else if (d->EventFlag == ImGuiInputTextFlags_CallbackCompletion)
    {
        if (g_at.open && g_at.n)
        {
            g_at.sel = (g_at.sel + (ImGui::GetIO().KeyShift ? g_at.n - 1 : 1)) % g_at.n, g_at.moved = true;
            return 0;
        }
        int to = d->CursorPos, from = to;
        while (from > 0 && d->Buf[from - 1] != ' ' && d->Buf[from - 1] != '}' && d->Buf[from - 1] != '{')
            --from;
        char word[64];
        int len = to - from < (int)sizeof word - 1 ? to - from : (int)sizeof word - 1;
        memcpy(word, d->Buf + from, (size_t)len);
        word[len] = 0;
        g_at.n = len ? gamestate_autotranslate_find(word, g_at.keys, g_at.texts, 200) : 0;
        g_at.open = g_at.n > 0, g_at.sel = 0, g_at.from = from, g_at.to = to, g_at.moved = true;
    }
    else if (d->EventFlag == ImGuiInputTextFlags_CallbackHistory && g_at.open && g_at.n)
    {
        g_at.sel = (g_at.sel + (d->EventKey == ImGuiKey_UpArrow ? g_at.n - 1 : 1)) % g_at.n, g_at.moved = true;
    }
    else if (d->EventFlag == ImGuiInputTextFlags_CallbackHistory)
        recall_sent(d, d->EventKey == ImGuiKey_UpArrow);
    return 0;
}

/* the chosen phrase in place of the word it was found for */
static void take_phrase(int i)
{
    char text[64];
    snprintf(text, sizeof text, "%s", g_at.texts[i]);
    char rest[256];
    snprintf(rest, sizeof rest, "%s", g_send + (g_at.to <= (int)strlen(g_send) ? g_at.to : strlen(g_send)));
    g_send[g_at.from < (int)sizeof g_send ? g_at.from : 0] = 0;
    snprintf(g_send + strlen(g_send), sizeof g_send - strlen(g_send), "{%s} %s", text, rest[0] == ' ' ? rest + 1 : rest);
    if (g_at_nused == (int)(sizeof g_at_used / sizeof *g_at_used))
        memmove(g_at_used, g_at_used + 1, sizeof g_at_used - sizeof *g_at_used), --g_at_nused;
    snprintf(g_at_used[g_at_nused].text, sizeof g_at_used[0].text, "%s", text);
    g_at_used[g_at_nused++].key = g_at.keys[i];
    g_at.open = false;
    g_send_refocus = true; /* back in the box, the cursor at the end */
}

/* the line as the game takes it: each phrase in braces as its key between two 0xFD */
static void with_phrases(char* out, size_t n, const char* in)
{
    size_t o = 0;
    for (const char* t = in; *t && o + 7 < n;)
    {
        const char* close = *t == '{' ? strchr(t, '}') : NULL;
        if (close)
        {
            uint32_t key = 0;
            size_t len = (size_t)(close - t - 1);
            for (int k = g_at_nused - 1; k >= 0 && !key; --k)
                if (strlen(g_at_used[k].text) == len && !memcmp(g_at_used[k].text, t + 1, len))
                    key = g_at_used[k].key;
            if (!key && len && len < 64)
            {
                /* typed by hand: the phrase with exactly that text */
                char want[64];
                memcpy(want, t + 1, len), want[len] = 0;
                uint32_t keys[8];
                const char* texts[8];
                int m = gamestate_autotranslate_find(want, keys, texts, 8);
                for (int k = 0; k < m && !key; ++k)
                    if (!strcasecmp(texts[k], want))
                        key = keys[k];
            }
            if (key)
            {
                out[o++] = (char)0xFD;
                for (int b = 3; b >= 0; --b)
                    out[o++] = (char)(key >> (8 * b));
                out[o++] = (char)0xFD;
                t = close + 1;
                continue;
            }
        }
        out[o++] = *t++;
    }
    out[o] = 0;
}

/* Knocked out, the player cannot speak aloud: Say, Shout and Yell (by the box's choice or typed as
 * a /command) are held back; Party, Linkshell and Tell still go. */
static bool aloud(const char* typed)
{
    static const char* const ALOUD[] = { "/s ", "/say ", "/sh ", "/shout ", "/y ", "/yell " };
    for (const char* a : ALOUD)
        if (!strncasecmp(typed, a, strlen(a)))
            return true;
    return false;
}
static double g_chat_note_at = -10;

static void chat_box(void)
{
    if (!g_run_line)
        return;
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("Linkshell 2").x + ImGui::GetFrameHeight() + 12.0f);
    if (ImGui::BeginCombo("##to", SEND_TO[g_send_to].name))
    {
        for (int i = 0; i < (int)(sizeof SEND_TO / sizeof *SEND_TO); ++i)
            if (ImGui::Selectable(SEND_TO[i].name, i == g_send_to))
                g_send_to = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (g_send_open)
    {
        g_sent_at = -1;
        /* opened by the game's own keys: Space empty, "/" with it typed */
        if (g_send_open == 2)
            snprintf(g_send, sizeof g_send, "/");
        else if (g_send_open == 3)
            snprintf(g_send, sizeof g_send, "!");
        g_send_open = 0;
        g_send_refocus = true;
    }
    if (g_send_refocus)
    {
        ImGui::SetKeyboardFocusHere();
        g_send_refocus = false;
        g_send_fresh = true;
    }
    const char* hint = g_send_to == 6 ? "name, then the message" : "Space, Enter, / or ! to type; Tab: auto-translate; Enter sends";
    ImGui::PushItemFlag(ImGuiItemFlags_NoTabStop, true);
    bool entered = ImGui::InputTextWithHint("##send", hint, g_send, sizeof g_send,
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackAlways | ImGuiInputTextFlags_CallbackCompletion |
            ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackEdit,
        chat_box_callback);
    ImGui::PopItemFlag();
    ImVec2 box0 = ImGui::GetItemRectMin(), box1 = ImGui::GetItemRectMax();
    if (ImGui::GetTime() - g_chat_note_at < 5.0)
    {
        /* why the line did not go: over the box */
        const char* note = "Knocked out: no Say, Shout or Yell. Party, Linkshell and Tell still work.";
        ImVec2 ts = ImGui::CalcTextSize(note);
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        fg->AddRectFilled(ImVec2(box0.x, box0.y - ts.y - 8), ImVec2(box0.x + ts.x + 12, box0.y - 2), IM_COL32(90, 20, 20, 230), 4);
        fg->AddText(ImVec2(box0.x + 6, box0.y - ts.y - 5), IM_COL32(255, 220, 210, 255), note);
    }
    bool active = ImGui::IsItemActive();
    int clicked = -1;
    bool list_hovered = false;
    if (g_at.open && g_at.n)
    {
        /* the list, over the box */
        float row = ImGui::GetTextLineHeightWithSpacing();
        float h = row * (float)(g_at.n < 12 ? g_at.n : 12) + ImGui::GetStyle().WindowPadding.y * 2;
        ImGui::SetNextWindowPos(ImVec2(box0.x, box0.y - 2), ImGuiCond_Always, ImVec2(0, 1));
        ImGui::SetNextWindowSize(ImVec2(ImMax(260.0f, (box1.x - box0.x) * 0.6f), h), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.92f);
        if (ImGui::Begin("##autotranslate", NULL,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                    ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav))
        {
            ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
            list_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
            for (int i = 0; i < g_at.n; ++i)
            {
                ImGui::PushID(i);
                char label[80];
                snprintf(label, sizeof label, "{%s}", g_at.texts[i]);
                if (ImGui::Selectable(label, i == g_at.sel))
                    clicked = i;
                if (i == g_at.sel && g_at.moved)
                    ImGui::SetScrollHereY(0.5f), g_at.moved = false;
                ImGui::PopID();
            }
        }
        ImGui::End();
    }
    if (clicked >= 0)
    {
        take_phrase(clicked);
        return;
    }
    if (g_at.open && !active && !entered && !list_hovered)
        g_at.open = false; /* Esc, or the box left (not for a click in the list) */
    if (entered && g_at.open && g_at.n)
    {
        take_phrase(g_at.sel); /* Enter takes the phrase; the next Enter sends */
        return;
    }
    if (entered)
    {
        const char* t = g_send;
        while (*t == ' ')
            ++t;
        if (*t)
        {
            char typed[300], line[600];
            /* a /command, or a server's own ! command (a GM's, say), goes as it is: as if typed in
             * the game's own input line */
            snprintf(typed, sizeof typed, "%s%s", *t == '/' || *t == '!' ? "" : SEND_TO[g_send_to].prefix, t);
            if (aloud(typed) && gamestate_dead(NULL))
            {
                g_chat_note_at = ImGui::GetTime(); /* kept in the box, to send another way */
                g_send_refocus = true;
                return;
            }
            follow_chat_mode(typed);
            with_phrases(line, sizeof line, typed);
            g_run_line(line);
            remember_sent(t);
        }
        g_send[0] = 0; /* and the box closes, as the game's input line does */
        g_at_nused = 0, g_sent_at = -1;
    }
}

/* The chat: no frame, just its tabs, lines and box. Pinned, it keeps to the bottom left corner
 * (its top and right edges still size it); either way, while the game asks something in a window
 * that would be under it, it moves up out of the way, and back after. */
static void chat_window(void)
{
    chat_defaults();
    ImVec2 disp = ImGui::GetIO().DisplaySize;
    const float margin = 8.0f;
    ImGuiWindow* cw = ImGui::FindWindowByName("Chat");
    ImVec2 size = cw ? cw->SizeFull : ImVec2(560, 280);
    static bool moved;
    static ImVec2 home;
    auto under_question = [&](ImVec2 pos) {
        return g_asking && pos.x < g_ask1.x && pos.x + size.x > g_ask0.x && pos.y < g_ask1.y && pos.y + size.y > g_ask0.y;
    };
    /* a question joins the chat: the game's window put on top of it, its left edge with the chat's.
     * So does any other of the game's windows asking something where the chat is (its commands menu,
     * at the bottom left): the chat stays where it is and the game's window sits on it. */
    static bool placing;
    static char joined[9]; /* the window placed (it stays placed while it asks, though no longer under) */
    /* not for now: the game measures some of its windows from the bottom of the screen and some from
     * the top, which is not yet known from the window, so a question moved "above the chat" could land
     * at the top of the screen. Until it is, the chat fades while the game asks (below). */
    bool join = false;
    snprintf(joined, sizeof joined, "%s", join ? g_asker : "");
    if (join && g_place_focus && cw)
    {
        float qh = g_ask1.y - g_ask0.y;
        g_place_focus(cw->Pos.x / disp.x, ImMax(0.0f, cw->Pos.y - qh - 2.0f) / disp.y);
        placing = true;
    }
    else if (placing)
    {
        if (g_place_focus)
            g_place_focus(-1, -1);
        placing = false;
    }
    auto under_other = [&](ImVec2 pos) { return false && under_question(pos); }; /* likewise: where it is is not known */
    if (g_set.chat_pinned && !g_edit)
    {
        /* the bottom left corner, where the game's own log was */
        ImVec2 at(margin, disp.y - margin - size.y);
        float bottom = disp.y - margin;
        if (under_other(at))
            bottom = ImMax(size.y + margin, g_ask0.y - margin);
        ImGui::SetNextWindowPos(ImVec2(margin, bottom), ImGuiCond_Always, ImVec2(0, 1));
        moved = false;
    }
    else
    {
        ImGui::SetNextWindowPos(ImVec2(24, 220), ImGuiCond_FirstUseEver);
        if (cw && !moved && under_other(cw->Pos))
        {
            home = cw->Pos, moved = true;
            ImGui::SetNextWindowPos(ImVec2(cw->Pos.x, ImMax(margin, g_ask0.y - margin - size.y)), ImGuiCond_Always);
        }
        else if (moved && !g_asking)
        {
            ImGui::SetNextWindowPos(home, ImGuiCond_Always); /* back where the player had it */
            moved = false;
        }
    }
    ImGui::SetNextWindowSize(ImVec2(560, 280), ImGuiCond_FirstUseEver);
    {
        /* Quiet for a while (no new line, not typed in, not pointed at, the game not asking): down to a
         * few lines, the bottom where it is (pinned), as the game's log; back to the player's size when
         * a line comes. Its full height is the player's, taken whenever it is at it. */
        static float full_h, cur_h;
        static double last;
        static int last_n = -1;
        int n = 0, mode;
        const char *sender, *text;
        while (gamestate_chat(n, &mode, &sender, &text))
            ++n;
        double now = ImGui::GetTime();
        bool pointed = cw && ImGui::IsMouseHoveringRect(cw->Pos, ImVec2(cw->Pos.x + cw->Size.x, cw->Pos.y + cw->Size.y), false);
        if (n != last_n || ImGui::GetIO().WantTextInput || pointed || g_asker[0] || g_at.open)
            last = now, last_n = n;
        bool quiet = g_set.chat_shrink && cw && now - last > g_set.chat_quiet_secs;
        if (cw && !quiet && (cur_h <= 0 || fabsf(cur_h - full_h) < 1.0f))
            full_h = cur_h = cw->SizeFull.y; /* at the player's size: theirs to change */
        else if (cw)
        {
            float line = g_set.chat_size * 1.2f + ImGui::GetStyle().ItemSpacing.y;
            float small = ImGui::GetFrameHeightWithSpacing() * 2.0f + line * (float)g_set.chat_quiet_lines + 12.0f;
            float want = quiet ? ImMin(small, full_h) : full_h;
            cur_h += (want - cur_h) * ImMin(1.0f, ImGui::GetIO().DeltaTime * 10.0f);
            if (fabsf(want - cur_h) < 0.5f)
                cur_h = want;
            ImGui::SetNextWindowSize(ImVec2(cw->SizeFull.x, cur_h), ImGuiCond_Always);
            size.y = cur_h;
        }
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2(240, 120), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(g_set.chat_bg[0], g_set.chat_bg[1], g_set.chat_bg[2], g_set.chat_bg[3]));
    ImGuiWindowFlags flags = lockable(ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                                      (g_set.chat_pinned && !g_edit ? ImGuiWindowFlags_NoMove : 0));
    /* while the game asks, the rest of the overlay fades; the chat stays readable, all of it to its
     * End (an NPC's words are in it, the game's log being hidden, and it waits on Enter for the
     * next line), since it has moved out of the way */
    /* an NPC talking (rem4...) keeps it at full strength; anything else the game asks (a question, a
     * menu) and the chat all but goes, so nothing of the game's is hidden behind it (the mouse goes
     * through it then too) */
    bool talk = !strncmp(g_asker, "rem4", 4);
    bool over = cw && g_asking && cw->Pos.x < g_ask1.x && cw->Pos.x + size.x > g_ask0.x && cw->Pos.y < g_ask1.y && cw->Pos.y + size.y > g_ask0.y;
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_asker[0] && !talk && over ? 0.15f : 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    bool shown = ImGui::Begin("Chat", NULL, flags);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    if (g_edit && GImGui->MovingWindow == ImGui::GetCurrentWindow())
        g_set.chat_pinned = false; /* moved by hand: where the player puts it */
    if (shown)
    {
        edit_outline();
        if (ImGui::BeginTabBar("tabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll))
        {
            for (int i = 0; i < g_ntabs; ++i)
            {
                ImGui::PushID(i);
                bool open = ImGui::BeginTabItem(g_tabs[i].name);
                if (ImGui::BeginPopupContextItem("tab"))
                {
                    if (ImGui::MenuItem("Edit tab..."))
                        g_tab_edit = i;
                    if (ImGui::MenuItem("Colors..."))
                        g_chat_colors = true;
                    ImGui::EndPopup();
                }
                if (open)
                {
                    chat_lines(g_tabs[i]);
                    chat_box();
                    ImGui::EndTabItem();
                }
                ImGui::PopID();
            }
            /* the pin: held to the bottom right corner, or free to move */
            if (ImGui::TabItemButton("##pin", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip))
            {
                g_set.chat_pinned = !g_set.chat_pinned;
                ImGui::MarkIniSettingsDirty();
            }
            {
                ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
                float r = (b.y - a.y) * 0.22f;
                ImU32 col = g_set.chat_pinned ? IM_COL32(255, 210, 90, 255) : IM_COL32(190, 190, 190, 200);
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddCircleFilled(ImVec2(c.x, c.y - r * 0.6f), r, col, 12);                                  /* its head */
                dl->AddLine(ImVec2(c.x, c.y - r * 0.2f), ImVec2(c.x, c.y + r * 1.9f), col, ImMax(1.5f, r * 0.35f)); /* its point */
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(g_set.chat_pinned ? "Pinned to the bottom left: click to move it freely" : "Click to pin it to the bottom left");
            }
            if (g_ntabs < 16 && ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip))
            {
                Tab& t = g_tabs[g_ntabs];
                snprintf(t.name, sizeof t.name, "Tab %d", g_ntabs + 1);
                t.kinds = ALL_KINDS;
                t.words[0] = 0;
                g_tab_edit = g_ntabs++;
            }
            ImGui::EndTabBar();
        }
        tab_editor();
    }
    ImGui::End();
    ImGui::PopStyleVar();
    color_editor();
}

/* --- the party ----------------------------------------------------------------------------------- */
static const char* job_name(int j)
{
    static const char* const JOBS[] = { "", "WAR", "MNK", "WHM", "BLM", "RDM", "THF", "PLD", "DRK", "BST", "BRD", "RNG",
                                        "SAM", "NIN", "DRG", "SMN", "BLU", "COR", "PUP", "DNC", "SCH", "GEO", "RUN" };
    return j > 0 && j < (int)(sizeof JOBS / sizeof *JOBS) ? JOBS[j] : "";
}

/* a thin bar, drawn where the cursor is, the width it is given */
static void thin_bar(float fraction, ImU32 color, float width, float height)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetCursorScreenPos();
    fraction = fraction < 0 ? 0 : fraction > 1 ? 1 : fraction;
    dl->AddRectFilled(a, ImVec2(a.x + width, a.y + height), IM_COL32(20, 20, 26, 200), 1.5f);
    if (fraction > 0)
        dl->AddRectFilled(a, ImVec2(a.x + width * fraction, a.y + height), color, 1.5f);
    ImGui::Dummy(ImVec2(width, height));
}

/* The party the way the game lays it out, small: per member a name with HP, MP and TP, and thin
 * HP and MP bars under it. Job and zone on hover. */
static void party_window(void)
{
    GameMember m[18];
    int n = gamestate_members(m, 18);
    /* the player's own party first (the player at its head), then the alliance's others in turn */
    for (int i = 1; i < n; ++i)
        for (int j = i; j > 1 && m[j].party < m[j - 1].party; --j)
        {
            GameMember t = m[j];
            m[j] = m[j - 1], m[j - 1] = t;
        }
    ImGui::SetNextWindowPos(ImVec2(24, 520), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(160, 0), ImVec2(600, FLT_MAX));
    /* its width is the player's; its height always fits who is in it */
    if (ImGuiWindow* pw = ImGui::FindWindowByName("Party"))
        ImGui::SetNextWindowSize(ImVec2(pw->SizeFull.x, 0));
    else
        ImGui::SetNextWindowSize(ImVec2(230, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(g_set.win_bg[3] * 0.8f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 1));
    if (ImGui::Begin("Party", g_edit ? &g_set.party : NULL, lockable(ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse)))
    {
        edit_outline();
        if (!n)
            ImGui::TextDisabled("Party");
        uint16_t here = gamestate_zone();
        float w = ImGui::GetContentRegionAvail().x;
        for (int i = 0; i < n; ++i)
        {
            const GameMember& p = m[i];
            bool away = here && p.zone && p.zone != here;
            if (i && p.party != m[i - 1].party)
            {
                /* the alliance's other parties: a rule between them */
                ImGui::Dummy(ImVec2(0, 3));
                ImVec2 a = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, a.y), ImVec2(a.x + w, a.y), IM_COL32(255, 255, 255, 50));
                ImGui::Dummy(ImVec2(0, 3));
            }
            else if (i)
                ImGui::Dummy(ImVec2(0, p.party ? 0 : 2));
            if (p.party)
            {
                /* someone in the alliance's other parties: a line of their name and HP */
                ImGui::BeginGroup();
                ImGui::PushStyleColor(ImGuiCol_Text, away ? IM_COL32(140, 140, 140, 255) : IM_COL32(225, 225, 225, 255));
                ImGui::PushFont(NULL, ImGui::GetStyle().FontSizeBase * 0.9f);
                ImGui::TextUnformatted(p.name);
                ImGui::PopFont();
                ImGui::PopStyleColor();
                ImGui::SameLine(w * 0.5f);
                float hh = ImMax(3.0f, ImGui::GetFontSize() * 0.3f);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (ImGui::GetTextLineHeight() - hh) * 0.5f);
                thin_bar(p.hpp / 100.0f, p.hpp <= 25 ? IM_COL32(230, 80, 70, 255) : p.hpp <= 50 ? IM_COL32(230, 190, 70, 255) : IM_COL32(90, 200, 110, 255),
                    w * 0.5f, hh);
                ImGui::EndGroup();
                if (g_run_line && ImGui::IsItemClicked(ImGuiMouseButton_Left))
                {
                    char line[48];
                    snprintf(line, sizeof line, "/target %s", p.name);
                    g_run_line(line);
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s %d / %s %d  HP %u (%u%%)%s", job_name(p.mjob), p.mjob_lv, job_name(p.sjob), p.sjob_lv, p.hp, p.hpp,
                        away ? "  (in another area)" : "");
                continue;
            }
            ImGui::BeginGroup();
            {
                char label[32];
                snprintf(label, sizeof label, "%s%s", p.name[0] ? p.name : "You", p.leader ? " *" : "");
                name_with_marks(p.name[0] ? p.name : label, away ? IM_COL32(140, 140, 140, 255) : IM_COL32(255, 255, 255, 255));
                if (p.leader)
                {
                    ImGui::SameLine(0, 2);
                    ImGui::TextDisabled("*");
                }
            }
            /* HP, MP and TP, right-aligned on the name's line */
            char hp[16], mp[16], tp[16];
            snprintf(hp, sizeof hp, "%u", p.hp);
            snprintf(mp, sizeof mp, "%u", p.mp);
            snprintf(tp, sizeof tp, "%u", p.tp);
            float col = ImGui::CalcTextSize("00000").x;
            float x0 = ImGui::GetCursorPosX();
            ImGui::SameLine(x0 + w - col * 3 - 8);
            ImGui::TextColored(ImVec4(0.55f, 0.9f, 0.6f, 1), "%*s", 5, hp);
            ImGui::SameLine(x0 + w - col * 2 - 4);
            ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.85f, 1), "%*s", 5, mp);
            ImGui::SameLine(x0 + w - col);
            ImGui::TextColored(p.tp >= 1000 ? ImVec4(0.55f, 0.8f, 1, 1) : ImVec4(0.45f, 0.55f, 0.7f, 1), "%*s", 5, tp);
            float h = ImMax(3.0f, ImGui::GetFontSize() * 0.28f);
            thin_bar(p.hpp / 100.0f, p.hpp <= 25 ? IM_COL32(230, 80, 70, 255) : p.hpp <= 50 ? IM_COL32(230, 190, 70, 255) : IM_COL32(90, 200, 110, 255), w, h);
            thin_bar(p.mpp / 100.0f, IM_COL32(210, 110, 190, 255), w, h * 0.75f);
            ImGui::EndGroup();
            /* a click targets them, as the game's own party list does: the player's own command */
            if (g_run_line && p.name[0] && ImGui::IsItemClicked(ImGuiMouseButton_Left))
            {
                char line[48];
                snprintf(line, sizeof line, "/target %s", p.name);
                g_run_line(line);
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                if (p.mjob && p.sjob)
                    ImGui::Text("%s %d / %s %d", job_name(p.mjob), p.mjob_lv, job_name(p.sjob), p.sjob_lv);
                else if (p.mjob)
                    ImGui::Text("%s %d", job_name(p.mjob), p.mjob_lv);
                ImGui::Text("HP %u (%u%%)  MP %u (%u%%)  TP %u", p.hp, p.hpp, p.mp, p.mpp, p.tp);
                if (away)
                    ImGui::TextDisabled("In another area");
                ImGui::EndTooltip();
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

/* --- items: icons, and the equipment and item windows ------------------------------------------ */
/* Icons from the install's item DATs (itemdat.h), 32x32 each in one 1024x1024 texture, filled as
 * items are first shown. */
static ImTextureData* g_icons;
static std::unordered_map<uint16_t, int> g_icon_cell; /* item -> cell, -1 when it has none */
static int g_icon_next;

static bool item_icon(uint16_t item, ImVec2* uv0, ImVec2* uv1)
{
    auto found = g_icon_cell.find(item);
    int cell;
    if (found != g_icon_cell.end())
        cell = found->second;
    else
    {
        const ItemInfo* it = item_info(item);
        cell = -1;
        if (it && it->has_icon)
        {
            if (!g_icons)
            {
                g_icons = IM_NEW(ImTextureData)();
                g_icons->Create(ImTextureFormat_RGBA32, 1024, 1024);
                g_icons->UseColors = true;
                ImGui::RegisterUserTexture(g_icons);
            }
            if (g_icon_next >= 32 * 32)
            {
                g_icon_next = 0; /* full: start again (those shown again are filled again) */
                g_icon_cell.clear();
            }
            cell = g_icon_next++;
            int cx = cell % 32 * 32, cy = cell / 32 * 32;
            for (int y = 0; y < 32; ++y)
                memcpy(g_icons->GetPixelsAt(cx, cy + y), it->icon + y * 32 * 4, 32 * 4);
            if (g_icons->Status == ImTextureStatus_OK || g_icons->Status == ImTextureStatus_WantUpdates)
            {
                ImTextureRect r = { (unsigned short)cx, (unsigned short)cy, 32, 32 };
                g_icons->Updates.push_back(r);
                g_icons->UpdateRect = r;
                g_icons->SetStatus(ImTextureStatus_WantUpdates);
            }
        }
        g_icon_cell[item] = cell;
    }
    if (cell < 0)
        return false;
    float u = (cell % 32) / 32.0f, v = (cell / 32) / 32.0f;
    *uv0 = ImVec2(u, v), *uv1 = ImVec2(u + 1 / 32.0f, v + 1 / 32.0f);
    return true;
}

/* an item's icon as an item of the window, with its count; true when clicked */
static bool item_button(const char* id, uint16_t item, uint32_t count, float size, bool worn)
{
    ImVec2 uv0, uv1;
    ImGui::PushID(id);
    ImVec2 at = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton("item", ImVec2(size, size));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(at, ImVec2(at.x + size, at.y + size), IM_COL32(20, 22, 28, 200), 4);
    if (item && item_icon(item, &uv0, &uv1))
        dl->AddImage(g_icons->GetTexRef(), ImVec2(at.x + 2, at.y + 2), ImVec2(at.x + size - 2, at.y + size - 2), uv0, uv1);
    if (ImGui::IsItemHovered())
        dl->AddRect(at, ImVec2(at.x + size, at.y + size), IM_COL32(255, 255, 255, 120), 4);
    if (worn)
        dl->AddText(ImVec2(at.x + 3, at.y + 1), IM_COL32(120, 230, 255, 255), "E");
    if (count > 1)
    {
        char n[12];
        snprintf(n, sizeof n, "%u", count);
        ImVec2 ts = ImGui::CalcTextSize(n);
        dl->AddText(ImVec2(at.x + size - ts.x - 3 + 1, at.y + size - ts.y), IM_COL32(0, 0, 0, 220), n);
        dl->AddText(ImVec2(at.x + size - ts.x - 3, at.y + size - ts.y - 1), IM_COL32(255, 255, 255, 255), n);
    }
    ImGui::PopID();
    return clicked;
}

static void item_tooltip(uint16_t item)
{
    const ItemInfo* it = item_info(item);
    if (!it)
        return;
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(it->name);
    if (it->level)
        ImGui::TextDisabled("Lv. %u", it->level);
    if (it->desc[0])
    {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 20);
        ImGui::TextUnformatted(it->desc);
        ImGui::PopTextWrapPos();
    }
    ImGui::EndTooltip();
}

/* the game's names for its equipment slots, as /equip takes them */
static const char* const EQUIP_NAME[EQUIP_SLOTS] = { "main", "sub", "range", "ammo", "head", "body", "hands", "legs",
                                                     "feet", "neck", "waist", "ear1", "ear2", "ring1", "ring2", "back" };
static const char* const EQUIP_LABEL[EQUIP_SLOTS] = { "Main", "Sub", "Range", "Ammo", "Head", "Body", "Hands", "Legs",
                                                      "Feet", "Neck", "Waist", "Ear 1", "Ear 2", "Ring 1", "Ring 2", "Back" };
/* the bags equipment can be worn from: the inventory and the wardrobes */
static const int WEAR_BAGS[] = { 0, 8, 10, 11, 12, 13, 14, 15, 16 };

static void run_equip(int slot, const char* name)
{
    char line[128];
    if (name)
        snprintf(line, sizeof line, "/equip %s \"%s\"", EQUIP_NAME[slot], name);
    else
        snprintf(line, sizeof line, "/equip %s", EQUIP_NAME[slot]); /* nothing: takes it off */
    g_run_line(line);
}

/* the slot an item goes in when equipped from the bags: its first (a ring's or an earring's, the
 * first free of the pair) */
static int slot_for(const ItemInfo* it)
{
    if (!it || !(it->type == ITEM_WEAPON || it->type == ITEM_ARMOR) || !it->slots)
        return -1;
    int b, s;
    for (int k = 0; k < EQUIP_SLOTS; ++k)
        if (it->slots >> k & 1)
        {
            if ((k == 11 || k == 13) && (it->slots >> (k + 1) & 1) && gamestate_equipped(k, &b, &s))
                return k + 1; /* the first of the pair is taken: the second */
            return k;
        }
    return -1;
}

static void equipment_window(void)
{
    ImGui::SetNextWindowPos(ImVec2(900, 300), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Equipment", &g_set.equip, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    }
    /* the game's own order: weapons, then head, neck, ears; body, hands, rings; back, waist, legs, feet */
    static const int ORDER[EQUIP_SLOTS] = { 0, 1, 2, 3, 4, 9, 11, 12, 5, 6, 13, 14, 15, 10, 7, 8 };
    float size = ImGui::GetFontSize() * 2.6f;
    static int choosing = -1;
    ImGui::BeginGroup();
    for (int i = 0; i < EQUIP_SLOTS; ++i)
    {
        int slot = ORDER[i], bag, at;
        const GameSlot* s = gamestate_equipped(slot, &bag, &at) ? gamestate_slot(bag, at) : NULL;
        if (i % 4)
            ImGui::SameLine();
        if (item_button(EQUIP_LABEL[slot], s ? s->item : 0, 0, size, false) && g_run_line)
        {
            choosing = slot;
            ImGui::OpenPopup("choose");
        }
        if (ImGui::IsItemHovered())
        {
            if (s)
                item_tooltip(s->item);
            else
                ImGui::SetTooltip("%s: nothing", EQUIP_LABEL[slot]);
        }
    }
    ImGui::EndGroup();
    /* what the player is: jobs, HP, MP, TP, and the attributes with what gear adds */
    {
        const GameStats* st = gamestate_stats();
        uint32_t hp = 0, mp = 0, tp = 0;
        bool vit = gamestate_self_vitals(&hp, &mp, &tp) != 0;
        ImGui::SameLine();
        ImGui::BeginGroup();
        if (st->known)
        {
            if (st->sjob)
                ImGui::Text("%s %u / %s %u", job_name(st->mjob), st->mjob_lv, job_name(st->sjob), st->sjob_lv);
            else
                ImGui::Text("%s %u", job_name(st->mjob), st->mjob_lv);
            ImGui::TextColored(ImVec4(0.55f, 0.9f, 0.6f, 1), "HP %u / %d", vit ? hp : 0, st->hp_max);
            ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.85f, 1), "MP %u / %d", vit ? mp : 0, st->mp_max);
            ImGui::TextColored(ImVec4(0.55f, 0.8f, 1, 1), "TP %u", vit ? tp : 0);
            ImGui::Separator();
            static const char* const ATTR[7] = { "STR", "DEX", "VIT", "AGI", "INT", "MND", "CHR" };
            for (int i = 0; i < 7; ++i)
            {
                ImGui::Text("%s %3u", ATTR[i], st->base[i]);
                if (st->add[i])
                {
                    ImGui::SameLine();
                    ImGui::TextColored(st->add[i] > 0 ? ImVec4(0.5f, 0.85f, 1, 1) : ImVec4(1, 0.5f, 0.45f, 1), "%+d", st->add[i]);
                }
            }
            ImGui::Separator();
            ImGui::Text("Attack %d", st->attack);
            ImGui::Text("Defense %d", st->defense);
        }
        else
            ImGui::TextDisabled("Stats come\nas the server\nsends them");
        ImGui::EndGroup();
    }
    if (ImGui::BeginPopup("choose"))
    {
        /* what can go in the slot, from the inventory and the wardrobes */
        ImGui::TextDisabled("%s", choosing >= 0 ? EQUIP_LABEL[choosing] : "");
        ImGui::Separator();
        int shown = 0;
        for (int b : WEAR_BAGS)
            for (int k = 0; k < gamestate_bag_size(b) && k < BAG_SLOTS; ++k)
            {
                const GameSlot* s = gamestate_slot(b, k);
                const ItemInfo* it = s ? item_info(s->item) : NULL;
                if (!it || choosing < 0 || !(it->slots >> choosing & 1))
                    continue;
                ImVec2 uv0, uv1;
                ImGui::PushID(b * 100 + k);
                if (item_icon(s->item, &uv0, &uv1))
                {
                    ImGui::Image(g_icons->GetTexRef(), ImVec2(ImGui::GetFontSize() * 1.3f, ImGui::GetFontSize() * 1.3f), uv0, uv1);
                    ImGui::SameLine();
                }
                char label[96];
                snprintf(label, sizeof label, "%s%s", it->name, s->locked ? "  (worn)" : "");
                if (ImGui::Selectable(label))
                {
                    run_equip(choosing, it->name);
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::IsItemHovered())
                    item_tooltip(s->item);
                ImGui::PopID();
                ++shown;
            }
        if (!shown)
            ImGui::TextDisabled("Nothing for it in your bags");
        ImGui::Separator();
        if (ImGui::Selectable("Take it off"))
        {
            run_equip(choosing, NULL);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::End();
}

static void items_window(void)
{
    static const char* const BAG_NAME[BAGS] = { "Inventory", "Safe", "Storage", "Temporary", "Locker", "Satchel", "Sack", "Case",
                                                "Wardrobe", "Safe 2", "Wardrobe 2", "Wardrobe 3", "Wardrobe 4", "Wardrobe 5",
                                                "Wardrobe 6", "Wardrobe 7", "Wardrobe 8", "Recycle" };
    ImGui::SetNextWindowPos(ImVec2(900, 520), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(430, 360), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Items", &g_set.items))
    {
        ImGui::End();
        return;
    }
    static char find[48];
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##find", "Find an item", find, sizeof find);
    if (ImGui::BeginTabBar("bags", ImGuiTabBarFlags_FittingPolicyScroll))
    {
        for (int b = 0; b < BAGS; ++b)
        {
            int size = gamestate_bag_size(b);
            if (!size || b == 3 || b == 17)
                continue;
            int used = 0;
            for (int k = 1; k <= size && k < BAG_SLOTS; ++k)
                used += gamestate_slot(b, k) != NULL;
            char tab[40];
            snprintf(tab, sizeof tab, "%s %d/%d###bag%d", BAG_NAME[b], used, size, b);
            if (!ImGui::BeginTabItem(tab))
                continue;
            float cell = ImGui::GetFontSize() * 2.4f, gap = ImGui::GetStyle().ItemSpacing.x;
            int per_row = ImMax(1, (int)((ImGui::GetContentRegionAvail().x + gap) / (cell + gap)));
            int n = 0;
            ImGui::BeginChild("slots");
            for (int k = 1; k <= size && k < BAG_SLOTS; ++k) /* slot 0 is the gil's */
            {
                const GameSlot* s = gamestate_slot(b, k);
                if (!s)
                    continue;
                const ItemInfo* it = item_info(s->item);
                if (find[0] && (!it || !strcasestr(it->name, find)))
                    continue;
                if (n++ % per_row)
                    ImGui::SameLine();
                char id[16];
                snprintf(id, sizeof id, "%d.%d", b, k);
                ImGui::PushID(b * 100 + k); /* each item its own menu */
                item_button(id, s->item, s->count, cell, s->locked && (it && (it->type == ITEM_WEAPON || it->type == ITEM_ARMOR)));
                if (ImGui::IsItemHovered())
                    item_tooltip(s->item);
                /* right-click: what can be done with it, each the game's own command */
                if (it && g_run_line && ImGui::BeginPopupContextItem("do"))
                {
                    ImGui::TextDisabled("%s", it->name);
                    ImGui::Separator();
                    int slot = slot_for(it);
                    bool wearable = slot >= 0 && (b == 0 || b == 8 || b >= 10);
                    if (wearable && ImGui::MenuItem("Equip"))
                        run_equip(slot, it->name);
                    if (it->type == ITEM_USABLE && b == 0)
                    {
                        char line[128];
                        if (ImGui::MenuItem("Use"))
                        {
                            snprintf(line, sizeof line, "/item \"%s\" <me>", it->name);
                            g_run_line(line);
                        }
                        if (ImGui::MenuItem("Use on your target"))
                        {
                            snprintf(line, sizeof line, "/item \"%s\" <t>", it->name);
                            g_run_line(line);
                        }
                    }
                    if (!wearable && !(it->type == ITEM_USABLE && b == 0))
                        ImGui::TextDisabled("Nothing to do with it here");
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            if (!n)
                ImGui::TextDisabled(find[0] ? "Nothing by that name here" : "Empty");
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

/* --- the target ---------------------------------------------------------------------------------- */
/* The player's target, small: its name, HP, how far. The game's own target window stays. */
static void target_window(void)
{
    GameEntity t;
    int self = 0;
    bool have = gamestate_target(&t, &self) != 0;
    ImGui::SetNextWindowPos(ImVec2(700, 60), ImGuiCond_FirstUseEver);
    if (ImGuiWindow* tw = ImGui::FindWindowByName("Target"))
        ImGui::SetNextWindowSize(ImVec2(tw->SizeFull.x, 0));
    else
        ImGui::SetNextWindowSize(ImVec2(260, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(160, 0), ImVec2(700, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(g_set.win_bg[3] * (have ? 0.8f : 0.35f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 2));
    if (ImGui::Begin("Target", g_edit ? &g_set.target : NULL, lockable(ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse)))
    {
        edit_outline();
        if (!have)
            ImGui::TextDisabled("No target");
        else
        {
            ImU32 col = t.kind == ENTITY_PC ? IM_COL32(160, 190, 255, 255)
                      : !t.mob              ? IM_COL32(120, 225, 130, 255)
                      : t.claimed           ? IM_COL32(255, 110, 100, 255)
                                            : IM_COL32(240, 215, 120, 255);
            if (t.kind == ENTITY_PC && t.name[0])
                name_with_marks(t.name, col);
            else
            {
                ImGui::PushStyleColor(ImGuiCol_Text, col);
                ImGui::TextUnformatted(t.name[0] ? t.name : "(no name yet)");
                ImGui::PopStyleColor();
            }
            float mx, my, mz, f;
            char right[32];
            if (!self && gamestate_self(&mx, &my, &mz, &f))
                snprintf(right, sizeof right, "%u%%  %.1f", t.hpp, sqrtf((t.x - mx) * (t.x - mx) + (t.z - mz) * (t.z - mz)));
            else
                snprintf(right, sizeof right, "%u%%", t.hpp);
            float w = ImGui::GetContentRegionAvail().x;
            ImGui::SameLine(ImGui::GetCursorPosX() + w - ImGui::CalcTextSize(right).x);
            ImGui::TextDisabled("%s", right);
            float h = ImMax(4.0f, ImGui::GetFontSize() * 0.35f);
            thin_bar(t.hpp / 100.0f, t.hpp <= 25 ? IM_COL32(230, 80, 70, 255) : t.hpp <= 50 ? IM_COL32(230, 190, 70, 255) : IM_COL32(90, 200, 110, 255), w, h);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

/* --- the map's colors: papyrus by default, each one the player's to change ------------------------ */

static ImU32 mapcol(int k)
{
    return g_map_col[k] ? g_map_col[k] : MC_DEFAULT[k];
}

static void map_colors_window(void)
{
    if (!g_map_colors_open)
        return;
    ImGui::SetNextWindowSize(ImVec2(260, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Map colors", &g_map_colors_open, ImGuiWindowFlags_AlwaysAutoResize))
    {
        for (int k = 0; k < MC_COUNT; ++k)
        {
            ImVec4 c = ImGui::ColorConvertU32ToFloat4(mapcol(k));
            if (ImGui::ColorEdit4(MC_NAME[k], &c.x, ImGuiColorEditFlags_NoInputs | (k == MC_BACK ? ImGuiColorEditFlags_AlphaBar : ImGuiColorEditFlags_NoAlpha)))
            {
                g_map_col[k] = ImGui::ColorConvertFloat4ToU32(c);
                ImGui::MarkIniSettingsDirty();
            }
        }
        if (ImGui::Button("Papyrus (the defaults)"))
        {
            memcpy(g_map_col, MC_DEFAULT, sizeof g_map_col);
            ImGui::MarkIniSettingsDirty();
        }
        ImGui::SameLine();
        if (ImGui::Button("Night"))
        {
            static const ImU32 NIGHT[MC_COUNT] = { IM_COL32(150, 170, 190, 255), IM_COL32(12, 16, 22, 200), IM_COL32(200, 210, 230, 255),
                IM_COL32(210, 215, 225, 255), IM_COL32(255, 110, 90, 255), IM_COL32(255, 255, 255, 255), IM_COL32(90, 230, 255, 255),
                IM_COL32(150, 180, 255, 255), IM_COL32(110, 220, 120, 255), IM_COL32(235, 205, 95, 255), IM_COL32(240, 80, 70, 255),
                IM_COL32(255, 255, 255, 255) };
            memcpy(g_map_col, NIGHT, sizeof g_map_col);
            ImGui::MarkIniSettingsDirty();
        }
    }
    ImGui::End();
}

/* --- the zone map under the radar (zonemap.h): one texture, filled again on each new zone ------- */
static ImTextureData* g_map_tex;
static ImTextureData* g_art_tex; /* the game's own map of the zone, where there is one */
static ZoneMap g_map; /* its placement in the world (the pixels are the textures') */

/* a texture, made or filled again with these pixels */
static void fill_texture(ImTextureData*& tex, const uint8_t* px, int size)
{
    if (tex && tex->Width != size)
        tex = NULL; /* a size it was not made for: a new one (the old is left, once) */
    if (!tex)
    {
        tex = IM_NEW(ImTextureData)();
        tex->Create(ImTextureFormat_RGBA32, size, size);
        tex->UseColors = true;
        memcpy(tex->GetPixels(), px, (size_t)size * size * 4);
        ImGui::RegisterUserTexture(tex);
        return;
    }
    memcpy(tex->GetPixels(), px, (size_t)size * size * 4);
    if (tex->Status == ImTextureStatus_OK)
    {
        ImTextureRect all = { 0, 0, (unsigned short)size, (unsigned short)size };
        tex->Updates.resize(0);
        tex->Updates.push_back(all);
        tex->UpdateRect = all;
        tex->SetStatus(ImTextureStatus_WantUpdates);
    }
}

static void map_texture_update(void)
{
    ZoneMap zm;
    if (zonemap_take(gamestate_zone(), &zm))
    {
        fill_texture(g_map_tex, zm.rgba, zm.size);
        free(zm.rgba);
        zm.rgba = NULL;
        g_map = zm; /* art_scale 0 until the game's own map comes, if it does */
    }
    if (zonemap_take_art(gamestate_zone(), &zm))
    {
        fill_texture(g_art_tex, zm.art, 512);
        free(zm.art);
        g_map.art_scale = zm.art_scale, g_map.art_ox = zm.art_ox, g_map.art_oy = zm.art_oy;
    }
}

/* --- the map ------------------------------------------------------------------------------------- */
/* A radar of who is around (the server's updates, gamestate.c): the player in the middle, facing up
 * (or north up), a compass ring, a dot per player, NPC and monster; the name on hover. The mouse
 * wheel zooms. The zone's own map art under it is to come. */
static void map_window(void)
{
    ImGui::SetNextWindowPos(ImVec2(1200, 24), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(260, 280), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.0f);
    if (!ImGui::Begin("Map", g_edit ? &g_set.map : NULL,
            lockable(ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | (g_edit ? 0 : ImGuiWindowFlags_NoTitleBar))))
    {
        ImGui::End();
        return;
    }
    edit_outline();
    /* its opacity (the right-click menu): everything it draws from here faded, at the end */
    ImDrawList* fade_dl = ImGui::GetWindowDrawList();
    int fade_from = fade_dl->VtxBuffer.Size;
    float me_x, me_y, me_z, t;
    bool known = gamestate_self(&me_x, &me_y, &me_z, &t) != 0;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float side = ImMax(60.0f, ImMin(avail.x, avail.y));
    ImVec2 at = ImGui::GetCursorScreenPos();
    ImVec2 c(at.x + avail.x * 0.5f, at.y + side * 0.5f);
    float r = side * 0.5f - 2.0f;
    ImGui::InvisibleButton("radar", ImVec2(avail.x, side));
    bool hovered = ImGui::IsItemHovered();
    if (hovered && ImGui::GetIO().MouseWheel != 0.0f)
    {
        g_set.map_range = ImClamp(g_set.map_range * (ImGui::GetIO().MouseWheel > 0 ? 0.8f : 1.25f), 10.0f, 250.0f);
        ImGui::MarkIniSettingsDirty();
    }
    if (ImGui::BeginPopupContextItem("map"))
    {
        if (ImGui::MenuItem("North up", NULL, &g_set.map_north_up))
            ImGui::MarkIniSettingsDirty();
        if (ImGui::MenuItem("Names", NULL, &g_set.map_names))
            ImGui::MarkIniSettingsDirty();
        if (ImGui::MenuItem("The game's map", NULL, &g_set.map_art, g_art_tex && g_map.art_scale > 0))
            ImGui::MarkIniSettingsDirty();
        ImGui::Separator();
        ImGui::SetNextItemWidth(150);
        float pct = g_set.map_alpha * 100.0f;
        if (ImGui::SliderFloat("Opacity", &pct, 10.0f, 100.0f, "%.0f%%"))
            g_set.map_alpha = pct / 100.0f, ImGui::MarkIniSettingsDirty();
        if (ImGui::MenuItem("Colors..."))
            g_map_colors_open = true;
        ImGui::EndPopup();
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(c, r, mapcol(MC_BACK), 64);
    map_texture_update();
    bool have_map = known && g_map_tex && g_map.zone == gamestate_zone() && g_map.half > 0;
    bool art = have_map && g_set.map_art && g_art_tex && g_map.art_scale > 0;

    /* the world (x east, z north) to the radar: facing up, or north up. Facing 0 is east and grows
     * clockwise (64 south), so facing t looks along (cos t, -sin t). */
    float ct = cosf(t), st = sinf(t);
    float scale = r / g_set.map_range;
    auto to_screen = [&](float dx, float dz) {
        if (g_set.map_north_up)
            return ImVec2(c.x + dx * scale, c.y - dz * scale);
        float fwd = dx * ct - dz * st, right = -dx * st - dz * ct;
        return ImVec2(c.x + right * scale, c.y - fwd * scale);
    };

    /* the zone's map, a disc of it: a fan whose corners are the world points under them (the radar
     * is an affine view of the world, so the texture interpolates exactly) */
    if (have_map)
    {
        const int SEG = 72;
        float inv = 1.0f / scale;
        auto uv_at = [&](float sx, float sy) {
            float dx, dz;
            if (g_set.map_north_up)
                dx = sx * inv, dz = -sy * inv;
            else
            {
                float right = sx * inv, fwd = -sy * inv;
                dx = fwd * ct - right * st;
                dz = -fwd * st - right * ct;
            }
            float wx = me_x + dx, wz = me_z + dz;
            if (art) /* the game's map: its pixel for the point, of 512 */
                return ImVec2((g_map.art_ox + g_map.art_scale * wx) / 512.0f, (g_map.art_oy - g_map.art_scale * wz) / 512.0f);
            return ImVec2((wx - (g_map.cx - g_map.half)) / (2.0f * g_map.half), ((g_map.cz + g_map.half) - wz) / (2.0f * g_map.half));
        };
        ImU32 tint = art ? IM_COL32_WHITE : mapcol(MC_GROUND); /* the game's art as it is; ours tinted */
        dl->PushTexture(art ? g_art_tex->GetTexRef() : g_map_tex->GetTexRef());
        dl->PrimReserve(SEG * 3, SEG + 1);
        ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
        dl->PrimWriteVtx(c, uv_at(0, 0), tint);
        for (int i = 0; i < SEG; ++i)
        {
            float a = (float)i / SEG * IM_PI * 2.0f;
            float sx = cosf(a) * r, sy = sinf(a) * r;
            dl->PrimWriteVtx(ImVec2(c.x + sx, c.y + sy), uv_at(sx, sy), tint);
        }
        for (int i = 0; i < SEG; ++i)
        {
            dl->PrimWriteIdx(base);
            dl->PrimWriteIdx((ImDrawIdx)(base + 1 + i));
            dl->PrimWriteIdx((ImDrawIdx)(base + 1 + (i + 1) % SEG));
        }
        dl->PopTexture();
    }

    {
        ImU32 rim = mapcol(MC_RING);
        dl->AddCircle(c, r * 0.5f, (rim & 0x00FFFFFFu) | 0x30000000u, 48);
        dl->AddCircle(c, r, rim, 64, 2.0f);
    }

    /* the compass ring's letters */
    static const struct { const char* l; float dx, dz; } DIRS[] = { { "N", 0, 1 }, { "E", 1, 0 }, { "S", 0, -1 }, { "W", -1, 0 } };
    for (const auto& d : DIRS)
    {
        ImVec2 p = to_screen(d.dx * g_set.map_range * 0.9f, d.dz * g_set.map_range * 0.9f);
        ImVec2 ts = ImGui::CalcTextSize(d.l);
        dl->AddText(ImVec2(p.x - ts.x * 0.5f, p.y - ts.y * 0.5f), d.l[0] == 'N' ? mapcol(MC_NORTH) : mapcol(MC_COMPASS), d.l);
    }

    static GameEntity ents[0x900];
    int n = known ? gamestate_entities(ents, 0x900) : 0;
    GameMember party[18];
    int np = gamestate_members(party, 18);
    GameEntity target;
    int target_self = 0;
    bool has_target = gamestate_target(&target, &target_self) && !target_self;
    const GameEntity* near_one = NULL;
    float near_d = 64.0f;
    ImVec2 mouse = ImGui::GetIO().MousePos;
    dl->PushClipRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), true);
    for (int i = 0; i < n; ++i)
    {
        const GameEntity& e = ents[i];
        if (e.hidden)
            continue; /* what the game does not draw: triggers, doors, markers, cutscene actors */
        float dx = e.x - me_x, dz = e.z - me_z;
        if (dx * dx + dz * dz > g_set.map_range * g_set.map_range)
            continue;
        bool in_party = false;
        for (int k = 0; k < np && !in_party; ++k)
            in_party = party[k].id == e.id;
        /* the game's own name colors: players white-blue, NPCs green, monsters yellow, claimed red */
        ImU32 col = e.kind == ENTITY_PC ? (in_party ? mapcol(MC_PARTY) : mapcol(MC_PC))
                  : !e.mob              ? mapcol(MC_NPC)
                  : e.hpp == 0          ? IM_COL32(120, 120, 120, 200)
                  : e.claimed           ? mapcol(MC_CLAIMED)
                                        : mapcol(MC_MOB);
        ImVec2 p = to_screen(dx, dz);
        dl->AddCircleFilled(p, e.kind == ENTITY_PC ? 3.5f : 3.0f, col, 10);
        dl->AddCircle(p, e.kind == ENTITY_PC ? 3.5f : 3.0f, IM_COL32(0, 0, 0, 90), 10, 1.0f); /* reads on light ground and dark */
        if (has_target && e.id == target.id)
            dl->AddCircle(p, 6.5f, mapcol(MC_TARGET), 16, 1.8f); /* the player's target */
        if (g_set.map_names && e.name[0])
            dl->AddText(ImVec2(p.x + 5, p.y - ImGui::GetFontSize() * 0.5f), mapcol(MC_COMPASS), e.name);
        float md = (p.x - mouse.x) * (p.x - mouse.x) + (p.y - mouse.y) * (p.y - mouse.y);
        if (hovered && md < near_d)
            near_d = md, near_one = &e;
    }
    dl->PopClipRect();

    /* the player: an arrow the way they face */
    {
        float a = g_set.map_north_up ? t : 0.0f; /* screen angle of facing: up is facing when not north up */
        ImVec2 fwd = g_set.map_north_up ? ImVec2(cosf(a), sinf(a)) : ImVec2(0, -1); /* east is +x; t grows clockwise = +y down */
        ImVec2 side_v(-fwd.y, fwd.x);
        float L = 7.0f;
        ImVec2 tip(c.x + fwd.x * L, c.y + fwd.y * L);
        ImVec2 b1(c.x - fwd.x * L * 0.6f + side_v.x * L * 0.6f, c.y - fwd.y * L * 0.6f + side_v.y * L * 0.6f);
        ImVec2 b2(c.x - fwd.x * L * 0.6f - side_v.x * L * 0.6f, c.y - fwd.y * L * 0.6f - side_v.y * L * 0.6f);
        dl->AddTriangleFilled(tip, b1, b2, mapcol(MC_SELF));
        dl->AddTriangle(tip, b1, b2, IM_COL32(255, 255, 255, 120), 1.0f);
    }

    char range[32];
    snprintf(range, sizeof range, "%.0f yalms", g_set.map_range);
    dl->AddText(ImVec2(at.x + 2, at.y + side - ImGui::GetFontSize()), IM_COL32(200, 200, 200, 150), range);
    if (!known)
        dl->AddText(ImVec2(c.x - ImGui::CalcTextSize("Waiting for the world").x * 0.5f, c.y + 12), IM_COL32(200, 200, 200, 180), "Waiting for the world");

    if (near_one)
    {
        float dx = near_one->x - me_x, dz = near_one->z - me_z;
        ImGui::BeginTooltip();
        ImGui::Text("%s", near_one->name[0] ? near_one->name : "(no name yet)");
        ImGui::TextDisabled("%.1f yalms, HP %u%%", sqrtf(dx * dx + dz * dz), near_one->hpp);
        ImGui::EndTooltip();
    }
    if (g_set.map_alpha < 1.0f)
        for (int i = fade_from; i < fade_dl->VtxBuffer.Size; ++i)
        {
            ImU32& c = fade_dl->VtxBuffer[i].col;
            c = (c & ~IM_COL32_A_MASK) | ((ImU32)((float)(c >> IM_COL32_A_SHIFT & 255) * g_set.map_alpha) << IM_COL32_A_SHIFT);
        }
    ImGui::End();
}

/* --- Knocked out ------------------------------------------------------------------------------------
 * The game's picture goes grey (the present's, gfx_set_grey; the overlay keeps its colors), and over
 * it the time until the game sends the player home itself, and two buttons: Return to Home Point (the game's own menus answered for them: its "dead" window,
 * then its Yes/No, by the keys the player would press) and Wait for a Raise (down to a strip at the
 * top). Any other window of the game's asking (a Raise offered)
 * and it steps back to the strip, so the game's question is answered as ever. */
extern "C" void gfx_set_grey(float amount);
static int (*g_focus_cursor)(void); /* host64: the cursor in the game's window with the keyboard (1 the first choice), -1 none */
extern "C" void overlay_set_focus_cursor(int (*cursor)(void)) { g_focus_cursor = cursor; }
static void (*g_hide_death_menu)(int on); /* host64: the game's own death menu (its time left) not drawn */
extern "C" void overlay_set_death_menu_hider(void (*hide)(int on)) { g_hide_death_menu = hide; }


/* one key at a time, as the player presses it: down, held a few frames (the game reads the
 * keyboard's state each frame), up */
static struct
{
    SDL_Keycode key;
    SDL_Scancode sc;
    int frames; /* 0 none */
} g_press;

static void press_key(SDL_Keycode key, SDL_Scancode sc)
{
    if (!g_press.frames)
        g_press.key = key, g_press.sc = sc, g_press.frames = 1;
}

static void press_tick(void)
{
    if (!g_press.frames)
        return;
    int f = g_press.frames++;
    if (f == 1 || f == 6)
    {
        SDL_Event e = {};
        e.type = f == 1 ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        e.key.scancode = g_press.sc;
        e.key.key = g_press.key;
        e.key.down = f == 1;
        SDL_PushEvent(&e);
    }
    if (f >= 12)
        g_press.frames = 0; /* and a few frames for the game to act on it */
}

static bool focus_is(const char* f, const char* name) { return !strncmp(f, name, strlen(name)); }

/* Return to Home Point, a frame at a time: Enter on the game's "dead" window opens its question
 * ("comyn": Yes and No side by side, its cursor at +0x4C 1 Yes, 2 No; it opens on the last answer
 * given), Left until it is on Yes, then Enter */
static void ko_return_tick(const char* f)
{
    if (!g_ko.step || g_press.frames)
        return;
    double now = ImGui::GetTime();
    if (now - g_ko.step_at > 4.0)
    {
        g_ko.step = 0, g_ko.note = "The game's menu did not answer: use its own (Enter).", g_ko.note_at = now;
        return;
    }
    if (g_ko.step == 1)
    {
        if (focus_is(f, "comyn"))
            g_ko.step = 3, g_ko.step_at = now;
        else if (focus_is(f, "dead"))
            press_key(SDLK_RETURN, SDL_SCANCODE_RETURN), g_ko.step = 2, g_ko.step_at = now;
        else
            g_ko.step = 0, g_ko.note = "The game's death menu is closed.", g_ko.note_at = now;
    }
    else if (g_ko.step == 2)
    {
        if (focus_is(f, "comyn"))
            g_ko.step = 3, g_ko.step_at = now;
    }
    else if (g_ko.step == 3)
    {
        if (!focus_is(f, "comyn"))
        {
            g_ko.step = 0;
            return;
        }
        if (now - g_ko.step_at < 0.3)
            return; /* a moment to open */
        int at = g_focus_cursor ? g_focus_cursor() : -1;
        if (at == 2)
            press_key(SDLK_LEFT, SDL_SCANCODE_LEFT);
        else if (at == 1)
            press_key(SDLK_RETURN, SDL_SCANCODE_RETURN), g_ko.step = 0;
        else
            g_ko.step = 0, g_ko.note = "Choose Yes in the game's question.", g_ko.note_at = now; /* not knowing where it is */
    }
}

/* each frame: whether knocked out (the screen's state, the grey), the presses Return makes */
static void ko_tick(const char* focus)
{
    double home;
    bool dead = gamestate_dead(&home) != 0 || getenv("XI_DEATH_PREVIEW");
    bool on = dead && ui_on() && g_set.death_screen;
    if (on && !g_ko.on)
        g_ko.waiting = false, g_ko.step = 0, g_ko.note = NULL;
    g_ko.on = on;
    float want = on ? 1.0f : 0.0f, dt = ImGui::GetIO().DeltaTime;
    g_ko.grey += (want - g_ko.grey) * ImMin(1.0f, dt * (on ? 1.2f : 3.0f));
    if (fabsf(want - g_ko.grey) < 0.01f)
        g_ko.grey = want;
    gfx_set_grey(g_ko.grey);
    press_tick();
    if (g_hide_death_menu)
        g_hide_death_menu(on); /* the screen says what it says: its time left, its Return */
    if (on)
        ko_return_tick(focus);
    else
        g_ko.step = 0;
}

static bool g_ko_other_asks; /* knocked out, and another of the game's windows asks (a Raise offered) */

static void ko_window(bool game_asks_else)
{
    if (!g_ko.on)
        return;
    double home;
    gamestate_dead(&home);
    if (getenv("XI_DEATH_PREVIEW"))
        home = 3600.0 - ImGui::GetTime();
    ImVec2 d = ImGui::GetIO().DisplaySize;
    char time_left[64];
    if (home < 0)
        snprintf(time_left, sizeof time_left, "The way home opens soon");
    else
        snprintf(time_left, sizeof time_left, "Home Point in %d:%02d", (int)home / 60, (int)home % 60);
    const ImGuiWindowFlags fixed = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize;
    if (g_ko.waiting || game_asks_else)
    {
        /* a strip at the top: the time, the buttons */
        ImGui::SetNextWindowPos(ImVec2(d.x * 0.5f, 10), ImGuiCond_Always, ImVec2(0.5f, 0));
        ImGui::SetNextWindowBgAlpha(0.8f);
        if (ImGui::Begin("##knockedout", NULL, fixed))
        {
            ImGui::Text("Knocked out: waiting for a Raise");
            ImGui::SameLine();
            ImGui::TextDisabled("%s", time_left);
            if (!game_asks_else)
            {
                ImGui::SameLine();
                if (ImGui::SmallButton("Return to Home Point"))
                    g_ko.step = 1, g_ko.step_at = ImGui::GetTime(), g_ko.note = NULL;
                ImGui::SameLine();
                if (ImGui::SmallButton("Bigger"))
                    g_ko.waiting = false;
            }
        }
        ImGui::End();
        return;
    }
    /* the whole screen: dimmed, the words and the time (behind every window of the overlay's, so the
     * chat stays to hand), and the buttons in a window of their own */
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddRectFilled(ImVec2(0, 0), d, IM_COL32(0, 0, 0, 70));
    ImFont* f = ImGui::GetFont();
    float cx = d.x * 0.5f, y = d.y * 0.3f;
    auto centered = [&](float size, ImU32 col, const char* text) {
        ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0, text);
        dl->AddText(f, size, ImVec2(cx - ts.x * 0.5f + 2, y + 2), IM_COL32(0, 0, 0, 170), text);
        dl->AddText(f, size, ImVec2(cx - ts.x * 0.5f, y), col, text);
        y += ts.y;
    };
    centered(ImGui::GetFontSize() * 2.4f, IM_COL32(255, 255, 255, 255), "Knocked out");
    y += 6;
    centered(ImGui::GetFontSize() * 1.1f, IM_COL32(225, 225, 230, 255), "Waiting for a Raise...");
    y += 4;
    centered(ImGui::GetFontSize() * 1.1f, IM_COL32(255, 210, 120, 255), time_left);
    y += 18;
    ImGui::SetNextWindowPos(ImVec2(cx, y), ImGuiCond_Always, ImVec2(0.5f, 0));
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (ImGui::Begin("##knockedoutbuttons", NULL, fixed))
    {
        ImVec2 bs(ImMax(220.0f, d.y * 0.2f), ImGui::GetFrameHeight() * 1.8f);
        bool busy = g_ko.step != 0;
        ImGui::BeginDisabled(busy);
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(150, 60, 50, 230));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(185, 75, 60, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(120, 45, 40, 255));
        if (ImGui::Button(busy ? "Returning..." : "Return to Home Point", bs))
            g_ko.step = 1, g_ko.step_at = ImGui::GetTime(), g_ko.note = NULL;
        ImGui::PopStyleColor(3);
        ImGui::SameLine(0, 16);
        if (ImGui::Button("Wait for a Raise", bs))
            g_ko.waiting = true;
        ImGui::EndDisabled();
        const char* hint = g_ko.note && ImGui::GetTime() - g_ko.note_at < 6.0 ? g_ko.note
                                                                             : "Space, Enter, / or ! to chat. A controller works the game's own menu.";
        float w = ImGui::GetContentRegionAvail().x, hw = ImGui::CalcTextSize(hint).x;
        if (hw < w)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (w - hw) * 0.5f);
        ImGui::TextDisabled("%s", hint);
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

extern "C" void overlay_build_frame(void)
{
    ImGui::GetStyle().FontScaleMain = g_set.ui_size / 16.0f;
    ImGui::GetStyle().Colors[ImGuiCol_WindowBg] = ImVec4(g_set.win_bg[0], g_set.win_bg[1], g_set.win_bg[2], g_set.win_bg[3]);
    ImGui::GetIO().FontDefault = font_named(g_set.ui_font);
    /* One of the game's own windows asking something (a question, a menu): the overlay fades and
     * lets the mouse through, so the game's window is never hidden behind it. The log, its typing
     * line and nothing at all do not count. */
    {
        const char* f = g_game_focus ? g_game_focus() : "";
        static const char* const PASSIVE[] = { "logwin", "inline", "fulllog", "partywin", "netstat", "buff", "helpwind",
                                               "titlewin", "targetwi" };
        bool game_asks = f[0] && dsound_in_world();
        for (const char* p : PASSIVE)
            if (!strncmp(f, p, strlen(p)))
                game_asks = false;
        ko_tick(f);
        /* knocked out: the game's death menu (and its Yes/No, while Return answers it) is the
         * screen's own business, not a question to fade for; anything else asking (a Raise
         * offered) is, and the screen steps back to its strip */
        g_ko_other_asks = false;
        if (g_ko.on && game_asks)
        {
            if (!strncmp(f, "dead", 4) || (g_ko.step && !strncmp(f, "comyn", 5)))
                game_asks = false;
            else
                g_ko_other_asks = true;
        }
        {
            /* which of the game's windows take the keyboard, while this is learned */
            static char last[9];
            static int told;
            if (told < 400 && strncmp(last, f, 8))
            {
                ++told;
                snprintf(last, sizeof last, "%.8s", f);
                extern void rt_log(const char* fmt, ...);
                rt_log("[recomp] the game's keyboard: %s%s\n", f[0] ? last : "(none)", game_asks ? " (the overlay fades)" : "");
            }
        }
        {
            /* XI_TEST_ENTER=<window>: Enter pressed once, a second and a half after that window has
             * the keyboard; for unattended tests of the lobby (XI_TEST_ENTER=ptc8lice: its license
             * screen). Nothing when unset. */
            static const char* test_enter = getenv("XI_TEST_ENTER");
            static int test_frames;
            if (test_enter && test_frames < 100 && f[0] && !strncmp(f, test_enter, strlen(test_enter)))
            {
                if (++test_frames == 90 || test_frames == 96)
                {
                    SDL_Event e = {};
                    e.type = test_frames == 90 ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
                    e.key.scancode = SDL_SCANCODE_RETURN;
                    e.key.key = SDLK_RETURN;
                    e.key.down = test_frames == 90;
                    SDL_PushEvent(&e);
                }
            }
        }
        g_asking = false;
        snprintf(g_asker, sizeof g_asker, "%.8s", game_asks ? f : "");
        /* a question: yes/no windows (their names end yn, yesn, yesno), a choice (query), a notice (ok) */
        size_t fl = strnlen(f, 8);
        while (fl && f[fl - 1] == ' ')
            --fl;
        g_question = game_asks && ((fl >= 2 && !strncmp(f + fl - 2, "yn", 2)) || (fl >= 4 && !strncmp(f + fl - 4, "yesn", 4)) ||
                                   (fl >= 5 && !strncmp(f + fl - 5, "yesno", 5)) || !strncmp(f, "query", 5) || !strncmp(f, "ok  ", 4));
        float ax, ay, aw, ah;
        if (game_asks && g_focus_rect && g_focus_rect(&ax, &ay, &aw, &ah))
        {
            ImVec2 d = ImGui::GetIO().DisplaySize;
            g_ask0 = ImVec2(ax * d.x, ay * d.y), g_ask1 = ImVec2((ax + aw) * d.x, (ay + ah) * d.y);
            g_asking = true;
        }
        ImGui::GetStyle().Alpha = game_asks ? 0.3f : 1.0f;
        if (game_asks)
            ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouse;
        else
            ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
        /* the game's own typing line opened (by a key the overlay does not take): closed by the game's
         * own close, and the overlay's chat box opened instead */
        if (!strncmp(f, "inline", 6) && ui_on() && g_set.chat && g_set.hide_game_log && g_close_game && !ImGui::GetIO().WantTextInput)
        {
            g_close_game("inline  ");
            if (!g_send_open)
                g_send_open = 1; /* not over a box already opening with its "/" or "!" */
        }
    }
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    bool was[7] = { g_set.chat, g_set.party, g_set.map, g_set.status, g_set.target, g_set.equip, g_set.items };
    if (ui_on() && g_set.plates && !d3d8_cam_hides_ui())
        draw_nameplates();
    else
        g_nplates = 0;
    if (!dsound_in_world())
        g_edit = false;
    if (ui_on() && !d3d8_cam_hides_ui())
    {
        if (g_edit)
            edit_banner();
        if (g_set.bar)
            bar_window();
        if (g_set.settings_open || !g_set.bar)
            overlay_window();
        if (g_set.status)
            status_window();
        if (g_set.chat)
            chat_window();
        if (g_set.party)
            party_window();
        if (g_set.map)
            map_window();
        map_colors_window();
        if (g_gfx_open)
            graphics_window();
        if (g_set.target)
            target_window();
        if (g_set.equip)
            equipment_window();
        if (g_set.items)
            items_window();
        ko_window(g_ko_other_asks);
    }
    g_send_open = 0; /* not taken up by the chat box this frame: dropped, so no key stays caught */
    /* the game's own windows the overlay's stand in for: back whenever the overlay is hidden */
    if (g_place_log)
    {
        ImGuiWindow* cw = ui_on() && g_set.chat ? ImGui::FindWindowByName("Chat") : NULL;
        ImVec2 d = ImGui::GetIO().DisplaySize;
        if (cw && !cw->Hidden && d.x > 0 && d.y > 0)
            g_place_log(cw->Pos.x / d.x, cw->Pos.y / d.y, (cw->Pos.x + cw->Size.x) / d.x, (cw->Pos.y + cw->Size.y) / d.y);
        else
            g_place_log(-1, -1, -1, -1);
    }
    if (g_hide_game)
        g_hide_game(ui_on() && g_set.chat && g_set.hide_game_log, ui_on() && g_set.party && g_set.hide_game_party,
            ui_on() && g_set.target && g_set.hide_game_target);
    if (was[0] != g_set.chat || was[1] != g_set.party || was[2] != g_set.map || was[3] != g_set.status || was[4] != g_set.target ||
        was[5] != g_set.equip || was[6] != g_set.items)
        ImGui::MarkIniSettingsDirty(); /* a window closed with its x */
    d3d8_set_weather_effects(g_set.wx_rain, g_set.wx_fog, g_set.wx_heat, g_set.wx_lightning, g_set.wx_snow);
    d3d8_set_sky(g_set.sky_stars, g_set.sky_shooting, g_set.sky_bright);
    d3d8_set_water(g_set.water, g_set.water_v);
    d3d8_set_water_body(g_set.water_body);
    entity_looks();
    precipitation();
    {
        float pace = g_set.cam_pace, smooth = g_set.cam_smooth;
        int hide = g_set.cam_hide;
        d3d8_cam_settings(&pace, &smooth, &hide, NULL, NULL);
        if (d3d8_cam_playing() && g_set.cam_bars)
        {
            ImVec2 d = ImGui::GetIO().DisplaySize;
            float bar = ImMax(0.0f, (d.y - d.x / 2.39f) * 0.5f); /* 2.39:1 */
            ImDrawList* bg = ImGui::GetBackgroundDrawList();
            bg->AddRectFilled(ImVec2(0, 0), ImVec2(d.x, bar), IM_COL32_BLACK);
            bg->AddRectFilled(ImVec2(0, d.y - bar), ImVec2(d.x, d.y), IM_COL32_BLACK);
        }
    }
    {
        /* the look: the server's event, if there is one and the player lets it; else the player's own */
        uint8_t sl[12];
        if (gamestate_look(sl) && g_set.fun_server)
        {
            const float aurora[4] = { sl[4] / 255.0f, sl[5] / 255.0f, sl[6] / 255.0f, sl[7] / 255.0f };
            const float tint[4] = { sl[8] / 255.0f, sl[9] / 255.0f, sl[10] / 255.0f, sl[11] / 255.0f };
            d3d8_set_look(sl[0], aurora, sl[1], sl[2], tint);
        }
        else
        {
            const float* a = g_set.fun_aurora_c;
            const float aurora[4] = { a[0], a[1], a[2], a[3] };
            d3d8_set_look(g_set.fun_aurora ? 1 : 0, aurora, g_set.fun_world, g_set.fun_filter, g_set.fun_filter_c);
        }
    }
    ImGui::Render();
}
