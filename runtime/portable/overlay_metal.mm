/* The overlay on Metal (overlay.h): ImGui's Metal renderer, into the present pass after MetalFX, so
 * its windows are drawn at every pixel of the screen. Compiled with ARC, as ImGui's backend is. */
#import <Metal/Metal.h>

#include "overlay.h"
#include "imgui.h"
#include "backends/imgui_impl_metal.h"

static bool g_metal;

extern "C" void overlay_metal_init(void* device)
{
    if (!g_metal && device && ImGui::GetCurrentContext())
        g_metal = ImGui_ImplMetal_Init((__bridge id<MTLDevice>)device);
}

/* In the present pass (encoder open on the drawable, described by pass): the overlay's windows. */
extern "C" void overlay_metal_draw(void* command_buffer, void* encoder, void* pass)
{
    if (!g_metal)
        return;
    MTLRenderPassDescriptor* p = (__bridge MTLRenderPassDescriptor*)pass;
    ImGui_ImplMetal_NewFrame(p);
    overlay_build_frame();
    ImDrawData* d = ImGui::GetDrawData();
    if (d && d->CmdListsCount > 0)
        ImGui_ImplMetal_RenderDrawData(d, (__bridge id<MTLCommandBuffer>)command_buffer, (__bridge id<MTLRenderCommandEncoder>)encoder);
}
