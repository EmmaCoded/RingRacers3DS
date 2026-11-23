// DR. ROBOTNIK'S RING RACERS — 3DS RHI
// srb2::sys::get_rhi y srb2::cvarhandler::on_set_vid_wait
// defined in sdl/i_video.cpp, which is not compiled on 3DS.

#include "../i_video.h"
#include "../rhi/ctr/ctr_rhi.hpp"

#include <memory>

// I_Quit from the engine (i_system.c) - C linkage
extern "C" void I_Quit(void);

namespace srb2
{
namespace rhi
{
// Called by CtrRhi::present() on a fatal GPU error.
void I_Quit(void)
{
    ::I_Quit();
}
} // namespace rhi

namespace sys
{

// Single instance of the CTR backend
static std::unique_ptr<rhi::CtrRhi> g_ctr_rhi;

rhi::Rhi* get_rhi(rhi::Handle<rhi::Rhi> handle)
{
    (void)handle;
    if (!g_ctr_rhi)
        g_ctr_rhi = std::make_unique<rhi::CtrRhi>();
    return g_ctr_rhi.get();
}

} // namespace sys

namespace cvarhandler
{
void on_set_vid_wait(void) {}
} // namespace cvarhandler

} // namespace srb2
