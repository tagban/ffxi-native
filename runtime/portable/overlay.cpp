/* The overlay's windows (overlay.h, docs/OVERLAY.md), with Dear ImGui (third_party/imgui). The back
 * end draws what overlay_build_frame makes (overlay_metal.mm). Display only. */
#include "overlay.h"
#include "gamestate.h"

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

extern "C" void overlay_init(SDL_Window* window)
{
    if (g_ready || !window)
        return;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    chat_register(); /* before overlay.ini is read */
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = g_ini[0] ? g_ini : NULL; /* where its windows were, kept between sessions */
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange; /* the game's cursor stays the game's */
    ImGui_ImplSDL3_InitForMetal(window);
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

/* What the host knows now, before the game-state feed (docs/OVERLAY.md, phase 2) */
static void status_window(void)
{
    ImGui::SetNextWindowPos(ImVec2(24, 24), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(300, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Status"))
    {
        ImGui::Text("%.0f FPS", g_present.fps);
        ImGui::Separator();
        ImGui::Text("Frame  %d x %d", g_present.frame_w, g_present.frame_h);
        ImGui::Text("Screen %d x %d", g_present.screen_w, g_present.screen_h);
        ImGui::Text("MetalFX %s", g_present.metalfx ? "upscaling" : "off");
        ImGui::Text("%s", dsound_in_world() ? "In the world" : "Title and login screens");
        ImGui::Separator();
        ImGui::TextDisabled("%s shows and hides the overlay", TOGGLE_NAME);
    }
    ImGui::End();
}

/* --- the chat ------------------------------------------------------------------------------------ */
/* Lines from the game's own log (gamestate.c), by kind: the game's chat mode sorted into what a
 * player means by say, tell, NPC... Colours default to the game's; tabs show the kinds they choose,
 * and may ask for words. Both are kept in overlay.ini. */
enum Kind
{
    SAY, SHOUT, YELL, TELL, PARTY, LS1, LS2, EMOTE, NPC, BATTLE, SYSTEM, KINDS
};
static const char* const KIND_NAME[KINDS] = { "Say", "Shout", "Yell", "Tell", "Party", "Linkshell 1", "Linkshell 2",
                                              "Emote", "NPC", "Battle", "System" };
static const char* const KIND_KEY[KINDS] = { "say", "shout", "yell", "tell", "party", "ls1", "ls2", "emote", "npc", "battle", "system" };
/* the game's default chat colours */
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
static bool g_chat_defaults_done, g_chat_colours;

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

/* overlay.ini: [Chat][Settings] with colour.<kind>=RRGGBB and tab=kinds|words|name lines */
static void* chat_ini_open(ImGuiContext*, ImGuiSettingsHandler*, const char*)
{
    g_ntabs = 0;
    return (void*)1;
}

static void chat_ini_line(ImGuiContext*, ImGuiSettingsHandler*, void*, const char* line)
{
    unsigned rgb, kinds;
    char key[32];
    if (sscanf(line, "colour.%31[a-z0-9]=%x", key, &rgb) == 2)
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
        out->appendf("colour.%s=%02x%02x%02x\n", KIND_KEY[k], c & 255, c >> 8 & 255, c >> 16 & 255);
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

static void colour_editor(void)
{
    if (!g_chat_colours)
        return;
    ImGui::SetNextWindowSize(ImVec2(300, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Chat colours", &g_chat_colours))
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
        if (ImGui::Button("The game's colours"))
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
    ImGui::BeginChild("lines");
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
    ImGui::EndChild();
}

static void chat_window(void)
{
    chat_defaults();
    ImGui::SetNextWindowPos(ImVec2(24, 220), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560, 280), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Chat"))
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
                    if (ImGui::MenuItem("Colours..."))
                        g_chat_colours = true;
                    ImGui::EndPopup();
                }
                if (open)
                {
                    chat_lines(g_tabs[i]);
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
    colour_editor();
}

extern "C" void overlay_build_frame(void)
{
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    if (g_shown)
    {
        status_window();
        chat_window();
    }
    ImGui::Render();
}
