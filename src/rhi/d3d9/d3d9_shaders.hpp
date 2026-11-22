// DR. ROBOTNIK'S RING RACERS (PC edition)
// HLSL shaders of the Direct3D 9 backend. PC only (Windows).
#ifndef __SRB2_RHI_D3D9_SHADERS_HPP__
#define __SRB2_RHI_D3D9_SHADERS_HPP__

#ifdef _WIN32

#include <string>

namespace srb2::rhi
{

// / Returns the complete HLSL (vs + ps) of program `name` ("unshaded", "unshadedpaletted", "sharpbilinear",
/// "postimg", "postprocesswipe", "crt", "crtsharp"). false if it does not exist.
bool d3d9_hlsl_source(const std::string& name, std::string& out_source, std::string& out_vs_entry, std::string& out_extra_define);

} // namespace srb2::rhi

#endif // _WIN32

#endif
