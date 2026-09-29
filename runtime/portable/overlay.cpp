/* The overlay's windows (overlay.h, docs/OVERLAY.md), with Dear ImGui (third_party/imgui). The back
 * end draws what overlay_build_frame makes (overlay_metal.mm). Display only. */
#include "overlay.h"
#include "gamestate.h"
#include "zonemap.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "imgui.h"
#include "imgui_internal.h" /* ImGuiSettingsHandler: the chat's settings in overlay.ini */
#include "backends/imgui_impl_sdl3.h"
#include "fonts/roboto_medium.h"

extern "C" int dsound_in_world(void);

static bool g_ready, g_shown;
static void chat_register(void);
static void overlay_register(void);
static int (*g_run_line)(const char* line); /* host64: the game's parser of a typed line */
static void (*g_hide_game)(int log, int party);  /* host64: the game's own windows off the screen */

/* The player's choices, kept in overlay.ini ([Overlay][Settings]) */
static struct
{
    bool chat = true, party = true, map = true, status = false, target = true;
    bool hide_game_log = false, hide_game_party = false; /* the game's own, where ours stand in */
    float ui_size = 15.0f;   /* the windows' text */
    float chat_size = 15.0f; /* the chat's lines */
    float map_range = 50.0f; /* yalms from the middle to the edge */
    bool map_north_up = false, map_names = false;
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
    if (k.key != SDLK_U)
        return false;
#if defined(__APPLE__)
    return (k.mod & SDL_KMOD_GUI) && !(k.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT));
#else
    return (k.mod & SDL_KMOD_CTRL) && (k.mod & SDL_KMOD_SHIFT) && !(k.mod & SDL_KMOD_ALT);
#endif
}
#if defined(__APPLE__)
static const char* const TOGGLE_NAME = "Cmd+U";
#else
static const char* const TOGGLE_NAME = "Ctrl+Shift+U";
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
    io.Fonts->AddFontFromMemoryCompressedTTF(roboto_medium_compressed_data, (int)roboto_medium_compressed_size, 16.0f, &cfg);
    ImGuiStyle& st = ImGui::GetStyle();
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 6.0f;
    st.FrameRounding = 4.0f;
    st.WindowBorderSize = 1.0f;
    st.Colors[ImGuiCol_WindowBg].w = 0.82f; /* the world shows through a little */
    g_ready = true;
}

extern "C" void overlay_set_game_windows(void (*hide)(int log, int party))
{
    g_hide_game = hide;
}

extern "C" void overlay_set_line_runner(int (*run)(const char* line))
{
    g_run_line = run;
}

extern "C" int overlay_shown(void)
{
    return g_ready && g_shown;
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
    if (e->type == SDL_EVENT_KEY_DOWN && is_toggle(e->key))
    {
        if (!e->key.repeat)
            g_shown = !g_shown;
        return 1;
    }
    if (e->type == SDL_EVENT_KEY_UP && e->key.key == SDLK_U && (e->key.mod & (SDL_KMOD_GUI | SDL_KMOD_CTRL)))
        return 1;
    ImGui_ImplSDL3_ProcessEvent(e);
    if (!g_shown)
        return 0;
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
    else if (sscanf(line, "ui_size=%f", &f) == 1 && f >= 10 && f <= 32) g_set.ui_size = f;
    else if (sscanf(line, "chat_size=%f", &f) == 1 && f >= 10 && f <= 32) g_set.chat_size = f;
    else if (sscanf(line, "map_range=%f", &f) == 1 && f >= 10 && f <= 250) g_set.map_range = f;
    else if (sscanf(line, "map_north_up=%d", &v) == 1) g_set.map_north_up = v != 0;
    else if (sscanf(line, "map_names=%d", &v) == 1) g_set.map_names = v != 0;
}

static void overlay_ini_write(ImGuiContext*, ImGuiSettingsHandler* h, ImGuiTextBuffer* out)
{
    out->appendf("[%s][Settings]\n", h->TypeName);
    out->appendf("chat=%d\nparty=%d\nmap=%d\nstatus=%d\n", g_set.chat, g_set.party, g_set.map, g_set.status);
    out->appendf("ui_size=%g\nchat_size=%g\nmap_range=%g\n", g_set.ui_size, g_set.chat_size, g_set.map_range);
    out->appendf("map_north_up=%d\nmap_names=%d\n", g_set.map_north_up, g_set.map_names);
    out->appendf("target=%d\nhide_game_log=%d\nhide_game_party=%d\n\n", g_set.target, g_set.hide_game_log, g_set.hide_game_party);
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

/* The overlay's own window: which windows show, text sizes, and what the host knows */
static void overlay_window(void)
{
    ImGui::SetNextWindowPos(ImVec2(24, 24), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(280, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Overlay"))
    {
        bool dirty = false;
        ImGui::TextDisabled("Windows");
        dirty |= ImGui::Checkbox("Chat", &g_set.chat);
        ImGui::SameLine(100);
        dirty |= ImGui::Checkbox("Party", &g_set.party);
        ImGui::SameLine(190);
        dirty |= ImGui::Checkbox("Map", &g_set.map);
        dirty |= ImGui::Checkbox("Target", &g_set.target);
        ImGui::SameLine(100);
        dirty |= ImGui::Checkbox("Performance", &g_set.status);
        if (g_hide_game)
        {
            ImGui::Separator();
            ImGui::TextDisabled("The game's own");
            dirty |= ImGui::Checkbox("Hide its chat log (while Chat is on)", &g_set.hide_game_log);
            dirty |= ImGui::Checkbox("Hide its party list (while Party is on)", &g_set.hide_game_party);
        }
        ImGui::Separator();
        ImGui::TextDisabled("Text size");
        ImGui::SetNextItemWidth(-60);
        dirty |= ImGui::SliderFloat("Windows##size", &g_set.ui_size, 11, 24, "%.0f");
        ImGui::SetNextItemWidth(-60);
        dirty |= ImGui::SliderFloat("Chat##size", &g_set.chat_size, 10, 28, "%.0f");
        ImGui::Separator();
        ImGui::TextDisabled("%s shows and hides the overlay", TOGGLE_NAME);
        if (dirty)
            ImGui::MarkIniSettingsDirty();
    }
    ImGui::End();
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
    IM_COL32(255, 255, 255, 255), /* NPC */
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
        sscanf(line, "colour.%31[a-z0-9]=%x", key, &rgb) == 2) /* the first test builds wrote colour. */
    {
        for (int k = 0; k < KINDS; ++k)
            if (!strcmp(key, KIND_KEY[k]))
                g_kind_col[k] = IM_COL32(rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255, 255);
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
            ImGui::PushStyleColor(ImGuiCol_Text, g_kind_col[k]);
            if (sender[0])
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
    { "Say", "/s " }, { "Party", "/p " }, { "Linkshell", "/l " }, { "Linkshell 2", "/l2 " },
    { "Shout", "/sh " }, { "Yell", "/yell " }, { "Tell", "/t " }, { "Emote", "/em " },
};
static int g_send_to;
static char g_send[256];
static bool g_send_refocus;

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
    if (g_send_refocus)
    {
        ImGui::SetKeyboardFocusHere();
        g_send_refocus = false;
    }
    const char* hint = g_send_to == 6 ? "name, then the message" : "Type here; Enter sends, Esc leaves";
    if (ImGui::InputTextWithHint("##send", hint, g_send, sizeof g_send, ImGuiInputTextFlags_EnterReturnsTrue))
    {
        const char* t = g_send;
        while (*t == ' ')
            ++t;
        if (*t)
        {
            char line[300];
            snprintf(line, sizeof line, "%s%s", *t == '/' ? "" : SEND_TO[g_send_to].prefix, t);
            g_run_line(line);
        }
        g_send[0] = 0;
        g_send_refocus = true; /* stay in the box for the next line */
    }
}

static void chat_window(void)
{
    chat_defaults();
    ImGui::SetNextWindowPos(ImVec2(24, 220), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560, 280), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Chat", &g_set.chat))
    {
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
    ImGui::SetNextWindowPos(ImVec2(24, 520), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(160, 0), ImVec2(600, FLT_MAX));
    /* its width is the player's; its height always fits who is in it */
    if (ImGuiWindow* pw = ImGui::FindWindowByName("Party"))
        ImGui::SetNextWindowSize(ImVec2(pw->SizeFull.x, 0));
    else
        ImGui::SetNextWindowSize(ImVec2(230, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 1));
    if (ImGui::Begin("Party", &g_set.party, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse))
    {
        if (!n)
            ImGui::TextDisabled("Party");
        uint16_t here = gamestate_zone();
        float w = ImGui::GetContentRegionAvail().x;
        for (int i = 0; i < n; ++i)
        {
            const GameMember& p = m[i];
            if (i && p.party != m[i - 1].party)
                ImGui::Dummy(ImVec2(0, 5)); /* the alliance's other parties */
            else if (i)
                ImGui::Dummy(ImVec2(0, 2));
            bool away = here && p.zone && p.zone != here;
            ImGui::BeginGroup();
            ImGui::PushStyleColor(ImGuiCol_Text, away ? IM_COL32(140, 140, 140, 255) : IM_COL32(255, 255, 255, 255));
            ImGui::Text("%s%s", p.name[0] ? p.name : "You", p.leader ? " *" : "");
            ImGui::PopStyleColor();
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
    ImGui::SetNextWindowBgAlpha(have ? 0.55f : 0.25f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 2));
    if (ImGui::Begin("Target", &g_set.target, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse))
    {
        if (!have)
            ImGui::TextDisabled("No target");
        else
        {
            ImU32 col = t.kind == ENTITY_PC ? IM_COL32(160, 190, 255, 255)
                      : !t.mob              ? IM_COL32(120, 225, 130, 255)
                      : t.claimed           ? IM_COL32(255, 110, 100, 255)
                                            : IM_COL32(240, 215, 120, 255);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextUnformatted(t.name[0] ? t.name : "(no name yet)");
            ImGui::PopStyleColor();
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

/* --- the zone map under the radar (zonemap.h): one texture, filled again on each new zone ------- */
static ImTextureData* g_map_tex;
static ZoneMap g_map; /* its placement in the world (the pixels are the texture's) */

static void map_texture_update(void)
{
    ZoneMap zm;
    if (!zonemap_take(gamestate_zone(), &zm))
        return;
    if (g_map_tex && g_map_tex->Width != zm.size)
        g_map_tex = NULL; /* a size it was not made for: a new one (the old is left, once) */
    if (!g_map_tex)
    {
        g_map_tex = IM_NEW(ImTextureData)();
        g_map_tex->Create(ImTextureFormat_RGBA32, zm.size, zm.size);
        g_map_tex->UseColors = true;
        memcpy(g_map_tex->GetPixels(), zm.rgba, (size_t)zm.size * zm.size * 4);
        ImGui::RegisterUserTexture(g_map_tex);
    }
    else
    {
        memcpy(g_map_tex->GetPixels(), zm.rgba, (size_t)zm.size * zm.size * 4);
        if (g_map_tex->Status == ImTextureStatus_OK)
        {
            ImTextureRect all = { 0, 0, (unsigned short)zm.size, (unsigned short)zm.size };
            g_map_tex->Updates.resize(0);
            g_map_tex->Updates.push_back(all);
            g_map_tex->UpdateRect = all;
            g_map_tex->SetStatus(ImTextureStatus_WantUpdates);
        }
    }
    free(zm.rgba);
    zm.rgba = NULL;
    g_map = zm;
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
    if (!ImGui::Begin("Map", &g_set.map, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
    {
        ImGui::End();
        return;
    }
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
        ImGui::EndPopup();
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(c, r, IM_COL32(12, 16, 22, 190), 64);
    map_texture_update();
    bool have_map = known && g_map_tex && g_map.zone == gamestate_zone() && g_map.half > 0;
    dl->AddCircle(c, r * 0.5f, IM_COL32(255, 255, 255, 28), 48);
    dl->AddCircle(c, r, IM_COL32(255, 255, 255, 70), 64, 1.5f);

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
            return ImVec2((wx - (g_map.cx - g_map.half)) / (2.0f * g_map.half), ((g_map.cz + g_map.half) - wz) / (2.0f * g_map.half));
        };
        dl->PushTexture(g_map_tex->GetTexRef());
        dl->PrimReserve(SEG * 3, SEG + 1);
        ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
        dl->PrimWriteVtx(c, uv_at(0, 0), IM_COL32_WHITE);
        for (int i = 0; i < SEG; ++i)
        {
            float a = (float)i / SEG * IM_PI * 2.0f;
            float sx = cosf(a) * r, sy = sinf(a) * r;
            dl->PrimWriteVtx(ImVec2(c.x + sx, c.y + sy), uv_at(sx, sy), IM_COL32_WHITE);
        }
        for (int i = 0; i < SEG; ++i)
        {
            dl->PrimWriteIdx(base);
            dl->PrimWriteIdx((ImDrawIdx)(base + 1 + i));
            dl->PrimWriteIdx((ImDrawIdx)(base + 1 + (i + 1) % SEG));
        }
        dl->PopTexture();
    }

    dl->AddCircle(c, r * 0.5f, IM_COL32(255, 255, 255, 28), 48);
    dl->AddCircle(c, r, IM_COL32(255, 255, 255, 90), 64, 1.5f);

    /* the compass ring's letters */
    static const struct { const char* l; float dx, dz; } DIRS[] = { { "N", 0, 1 }, { "E", 1, 0 }, { "S", 0, -1 }, { "W", -1, 0 } };
    for (const auto& d : DIRS)
    {
        ImVec2 p = to_screen(d.dx * g_set.map_range * 0.9f, d.dz * g_set.map_range * 0.9f);
        ImVec2 ts = ImGui::CalcTextSize(d.l);
        dl->AddText(ImVec2(p.x - ts.x * 0.5f, p.y - ts.y * 0.5f), d.l[0] == 'N' ? IM_COL32(255, 110, 90, 255) : IM_COL32(220, 220, 220, 200), d.l);
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
        ImU32 col = e.kind == ENTITY_PC ? (in_party ? IM_COL32(90, 230, 255, 255) : IM_COL32(150, 180, 255, 255))
                  : !e.mob              ? IM_COL32(110, 220, 120, 255)
                  : e.hpp == 0          ? IM_COL32(120, 120, 120, 200)
                  : e.claimed           ? IM_COL32(240, 80, 70, 255)
                                        : IM_COL32(235, 205, 95, 255);
        ImVec2 p = to_screen(dx, dz);
        dl->AddCircleFilled(p, e.kind == ENTITY_PC ? 3.5f : 3.0f, col, 10);
        if (has_target && e.id == target.id)
            dl->AddCircle(p, 6.5f, IM_COL32(255, 255, 255, 230), 16, 1.5f); /* the player's target */
        if (g_set.map_names && e.name[0])
            dl->AddText(ImVec2(p.x + 5, p.y - ImGui::GetFontSize() * 0.5f), IM_COL32(230, 230, 230, 200), e.name);
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
        dl->AddTriangleFilled(tip, b1, b2, IM_COL32(255, 255, 255, 255));
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
    ImGui::End();
}

extern "C" void overlay_build_frame(void)
{
    ImGui::GetStyle().FontScaleMain = g_set.ui_size / 16.0f;
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    bool was[5] = { g_set.chat, g_set.party, g_set.map, g_set.status, g_set.target };
    if (g_shown)
    {
        overlay_window();
        if (g_set.status)
            status_window();
        if (g_set.chat)
            chat_window();
        if (g_set.party)
            party_window();
        if (g_set.map)
            map_window();
        if (g_set.target)
            target_window();
    }
    /* the game's own windows the overlay's stand in for: back whenever the overlay is hidden */
    if (g_hide_game)
        g_hide_game(g_shown && g_set.chat && g_set.hide_game_log, g_shown && g_set.party && g_set.hide_game_party);
    if (was[0] != g_set.chat || was[1] != g_set.party || was[2] != g_set.map || was[3] != g_set.status || was[4] != g_set.target)
        ImGui::MarkIniSettingsDirty(); /* a window closed with its x */
    ImGui::Render();
}
