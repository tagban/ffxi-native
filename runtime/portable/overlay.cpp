/* The overlay's windows (overlay.h, docs/OVERLAY.md), with Dear ImGui (third_party/imgui). The back
 * end draws what overlay_build_frame makes (overlay_metal.mm). Display only. */
#include "overlay.h"
#include "gamestate.h"

#include <stdio.h>
#include <string.h>

#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "fonts/roboto_medium.h"

extern "C" int dsound_in_world(void);

static bool g_ready, g_shown;
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

/* The chat, from the game's own packets (gamestate.c): the first proof of the feed */
static void chat_window(void)
{
    ImGui::SetNextWindowPos(ImVec2(24, 220), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(520, 260), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Chat"))
    {
        ImGui::TextDisabled("%u packets received", gamestate_udp_packets());
        ImGui::Separator();
        ImGui::BeginChild("lines");
        int kind;
        const char *sender, *text;
        /* oldest first, the newest at the bottom, kept in view */
        int n = 0;
        while (gamestate_chat(n, &kind, &sender, &text))
            ++n;
        for (int i = n - 1; i >= 0; --i)
            if (gamestate_chat(i, &kind, &sender, &text))
            {
                ImVec4 c = kind == 3 ? ImVec4(0.55f, 0.95f, 1.0f, 1) /* tell */
                         : kind == 4 ? ImVec4(0.55f, 1.0f, 0.55f, 1) /* party */
                         : kind == 5 ? ImVec4(0.6f, 1.0f, 0.8f, 1)   /* linkshell */
                         : kind == 1 ? ImVec4(1.0f, 0.75f, 0.45f, 1) /* shout */
                                     : ImVec4(1, 1, 1, 1);
                ImGui::TextColored(c, "%s: %s", sender, text);
            }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f)
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }
    ImGui::End();
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
