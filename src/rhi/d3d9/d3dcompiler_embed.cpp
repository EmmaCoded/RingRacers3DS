// d3dcompiler_47.dll embedded INSIDE the .exe (PC edition).
// Windows 7 does not ship the HLSL compiler, and the Direct3D 9 backend needs it for the shaders. If the system does not
// have it, d3d9_rhi.cpp extracts this copy to the temp folder and loads it from there.
// The .dll path arrives via -DRR_D3DC_PATH=... from src/hardware/CMakeLists.txt (embed\ folder next to code\).
// Without that definition nothing is included and rr_d3dc_embedded() returns NULL.
#include <cstddef>

#define RR_STR2(x) #x
#define RR_STR(x) RR_STR2(x)

#ifdef RR_D3DC_PATH
extern "C" const unsigned char rr_d3dc_start[];
extern "C" const unsigned char rr_d3dc_end[];

// __USER_LABEL_PREFIX__ is "_" on 32-bit and empty on 64-bit: that way the assembled symbol matches the C one.
__asm__(
	R"(.section .rdata,"dr"; .balign 16; .globl )" RR_STR(__USER_LABEL_PREFIX__) "rr_d3dc_start; "
	RR_STR(__USER_LABEL_PREFIX__) "rr_d3dc_start: .incbin \"" RR_STR(RR_D3DC_PATH) "\"; "
	".globl " RR_STR(__USER_LABEL_PREFIX__) "rr_d3dc_end; "
	RR_STR(__USER_LABEL_PREFIX__) "rr_d3dc_end: .text"
);
#endif

extern "C" const unsigned char* rr_d3dc_embedded(size_t* size)
{
#ifdef RR_D3DC_PATH
	*size = static_cast<size_t>(rr_d3dc_end - rr_d3dc_start);
	return rr_d3dc_start;
#else
	*size = 0;
	return nullptr;
#endif
}
