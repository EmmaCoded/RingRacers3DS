// DR. ROBOTNIK'S RING RACERS — 3DS port
//-----------------------------------------------------------------------------
// Hardware renderer backend (hardware/hw_*.cpp, "Legacy GL") on top of
// citro3d / PICA200 GPU. Implements the hw_drv.h interface.
//
// Based on the semantics of hardware/r_opengl/r_opengl.cpp and on the idea of the
// SRB2 port to New 3DS (derrekr/srb2_3ds, src/nds/r_nds3d.c, GPL).
//
// Notes:
// - No "GLSL" shaders: light and color arrive in PolyColor (PF_Modulated), which
// applied via the TexEnv constant color.
// - Textures: converted to RGBA5551 (or RGBA4 if they have gradual alpha),
// they are rescaled to power of 2 and uploaded to VRAM.
// - Vertices: copied into a per-frame linear buffer (a ring that resets
// at the start of each frame).
// - The frame is opened by I_3DS_GpuBegin3D() (gpu_present_3ds.c) and closed by the
// present, which draws the 2D layer (menus/HUD in software) on top.
//-----------------------------------------------------------------------------

#if defined(__3DS__) && defined(HWRENDER)

#include <unistd.h> // lseek/read
#include "../../dummy/i_3ds_log.h"
#include <algorithm>
#include "../../rr_config.h"  // single-file configuration
#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <type_traits>
// [DRV] (see rr3ds_drvstats)
enum { kDrvN = 16 };
static unsigned long long s_drvT[kDrvN];
static unsigned s_drvC[kDrvN];
static const char *s_drvName[kDrvN];
struct DrvScope { int id; u64 t0; DrvScope(int i) : id(i), t0(svcGetSystemTick()) {} ~DrvScope() { s_drvT[id] += svcGetSystemTick() - t0; s_drvC[id]++; } };
#include <vector>
#include <deque>
#include <unordered_map>
#include <malloc.h>

#define I3DS_VIDWIDTH_DEF 400
#define I3DS_VIDHEIGHT_DEF 240
#include "../hw_drv.h"
#include "../hw_data.h"

extern "C" {
int  I_3DS_GpuBegin3D(void);        // gpu_present_3ds.c: opens the frame (returns the frame id)
extern int g_gpu_stat[8];
extern int g_3ds_dbgflat;
void CONS_Printf(const char *fmt, ...);
}

extern "C" unsigned long long g_gpu_t[10]; // measurement (i_video_3ds.c)

namespace
{

const u8 s_world_shbin[] __attribute__((aligned(4))) = {
	0x44,0x56,0x4c,0x42,0x01,0x00,0x00,0x00,0x18,0x01,0x00,0x00,0x44,0x56,0x4c,0x50,
	0x00,0x00,0x00,0x00,0x28,0x00,0x00,0x00,0x1b,0x00,0x00,0x00,0x94,0x00,0x00,0x00,
	0x0f,0x00,0x00,0x00,0x0c,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x4e,0x01,0xf0,0x07,0x4e,0x02,0x48,0x22,0x0a,
	0x03,0x58,0x22,0x0a,0x04,0x68,0x22,0x0a,0x05,0x78,0x22,0x0a,0x82,0x08,0x02,0x08,
	0x83,0x18,0x02,0x08,0x84,0x28,0x02,0x08,0x85,0x38,0x02,0x08,0x06,0x10,0x20,0x4c,
	0x06,0x10,0x40,0x4c,0x87,0xf8,0x47,0x22,0x02,0xf9,0x47,0x32,0x08,0xf9,0x47,0x36,
	0x09,0xf9,0x47,0x02,0x02,0x20,0x41,0x3a,0x02,0xe9,0x47,0x22,0x0a,0x89,0x42,0x02,
	0x09,0x89,0x42,0x32,0x02,0xf9,0x47,0x32,0x09,0xe9,0x47,0x36,0x0b,0xe9,0x47,0x22,
	0x0c,0xf9,0x47,0x02,0x0d,0xf0,0x67,0x4c,0x0e,0x20,0x61,0x4c,0x00,0x00,0x00,0x88,
	0x6e,0x03,0x00,0x00,0x00,0x00,0x00,0x00,0xa1,0x0a,0x00,0x00,0x00,0x00,0x00,0x00,
	0x68,0xc3,0x06,0x00,0x00,0x00,0x00,0x00,0x64,0xc3,0x06,0x00,0x00,0x00,0x00,0x00,
	0x62,0xc3,0x06,0x00,0x00,0x00,0x00,0x00,0x61,0xc3,0x06,0x00,0x00,0x00,0x00,0x00,
	0x6f,0x03,0x00,0x00,0x00,0x00,0x00,0x00,0x48,0xb5,0x2a,0x00,0x00,0x00,0x00,0x00,
	0xe8,0x1f,0x00,0x00,0x00,0x00,0x00,0x00,0xa8,0x0a,0x00,0x00,0x00,0x00,0x00,0x00,
	0x08,0x20,0x00,0x00,0x00,0x00,0x00,0x00,0x48,0x15,0x00,0x00,0x00,0x00,0x00,0x00,
	0xa8,0x2a,0x00,0x00,0x00,0x00,0x00,0x00,0xae,0x0a,0x00,0x00,0x00,0x00,0x00,0x00,
	0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x44,0x56,0x4c,0x45,0x02,0x10,0x00,0x00,
	0x00,0x00,0x00,0x00,0x1b,0x00,0x00,0x00,0x00,0x00,0x0f,0x00,0x00,0x00,0x00,0x00,
	0x40,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x68,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x68,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x88,0x00,0x00,0x00,0x03,0x00,0x00,0x00,
	0xa0,0x00,0x00,0x00,0x1c,0x00,0x00,0x00,0x02,0x00,0x5f,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x3f,0x00,0x00,0x00,0x3b,0x00,0x00,0xfc,0x45,0x00,0x02,0x00,0x5e,0x00,
	0x00,0x70,0x45,0x00,0x00,0xf0,0x43,0x00,0x00,0x00,0x3a,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x0f,0x00,0x00,0x00,0x03,0x00,0x01,0x00,0x0f,0x00,0x00,0x00,
	0x05,0x00,0x02,0x00,0x0f,0x00,0x00,0x00,0x02,0x00,0x03,0x00,0x0f,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x10,0x00,0x13,0x00,0x0b,0x00,0x00,0x00,0x14,0x00,0x17,0x00,
	0x15,0x00,0x00,0x00,0x18,0x00,0x18,0x00,0x70,0x72,0x6f,0x6a,0x65,0x63,0x74,0x69,
	0x6f,0x6e,0x00,0x6d,0x6f,0x64,0x65,0x6c,0x56,0x69,0x65,0x77,0x00,0x6c,0x69,0x67,
	0x68,0x74,0x70,0x00,
};

// Static geometry shader (static.v.pica): same + per-vertex color (u8).
const u8 s_static_shbin[] __attribute__((aligned(4))) = {
	0x44,0x56,0x4c,0x42,0x01,0x00,0x00,0x00,0x50,0x01,0x00,0x00,0x44,0x56,0x4c,0x50,
	0x00,0x00,0x00,0x00,0x28,0x00,0x00,0x00,0x21,0x00,0x00,0x00,0xac,0x00,0x00,0x00,
	0x13,0x00,0x00,0x00,0x44,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x4e,0x01,0xf0,0x07,0x4e,0x02,0x48,0x22,0x0a,
	0x03,0x58,0x22,0x0a,0x04,0x68,0x22,0x0a,0x05,0x78,0x22,0x0a,0x82,0x08,0x02,0x08,
	0x83,0x18,0x02,0x08,0x84,0x28,0x02,0x08,0x85,0x38,0x02,0x08,0x06,0x10,0x20,0x4c,
	0x06,0x10,0x40,0x4c,0x07,0xd1,0x67,0x22,0x88,0xd9,0x67,0x36,0x89,0xd9,0x67,0x22,
	0x8a,0xd9,0x67,0x02,0x0b,0xc1,0x67,0x22,0x8c,0xc9,0x67,0x02,0x8d,0xf8,0x47,0x22,
	0x02,0xf9,0x47,0x32,0x0e,0xe9,0x47,0x36,0x08,0xf9,0x47,0x02,0x02,0x20,0x41,0x3a,
	0x02,0xe9,0x47,0x22,0x0f,0x39,0x41,0x02,0x08,0x39,0x41,0x32,0x02,0xf9,0x47,0x32,
	0x08,0xe9,0x47,0x36,0x09,0xe9,0x47,0x22,0x10,0xf9,0x47,0x02,0x11,0xf1,0x67,0x20,
	0x12,0x20,0x61,0x4c,0x00,0x00,0x00,0x88,0x6e,0x03,0x00,0x00,0x00,0x00,0x00,0x00,
	0xa1,0x0a,0x00,0x00,0x00,0x00,0x00,0x00,0x68,0xc3,0x06,0x00,0x00,0x00,0x00,0x00,
	0x64,0xc3,0x06,0x00,0x00,0x00,0x00,0x00,0x62,0xc3,0x06,0x00,0x00,0x00,0x00,0x00,
	0x61,0xc3,0x06,0x00,0x00,0x00,0x00,0x00,0x6f,0x03,0x00,0x00,0x00,0x00,0x00,0x00,
	0x08,0xc0,0x3f,0x00,0x00,0x00,0x00,0x00,0xa8,0x0a,0x00,0x00,0x00,0x00,0x00,0x00,
	0x48,0x15,0x00,0x00,0x00,0x00,0x00,0x00,0xe8,0x3f,0x00,0x00,0x00,0x00,0x00,0x00,
	0x04,0xc0,0x3f,0x00,0x00,0x00,0x00,0x00,0xa4,0x6a,0x15,0x00,0x00,0x00,0x00,0x00,
	0xe8,0xbf,0x2a,0x00,0x00,0x00,0x00,0x00,0xe8,0x1f,0x00,0x00,0x00,0x00,0x00,0x00,
	0x08,0x20,0x00,0x00,0x00,0x00,0x00,0x00,0xa8,0x2a,0x00,0x00,0x00,0x00,0x00,0x00,
	0x4e,0x95,0x06,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x44,0x56,0x4c,0x45,0x02,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x21,0x00,0x00,0x00,
	0x00,0x00,0x0f,0x00,0x00,0x00,0x00,0x00,0x40,0x00,0x00,0x00,0x04,0x00,0x00,0x00,
	0x90,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x90,0x00,0x00,0x00,0x04,0x00,0x00,0x00,
	0xb0,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0xc0,0x00,0x00,0x00,0x15,0x00,0x00,0x00,
	0x02,0x00,0x5f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3f,0x00,0x01,0x01,0x37,0x00,
	0x00,0x00,0x3b,0x00,0x02,0x00,0x5e,0x00,0x00,0x70,0x45,0x00,0x00,0xf0,0x43,0x00,
	0x00,0x00,0x3a,0x00,0x00,0xfc,0x45,0x00,0x02,0x00,0x5d,0x00,0xe1,0xe1,0x3a,0x00,
	0x00,0xe0,0x42,0x00,0x5c,0x0f,0x41,0x00,0xcc,0xfc,0x44,0x00,0x02,0x00,0x5c,0x00,
	0x3d,0x0a,0x3d,0x00,0x66,0x42,0x44,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x0f,0x00,0x00,0x00,0x03,0x00,0x01,0x00,0x0f,0x00,0x00,0x00,
	0x05,0x00,0x02,0x00,0x0f,0x00,0x00,0x00,0x02,0x00,0x03,0x00,0x0f,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x10,0x00,0x13,0x00,0x0b,0x00,0x00,0x00,0x14,0x00,0x17,0x00,
	0x70,0x72,0x6f,0x6a,0x65,0x63,0x74,0x69,0x6f,0x6e,0x00,0x6d,0x6f,0x64,0x65,0x6c,
	0x56,0x69,0x65,0x77,0x00,0x00,0x00,0x00,
};

constexpr float kNear = 0.9f;          // NZCLIP_PLANE (GClipRect changes it)
constexpr float kFar  = 32768.0f;      // r_opengl's FAR_CLIPPING_PLANE
constexpr float kDefaultFov = 90.0f;

// Screen region of the top screen used by the game: height 240, width according to vid
// (320x200 -> 384x240; 400x240 -> 400x240).
extern "C" int I_3DS_VidW(void);
extern "C" int I_3DS_VidH(void);
static inline float c3d_RegionW() { float w = 240.0f * I_3DS_VidW() / (float)I_3DS_VidH(); return w > 400.0f ? 400.0f : w; }
constexpr float kScreenW = 400.0f;

constexpr size_t kMaxVerts   = 48 * 1024;       // per frame (20 B each = 960 KB)
constexpr size_t kMaxIndices = 64 * 1024;       // per frame (u16)
constexpr int    kMaxTexDim  = 256; // 512->256 (avoids evicting/re-uploading textures nonstop)

bool            s_ready = false;
DVLB_s*         s_dvlb = nullptr;
shaderProgram_s s_prog;
int             s_uProj = -1, s_uMV = -1;
int             s_uLight = -1;   // distance light (world.v.pica "lightp")
int             s_boundFrame = -1;

FOutVector*     s_vbuf = nullptr;
u16*            s_ibuf = nullptr;
size_t          s_vcount = 0, s_icount = 0;
int             s_vbufFrame = -1;
static int      s_vramMinAge = 60;

u16*            s_staging = nullptr;  // texture conversion (linear)
// VRAM upload by DMA WITHOUT WAITING. On real hardware the CPU cannot write VRAM.
// citro3d has a GX command queue bound during the frame: GX_TextureCopy enters that queue
// and executes (in order) BEFORE the frame's draw list -> the texture is ready when drawing.
// Waiting (gspWaitForPPF) with the queue bound never finishes -> Azahar used to close.
// The data is copied into a linear ring that lives until the GPU finishes the frame
// (I_3DS_GpuUploadsDone, after gspWaitForP3D en gpu_present_3ds.c).
static u8*    s_upRing = nullptr;
static size_t s_upUsed = 0;
static const size_t kUpRing = 768u * 1024u; // 1536 -> 768: frees 0.75 MB linear (Coaster)
// citro3d's GX queue has few entries; filling it = svcBreak ("Break" crash in Azahar).
// Cap on copies per frame; the other new textures that frame go to linear RAM.
static int s_upCount = 0;
static const int kUpMax = 20; // 8 dropped fps (30.1 -> 26.4: many textures to linear). citro3d queue ~32 entries.
extern "C" void I_3DS_GpuUploadsDone(void) { s_upUsed = 0; s_upCount = 0; }
static bool vram_dma(void* dst, const void* src, size_t bytes)
{
	if (!bytes || (bytes & 15) || (bytes >> 4) > 0xFFFF) return false;
	if (!s_upRing) { s_upRing = (u8*)linearAlloc(kUpRing); if (!s_upRing) return false; }
	if (s_upUsed + bytes > kUpRing || s_upCount >= kUpMax) return false; // full this frame: texture goes to linear RAM
	s_upCount++;
	u8* tmp = s_upRing + s_upUsed;
	s_upUsed += (bytes + 127) & ~(size_t)127;
	memcpy(tmp, src, bytes);
	GSPGPU_FlushDataCache(tmp, (u32)bytes);
	const u32 lines = (u32)(bytes >> 4);
	return R_SUCCEEDED(GX_TextureCopy((u32*)tmp, GX_BUFFER_DIM(lines, 0), (u32*)dst, GX_BUFFER_DIM(lines, 0), (u32)bytes, 8));
}

// Static geometry (world cache, see RR3DS_Static* in hw_main.cpp).
// Persistent vertex pool in linear memory (emptied on map change
// or when full) + per-frame indices.
struct SVert { float x, y, z, s, t; u32 c; };   // 24 B
constexpr size_t kStaticVerts   = 65535;           // 1.5 MB (indices u16)
constexpr size_t kStaticIndices = 96 * 1024;       // per frame (192 KB)
DVLB_s*         s_sdvlb = nullptr;
shaderProgram_s s_sprog;
int             s_suProj = -1, s_suMV = -1;
SVert*          s_spool = nullptr;
size_t          s_spoolUsed = 0;
size_t          s_spoolDirtyLo = (size_t)-1, s_spoolDirtyHi = 0;
u16*            s_sibuf = nullptr;
size_t          s_sicount = 0;
int             s_sibufFrame = -1;
int             s_staticDrawFrame = -1;
bool            s_staticBound = false;

RGBA_t          s_palette[256];

// Matrices (rows, OpenGL convention)
struct M4 { float m[4][4]; };
M4 s_mv, s_proj;
float s_fov = kDefaultFov;
float s_near = kNear;
bool  s_splitscreen = false;
bool  s_shearing = false;
float s_shearY = 0.0f;
bool  s_flipY = false;

// View rectangle (vid coordinates: 320x200)
int s_clipMinX = 0, s_clipMinY = 0, s_clipMaxX = I3DS_VIDWIDTH_DEF, s_clipMaxY = I3DS_VIDHEIGHT_DEF;

// Textures
struct TexEntry { C3D_Tex tex; GLMipmap_t* owner; bool used; int lastFrame; bool pinned; }; // pinned: fixed atlas sheet (never evicted)
std::deque<TexEntry> s_tex; // deque: push_back does NOT move entries (before it was a vector -> dangling s_curTex/s_curBright)
std::vector<int> s_freeIds;
C3D_Tex* s_curTex = nullptr;
C3D_Tex* s_curBright = nullptr; // brightmap (unit 1) of the current texture
C3D_Tex  s_checker;            // white/gray checkerboard for untextured polygons
bool     s_checkerOk = false;
int      s_texVram = 0, s_texLinear = 0;
size_t s_texBytes = 0;
size_t s_texLinBytes = 0, s_texVramBytes = 0; // separate
static bool s_vramOk = true;
static inline bool is_vram(const void* p) { return (u32)p >= 0x1F000000u && (u32)p < 0x1F600000u; }

FBITFIELD s_curFlags = 0;

// ATLAS of common objects (assets3ds/convert_atlas.py). If the current texture
// is a sprite that lives in an atlas sheet, the sheet is used and the UVs are transformed.
static bool s_uvOn = false;
static float s_uvSx = 1.f, s_uvSy = 1.f, s_uvOx = 0.f, s_uvOy = 0.f;

// ---------------------------------------------------------------------------
// Matrices
// ---------------------------------------------------------------------------
M4 m4_identity() { M4 r{}; for (int i = 0; i < 4; i++) r.m[i][i] = 1.0f; return r; }
M4 m4_mul(const M4& a, const M4& b)
{
	M4 r{};
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++)
			r.m[i][j] = a.m[i][0]*b.m[0][j] + a.m[i][1]*b.m[1][j] + a.m[i][2]*b.m[2][j] + a.m[i][3]*b.m[3][j];
	return r;
}
M4 m4_scale(float x, float y, float z) { M4 r = m4_identity(); r.m[0][0] = x; r.m[1][1] = y; r.m[2][2] = z; return r; }
M4 m4_translate(float x, float y, float z) { M4 r = m4_identity(); r.m[0][3] = x; r.m[1][3] = y; r.m[2][3] = z; return r; }
// Same as glRotatef (degrees, unit axis)
M4 m4_rotate(float deg, float x, float y, float z)
{
	const float a = deg * (float)M_PI / 180.0f, c = cosf(a), s = sinf(a), t = 1.0f - c;
	M4 r = m4_identity();
	r.m[0][0] = x*x*t + c;   r.m[0][1] = x*y*t - z*s; r.m[0][2] = x*z*t + y*s;
	r.m[1][0] = y*x*t + z*s; r.m[1][1] = y*y*t + c;   r.m[1][2] = y*z*t - x*s;
	r.m[2][0] = x*z*t - y*s; r.m[2][1] = y*z*t + x*s; r.m[2][2] = z*z*t + c;
	return r;
}
// Same as r_opengl.cpp's GLPerspective
M4 m4_perspective(float fovy, float aspect, float zn, float zf)
{
	M4 r{};
	const float rad = fovy / 2.0f * (float)M_PI / 180.0f;
	const float cot = cosf(rad) / sinf(rad);
	r.m[0][0] = cot / aspect;
	r.m[1][1] = cot;
	r.m[2][2] = -(zf + zn) / (zf - zn);
	r.m[2][3] = -2.0f * zn * zf / (zf - zn);
	r.m[3][2] = -1.0f;
	return r;
}

// (perf): state cache. Before, both matrices and everything were uploaded
// blend/TexEnv state on EVERY polygon (~5000 per frame).
static bool s_mtxDirty = true;
static bool s_stateValid = false;
static FBITFIELD s_lastFlags = 0;
static u32 s_lastCol = 0;
static C3D_Tex* s_lastTex = nullptr;
static int s_lastDbg = -1;
// Software light (uniform lightp) + fog/fade + brightmap
static bool s_lightValid = false;
static float s_lastLx = 0.0f, s_lastLy = 0.0f;
static u32 s_lastFade = 0;
static C3D_Tex* s_lastBright = nullptr;

// Software-mode light parameters (the PC shader's R_DoomColormap)
static inline void sw_light_params(int L, float* lx, float* ly)
{
	if (L < 0) L = 0; if (L > 255) L = 255;
	float ln = (float)L / 17.0f; if (ln > 15.0f) ln = 15.0f;
	*lx = (15.0f - ln) * 4.0f * 1.06f;   // startmap * STARTMAP_FUDGE
	*ly = (155.0f - (float)L) * 0.26f;   // cap
}

// combiners (TexEnv) for a textured polygon with Software light.
// 0: rgb = texture * tint (constant or vertex color)
// 1: alpha = light (the shader's color.a) [+ brightmap: marked pixel = full light]
// 2: rgb = mix(result, fade, light); alpha = texture.a * polygon alpha
static void setup_lit_env(u32 tint, u32 fade, bool vertexTint, bool bright)
{
	C3D_TexEnv* e0 = C3D_GetTexEnv(0);
	C3D_TexEnvInit(e0);
	C3D_TexEnvColor(e0, tint);
	C3D_TexEnvSrc(e0, C3D_RGB, GPU_TEXTURE0, vertexTint ? GPU_PRIMARY_COLOR : GPU_CONSTANT, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(e0, C3D_RGB, GPU_MODULATE);
	C3D_TexEnvSrc(e0, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(e0, C3D_Alpha, GPU_REPLACE);

	C3D_TexEnv* e1 = C3D_GetTexEnv(1);
	C3D_TexEnvInit(e1);
	C3D_TexEnvSrc(e1, C3D_RGB, GPU_PREVIOUS, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(e1, C3D_RGB, GPU_REPLACE);
	if (bright)
	{
		C3D_TexEnvSrc(e1, C3D_Alpha, GPU_PRIMARY_COLOR, GPU_TEXTURE1, GPU_PRIMARY_COLOR);
		C3D_TexEnvFunc(e1, C3D_Alpha, GPU_ADD);
	}
	else
	{
		C3D_TexEnvSrc(e1, C3D_Alpha, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
		C3D_TexEnvFunc(e1, C3D_Alpha, GPU_REPLACE);
	}

	C3D_TexEnv* e2 = C3D_GetTexEnv(2);
	C3D_TexEnvInit(e2);
	C3D_TexEnvColor(e2, (fade & 0x00FFFFFFu) | (tint & 0xFF000000u));
	C3D_TexEnvSrc(e2, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_PREVIOUS);
	C3D_TexEnvOpRgb(e2, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
	C3D_TexEnvFunc(e2, C3D_RGB, GPU_INTERPOLATE);
	C3D_TexEnvSrc(e2, C3D_Alpha, GPU_TEXTURE0, GPU_CONSTANT, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(e2, C3D_Alpha, GPU_MODULATE);
}

// citro3d does not allow "unbinding" unit 1 with NULL (C3D_TexBind reads
// tex->param -> it would hang). If there is no brightmap a minimal texture is left in place.
static inline void bind_unit1(C3D_Tex* t)
{
	if (t) C3D_TexBind(1, t);
	else if (s_checkerOk) C3D_TexBind(1, &s_checker);
}

static inline void reset_env12(void)
{
	C3D_TexEnvInit(C3D_GetTexEnv(1));
	C3D_TexEnvInit(C3D_GetTexEnv(2));
}

void upload_mtx(int loc, const M4& m)
{
	C3D_Mtx c;
	for (int i = 0; i < 4; i++)
	{
		c.r[i].x = m.m[i][0];
		c.r[i].y = m.m[i][1];
		c.r[i].z = m.m[i][2];
		c.r[i].w = m.m[i][3];
	}
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, loc, &c);
}

// sky (skybox) drawn into a texture every few frames and reused.
// The texture is 256x512 (240x400 screen + side margin) with the SAME pixel density:
// when turning, the image is shifted instead of redrawing the whole skybox.
// Turn off: sdmc:/3ds/RingRacers3ds/rr_noskytex with "1" ("0" or no file = on). Flips (if it shows upside down):
// sdmc:/3ds/RingRacers3ds/rr_skyflip with a number: 1 = flip s, 2 = flip t, 3 = both.
constexpr int   kSkyW = 256, kSkyH = 512;
constexpr float kSkySX = 240.0f / kSkyW, kSkySY = 400.0f / kSkyH;
static C3D_Tex           s_skyTex;
static C3D_RenderTarget* s_skyRT = nullptr;
static int  s_skyState = 0;      // 0 untested, 1 ready, -1 off
static bool s_skyDrawing = false; // drawing the skybox into the texture
static bool s_skyHave = false;    // the texture holds a valid sky
static int  s_skyFlip = 0;
static M4   s_projScreen;         // screen projection (without the texture scale)
static M4   s_skyMV, s_skyP;      // matrices the texture was drawn with
static M4   s_mainMV, s_mainP;    // main camera of the last frame (for NeedRedraw)
static bool s_mainHave = false;
static bool s_skyGotMV = false;   // skybox camera = 1st SetTransform of the draw-to-texture
static float s_lastOx = 0, s_lastOy = 0; static int s_redrawWhy[3] = {0};

// Final projection for the PICA: viewport -> depth [-1,0] -> screen rotation
void rebuild_projection()
{
	float fov = s_fov;
	float aspect = 1.0f; // r_opengl's ASPECT_RATIO (the game already scales in SetTransform)
	if (s_splitscreen)
	{
		fov = atanf(tanf(fov * (float)M_PI / 360.0f) * 0.8f) * 360.0f / (float)M_PI;
		aspect *= 2.0f;
	}
	M4 p = m4_perspective(fov, aspect, s_near, kFar);

	if (s_shearing)
	{
		float fdy = s_shearY * 2.0f;
		if (s_flipY) fdy = -fdy;
		p = m4_mul(m4_translate(0.0f, -fdy / (float)I_3DS_VidH(), 0.0f), p);
	}

	// Viewport: the vid rectangle (320x200) inside the 384x240 region
	// centered on the 400x240 screen. In full-screen NDC.
	const float rx = c3d_RegionW() / kScreenW;
	const float vw = (float)I_3DS_VidW(), vh = (float)I_3DS_VidH();
	const float xl = -rx + 2.0f * rx * (s_clipMinX / vw);
	const float xr = -rx + 2.0f * rx * (s_clipMaxX / vw);
	const float yt = 1.0f - 2.0f * (s_clipMinY / vh);
	const float yb = 1.0f - 2.0f * (s_clipMaxY / vh);
	M4 vp = m4_identity();
	vp.m[0][0] = (xr - xl) * 0.5f; vp.m[0][3] = (xr + xl) * 0.5f;
	vp.m[1][1] = (yt - yb) * 0.5f; vp.m[1][3] = (yt + yb) * 0.5f;
	// (x_clip' = sx*x + cx*w): since w is row 3, multiply by vp on the left
	p = m4_mul(vp, p);

	// GL depth [-1,1] -> PICA [-1,0]
	M4 depth = m4_identity();
	depth.m[2][2] = 0.5f; depth.m[2][3] = -0.5f;
	p = m4_mul(depth, p);

	// Rotated top screen (same rotation as Mtx_OrthoTilt/PerspTilt)
	M4 tilt{};
	tilt.m[0][1] = 1.0f;
	tilt.m[1][0] = -1.0f;
	tilt.m[2][2] = 1.0f;
	tilt.m[3][3] = 1.0f;
	s_proj = m4_mul(tilt, p);
	s_projScreen = s_proj;
	if (s_skyDrawing) // the texture is larger than the screen: same pixel scale
		for (int j = 0; j < 4; j++) { s_proj.m[0][j] *= kSkySX; s_proj.m[1][j] *= kSkySY; }
	s_mtxDirty = true;
}

// ---------------------------------------------------------------------------
// Frame / state
// ---------------------------------------------------------------------------
static bool sky_init();
bool begin_draw()
{
	if (!s_ready)
		return false;
	const int frame = I_3DS_GpuBegin3D();
	if (frame < 0)
		return false;
	// create the sky target on the FIRST frame, before textures
	// fill the VRAM (otherwise its depth buffer did not fit -> no depth,
	// the skybox chains appeared in front of the keyhole)
	{ static bool skyOnce = false; if (!skyOnce) { skyOnce = true; sky_init(); } }

	if (s_vbufFrame != frame)
	{
		s_vbufFrame = frame;
		{ // age for evicting from VRAM measured in TIME (~2 s), not in frames (60 frames at 7 fps = 8 s)
			static u64 lastMs = 0; static u32 emaMs = 33; static int cfgMs = -1;
			if (cfgMs < 0) cfgMs = RRCfg_Int("vram_age_ms", 2000);
			const u64 nowMs = osGetTime();
			if (lastMs) { u64 d = nowMs - lastMs; if (d > 500) d = 500; if (d < 1) d = 1; emaMs = (emaMs * 7 + (u32)d) / 8; if (emaMs < 1) emaMs = 1; }
			lastMs = nowMs;
			int a = cfgMs > 0 ? (int)((u32)cfgMs / emaMs) : 60;
			s_vramMinAge = a < 6 ? 6 : (a > 60 ? 60 : a);
		}
		s_vcount = 0;
		s_icount = 0;
	}

	if (s_boundFrame != frame)
	{
		s_boundFrame = frame;
		C3D_BindProgram(&s_prog);

		C3D_AttrInfo* attr = C3D_GetAttrInfo();
		AttrInfo_Init(attr);
		AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3); // x,y,z
		AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2); // s,t

		C3D_BufInfo* buf = C3D_GetBufInfo();
		BufInfo_Init(buf);
		BufInfo_Add(buf, s_vbuf, sizeof(FOutVector), 2, 0x10);

		C3D_CullFace(GPU_CULL_NONE);
		// Do not clear s_curTex: the game already chose the texture (SetTexture)
		// before this DrawPolygon. Clearing it made the 1st polygon after
		// re-binding come out with the checkerboard (white squares). apply_state
		// it is bound again because s_stateValid = false.
		s_mtxDirty = true;
		s_stateValid = false;
		s_lightValid = false;
	}

	if (s_mtxDirty)
	{
		upload_mtx(s_uProj, s_proj);
		upload_mtx(s_uMV, s_mv);
		s_mtxDirty = false;
	}
	return true;
}

u32 rgba_to_c3d(const RGBA_t& c)
{
	return (u32)c.s.red | ((u32)c.s.green << 8) | ((u32)c.s.blue << 16) | ((u32)c.s.alpha << 24);
}

void apply_state(FSurfaceInfo* surf, FBITFIELD flags)
{
	s_curFlags = flags;
	{
		RGBA_t w; w.rgba = 0xFFFFFFFF;
		const u32 kc = ((surf && (flags & (PF_Modulated | PF_NoTexture))) ? surf->PolyColor : w).rgba;
		// Software distance light (shader uniform) and fade
		const bool lit = surf && (flags & PF_Modulated) && !(flags & (PF_NoTexture | PF_Fog));
		float lx = 0.0f, ly = -100.0f; // no Software light: full light
		u32 fade = 0;
		if (lit)
		{
			sw_light_params(surf->LightInfo.light_level, &lx, &ly);
			fade = surf->FadeColor.rgba & 0x00FFFFFFu;
		}
		if (!s_lightValid || lx != s_lastLx || ly != s_lastLy)
		{
			C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uLight, lx, ly, 0.0f, 0.0f);
			s_lightValid = true; s_lastLx = lx; s_lastLy = ly;
		}
		C3D_Tex* const br = lit ? s_curBright : nullptr;
		if (s_stateValid && flags == s_lastFlags && kc == s_lastCol
			&& s_curTex == s_lastTex && g_3ds_dbgflat == s_lastDbg
			&& fade == s_lastFade && br == s_lastBright)
			return;
		s_stateValid = true; s_lastFlags = flags; s_lastCol = kc;
		s_lastTex = s_curTex; s_lastDbg = g_3ds_dbgflat;
		s_lastFade = fade; s_lastBright = br;
	}

	// Blending (see r_opengl.cpp's SetBlendMode)
	GPU_BLENDEQUATION eq = GPU_BLEND_ADD;
	GPU_BLENDFACTOR src = GPU_ONE, dst = GPU_ZERO;
	bool alphatest = !(flags & PF_NoAlphaTest);
	GPU_TESTFUNC afunc = GPU_GREATER;
	int aref = 127;

	if (flags & PF_Fog)
	{
		src = GPU_SRC_ALPHA; dst = GPU_SRC_COLOR;
		alphatest = false;
	}
	else
	{
		switch (flags & (PF_Blending & ~(PF_Masked | PF_Occlude)))
		{
		case PF_Translucent:   src = GPU_SRC_ALPHA; dst = GPU_ONE_MINUS_SRC_ALPHA; afunc = GPU_NOTEQUAL; aref = 0; break;
		case PF_Additive:      src = GPU_SRC_ALPHA; dst = GPU_ONE; afunc = GPU_NOTEQUAL; aref = 0; break;
		case PF_Subtractive:   src = GPU_SRC_ALPHA; dst = GPU_ONE; eq = GPU_BLEND_SUBTRACT; afunc = GPU_NOTEQUAL; aref = 0; break;
		case PF_ReverseSubtract: src = GPU_SRC_ALPHA; dst = GPU_ONE; eq = GPU_BLEND_REVERSE_SUBTRACT; afunc = GPU_NOTEQUAL; aref = 0; break;
		case PF_Environment:   src = GPU_ONE; dst = GPU_ONE_MINUS_SRC_ALPHA; afunc = GPU_NOTEQUAL; aref = 0; break;
		case PF_Multiplicative: src = GPU_DST_COLOR; dst = GPU_ZERO; afunc = GPU_NOTEQUAL; aref = 0; break;
		default:
			if (flags & PF_Masked) { src = GPU_SRC_ALPHA; dst = GPU_ZERO; }
			break;
		}
	}
	// DEBUG: with SELECT, flat world (no texture, no alpha test, no
	// depth) to separate texture failures from matrix failures.
	// Modes (contents of sdmc:/3ds/rr_dbgflat): 1 flat untextured,
	// 2 texture without alpha/blend/depth, 3 texture + alpha test without
	// depth, 4 normal (only hides the 2D layer), 5 like 2 with a fixed pattern.
	const int dbgMode = g_3ds_dbgflat;
	const bool dbgFlat = dbgMode == 1;
	const bool dbgNoDepth = (dbgMode >= 1 && dbgMode <= 3) || dbgMode == 5;
	if (dbgMode == 1 || dbgMode == 2 || dbgMode == 5) { alphatest = false; src = GPU_ONE; dst = GPU_ZERO; eq = GPU_BLEND_ADD; }
	C3D_AlphaBlend(eq, eq, src, dst, src, dst);
	C3D_AlphaTest(alphatest, afunc, aref);

	// Depth: PICA stores -z (near = greater), hence GEQUAL instead of LEQUAL
	GPU_WRITEMASK wm = (GPU_WRITEMASK)0;
	if (!(flags & PF_Invisible)) wm = (GPU_WRITEMASK)(wm | GPU_WRITE_COLOR);
	if (flags & PF_Occlude)      wm = (GPU_WRITEMASK)(wm | GPU_WRITE_DEPTH);
	C3D_DepthTest(true, (dbgNoDepth || (flags & PF_NoDepthTest)) ? GPU_ALWAYS : GPU_GEQUAL,
		dbgNoDepth ? GPU_WRITE_COLOR : wm);

	// Color: PolyColor as the TexEnv constant
	RGBA_t white; white.rgba = 0xFFFFFFFF;
	const RGBA_t col = (surf && (flags & (PF_Modulated | PF_NoTexture))) ? surf->PolyColor : white;
	const bool textured = !dbgFlat && !(flags & PF_NoTexture) && s_curTex;
	{
		// DEBUG: textured polygons tinted BLACK by PolyColor
		// (suspicion: rings/springs/capsules black due to lighting, not texture)
		static int s_darkLogged = 0;
		if (textured && (flags & PF_Modulated) && s_darkLogged < 30
			&& col.s.red < 24 && col.s.green < 24 && col.s.blue < 24 && col.s.alpha > 0)
		{
			s_darkLogged++;
			CONS_Printf("[DARK] flags=%x col=%08lx tint=%08lx fade=%08lx light=%d tex=%dx%d\n",
				(unsigned)flags, (unsigned long)col.rgba,
				surf ? (unsigned long)surf->TintColor.rgba : 0ul,
				surf ? (unsigned long)surf->FadeColor.rgba : 0ul,
				surf ? (int)surf->LightInfo.light_level : -1,
				(int)s_curTex->width, (int)s_curTex->height);
		}
	}

	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvColor(env, rgba_to_c3d(col));
	reset_env12();
	bind_unit1(s_lastBright);
	if (dbgMode == 6)
	{
		// DEBUG gray band: colors according to the blend type.
		// red=translucent, yellow=additive/other, blue=no
		// depth, green=masked (sprites), grey normal=opaque.
		u32 dc = 0;
		const FBITFIELD b = flags & (PF_Blending & ~(PF_Masked | PF_Occlude));
		if (flags & PF_NoDepthTest) dc = 0xFFFF0000;
		// one color per blend (ABGR: 0xAABBGGRR)
		else if (b == PF_Translucent) dc = 0xFF0000FF;        // red
		else if (b == PF_Additive) dc = 0xFF00FFFF;           // yellow
		else if (b == PF_Subtractive) dc = 0xFFFF00FF;        // magenta
		else if (b == PF_ReverseSubtract) dc = 0xFFFFFF00;    // cyan
		else if (b == PF_Multiplicative) dc = 0xFF0080FF;     // orange
		else if (b == PF_Environment) dc = 0xFFFFFFFF;        // white
		else if (b) dc = 0xFF800080;                          // purple (other)
		else if (flags & PF_Masked) dc = 0xFF00FF00;
		if (dc)
		{
			C3D_TexEnvColor(env, dc);
			C3D_TexEnvSrc(env, C3D_RGB, GPU_CONSTANT, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
			C3D_TexEnvSrc(env, C3D_Alpha, GPU_CONSTANT, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
			C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
			if (s_curTex) C3D_TexBind(0, s_curTex);
			return;
		}
	}
	if (textured)
	{
		C3D_TexBind(0, s_curTex);
		if ((flags & PF_Modulated) && surf && !(flags & PF_Fog))
		{
			// Software light (distance + fade + brightmap)
			setup_lit_env(rgba_to_c3d(col), s_lastFade, false, s_lastBright != nullptr);
		}
		else if (flags & PF_Modulated)
		{
			C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_CONSTANT, GPU_PRIMARY_COLOR);
			C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
		}
		else
		{
			C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
			C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
		}
	}
	else
	{
		if (dbgFlat && !(flags & PF_NoTexture))
		{
			// color according to position in the buffer: tells polygons apart
			static const u32 pal[4] = { 0xFF00FF00, 0xFFFF0000, 0xFF00FFFF, 0xFFFFFF00 };
			C3D_TexEnvColor(env, pal[(s_vcount >> 2) & 3]);
		}
		C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
		C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	}
}

FOutVector* push_verts(const FOutVector* v, size_t n, size_t* first)
{
	if (s_vcount + n > kMaxVerts)
		return nullptr;
	FOutVector* dst = s_vbuf + s_vcount;
	memcpy(dst, v, n * sizeof(FOutVector));
	if (s_uvOn) // sprite inside an atlas sheet
		for (size_t i = 0; i < n; i++)
		{
			dst[i].s = s_uvOx + dst[i].s * s_uvSx;
			dst[i].t = s_uvOy + dst[i].t * s_uvSy;
		}
	*first = s_vcount;
	s_vcount += n;
	return dst;
}

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------
int next_pow2(int v)
{
	int p = 8;
	while (p < v && p < kMaxTexDim) p <<= 1;
	return p;
}

inline u32 morton8(u32 x, u32 y)
{
	return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

// Reads texel (x,y) of the GLMipmap as RGBA
RGBA_t fetch_texel(const GLMipmap_t* t, int x, int y)
{
	RGBA_t c; c.rgba = 0;
	const UINT8* p = (const UINT8*)t->data;
	const int w = t->width;
	switch (t->format)
	{
	case GL_TEXFMT_RGBA:
		memcpy(&c, p + (y * w + x) * 4, 4);
		break;
	case GL_TEXFMT_P_8:
	{
		const UINT8 i = p[y * w + x];
		if (i == HWR_PATCHES_CHROMAKEY_COLORINDEX && (t->flags & TF_CHROMAKEYED))
			c.rgba = 0;
		else
			c = s_palette[i];
		break;
	}
	case GL_TEXFMT_AP_88:
	{
		const UINT8 i = p[(y * w + x) * 2];
		c = s_palette[i];
		if (!(t->flags & TF_CHROMAKEYED))
			c.s.alpha = p[(y * w + x) * 2 + 1];
		else if (i == HWR_PATCHES_CHROMAKEY_COLORINDEX)
			c.rgba = 0;
		break;
	}
	case GL_TEXFMT_ALPHA_8:
		c.s.red = c.s.green = c.s.blue = 255;
		c.s.alpha = p[y * w + x];
		break;
	case GL_TEXFMT_INTENSITY_8:
		c.s.red = c.s.green = c.s.blue = p[y * w + x];
		c.s.alpha = 255;
		break;
	case GL_TEXFMT_ALPHA_INTENSITY_88:
		c.s.red = c.s.green = c.s.blue = p[(y * w + x) * 2];
		c.s.alpha = p[(y * w + x) * 2 + 1];
		break;
	default:
		break;
	}
	return c;
}

extern "C" void I_3DS_PresentReport(void);
extern "C" void I_3DS_PresentJoin(void); // the core 1 thread may be using this texture
void free_tex(int id)
{
	I_3DS_PresentJoin();
	s_stateValid = false;
	if (id < 0 || id >= (int)s_tex.size() || !s_tex[id].used)
		return;
	TexEntry& e = s_tex[id];
	if (s_curTex == &e.tex)
		s_curTex = nullptr;
	if (s_curBright == &e.tex)
		s_curBright = nullptr;
	s_texBytes -= e.tex.size;
	if (is_vram(e.tex.data)) { s_texVram--; s_texVramBytes -= e.tex.size; }
	else { s_texLinear--; s_texLinBytes -= e.tex.size; }
	C3D_TexDelete(&e.tex);
	if (e.owner)
		e.owner->downloaded = 0;
	e.owner = nullptr;
	e.used = false;
	e.pinned = false;
	s_freeIds.push_back(id);
}

extern "C" void RR3DS_LinFallbackStats(unsigned *n, unsigned *bytes, unsigned *peak);
void free_all_tex()
{
	for (size_t i = 0; i < s_tex.size(); i++)
		if (!s_tex[i].pinned) // fixed atlas sheets stay (ETC, independent of the palette)
			free_tex((int)i);
}

// LRU: frees the textures that have gone longest without being drawn (never those
// used in this frame or the previous one: the GPU may still read them).
static int s_lruEvicted = 0;
static int s_dbgPressure = 0, s_dbgInitFail = 0, s_dbgAllOut = 0, s_dbgDefer = 0; // [TEXEVICT]
static bool evict_lru(size_t want, int pool = 1, int minAge = 1)
{
	std::vector<int> cand;
	for (size_t i = 0; i < s_tex.size(); i++)
		if (s_tex[i].used && !s_tex[i].pinned && s_tex[i].lastFrame < s_vbufFrame - minAge
			&& (pool == 0 || (pool == 2) == is_vram(s_tex[i].tex.data)))
			cand.push_back((int)i);
	if (cand.empty())
		return false;
	std::sort(cand.begin(), cand.end(), [](int a, int b) { return s_tex[a].lastFrame < s_tex[b].lastFrame; });
	size_t freed = 0;
	for (int id : cand)
	{
		freed += s_tex[id].tex.size;
		free_tex(id);
		s_lruEvicted++; g_gpu_t[4]++;
		if (freed >= want)
			break;
	}
	return true;
}

// [TEXOP] live textures grouped by size/format/pool, the 12 heaviest.
// "idle" = not drawn for >120 frames. Only called with diag=1.
static void report_textop(void)
{
	struct G { u16 w, h; u8 fmt; bool vram; int n, idle; u32 kb, idleKb; };
	std::vector<G> g;
	for (size_t i = 0; i < s_tex.size(); i++)
	{
		if (!s_tex[i].used) continue;
		const C3D_Tex& t = s_tex[i].tex;
		const bool v = is_vram(t.data);
		const bool idle = s_tex[i].lastFrame < s_vbufFrame - 120;
		G* f = nullptr;
		for (G& x : g)
			if (x.w == t.width && x.h == t.height && x.fmt == (u8)t.fmt && x.vram == v) { f = &x; break; }
		if (!f)
		{
			g.push_back(G{ (u16)t.width, (u16)t.height, (u8)t.fmt, v, 0, 0, 0, 0 });
			f = &g.back();
		}
		f->n++; f->kb += t.size >> 10;
		if (idle) { f->idle++; f->idleKb += t.size >> 10; }
	}
	std::sort(g.begin(), g.end(), [](const G& a, const G& b) { return a.kb > b.kb; });
	// black/flickering textures after several laps: check for constant eviction/re-upload
	CONS_Printf("[TEXEVICT] evicted(total)=%d by_linear_pressure(total)=%d init_fail(total)=%d all_out(total)=%d linear_free=%uKB vram_free=%uKB\n",
		s_lruEvicted, s_dbgPressure, s_dbgInitFail, s_dbgAllOut,
		(unsigned)(linearSpaceFree() >> 10), (unsigned)(vramSpaceFree() >> 10));
	for (size_t i = 0; i < g.size() && i < 12; i++)
		CONS_Printf("[TEXTOP] %ux%u fmt=%u %s: n=%d %uKB (idle %d = %uKB)\n",
			g[i].w, g[i].h, g[i].fmt, g[i].vram ? "VRAM" : "linear", g[i].n, g[i].kb, g[i].idle, g[i].idleKb);
}

// statistics on which textures get uploaded (to find out why there are ~650)
struct TexStat { u16 w, h; u8 fmt; u32 flags; const void* cmsrc; int n; };
static std::unordered_map<const void*, TexStat> s_tstat;
static std::unordered_map<const void*, int> s_cmsrc;
static int s_tstatTotal = 0;
extern "C" int RR3DS_DiagOn(void); // hw_main.cpp 
static void tstat_note(GLMipmap_t* t)
{
	// texture statistics ONLY with diagnostics (before always: 2 maps that grew without limit)
	if (!RR3DS_DiagOn()) return;
	s_tstatTotal++;
	const void* src = t->colormap ? (const void*)t->colormap->source : nullptr;
	TexStat& e = s_tstat[t];
	if (!e.n) { e.w = t->width; e.h = t->height; e.fmt = (u8)t->format; e.flags = t->flags; e.cmsrc = src; }
	e.n++;
	s_cmsrc[src]++;
}
extern "C" void rr3ds_texstats(FILE* f)
{
	if (!s_tstatTotal) return;
	int reup = 0, nocm = 0, cm = 0, pow2 = 0; int dim[5] = {0};
	std::vector<std::pair<int, const void*>> v;
	for (auto& kv : s_tstat)
	{
		const TexStat& e = kv.second;
		if (e.n > 1) reup += e.n - 1;
		if (e.cmsrc) cm++; else nocm++;
		if (!(e.w & (e.w - 1)) && !(e.h & (e.h - 1))) pow2++;
		int m = e.w > e.h ? e.w : e.h;
		dim[m <= 32 ? 0 : m <= 64 ? 1 : m <= 128 ? 2 : m <= 256 ? 3 : 4]++;
		v.push_back({e.n, kv.first});
	}
	fprintf(f, "   MEM: texVRAM=%uKB (%d) texLinear=%uKB (%d) VRAMfree=%uKB linearFree=%uKB vramOk=%d\n",
		(unsigned)(s_texVramBytes >> 10), s_texVram, (unsigned)(s_texLinBytes >> 10), s_texLinear,
		(unsigned)(vramSpaceFree() >> 10), (unsigned)(linearSpaceFree() >> 10), (int)s_vramOk);
	fprintf(f, "   TEXS: uploads=%d distinct=%d reuploads=%d noCM=%d withCM=%d pow2=%d CMsources=%d dim<=32:%d <=64:%d <=128:%d <=256:%d >256:%d\n",
		s_tstatTotal, (int)s_tstat.size(), reup, nocm, cm, pow2, (int)s_cmsrc.size(), dim[0], dim[1], dim[2], dim[3], dim[4]);
	std::vector<std::pair<int, const void*>> c;
	for (auto& kv : s_cmsrc) c.push_back({kv.second, kv.first});
	std::sort(c.begin(), c.end(), [](auto& a, auto& b) { return a.first > b.first; });
	fprintf(f, "   TEXS cm:");
	for (size_t i = 0; i < c.size() && i < 12; i++) fprintf(f, " %p=%d", c[i].second, c[i].first);
	fprintf(f, "\n");
	std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
	fprintf(f, "   TEXS top:");
	for (size_t i = 0; i < v.size() && i < 10; i++)
	{
		const TexStat& e = s_tstat[v[i].second];
		fprintf(f, " [%dx%d f%d fl%lx cm%p x%d]", e.w, e.h, e.fmt, (unsigned long)e.flags, e.cmsrc, e.n);
	}
	fprintf(f, "\n");
	s_tstat.clear(); s_cmsrc.clear(); s_tstatTotal = 0;
}

// ---------------------------------------------------------------------------
// already-converted ETC1/ETC1A4 textures (assets3ds/convert_etc1.py).
// File romfs:/ringracers_3ds_etc.bin (or sdmc:/3ds/data/): index in memory
// (16 B per texture) and data read from the file when each texture is uploaded.
// ---------------------------------------------------------------------------
struct EtcEnt { u16 w, h; u8 fmt, part; u32 off, size; };
static std::vector<EtcEnt> s_etc;
static FILE* s_etcf = nullptr;            // index (only while it is being read)
static bool s_etcOK = false;              // index loaded (the file is closed to save a handle)
static FILE* s_etcp[32] = {};             // data parts (_00.bin, _01.bin, ...)
static char s_etcbase[64] = "";

static FILE* etc_part(int k)
{
	if (k < 0 || k >= 32) return nullptr;
	// real 3DS leaves ~16 files open at once. Only 3 parts open (the oldest is closed);
	// before, all 8 stayed open and, if fopen failed, the texture was NOT loaded (invisible fences).
	static int s_order[3] = { -1, -1, -1 }; // [0] = the oldest
	if (!s_etcp[k])
	{
		char p[96];
		snprintf(p, sizeof p, "%s_%02d.bin", s_etcbase, k);
		if (s_order[0] >= 0 && s_etcp[s_order[0]]) { fclose(s_etcp[s_order[0]]); s_etcp[s_order[0]] = nullptr; }
		s_etcp[k] = fopen(p, "rb");
		if (!s_etcp[k]) // out of resources: close all the others and retry
		{
			for (int i = 0; i < 3; i++) if (s_order[i] >= 0 && s_etcp[s_order[i]]) { fclose(s_etcp[s_order[i]]); s_etcp[s_order[i]] = nullptr; }
			s_etcp[k] = fopen(p, "rb");
			if (!s_etcp[k]) CONS_Printf("[ETC] could NOT open %s\n", p);
		}
		s_order[0] = s_order[1]; s_order[1] = s_order[2]; s_order[2] = s_etcp[k] ? k : -1;	}
	return s_etcp[k];
}
// Called when the FS runs out of handles: close all ETC data parts (they reopen on demand)
extern "C" void I_3DS_EtcReleaseHandles(void)
{
	for (int k = 0; k < 32; k++) if (s_etcp[k]) { fclose(s_etcp[k]); s_etcp[k] = nullptr; }
}
static bool s_etcTried = false;
static std::unordered_map<GLMipmap_t*, int> s_etcOf;

static void etc_load_index()
{
	s_etcTried = true;
	snprintf(s_etcbase, sizeof s_etcbase, "romfs:/ringracers_3ds_etc");
	s_etcf = fopen("romfs:/ringracers_3ds_etc.bin", "rb");
	if (!s_etcf)
	{
		snprintf(s_etcbase, sizeof s_etcbase, "sdmc:/3ds/RingRacers3ds/data/ringracers_3ds_etc");
		s_etcf = fopen("sdmc:/3ds/RingRacers3ds/data/ringracers_3ds_etc.bin", "rb");
	}
	if (!s_etcf) { CONS_Printf("[ETC] no ringracers_3ds_etc.bin: textures as before\n"); return; }
	u8 h[12];
	if (fread(h, 1, 12, s_etcf) != 12 || memcmp(h, "T3E1", 4) || h[4] != 2) { fclose(s_etcf); s_etcf = nullptr; return; }
	const u32 n = h[8] | (h[9] << 8) | (h[10] << 16) | ((u32)h[11] << 24);
	s_etc.resize(n);
	u8 e[24];
	for (u32 i = 0; i < n; i++)
	{
		if (fread(e, 1, 24, s_etcf) != 24) { s_etc.clear(); fclose(s_etcf); s_etcf = nullptr; return; }
		EtcEnt& x = s_etc[i];
		x.w = (u16)(e[8] | (e[9] << 8)); x.h = (u16)(e[10] | (e[11] << 8)); x.fmt = e[12]; x.part = e[13];
		x.off = e[16] | (e[17] << 8) | (e[18] << 16) | ((u32)e[19] << 24);
		x.size = e[20] | (e[21] << 8) | (e[22] << 16) | ((u32)e[23] << 24);
	}
	fclose(s_etcf); s_etcf = nullptr; s_etcOK = true; // save an open file
	CONS_Printf("[ETC] %u ETC1/ETC1A4 textures ready\n", (unsigned)n);
}

// ---------------------------------------------------------------------------
// ETC1 also for CUSTOM PALETTES (Sealed Stars). Measured on Balconies:
// 89-120 textures regenerated every 2 s (~38 ms each: indices -> RGBA5551 on the
// CPU) because the animated ones (SSSKY, SSCHNY/Z, SSRING, SSLOCK) do not fit at 16 bpp
// and get evicted nonstop -> 5 fps and fragmented heap (out-of-memory crash).
// MK7 style: everything preconverted. With a non-normal palette:
// 1) the game records which textures it uses and saves the palette in
//     sdmc:/3ds/ringracers/pal_<hash>.bin ("RPAL", 768 B RGB, u32 n, n x u32 id)
// 2) on the PC, assets3ds/convert_etcpal.py creates rr3ds_etcpal_<hash>.bin
//     ("T3P1", u32 n, n x {u32 id, u16 w, u16 h, u8 fmt, u8 0, u16 0, u32 off,
// u32 size} and the data; absolute offsets) -> sdmc:/3ds/data/ or romfs:/
// 3) the next time those textures go straight to the GPU (4-8 bpp, no CPU).
// ---------------------------------------------------------------------------
static u32 c3d_PalHash();
static void c3d_PalRGB(u8* out768);
// measured in Balconies "[REGEN] why": normal_palette=1 but different colormap
// (Encore/map remap) in 103-236 of every 2 s -> the key is the
// EFFECTIVE palette = palette[colormap[i]]. Works for custom palette and colormap.
static u8 s_effRGB[768];
static u32 s_effKey = 0;
static const u8* s_effCmap = (const u8*)1;
static u32 s_effPalSum = 0;
static u32 eff_key(const u8* cmap)
{
	if (cmap == s_effCmap && s_effPalSum == c3d_PalHash()) return s_effKey;
	u8 base[768]; c3d_PalRGB(base);
	u32 h = 2166136261u;
	for (int i = 0; i < 256; i++)
	{
		const int k = cmap ? cmap[i] : i;
		s_effRGB[i*3] = base[k*3]; s_effRGB[i*3+1] = base[k*3+1]; s_effRGB[i*3+2] = base[k*3+2];
		const u32 c = s_effRGB[i*3] | (s_effRGB[i*3+1] << 8) | ((u32)s_effRGB[i*3+2] << 16);
		h = (h ^ c) * 16777619u;
	}
	s_effCmap = cmap; s_effPalSum = c3d_PalHash(); s_effKey = h;
	return h;
}
struct EtcAlt { u32 id; EtcEnt e; };
static std::vector<EtcAlt> s_alt;
static FILE* s_altf = nullptr;
static u32 s_altPal = 0;
static bool s_altTried = false;
static std::vector<u8> s_altSeen;      // bit per texture: used with this palette and without ETC
static std::vector<u32> s_altMiss;     // list for the pal_<hash>.bin file
static u32 s_altMissSaved = 0;
static u64 s_altLastSave = 0;
static int s_altDumps = 0;

static void alt_load()
{
	if (s_altf) { fclose(s_altf); s_altf = nullptr; }
	s_alt.clear(); s_altSeen.clear(); s_altMiss.clear(); s_altMissSaved = 0;
	s_altTried = true; s_altPal = s_effKey;
	char p[96];
	snprintf(p, sizeof p, "romfs:/rr3ds_etcpal_%08lx.bin", (unsigned long)s_altPal);
	s_altf = fopen(p, "rb");
	if (!s_altf)
	{
		snprintf(p, sizeof p, "sdmc:/3ds/RingRacers3ds/data/rr3ds_etcpal_%08lx.bin", (unsigned long)s_altPal);
		s_altf = fopen(p, "rb");
	}
	if (!s_altf) { CONS_Printf("[ETCPAL] palette %08lx: no ETC (textures are recorded)\n", (unsigned long)s_altPal); return; }
	u8 h[8];
	if (fread(h, 1, 8, s_altf) != 8 || memcmp(h, "T3P1", 4)) { fclose(s_altf); s_altf = nullptr; return; }
	const u32 n = h[4] | (h[5] << 8) | (h[6] << 16) | ((u32)h[7] << 24);
	s_alt.resize(n);
	u8 e[20];
	for (u32 i = 0; i < n; i++)
	{
		if (fread(e, 1, 20, s_altf) != 20) { s_alt.clear(); fclose(s_altf); s_altf = nullptr; return; }
		EtcAlt& a = s_alt[i];
		a.id = e[0] | (e[1] << 8) | (e[2] << 16) | ((u32)e[3] << 24);
		a.e.w = (u16)(e[4] | (e[5] << 8)); a.e.h = (u16)(e[6] | (e[7] << 8)); a.e.fmt = e[8]; a.e.part = 0;
		a.e.off = e[12] | (e[13] << 8) | (e[14] << 16) | ((u32)e[15] << 24);
		a.e.size = e[16] | (e[17] << 8) | (e[18] << 16) | ((u32)e[19] << 24);
	}
	std::sort(s_alt.begin(), s_alt.end(), [](const EtcAlt& a, const EtcAlt& b) { return a.id < b.id; });
	CONS_Printf("[ETCPAL] palette %08lx: %u ETC1 textures ready\n", (unsigned long)s_altPal, (unsigned)n);
}

static const EtcAlt* alt_find(u32 id)
{
	auto it = std::lower_bound(s_alt.begin(), s_alt.end(), id, [](const EtcAlt& a, u32 v) { return a.id < v; });
	return (it != s_alt.end() && it->id == id) ? &*it : nullptr;
}

static void alt_note_miss(u32 id)
{
	if (id >= 65536) return;
	if (s_altSeen.empty()) s_altSeen.assign(65536 / 8, 0);
	if (s_altSeen[id >> 3] & (1 << (id & 7))) return;
	s_altSeen[id >> 3] |= (u8)(1 << (id & 7));
	s_altMiss.push_back(id);
	const u64 now = osGetTime();
	if (s_altMiss.size() != s_altMissSaved && now - s_altLastSave > 3000 && s_altDumps < 64)
	{
		s_altLastSave = now; s_altDumps++;
		char p[96];
		snprintf(p, sizeof p, "sdmc:/3ds/RingRacers3ds/cache/pal_%08lx.bin", (unsigned long)s_altPal);
		FILE* f = fopen(p, "wb");
		if (f)
		{
			const u8* rgb = s_effRGB;
			const u32 n = (u32)s_altMiss.size();
			fwrite("RPAL", 1, 4, f); fwrite(rgb, 1, 768, f); fwrite(&n, 4, 1, f);
			fwrite(s_altMiss.data(), 4, n, f); fclose(f);
			s_altMissSaved = n;
		}
	}
}

static int etc_check(const EtcEnt& x, int* w, int* h, int* fmt)
{
	if (x.w > kMaxTexDim || x.h > kMaxTexDim || x.w < 8 || x.h < 8 || !x.size) return 0;
	*w = x.w; *h = x.h; *fmt = x.fmt;
	return 1;
}

static int s_etcLastAlt = 0; // what the last I_3DS_EtcGet returned (for TexSetEtc)
static std::unordered_map<GLMipmap_t*, u32> s_etcAltKey; // key of each mip's ETC set
extern "C" int I_3DS_EtcGetCmap(int id, const u8* cmap, int* w, int* h, int* fmt);
extern "C" int I_3DS_PaletteIsDefault(void);
extern "C" int I_3DS_EtcGet(int id, int* w, int* h, int* fmt)
{
	s_etcLastAlt = 0;
	if (id < 0) return 0;
	if (I_3DS_PaletteIsDefault())
	{
		if (!s_etcTried) etc_load_index();
		if (!s_etcOK || (size_t)id >= s_etc.size()) return 0;
		return etc_check(s_etc[id], w, h, fmt);
	}
	// custom palette
	return I_3DS_EtcGetCmap(id, nullptr, w, h, fmt);
}

// texture with a different colormap (map remap) and/or custom palette
extern "C" int I_3DS_EtcGetCmap(int id, const u8* cmap, int* w, int* h, int* fmt)
{
	s_etcLastAlt = 0;
	if (id < 0) return 0;
	const u32 key = eff_key(cmap);
	if (!s_altTried || s_altPal != key) alt_load();
	const EtcAlt* a = s_altf ? alt_find((u32)id) : nullptr;
	if (a && etc_check(a->e, w, h, fmt)) { s_etcLastAlt = 1; return 1; }
	alt_note_miss((u32)id);
	return 0;
}

// value in s_etcOf: id (normal palette) or 0x80000000|id (custom palette s_altPal)
extern "C" void I_3DS_TexSetEtc(GLMipmap_t* mip, int id)
{
	s_etcOf[mip] = s_etcLastAlt ? (int)(0x80000000u | (u32)id) : id;
	if (s_etcLastAlt) s_etcAltKey[mip] = s_altPal;
}

// ---------------------------------------------------------------------------
// ETC1A4/ETC1 SPRITES already converted on the PC (assets3ds/convert_sprites.py).
// Key = CRC32 of the lump + length (recorded when the patch is loaded, w_wad.cpp MakePatch).
// File ringracers_3ds_spr.bin: index (24 B per sprite, sorted) + data.
// ---------------------------------------------------------------------------
// compact index (12 B per sprite, before 24 B + a map per patch): ~0.3 MB less RAM.
// lm = lump length (24 bits) | meta<<24; meta = (log2(width)-3)<<4 | (log2(height)-3)<<1 | format
// (the offset is stored whole: in the file it is NOT 16-aligned)
struct SprEnt { u32 hash, lm, off; };
static std::vector<SprEnt> s_spr;
static FILE* s_sprf = nullptr;
static bool s_sprTried = false;
static u32 s_crcTab[256];
static inline void spr_dec(const SprEnt& x, EtcEnt& e)
{
	const u32 m = x.lm >> 24;
	e.w = (u16)(8u << ((m >> 4) & 7)); e.h = (u16)(8u << ((m >> 1) & 7));
	e.fmt = (u8)(m & 1); e.part = 0; e.off = x.off;
	e.size = e.fmt ? (u32)e.w * e.h : (u32)e.w * e.h / 2;
}
// ATLAS. File ringracers_3ds_atlas.bin: "T3A1" u32 ver=1, u32 sheets, u32 n,
// n x {u32 hash, u32 length, u16 sheet, u16 x, u16 y, u16 0}, padded to 16, sheets of
// 512x512 ETC1A4 (262144 B each, already flipped like the other textures).
struct AtlEnt { u32 hash, len; u16 sheet, x, y, pad; };
static FILE* s_atlf = nullptr;
static u32 s_atlSheets = 0, s_atlData = 0;
static std::vector<AtlEnt> s_atl;
static std::vector<int> s_sprAtl;            // per sprite index: atlas entry or -1
static std::vector<GLMipmap_t> s_atlMip;     // one "texture" per sheet
static u8 s_atlDummy[4];
static constexpr u32 kAtlDim = 512, kAtlBytes = 512u * 512u;
static bool s_atlOn = false, s_atlPre = false; // atlas active / sheets already preloaded
static void atl_load()
{
	s_atlf = fopen("romfs:/ringracers_3ds_atlas.bin", "rb");
	if (!s_atlf) s_atlf = fopen("sdmc:/3ds/RingRacers3ds/data/ringracers_3ds_atlas.bin", "rb");
	if (!s_atlf) { CONS_Printf("[ATLAS] no ringracers_3ds_atlas.bin: loose sprites\n"); return; }
	u8 h[16];
	if (fread(h, 1, 16, s_atlf) != 16 || memcmp(h, "T3A1", 4) || h[4] != 1) { fclose(s_atlf); s_atlf = nullptr; return; }
	auto r32 = [](const u8* q) { return (u32)q[0] | ((u32)q[1] << 8) | ((u32)q[2] << 16) | ((u32)q[3] << 24); };
	s_atlSheets = r32(h + 8);
	const u32 n = r32(h + 12);
	s_atl.resize(n);
	if (n && fread(s_atl.data(), sizeof(AtlEnt), n, s_atlf) != n) { s_atl.clear(); fclose(s_atlf); s_atlf = nullptr; return; }
	s_atlData = (16 + n * (u32)sizeof(AtlEnt) + 15) & ~15u;
	s_sprAtl.assign(s_spr.size(), -1);
	u32 hits = 0;
	for (u32 k = 0; k < n; k++)
	{
		const AtlEnt& a = s_atl[k];
		if (a.sheet >= s_atlSheets) continue;
		size_t lo = 0, hi = s_spr.size();
		while (lo < hi)
		{
			const size_t m = (lo + hi) / 2;
			const SprEnt& x = s_spr[m];
			if (x.hash < a.hash || (x.hash == a.hash && (x.lm & 0xFFFFFFu) < a.len)) lo = m + 1; else hi = m;
		}
		if (lo < s_spr.size() && s_spr[lo].hash == a.hash && (s_spr[lo].lm & 0xFFFFFFu) == a.len && s_spr[lo].off != 0xFFFFFFFFu)
		{ s_sprAtl[lo] = (int)k; hits++; }
	}
	s_atlMip.assign(s_atlSheets, GLMipmap_t{});
	for (u32 k = 0; k < s_atlSheets; k++)
	{
		GLMipmap_t& m = s_atlMip[k];
		m.width = (UINT16)kAtlDim; m.height = (UINT16)kAtlDim;
		m.data = s_atlDummy;
		m.flags = TF_3DS_ETC;
		s_etcOf[&m] = (int)(0xC0000000u | k);
	}
	CONS_Printf("[ATLAS] %u sheets, %u objects (%u linked)\n", (unsigned)s_atlSheets, (unsigned)n, (unsigned)hits);
	s_atlOn = s_atlSheets > 0;
}
static void spr_load()
{
	s_sprTried = true;
	for (u32 i = 0; i < 256; i++) { u32 c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; s_crcTab[i] = c; }
	s_sprf = fopen("romfs:/ringracers_3ds_spr.bin", "rb");
	if (!s_sprf) s_sprf = fopen("sdmc:/3ds/RingRacers3ds/data/ringracers_3ds_spr.bin", "rb");
	if (!s_sprf) { CONS_Printf("[SPR] no ringracers_3ds_spr.bin: sprites as before\n"); return; }
	u8 h[24];
	if (fread(h, 1, 12, s_sprf) != 12 || memcmp(h, "T3S1", 4) || h[4] != 1) { fclose(s_sprf); s_sprf = nullptr; return; }
	const u32 n = h[8] | (h[9] << 8) | (h[10] << 16) | ((u32)h[11] << 24);
	s_spr.reserve(n);
	auto rd32 = [&](int o) { return (u32)h[o] | ((u32)h[o+1] << 8) | ((u32)h[o+2] << 16) | ((u32)h[o+3] << 24); };
	auto lg = [](u32 v) { int l = 0; while ((1u << l) < v) l++; return l; };
	u32 bad = 0;
	for (u32 i = 0; i < n; i++)
	{
		if (fread(h, 1, 24, s_sprf) != 24) { s_spr.clear(); fclose(s_sprf); s_sprf = nullptr; return; }
		const u32 w = h[8] | (h[9] << 8), hh = h[10] | (h[11] << 8), fmt = h[12], off = rd32(16), size = rd32(20);
		const int lw = lg(w) - 3, lh = lg(hh) - 3;
		const u32 want = fmt ? w * hh : w * hh / 2;
		const u32 len = rd32(4);
		SprEnt x{ rd32(0), len & 0xFFFFFFu, off };
		if (lw < 0 || lw > 7 || lh < 0 || lh > 7 || (8u << lw) != w || (8u << lh) != hh || len > 0xFFFFFFu || size != want || fmt > 1)
			{ x.off = 0xFFFFFFFFu; bad++; } // unused
		else
			x.lm |= (((u32)lw << 4) | ((u32)lh << 1) | fmt) << 24;
		s_spr.push_back(x);
	}
	CONS_Printf("[SPR] %u ETC sprites ready (%u odd)\n", (unsigned)n, (unsigned)bad);
	atl_load();
}
extern "C" int I_3DS_SprFind(const void* lump, size_t len)
{
	if (!s_sprTried) spr_load();
	if (!s_sprf) return -1;
	const u8* p = (const u8*)lump;
	u32 c = 0xFFFFFFFFu;
	for (size_t i = 0; i < len; i++) c = s_crcTab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
	c ^= 0xFFFFFFFFu;
	size_t lo = 0, hi = s_spr.size();
	while (lo < hi)
	{
		const size_t m = (lo + hi) / 2;
		const SprEnt& x = s_spr[m];
		if (x.hash < c || (x.hash == c && (x.lm & 0xFFFFFFu) < (u32)len)) lo = m + 1; else hi = m;
	}
	if (lo < s_spr.size() && s_spr[lo].hash == c && (s_spr[lo].lm & 0xFFFFFFu) == (u32)len && s_spr[lo].off != 0xFFFFFFFFu)
		return (int)lo;
	return -1;
}
extern "C" int I_3DS_PaletteIsDefault(void);
static int s_sprHit = 0, s_sprMiss = 0;
extern "C" int I_3DS_SprGet(int idx, int* w, int* h, int* fmt)
{
	if (!s_sprf || !I_3DS_PaletteIsDefault()) return -1;
	if (idx < 0 || (size_t)idx >= s_spr.size()) { s_sprMiss++; return -1; }
	EtcEnt x; spr_dec(s_spr[idx], x);
	*w = x.w; *h = x.h; *fmt = x.fmt;
	s_sprHit++;
	return idx;
}
extern "C" void I_3DS_TexSetSpr(GLMipmap_t* mip, int idx) { s_etcOf[mip] = (int)(0x40000000u | (u32)idx); }
extern "C" void I_3DS_SprStats(int* hit, int* miss) { *hit = s_sprHit; *miss = s_sprMiss; s_sprHit = s_sprMiss = 0; }

// cost breakdown of uploading textures (ARM11 ticks)
static u64 s_upT[6]; // 0 convert/read, 1 evict, 2 reserve, 3 VRAM DMA, 4 linear copy, 5 total
static u32 s_upN = 0;
static size_t s_ecBytes = 0; static u32 s_ecClock = 0;
static u32 s_ecHit = 0, s_ecMiss = 0;
static u64 s_upSeekT = 0, s_upReadT = 0, s_upReadB = 0;
static u64 s_upEtcT = 0, s_upRgbT = 0, s_upPalT = 0; static u32 s_upEtcN = 0, s_upRgbN = 0, s_upPalN = 0;
#define UPT(k, expr) do { const u64 _t = svcGetSystemTick(); expr; s_upT[k] += svcGetSystemTick() - _t; } while (0)
extern "C" unsigned I_3DS_UpTotalUs(void) { return (unsigned)(s_upT[5] * 1000000.0 / 268111856.0); }
extern "C" unsigned I_3DS_UpPartUs(int k) { u64 v = k<6 ? s_upT[k] : k==6 ? s_upEtcT : k==7 ? s_upRgbT : k==8 ? s_upPalT : s_upReadT; return (unsigned)(v * 1000000.0 / 268111856.0); } // diag: cumulative, to subtract
extern "C" int I_3DS_UpBreakdown(char* buf, int n, int frames)
{
	const double us = 1000000.0 / 268111856.0 / (frames ? frames : 1);
	const double f = (double)(frames ? frames : 1);
	const int r = snprintf(buf, n, "conv=%.0f evict=%.0f reserve=%.0f dma=%.0f lin=%.0f total=%.0f us/frame n=%.1f | conv etc=%.0f/%.1f rgba=%.0f/%.1f pal=%.0f/%.1f | etc fread=%.0f us %.0f KB | cache etc %u KB hit=%u miss=%u",
		s_upT[0]*us, s_upT[1]*us, s_upT[2]*us, s_upT[3]*us, s_upT[4]*us, s_upT[5]*us, s_upN/f,
		s_upEtcT*us, s_upEtcN/f, s_upRgbT*us, s_upRgbN/f, s_upPalT*us, s_upPalN/f, s_upReadT*us, s_upReadB/1024.0/f, (unsigned)(s_ecBytes >> 10), (unsigned)s_ecHit, (unsigned)s_ecMiss);
	s_ecHit = s_ecMiss = 0;
	memset(s_upT, 0, sizeof(s_upT)); s_upN = 0;
	s_upSeekT = s_upReadT = s_upReadB = 0;
	s_upEtcT = s_upRgbT = s_upPalT = 0; s_upEtcN = s_upRgbN = s_upPalN = 0;
	return r;
}
// RAM cache of already-read ETC blocks. A full VRAM evicts textures that
// they come back right away (765's animated screen, karts); reading them from the SD costs ~4.5 ms each.
// rr_config: etc_cache_kb=N (0 = off). Default 1024. Loose blocks with malloc
// (<= 64 KB each: they fit even if the heap is fragmented); if malloc fails, it is not stored.
struct EtcCacheEnt { u64 key; u8* p; u32 n; u32 last; };
static std::vector<EtcCacheEnt> s_ecache;
static size_t etc_cache_cap()
{
	static int kb = -1;
	if (kb < 0)
	{
		kb = RRCfg_Int("etc_cache_kb", 1024);
		// More than this starved the memory arena on a real Old 3DS (arena requests fell through to linear
		// memory and textures got corrupted), so an old config value of 3072 is capped.
		if (kb > 1536) kb = 1536;
	}
	return (size_t)(kb > 0 ? kb : 0) * 1024u;
}
static void etc_cache_drop_oldest()
{
	size_t bi = 0;
	for (size_t i = 1; i < s_ecache.size(); i++) if (s_ecache[i].last < s_ecache[bi].last) bi = i;
	s_ecBytes -= s_ecache[bi].n; free(s_ecache[bi].p);
	s_ecache[bi] = s_ecache.back(); s_ecache.pop_back();
}
extern "C" size_t I_3DS_EtcCacheBytes(void) { return s_ecBytes; }
extern "C" void I_3DS_EtcCacheFree(void) // also for the crash trap (out of memory)
{
	for (auto& e : s_ecache) free(e.p);
	s_ecache.clear(); s_ecBytes = 0;
}
static bool raw_read_at(FILE* f, long off, void* buf, size_t n);
static bool etc_cached_read(u64 key, FILE* f, long off, void* buf, size_t n)
{
	const size_t cap = etc_cache_cap();
	if (cap)
		for (auto& e : s_ecache)
			if (e.key == key && e.n == n) { memcpy(buf, e.p, n); e.last = ++s_ecClock; s_ecHit++; return true; }
	if (!raw_read_at(f, off, buf, n)) return false;
	s_ecMiss++;
	if (!cap || n > 128u * 1024u || n > cap / 4) return true;
	while (!s_ecache.empty() && s_ecBytes + n > cap) etc_cache_drop_oldest();
	u8* p = (u8*)malloc(n);
	if (!p) return true;
	memcpy(p, buf, n);
	s_ecache.push_back(EtcCacheEnt{ key, p, (u32)n, ++s_ecClock });
	s_ecBytes += n;
	return true;
}
static bool upload_tex_(GLMipmap_t* t);
// direct read (1 FS call) instead of stdio's fseek+fread: newlib read in
// chunks of ~1 KB and fseek refilled the buffer -> ~4 ms per 14 KB ETC texture.
// After loading the indices, these FILE* are only read through here (do not mix with stdio).
static bool raw_read_at(FILE* f, long off, void* buf, size_t n)
{
	const int fd = fileno(f);
	if (fd < 0 || lseek(fd, off, SEEK_SET) != off) return false;
	size_t done = 0;
	while (done < n)
	{
		const ssize_t r = read(fd, (u8*)buf + done, n - done);
		if (r <= 0) return false;
		done += (size_t)r;
	}
	return true;
}
int s_upWhy = 0; // line of upload_tex_'s "return false" (diag [TEXFAIL])
static int s_tfWhy[8], s_tfN[8], s_tfW[8], s_tfH[8], s_tfFmt[8], s_tfFl[8];
bool upload_tex(GLMipmap_t* t)
{
	const u64 t0 = svcGetSystemTick();
	const bool r = upload_tex_(t);
	s_upT[5] += svcGetSystemTick() - t0; s_upN++;
	return r;
}
static bool upload_tex_(GLMipmap_t* t)
{
	const u64 tConv0 = svcGetSystemTick();
	s_stateValid = false;
	if (!t || (!t->data && !(t->flags & TF_3DS_ETC)) || t->width == 0 || t->height == 0) // ETC reads from the file, needs no data (the marker is purged under low memory)
		{ s_upWhy = __LINE__; return false; }
	tstat_note(t);

	int w2 = next_pow2(t->width), h2 = next_pow2(t->height);
	const bool soft_alpha = (t->format == GL_TEXFMT_AP_88 && !(t->flags & TF_CHROMAKEYED))
		|| t->format == GL_TEXFMT_ALPHA_8 || t->format == GL_TEXFMT_ALPHA_INTENSITY_88
		|| t->format == GL_TEXFMT_RGBA;
	GPU_TEXCOLOR fmt = soft_alpha ? GPU_RGBA4 : GPU_RGBA5551;
	u32 bytes = (u32)(w2 * h2 * 2);

	// ETC1/ETC1A4 already converted: read from the file straight into the GPU format
	bool etc = false;
	FILE* streamF = nullptr; long streamOff = 0; // large sprite: read straight into the texture
	if (t->flags & TF_3DS_ETC)
	{
		auto it = s_etcOf.find(t);
		if (it == s_etcOf.end()) { s_upWhy = __LINE__; return false; }
		const u32 v = (u32)it->second;
		const EtcEnt* xp = nullptr; FILE* pf = nullptr;
		if ((v & 0xC0000000u) == 0xC0000000u)
		{
			// atlas sheet
			const u32 k = v & 0x3FFFFFFFu;
			if (!s_atlf || k >= s_atlSheets) { s_upWhy = __LINE__; return false; }
			static EtcEnt s_atlTmp;
			s_atlTmp.w = (u16)kAtlDim; s_atlTmp.h = (u16)kAtlDim; s_atlTmp.fmt = 1; s_atlTmp.part = 0;
			s_atlTmp.off = s_atlData + k * kAtlBytes; s_atlTmp.size = kAtlBytes;
			xp = &s_atlTmp; pf = s_atlf;
		}
		else if ((v & 0xC0000000u) == 0x40000000u)
		{
			// precomputed sprite
			const u32 si = v & 0x3FFFFFFFu;
			if (!s_sprf || si >= s_spr.size()) { s_upWhy = __LINE__; return false; }
			static EtcEnt s_sprTmp; spr_dec(s_spr[si], s_sprTmp);
			xp = &s_sprTmp; pf = s_sprf;
		}
		else if (v & 0x80000000u)
		{
			// custom palette (if the palette changed since then, it is not valid)
			auto ki = s_etcAltKey.find(t);
			const EtcAlt* a = (s_altf && ki != s_etcAltKey.end() && ki->second == s_altPal) ? alt_find(v & 0x7FFFFFFFu) : nullptr;
			if (!a) { s_upWhy = __LINE__; return false; }
			xp = &a->e; pf = s_altf;
		}
		else
		{
			if (!s_etcOK || v >= s_etc.size()) { s_upWhy = __LINE__; return false; }
			xp = &s_etc[v]; pf = etc_part(xp->part);
		}
		const EtcEnt& x = *xp;
		bool skerr = !pf;
		if (skerr)
			{ s_upWhy = __LINE__; return false; }
		if (x.size > (u32)(kMaxTexDim * kMaxTexDim * 2))
		{
			// sprites at full resolution (up to 1024): no intermediate buffer
			if (((v & 0xC0000000u) != 0x40000000u && (v & 0xC0000000u) != 0xC0000000u) || x.w > 1024 || x.h > 1024) { s_upWhy = __LINE__; return false; }
			streamF = pf; streamOff = (long)x.off;
		}
		else if (![&]{ const u64 _t = svcGetSystemTick(); const bool g = etc_cached_read(((u64)(v >> 30) << 40) | ((u64)(x.part & 0xFF) << 32) | (u64)x.off, pf, (long)x.off, s_staging, x.size); s_upReadT += svcGetSystemTick() - _t; s_upReadB += x.size; return g; }())
			{ s_upWhy = __LINE__; return false; }
		w2 = x.w; h2 = x.h;
		fmt = x.fmt ? GPU_ETC1A4 : GPU_ETC1;
		bytes = x.size;
		etc = true;
	}
	if (!etc) {

	// Convert + rescale (nearest neighbor) + swizzle into 8x8 tiles.
	// Image row 0 -> last texture row (v=0 at the top, like GL).
	// fast version (no per-texel divisions, LUT for P_8).
	const int tilesX = w2 / 8;
	static int s_sx[kMaxTexDim];
	static u16 s_xoff[kMaxTexDim]; // x part of the swizzle index
	for (int x = 0; x < w2; x++)
	{
		s_sx[x] = x * t->width / w2;
		s_xoff[x] = (u16)((x >> 3) * 64 + morton8((u32)x & 7, 0));
	}
	auto pack = [fmt](RGBA_t c) -> u16 {
		if (fmt == GPU_RGBA4)
			return (u16)(((c.s.red >> 4) << 12) | ((c.s.green >> 4) << 8) | ((c.s.blue >> 4) << 4) | (c.s.alpha >> 4));
		return (u16)(((c.s.red >> 3) << 11) | ((c.s.green >> 3) << 6) | ((c.s.blue >> 3) << 1) | (c.s.alpha >= 128 ? 1 : 0));
	};
	const bool dbgpat = (g_3ds_dbgflat == 5);
	if (t->format == GL_TEXFMT_P_8 && !dbgpat)
	{
		u16 lut[256];
		for (int i = 0; i < 256; i++) lut[i] = pack(s_palette[i]);
		if (t->flags & TF_CHROMAKEYED) lut[HWR_PATCHES_CHROMAKEY_COLORINDEX] = 0;
		const UINT8* src = (const UINT8*)t->data;
		for (int y = 0; y < h2; y++)
		{
			const UINT8* row = src + (size_t)(y * t->height / h2) * t->width;
			const int ty = h2 - 1 - y;
			u16* base = s_staging + (ty >> 3) * tilesX * 64 + morton8(0, (u32)ty & 7);
			for (int x = 0; x < w2; x++)
				base[s_xoff[x]] = lut[row[s_sx[x]]];
		}
	}
	else if (t->format == GL_TEXFMT_RGBA && !dbgpat)
	{
		// fast RGBA path (patches/sprites with skin color). Bytes r,g,b,a.
		const bool a4 = (fmt == GPU_RGBA4);
		const UINT8* src = (const UINT8*)t->data;
		for (int y = 0; y < h2; y++)
		{
			const u32* row = (const u32*)(src + (size_t)(y * t->height / h2) * t->width * 4);
			const int ty = h2 - 1 - y;
			u16* base = s_staging + (ty >> 3) * tilesX * 64 + morton8(0, (u32)ty & 7);
			if (a4)
				for (int x = 0; x < w2; x++)
				{
					u32 c; memcpy(&c, row + s_sx[x], 4);
					base[s_xoff[x]] = (u16)(((c & 0xF0u) << 8) | ((c >> 4) & 0x0F00u) | ((c >> 16) & 0x00F0u) | (c >> 28));
				}
			else
				for (int x = 0; x < w2; x++)
				{
					u32 c; memcpy(&c, row + s_sx[x], 4);
					base[s_xoff[x]] = (u16)(((c & 0xF8u) << 8) | ((c >> 5) & 0x07C0u) | ((c >> 18) & 0x003Eu) | (c >> 31));
				}
		}
	}
	else if (t->format == GL_TEXFMT_AP_88 && !dbgpat)
	{
		// fast path for patches/sprites (AP_88). Before: fetch_texel per texel:
		// uploading a kart sprite cost ~7 ms (30-60 ms/frame with 8 racers).
		u16 lut[256];
		const bool chroma = (t->flags & TF_CHROMAKEYED) != 0;
		for (int i = 0; i < 256; i++)
		{
			RGBA_t c = s_palette[i];
			if (!chroma) c.s.alpha = 0; // alpha comes from byte 2
			lut[i] = pack(c);
		}
		if (chroma) lut[HWR_PATCHES_CHROMAKEY_COLORINDEX] = 0;
		const bool a4 = (fmt == GPU_RGBA4);
		const UINT8* src = (const UINT8*)t->data;
		for (int y = 0; y < h2; y++)
		{
			const UINT8* row = src + (size_t)(y * t->height / h2) * t->width * 2;
			const int ty = h2 - 1 - y;
			u16* base = s_staging + (ty >> 3) * tilesX * 64 + morton8(0, (u32)ty & 7);
			if (chroma)
				for (int x = 0; x < w2; x++)
					base[s_xoff[x]] = lut[row[s_sx[x] * 2]];
			else if (a4)
				for (int x = 0; x < w2; x++)
				{
					const UINT8* px = row + s_sx[x] * 2;
					base[s_xoff[x]] = (u16)(lut[px[0]] | (px[1] >> 4));
				}
			else
				for (int x = 0; x < w2; x++)
				{
					const UINT8* px = row + s_sx[x] * 2;
					base[s_xoff[x]] = (u16)(lut[px[0]] | (px[1] >= 128 ? 1 : 0));
				}
		}
	}
	else
	{
		for (int y = 0; y < h2; y++)
		{
			const int sy = y * t->height / h2;
			const int ty = h2 - 1 - y;
			u16* base = s_staging + (ty >> 3) * tilesX * 64 + morton8(0, (u32)ty & 7);
			for (int x = 0; x < w2; x++)
			{
				RGBA_t c = fetch_texel(t, s_sx[x], sy);
				if (dbgpat)
					c.rgba = (((x >> 3) ^ (y >> 3)) & 1) ? 0xFF0000FF : 0xFFFFFFFF;
				base[s_xoff[x]] = pack(c);
			}
		}
	}
	} // !etc
	{ const u64 d = svcGetSystemTick() - tConv0; s_upT[0] += d; if (etc) { s_upEtcT += d; s_upEtcN++; } else if (t->format == GL_TEXFMT_RGBA) { s_upRgbT += d; s_upRgbN++; } else { s_upPalT += d; s_upPalN++; } }

	{
		// DEBUG: dump the first textures to the log
		static int s_dumped = 0;
		if (s_dumped < 6 && !etc)
		{
			s_dumped++;
			const RGBA_t mid = fetch_texel(t, t->width / 2, t->height / 2);
			I_3DS_Both("[3DS] TEX %dx%d fmt=%d flags=%x data=%p mid=%08lx px0=%04x pal1=%08lx pal100=%08lx\n",
				(int)t->width, (int)t->height, (int)t->format, (unsigned)t->flags, t->data,
				(unsigned long)mid.rgba, (unsigned)s_staging[0],
				(unsigned long)s_palette[1].rgba, (unsigned long)s_palette[100].rgba);
		}
	}

	const u64 rrTa = svcGetSystemTick(); // diag
	int id;
	if (!s_freeIds.empty()) { id = s_freeIds.back(); s_freeIds.pop_back(); }
	else { id = (int)s_tex.size(); s_tex.push_back(TexEntry{}); }

	// Texture in linear memory (not VRAM): CPU copy + flush. The copy
	// by TextureCopy to VRAM left textures empty (alpha 0 -> the alpha test
	// discarded the whole world).
	// first VRAM (6MB, the GPU's own) copying with DMA and checking that
	// it arrived intact; if not, linear memory as before.
	// VRAM first (CPU copy, no DMA: C3D_SyncTextureCopy mid-
	// frame led to FrameSplit -> svcBreak). kVramReserve is always left
	// free to avoid overflowing. If it does not fit, old VRAM textures are evicted
	// (LRU, never those of this frame or the previous one); if still not, linear RAM.
	bool inVram = false;
	constexpr size_t kVramReserve = 512u * 1024u; // 1 MB -> 512 KB (textures were evicting each other with ~1 MB of free VRAM)
	if (s_vramOk && !(t->flags & TF_BRIGHTMAP))
	{
		// Only textures unused for ~2 s are evicted from VRAM (otherwise
		// evicted and re-uploaded nonstop). The rest goes to linear RAM.
		if (vramSpaceFree() < bytes + kVramReserve)
			UPT(1, evict_lru(bytes + kVramReserve - vramSpaceFree() + 64 * 1024, 2, s_vramMinAge));
		// large texture (>= 512 KB: 1024x1024 ETC sprites of Opulence) that still does not fit:
		// also evict what was used recently (not this frame's or the previous one's). Before, it fell to linear RAM
		// (456 KB free) and failed -> checkerboard.
		if (bytes >= 512u * 1024u && vramSpaceFree() < bytes + kVramReserve)
			UPT(1, evict_lru(bytes + kVramReserve - vramSpaceFree() + 64 * 1024, 2, 1));
		bool vok = false;
		UPT(2, vok = vramSpaceFree() >= bytes + kVramReserve && C3D_TexInitVRAM(&s_tex[id].tex, (u16)w2, (u16)h2, fmt));
		if (vok)
		{
			u16* dst = (u16*)s_tex[id].tex.data;
			if (streamF)
			{
				// Do NOT fread straight into VRAM (on console the FS cannot write to VRAM):
				// read into s_staging (linear) in chunks and copy with the CPU.
				const size_t kChunk = kMaxTexDim * kMaxTexDim * sizeof(u16);
				size_t done = 0;
				while (done < bytes)
				{
					const size_t c = (bytes - done < kChunk) ? (bytes - done) : kChunk;
					if (!raw_read_at(streamF, streamOff + (long)done, s_staging, c)) break;
					if (!vram_dma((u8*)dst + done, s_staging, c)) break;
					done += c;
				}
				if (done == bytes) inVram = true;
				else { C3D_TexDelete(&s_tex[id].tex); }
			}
			else {
			// REAL CONSOLE: the CPU can NOT write to VRAM (data abort "Permission",
			// Azahar does allow it). DMA copy by the GPU (TextureCopy) from s_staging (linear).
			bool dok = false;
			UPT(3, dok = vram_dma(dst, s_staging, bytes));
			if (dok)
				inVram = true;
			else
			{
				C3D_TexDelete(&s_tex[id].tex); // upload ring full this frame -> this texture goes to linear RAM
			}
			}
		}
	}
	// Cap on textures in linear memory: without it, it reached 25 MB and left
	// < 300 KB free (risk of OOM in hw_cache / audio).
	// 20 -> 15 MB because now ~4.4 MB go to VRAM (similar total).
	constexpr size_t kTexLinearCap = 6u * 1024 * 1024; // 4->6 MB (400x240 sees more textures: it evicted ~100/s, uploads 7-10 ms). kLinMin below still protects RAM.
	// with ETC1 almost everything goes to VRAM; linear is now 12 MB in total
	if (!inVram && s_texLinBytes + bytes > kTexLinearCap)
		UPT(1, (g_gpu_t[6]++, evict_lru(s_texLinBytes + bytes - kTexLinearCap + 512 * 1024, 1)));
	// MEMORY PRESSURE. Linear memory is also the heap's safety net
	// (memtrack_3ds.cpp). If < 2.5 MB remain free, old textures in linear yield
	// room (765Stadium crash mid-race: linear 1.3 MB free with 62 textures there).
	{
		constexpr size_t kLinMin = 2560u * 1024u;
		const size_t lf = linearSpaceFree();
		if (!inVram && lf < bytes + kLinMin)
			UPT(1, (g_gpu_t[6]++, s_dbgPressure++, evict_lru(bytes + kLinMin - lf, 1)));
	}
	// DEFER: no room in linear after evicting -> skip this texture this frame (retried next
	// frame) instead of failing C3D_TexInit and wiping ALL textures (: Coaster first frame,
	// upload ring full, linear ~0.2 MB -> corrupt textures / missing polygons).
	if (!inVram && RRCfg_Int("defer_uploads", 1))
	{
		// the 192 KB reserve deferred even a 1 KB sprite while VRAM had 4 MB free (upload ring/20 copies per frame
		// used up, linear 141 KB): at 2 fps that is seconds of white/gray squares. Small textures (<= 16 KB) only
		// keep a 48 KB reserve (they are evicted by the pressure rule above if linear gets short).
		const size_t reserve = (bytes <= 16u * 1024u && RRCfg_Int("defer_small", 1)) ? 48u * 1024u : 192u * 1024u;
		if (linearSpaceFree() < (size_t)bytes + reserve)
		{
			s_dbgDefer++;
			{ static int s_dn = 0; if (s_dn < 20) { s_dn++; CONS_Printf("[DEFERRED] %ux%u fmt=%u %uKB | vram free=%uKB linear free=%uKB\n", (unsigned)w2, (unsigned)h2, (unsigned)fmt, (unsigned)(bytes >> 10), (unsigned)(vramSpaceFree() >> 10), (unsigned)(linearSpaceFree() >> 10)); } }
			s_freeIds.push_back(id);
			{ s_upWhy = __LINE__; return false; }
		}
	}
	const u64 rrTb = svcGetSystemTick(); // diag
	bool lok = inVram;
	if (!inVram) UPT(2, lok = C3D_TexInit(&s_tex[id].tex, (u16)w2, (u16)h2, fmt));
	if (!lok)
	{
		// Memory full: free by LRU (before: free_all_tex -> stutter)
		g_gpu_t[7]++; s_dbgInitFail++;
		if (s_dbgInitFail <= 12)
			CONS_Printf("[TEXALLOCFAIL] %ux%u fmt=%u %uKB | linear free=%uKB (tex %uKB) vram free=%uKB\n",
				(unsigned)w2, (unsigned)h2, (unsigned)fmt, (unsigned)(bytes >> 10),
				(unsigned)(linearSpaceFree() >> 10), (unsigned)(s_texLinBytes >> 10), (unsigned)(vramSpaceFree() >> 10));
		bool ok = false;
		size_t want = (size_t)bytes * 2 + 256 * 1024;
		while (!ok && evict_lru(want, 1))
		{
			ok = C3D_TexInit(&s_tex[id].tex, (u16)w2, (u16)h2, fmt);
			want *= 2;
		}
		if (!ok)
		{
			// Last resort: everything out (previous behavior). Suspect: deletes textures
			// already queued for this frame (corrupt/missing in RobotnikCoaster).
			// Skipping instead made failures go 17 -> 1152 at map start: not changed yet.
			s_freeIds.push_back(id);
			s_dbgAllOut++;
			free_all_tex();
			id = s_freeIds.back(); s_freeIds.pop_back();
			if (!C3D_TexInit(&s_tex[id].tex, (u16)w2, (u16)h2, fmt))
			{
				s_freeIds.push_back(id);
				{ s_upWhy = __LINE__; return false; }
			}
		}
	}
	TexEntry& ent = s_tex[id];
	g_gpu_t[5] += bytes;
	g_gpu_t[8] = s_texBytes; g_gpu_t[9] = (unsigned long long)(s_texLinear + s_texVram);
	if (inVram) { s_texVram++; s_texVramBytes += s_tex[id].tex.size; }
	else
	{
		s_texLinear++; s_texLinBytes += s_tex[id].tex.size;
		if (streamF)
		{
			if (!raw_read_at(streamF, streamOff, ent.tex.data, bytes)) memset(ent.tex.data, 0, bytes);
		}
		else
			UPT(4, memcpy(ent.tex.data, s_staging, bytes));
		UPT(4, GSPGPU_FlushDataCache(ent.tex.data, bytes));
	}
	C3D_TexSetFilter(&ent.tex, GPU_NEAREST, GPU_NEAREST);
	C3D_TexSetWrap(&ent.tex,
		(t->flags & TF_WRAPX) ? GPU_REPEAT : GPU_CLAMP_TO_EDGE,
		(t->flags & TF_WRAPY) ? GPU_REPEAT : GPU_CLAMP_TO_EDGE);
	ent.owner = t;
	ent.used = true;
	ent.pinned = false;
	ent.lastFrame = s_vbufFrame;
	s_texBytes += ent.tex.size;
	t->downloaded = (UINT32)(id + 1);
	{ static int n = 0; const u64 rrTe = svcGetSystemTick(); if (n < 12 && rrTe - tConv0 > 268111u * 5) { n++; I_3DS_Both("[UPSLOW] %dx%d fmt=%d etc=%d conv=%u vram1=%u rest=%u us streamF=%d inVram=%d\n", (int)t->width, (int)t->height, (int)t->format, (int)etc, (unsigned)((rrTa - tConv0) / 268), (unsigned)((rrTb - rrTa) / 268), (unsigned)((rrTe - rrTb) / 268), streamF ? 1 : 0, inVram ? 1 : 0); } }
	return true;
}

// ---------------------------------------------------------------------------
// HWRAPI
// ---------------------------------------------------------------------------
boolean c3d_Init(void)
{
	if (s_ready)
		return true;

	s_vbuf = (FOutVector*)linearAlloc(kMaxVerts * sizeof(FOutVector));
	s_ibuf = (u16*)linearAlloc(kMaxIndices * sizeof(u16));
	s_staging = (u16*)linearAlloc(kMaxTexDim * kMaxTexDim * sizeof(u16));
	// reserve the table buckets AT STARTUP (whole heap). When growing in
	// race they requested a large contiguous block (rehash) -> bad_alloc on 765Stadium 64 MB.
	s_etcOf.reserve(16384); s_etcAltKey.reserve(2048);
	s_spool  = (SVert*)linearAlloc(kStaticVerts * sizeof(SVert));
	s_sibuf  = (u16*)linearAlloc(kStaticIndices * sizeof(u16));
	if (!s_spool || !s_sibuf)
		I_3DS_Both("[3DS] r_citro3d: out of linear memory for static geometry\n");
	if (!s_vbuf || !s_ibuf || !s_staging)
	{
		I_3DS_Both("[3DS] r_citro3d: out of linear memory\n");
		return false;
	}

	s_dvlb = DVLB_ParseFile((u32*)s_world_shbin, sizeof(s_world_shbin));
	shaderProgramInit(&s_prog);
	shaderProgramSetVsh(&s_prog, &s_dvlb->DVLE[0]);
	s_uProj = shaderInstanceGetUniformLocation(s_prog.vertexShader, "projection");
	s_uMV   = shaderInstanceGetUniformLocation(s_prog.vertexShader, "modelView");
	s_uLight = shaderInstanceGetUniformLocation(s_prog.vertexShader, "lightp");

	s_sdvlb = DVLB_ParseFile((u32*)s_static_shbin, sizeof(s_static_shbin));
	shaderProgramInit(&s_sprog);
	shaderProgramSetVsh(&s_sprog, &s_sdvlb->DVLE[0]);
	s_suProj = shaderInstanceGetUniformLocation(s_sprog.vertexShader, "projection");
	s_suMV   = shaderInstanceGetUniformLocation(s_sprog.vertexShader, "modelView");

	if (C3D_TexInit(&s_checker, 64, 64, GPU_RGBA5551))
	{
		u16* d = (u16*)s_checker.data;
		for (int y = 0; y < 64; y++)
			for (int x = 0; x < 64; x++)
			{
				const bool w = (((x >> 3) ^ (y >> 3)) & 1);
				const u16 px = w ? 0xFFFF : (u16)((20 << 11) | (20 << 6) | (20 << 1) | 1);
				d[(y >> 3) * 8 * 64 + (x >> 3) * 64 + morton8((u32)x & 7, (u32)y & 7)] = px;
			}
		GSPGPU_FlushDataCache(s_checker.data, s_checker.size);
		C3D_TexSetFilter(&s_checker, GPU_NEAREST, GPU_NEAREST);
		C3D_TexSetWrap(&s_checker, GPU_REPEAT, GPU_REPEAT);
		s_checkerOk = true;
	}
	for (int i = 0; i < 256; i++) s_palette[i].rgba = 0xFFFFFFFF;
	s_mv = m4_scale(1.0f, 1.0f, -1.0f);
	rebuild_projection();

	s_ready = true;
	I_3DS_Both("[3DS] r_citro3d ready (GPU 3D)\n");
	return true;
}

// the boot palette is the "normal" one; ETC1 textures are only valid with it.
static u32 s_palSumDefault = 0, s_palSum = 0;
static bool s_palHaveDefault = false;
extern "C" int I_3DS_PaletteIsDefault(void)
{
	return !s_palHaveDefault || s_palSum == s_palSumDefault;
}
static u32 c3d_PalHash() { return s_palSum; }
static void c3d_PalRGB(u8* out)
{
	for (int i = 0; i < 256; i++)
	{ out[i*3] = s_palette[i].s.red; out[i*3+1] = s_palette[i].s.green; out[i*3+2] = s_palette[i].s.blue; }
}

void c3d_SetPalette(RGBA_t* pal)
{
	if (pal)
	{
		memcpy(s_palette, pal, sizeof(s_palette));
		u32 h = 2166136261u;
		for (int i = 0; i < 256; i++) h = (h ^ (pal[i].rgba & 0x00FFFFFFu)) * 16777619u;
		s_palSum = h;
		if (!s_palHaveDefault) { s_palHaveDefault = true; s_palSumDefault = h; }
	}
	// P_8 textures depend on the palette: force them to be re-uploaded
	free_all_tex();
}

void c3d_FinishUpdate(INT32 waitvbl) { (void)waitvbl; }

void c3d_Draw2DLine(F2DCoord* v1, F2DCoord* v2, RGBA_t color)
{
	(void)v1; (void)v2; (void)color; // unused by RR's 2D (goes through twodee)
}


// 3DS: the GPU command buffer is fixed per frame (C3D_FrameSplit
// does NOT free space). If it is almost full, we skip the polygon instead of
// let GPUCMD_AddInternal do svcBreak.
// Gray band: polygons the game wants textured but
// whose texture could not be uploaded were drawn as a flat gray plane of the light
// color (PolyColor). And fog blocks (PF_Fog) without a shader also
// come out as gray planes. We skip them and count them.
static int s_skipNoTex = 0, s_skipFog = 0, s_skipFrames = 0;
static bool s_bigTexFail = false; // the last LARGE texture did not fit in memory: do not draw (before: checkerboard)
static inline bool skip_bad_poly(FBITFIELD flags)
{
	if (flags & PF_Fog) { s_skipFog++; return true; }
	if (s_bigTexFail && !(flags & PF_NoTexture) && !s_curTex) return true;
	if (!(flags & PF_NoTexture) && !s_curTex)
	{
		// Instead of skipping it (it left holes, e.g. the floor under the kart),
		// use the checkerboard.
		s_skipNoTex++;
		if (!s_checkerOk) return true;
		s_curTex = &s_checker;
	}
	return false;
}
extern "C" void Z_3DS_ReportTags(const char *why);
extern "C" char *fake_heap_end; // libctru: end of newlib's heap
extern "C" void *__real_malloc(size_t); // without purging caches (memtrack_3ds)
extern "C" void __real_free(void *);
extern "C" void RR3DS_ArenaInfo(unsigned *totalKB, unsigned *freeKB, unsigned *bigKB, unsigned *topN); // z_zone.cpp
extern "C" void HWR_BatchStats(int *peakPolys, int *peakVerts, unsigned *flushes, unsigned *direct); // hw_batching.cpp
// [FREEMEM] = REAL free memory. free-in-heap (fordblks) only counts
// holes already returned to malloc; what sbrk has not yet handed over is missing (up to
// fake_heap_end). And the real largest block is measured by probing malloc (binary
// search, ~10 attempts every 20 s; freed immediately).
static void report_free(const struct mallinfo &mi)
{
	char *top = (char *)sbrk(0);
	unsigned unused = (fake_heap_end && top && fake_heap_end > top) ? (unsigned)(fake_heap_end - top) : 0;
	unsigned total = (unsigned)mi.fordblks + unused;
	unsigned lo = 0, hi = total; // en bytes, paso 4 KB
	while (hi - lo > 4096)
	{
		unsigned mid = lo + ((hi - lo) / 2 & ~4095u);
		if (mid <= lo) break;
		void *p = __real_malloc(mid);
		if (p) { __real_free(p); lo = mid; } else hi = mid;
	}
	CONS_Printf("[FREEMEM] total=%uKB (holes=%uKB + unused=%uKB) largest-block=%uKB\n",
		total >> 10, (unsigned)(mi.fordblks >> 10), unused >> 10, lo >> 10);
	{ // single arena (zone + large mallocs). What matters: largest block of the ARENA.
		unsigned at = 0, af = 0, ab = 0, an = 0;
		RR3DS_ArenaInfo(&at, &af, &ab, &an);
		CONS_Printf("[ARENA] total=%uKB free=%uKB largest-block=%uKB | big-mallocs=%u\n", at, af, ab, an);
	}
	{ // fixed batching lists (hw_batching.cpp): per-frame peak since the last report
		int pp = 0, pv = 0; unsigned fl = 0, di = 0;
		HWR_BatchStats(&pp, &pv, &fl, &di);
		CONS_Printf("[BATCH] peak polys=%d/8192 verts=%d/32768 flushes=%u direct=%u\n", pp, pv, fl, di);
	}
}
extern "C" void I_3DS_GpuSkipReport(void)
{
	if (++s_skipFrames >= 120)
	{
		if (s_skipNoTex || s_skipFog)
			CONS_Printf("[3DS] 120f: checker(no-texture)=%d fog-skipped=%d\n", s_skipNoTex, s_skipFog);
		for (int k = 0; k < 8; k++) if (s_tfN[k])
		{
			CONS_Printf("[TEXFAIL] line=%d n=%d last=%dx%d fmt=0x%x flags=0x%x\n", s_tfWhy[k], s_tfN[k], s_tfW[k], s_tfH[k], s_tfFmt[k], s_tfFl[k]);
			s_tfWhy[k] = s_tfN[k] = 0;
		}
		s_skipNoTex = s_skipFog = s_skipFrames = 0;
		static u64 s_memLast = 0;
		u64 now = osGetTime();
		if (now - s_memLast >= 20000) // every ~20 real seconds (before 5x120 frames: at 6 fps very few reports came out). [RAM] always; [ZONE]/[MEMT] only with diagnostics
		{
			s_memLast = now;
			struct mallinfo mi = mallinfo();
			CONS_Printf("[RAM] heap used=%uKB free-in-heap=%uKB | linear free=%uKB | VRAM free=%uKB | tex: %uKB (%d VRAM, %d linear)\n",
				(unsigned)(mi.uordblks >> 10), (unsigned)(mi.fordblks >> 10),
				(unsigned)(linearSpaceFree() >> 10), (unsigned)(vramSpaceFree() >> 10),
				(unsigned)(s_texBytes >> 10), s_texVram, s_texLinear);
			report_free(mi);
			{
				unsigned ln = 0, lb = 0, lp = 0;
				RR3DS_LinFallbackStats(&ln, &lb, &lp);
				CONS_Printf("[LINUSE] red-heap=%uKB (peak %uKB, %u requests) tex-linear=%uKB deferred=%d\n",
					lb >> 10, lp >> 10, ln, (unsigned)(s_texLinBytes >> 10), s_dbgDefer);
			}
			if (RR3DS_DiagOn()) { Z_3DS_ReportTags("periodic"); report_textop(); I_3DS_PresentReport(); }
		}
	}
}

static inline bool ensure_cmd_space(void)
{
	u32* buf; u32 size, offset;
	GPUCMD_GetBuffer(&buf, &size, &offset);
	if (size - offset < 0x400) // free 32-bit words
	{
		g_gpu_stat[7]++;
		return false;
	}
	return true;
}

void c3d_DrawPolygon(FSurfaceInfo* surf, FOutVector* verts, FUINT n, FBITFIELD flags)
{
	if (skip_bad_poly(flags)) return;
	if (n < 3 || !begin_draw())
	{
		g_gpu_stat[7]++;
		return;
	}
	if (!ensure_cmd_space()) return; // before spending vertices
	size_t first;
	if (!push_verts(verts, n, &first))
	{
		g_gpu_stat[7]++;
		return;
	}
	apply_state(surf, flags);
	g_gpu_stat[0]++; g_gpu_stat[1] += (int)n;
	if ((flags & PF_NoTexture) || !s_curTex) g_gpu_stat[4]++;
	C3D_DrawArrays(GPU_TRIANGLE_FAN, (int)first, (int)n);
}

void c3d_DrawIndexedTriangles(FSurfaceInfo* surf, FOutVector* verts, FUINT nidx, FBITFIELD flags, UINT32* idx)
{
	if (skip_bad_poly(flags)) return;
	if (nidx < 3 || !begin_draw())
	{
		g_gpu_stat[7]++;
		return;
	}
	UINT32 maxi = 0;
	for (FUINT i = 0; i < nidx; i++)
		if (idx[i] > maxi) maxi = idx[i];
	const size_t nverts = (size_t)maxi + 1;
	if (!ensure_cmd_space()) return; // before spending vertices/indices
	size_t first;
	if (s_icount + nidx > kMaxIndices || !push_verts(verts, nverts, &first) || first + nverts > 65535)
	{
		g_gpu_stat[7]++;
		return;
	}
	g_gpu_stat[0] += (int)(nidx / 3); g_gpu_stat[1] += (int)nverts;
	if ((flags & PF_NoTexture) || !s_curTex) g_gpu_stat[4]++;
	u16* ib = s_ibuf + s_icount;
	for (FUINT i = 0; i < nidx; i++)
		ib[i] = (u16)(first + idx[i]);
	s_icount += nidx;
	apply_state(surf, flags);
	C3D_DrawElements(GPU_TRIANGLES, (int)nidx, C3D_UNSIGNED_SHORT, ib);
}

void c3d_RenderSkyDome(gl_sky_t* sky) { (void)sky; } // TODO: sky

void c3d_SetBlend(FBITFIELD flags) { s_curFlags = flags; }

void c3d_ResetRenderState(void) { s_curFlags = 0; s_curTex = nullptr; s_stateValid = false; }

void c3d_ClearBuffer(FBOOLEAN colorMask, FBOOLEAN depthMask, FRGBAFloat* clearColor)
{
	// The frame already starts with clean color and depth (I_3DS_GpuBegin3D).
	// For mid-frame clears (skies/portals) we draw a quad.
	g_gpu_stat[5]++;
	if (!begin_draw())
		return;
	(void)colorMask; (void)clearColor;
	// clear DEPTH mid-frame. The game draws first
	// the skybox (mini-world seen from its camera) and then clears the
	// depth before the real world. Without this, the skybox floor covered
	// the track ("the floor on top of the floor").
	if (!depthMask)
		return;
	FOutVector q[4];
	memset(q, 0, sizeof(q));
	q[0].x = -1.0f; q[0].y = -1.0f;
	q[1].x =  1.0f; q[1].y = -1.0f;
	q[2].x =  1.0f; q[2].y =  1.0f;
	q[3].x = -1.0f; q[3].y =  1.0f;
	// z = 0, w = 1 with identity matrices -> depth 0 (the farthest)
	size_t first;
	if (!ensure_cmd_space() || !push_verts(q, 4, &first)) // order
	{
		g_gpu_stat[7]++;
		return;
	}
	const M4 id = m4_identity();
	upload_mtx(s_uProj, id);
	upload_mtx(s_uMV, id);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ZERO, GPU_ONE, GPU_ZERO, GPU_ONE);
	C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_DEPTH);
	C3D_DrawArrays(GPU_TRIANGLE_FAN, (int)first, 4);
	upload_mtx(s_uProj, s_proj);
	upload_mtx(s_uMV, s_mv);
	s_stateValid = false;
}

// ---------------------------------------------------------------------------
// sky to texture (see kSkyW above). Used by hw_main.cpp (HWR_RenderPlayerView).
// ---------------------------------------------------------------------------
extern "C" C3D_RenderTarget* I_3DS_GpuMainTarget(void); // gpu_present_3ds.c
static bool sky_init()
{
	if (s_skyState) return s_skyState > 0;
	s_skyState = -1;
	{ if (RRCfg_On("noskytex")) { /* rr_config.txt */ CONS_Printf("[SKYTEX] disabled (noskytex=1)\n"); return false; } }
	s_skyFlip = RRCfg_Int("skyflip", 0); // rr_config.txt
	if (!C3D_TexInitVRAM(&s_skyTex, kSkyW, kSkyH, GPU_RGB565)) // RGBA8 -> RGB565 (512 -> 256 KB VRAM; the sky is opaque)
	{
		CONS_Printf("[SKYTEX] out of VRAM for the texture: disabled\n");
		return false;
	}
	C3D_TexSetFilter(&s_skyTex, GPU_LINEAR, GPU_LINEAR);
	C3D_TexSetWrap(&s_skyTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	s_skyRT = C3D_RenderTargetCreateFromTex(&s_skyTex, GPU_TEXFACE_2D, 0, GPU_RB_DEPTH16); // depth24 -> 16 (512 -> 256 KB)
	if (!s_skyRT)
	{
		C3D_TexDelete(&s_skyTex);
		CONS_Printf("[SKYTEX] could not create the target: disabled\n");
		return false;
	}
	if (!s_skyRT->frameBuf.depthBuf)
	{
		C3D_RenderTargetDelete(s_skyRT); s_skyRT = nullptr;
		C3D_TexDelete(&s_skyTex);
		CONS_Printf("[SKYTEX] out of VRAM for the depth buffer: disabled\n");
		return false;
	}
	s_skyState = 1;
	CONS_Printf("[SKYTEX] ready %dx%d RGB565+depth16 = 512 KB VRAM (flip=%d)\n", kSkyW, kSkyH, s_skyFlip);
	return true;
}
extern "C" int I_3DS_SkyTexOn(void) { return sky_init() ? 1 : 0; }
// why it is redrawn (for the hw_main log)
extern "C" void I_3DS_SkyTexWhy(int* noData, int* behind, int* turn, float* ox, float* oy)
{ *noData = s_redrawWhy[0]; *behind = s_redrawWhy[1]; *turn = s_redrawWhy[2]; *ox = s_lastOx; *oy = s_lastOy; s_redrawWhy[0] = s_redrawWhy[1] = s_redrawWhy[2] = 0; }
extern "C" int I_3DS_SkyTexDrawing(void) { return s_skyDrawing ? 1 : 0; }
extern "C" void I_3DS_SkyTexInvalidate(void) { s_skyHave = false; }

// Where what is now at the camera center was (in the stored image).
// Difference in rotated-screen NDC. false if it ends up behind.
static bool sky_offset(const M4& a, const M4& curP, float* ox, float* oy)
{
	const float a00 = a.m[0][0], a01 = a.m[0][1], a02 = a.m[0][2];
	const float a10 = a.m[1][0], a11 = a.m[1][1], a12 = a.m[1][2];
	const float a20 = a.m[2][0], a21 = a.m[2][1], a22 = a.m[2][2];
	const float det = a00 * (a11 * a22 - a12 * a21) - a01 * (a10 * a22 - a12 * a20) + a02 * (a10 * a21 - a11 * a20);
	if (fabsf(det) < 1e-12f) return false;
	// world direction = inverse(3x3) * (0,0,-1) = -(column 2 of the inverse)
	const float id = 1.0f / det;
	const float dx = -((a01 * a12 - a02 * a11) * id);
	const float dy = -((a02 * a10 - a00 * a12) * id);
	const float dz = -((a00 * a11 - a01 * a10) * id);
	auto proj = [](const M4& mv, const M4& p, float wx, float wy, float wz, float* nx, float* ny) -> bool
	{
		float e[4];
		for (int i = 0; i < 4; i++) e[i] = mv.m[i][0] * wx + mv.m[i][1] * wy + mv.m[i][2] * wz; // w = 0 (direction)
		float cl[4];
		for (int i = 0; i < 4; i++) cl[i] = p.m[i][0] * e[0] + p.m[i][1] * e[1] + p.m[i][2] * e[2] + p.m[i][3] * e[3];
		if (cl[3] <= 1e-6f) return false;
		*nx = cl[0] / cl[3]; *ny = cl[1] / cl[3];
		return true;
	};
	float ox0, oy0, nx0, ny0;
	// the camera can look toward +z (z scale -1 in the matrix): if "forward" ends up
	// behind, use the opposite direction. Before it ALWAYS returned false -> redraw every frame.
	if (!proj(a, curP, dx, dy, dz, &nx0, &ny0))
	{
		if (!proj(a, curP, -dx, -dy, -dz, &nx0, &ny0)) return false;
		if (!proj(s_skyMV, s_skyP, -dx, -dy, -dz, &ox0, &oy0)) return false;
	}
	else if (!proj(s_skyMV, s_skyP, dx, dy, dz, &ox0, &oy0)) return false;
	*ox = ox0 - nx0; *oy = oy0 - ny0;
	return true;
}

// 1 = the sky must be redrawn this frame (none yet, or it turned too far)
static bool s_skyForce = false; // the stored sky was sampled outside its margin: redraw next frame
extern "C" int I_3DS_SkyTexNeedRedraw(void)
{
	if (s_skyForce) { s_skyForce = false; return 1; }
	{ static int always = -1; if (always < 0) always = RRCfg_On("sky_always") ? 1 : 0; if (always) return 1; } // test: never reuse the stored sky (Pico Park blue flicker)
	if (!s_skyHave || !s_mainHave) { s_redrawWhy[0]++; return 1; }
	// with the PREVIOUS frame's MAIN camera (s_mv may now be another
	// transformation: sprites, 2D, dome -> it used to redraw almost always)
	float ox, oy;
	if (!sky_offset(s_mainMV, s_mainP, &ox, &oy)) { s_redrawWhy[1]++; return 1; }
	s_lastOx = ox; s_lastOy = oy;
	if (fabsf(ox) > 0.058f || fabsf(oy) > 0.25f) s_redrawWhy[2]++;
	// margin: x' (240 axis) 256/240-1 = 0.067; y' (400 axis) 512/400-1 = 0.28. Redraw at 60%.
	// redraw at ~90% of the margin (before 60%: it redrew almost every time when turning)
	return (fabsf(ox) > 0.058f || fabsf(oy) > 0.25f) ? 1 : 0;
}

extern "C" int I_3DS_SkyTexBegin(void)
{
	if (!sky_init() || !begin_draw())
		return 0;
	C3D_RenderTargetClear(s_skyRT, C3D_CLEAR_ALL, 0x000000FF, 0);
	C3D_FrameDrawOn(s_skyRT);
	s_skyDrawing = true;
	s_skyGotMV = false;
	rebuild_projection();
	s_stateValid = false;
	return 1;
}

extern "C" void I_3DS_SkyTexEnd(void)
{
	if (!s_skyDrawing)
		return;
	if (!s_skyGotMV) s_skyMV = s_mv;
	s_skyP = s_projScreen;
	if (C3D_RenderTarget* t = I_3DS_GpuMainTarget()) C3D_FrameDrawOn(t);
	s_skyDrawing = false;
	s_skyHave = true;
	rebuild_projection();
	s_stateValid = false;
}

// Draws the sky texture behind everything (call with the main camera already set).
extern "C" void I_3DS_SkyTexDraw(void)
{
	DrvScope drvScope_(1);
	if (s_skyState <= 0 || !s_skyHave || !begin_draw())
		return;
	s_mainMV = s_mv; s_mainP = s_projScreen; s_mainHave = true;
	float ox = 0.0f, oy = 0.0f;
	if (!sky_offset(s_mv, s_projScreen, &ox, &oy)) { ox = oy = 0.0f; }
	// The redraw decision used the PREVIOUS frame's camera; if the view turned more than the texture
	// margin since then, CLAMP_TO_EDGE stretched one edge texel over the whole screen (solid
	// blue/green flicker in Pico Park). Keep the sample inside the texture and redraw next frame.
	{
		constexpr float kMaxOx = 0.0660f, kMaxOy = 0.2780f; // texture margin: 256/240-1 over 2, 512/400-1 over 2
		if (ox > kMaxOx) { ox = kMaxOx; s_skyForce = true; }
		else if (ox < -kMaxOx) { ox = -kMaxOx; s_skyForce = true; }
		if (oy > kMaxOy) { oy = kMaxOy; s_skyForce = true; }
		else if (oy < -kMaxOy) { oy = -kMaxOy; s_skyForce = true; }
		if (s_skyForce) { static int nClamp = 0; nClamp++; if (nClamp <= 40 || (nClamp % 50) == 0) CONS_Printf("[SKYTEX] out of margin (before: flat color on screen) ox=%.3f oy=%.3f n=%d\n", ox, oy, nClamp); }
	}
	FOutVector q[4];
	memset(q, 0, sizeof(q));
	const float px[4] = { -1.0f, 1.0f, 1.0f, -1.0f }, py[4] = { -1.0f, -1.0f, 1.0f, 1.0f };
	for (int i = 0; i < 4; i++)
	{
		q[i].x = px[i]; q[i].y = py[i]; q[i].z = 0.0f; // depth 0 = the farthest
		float ss = 0.5f + (px[i] + ox) * kSkySX * 0.5f;
		float tt = 0.5f + (py[i] + oy) * kSkySY * 0.5f;
		if (s_skyFlip & 1) ss = 1.0f - ss;
		if (s_skyFlip & 2) tt = 1.0f - tt;
		q[i].s = ss; q[i].t = tt;
	}
	size_t first;
	if (!ensure_cmd_space() || !push_verts(q, 4, &first))
	{
		g_gpu_stat[7]++;
		return;
	}
	const M4 idm = m4_identity();
	upload_mtx(s_uProj, idm);
	upload_mtx(s_uMV, idm);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_uLight, 0.0f, -100.0f, 0.0f, 0.0f); s_lightValid = false;
	C3D_TexBind(0, &s_skyTex);
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	reset_env12();
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_COLOR);
	C3D_DrawArrays(GPU_TRIANGLE_FAN, (int)first, 4);
	upload_mtx(s_uProj, s_proj);
	upload_mtx(s_uMV, s_mv);
	s_stateValid = false;
}

// FIXED ATLAS. Sheets are uploaded ONCE (first draw after loading the atlas, with the
// VRAM still clean) and stay fixed: LRU and free_all_tex do not touch them -> they neither fragment
// nor get re-uploaded. If all were uploaded, the file is closed (one fewer open file).
// rr_config: fixed_atlas=0 -> as before (sheets via LRU).
static void atl_preload()
{
	s_atlPre = true;
	if (!RRCfg_Int("fixed_atlas", 1)) { CONS_Printf("[ATLAS] fixed atlas off (fixed_atlas=0): sheets by LRU\n"); return; }
	int nv = 0, nl = 0, nf = 0;
	for (u32 k = 0; k < s_atlSheets; k++)
	{
		GLMipmap_t& m = s_atlMip[k];
		if (!upload_tex(&m) || !m.downloaded || m.downloaded > s_tex.size()) { nf++; continue; }
		TexEntry& e = s_tex[m.downloaded - 1];
		e.pinned = true;
		if (is_vram(e.tex.data)) nv++; else nl++;
	}
	CONS_Printf("[ATLAS] fixed: %d VRAM, %d linear, %d failed | VRAM free=%uKB linear free=%uKB\n",
		nv, nl, nf, (unsigned)(vramSpaceFree() >> 10), (unsigned)(linearSpaceFree() >> 10));
	if (!nf) { fclose(s_atlf); s_atlf = nullptr; }
}
void c3d_SetTexture(GLMipmap_t* t)
{
	if (!t)
	{
		s_curTex = nullptr;
		s_curBright = nullptr;
		s_uvOn = false;
		return;
	}
	// brightmaps go to unit 1 (s_curBright). The game always
	// picks the base texture first and then its brightmap (if it has one).
	const bool bright = (t->flags & TF_BRIGHTMAP) != 0;
	// atlas. Brightmap of an atlas sprite: not used (its UVs do not match the sheet).
	if (bright && s_uvOn) { s_curBright = nullptr; return; }
	if (!bright)
	{
		s_uvOn = false;
		if (s_atlf && !s_atlPre) atl_preload();
		if (s_atlOn && (t->flags & TF_3DS_ETC))
		{
			auto it = s_etcOf.find(t);
			if (it != s_etcOf.end() && ((u32)it->second & 0xC0000000u) == 0x40000000u)
			{
				const u32 si = (u32)it->second & 0x3FFFFFFFu;
				if (si < s_sprAtl.size() && s_sprAtl[si] >= 0)
				{
					const AtlEnt& a = s_atl[s_sprAtl[si]];
					EtcEnt x; spr_dec(s_spr[si], x);
					c3d_SetTexture(&s_atlMip[a.sheet]);
					if (s_curTex)
					{
						s_uvOn = true;
						s_uvSx = (float)x.w / kAtlDim; s_uvSy = (float)x.h / kAtlDim;
						s_uvOx = (float)a.x / kAtlDim; s_uvOy = (float)a.y / kAtlDim;
					}
					return;
				}
			}
		}
	}
	if (!bright)
		s_curBright = nullptr;
	if (!t->downloaded || t->downloaded > s_tex.size() || !s_tex[t->downloaded - 1].used
		|| s_tex[t->downloaded - 1].owner != t)
	{
		t->downloaded = 0;
		const u64 t0_ = svcGetSystemTick();
		const bool ok_ = upload_tex(t);
		g_gpu_t[1] += svcGetSystemTick() - t0_;
		if (!ok_)
		{
			g_gpu_stat[3]++;
			{ // diag [TEXFAIL]: why it did not upload (line) and which texture it was
				int k = 0; while (k < 7 && s_tfWhy[k] && s_tfWhy[k] != s_upWhy) k++;
				s_tfWhy[k] = s_upWhy; s_tfN[k]++; s_tfW[k] = t->width; s_tfH[k] = t->height; s_tfFmt[k] = (int)t->format; s_tfFl[k] = (int)t->flags;
			}
			if (bright) s_curBright = nullptr;
			else { s_curTex = nullptr; s_bigTexFail = (t->flags & TF_3DS_ETC) && t->width >= 512 && t->height >= 512; }
			return;
		}
		g_gpu_stat[2]++;
	}
	if (!bright) s_bigTexFail = false;
	s_tex[t->downloaded - 1].lastFrame = s_vbufFrame;
	if (bright) s_curBright = &s_tex[t->downloaded - 1].tex;
	else s_curTex = &s_tex[t->downloaded - 1].tex;
}

void c3d_UpdateTexture(GLMipmap_t* t)
{
	if (t && t->downloaded)
		free_tex((int)t->downloaded - 1);
	if (t)
		upload_tex(t);
}

void c3d_DeleteTexture(GLMipmap_t* t)
{
	if (t && t->downloaded)
		free_tex((int)t->downloaded - 1);
}

void c3d_ReadRect(INT32 x, INT32 y, INT32 w, INT32 h, INT32 stride, UINT16* dst)
{
	(void)x; (void)y; (void)stride;
	if (dst) memset(dst, 0, (size_t)w * h * 2);
}

void c3d_GClipRect(INT32 minx, INT32 miny, INT32 maxx, INT32 maxy, float nearclip)
{
	s_clipMinX = minx; s_clipMinY = miny; s_clipMaxX = maxx; s_clipMaxY = maxy;
	s_near = nearclip;
	rebuild_projection();
}

void c3d_ClearMipMapCache(void) { free_all_tex(); }
// The upload ring (768 KB linear) is only needed in 3D; when the menus run short of
// linear RAM ([GPUBLEND] out of memory) give it back: it is re-allocated on the next upload.
extern "C" void I_3DS_FreeWorldTextures(void)
{
	free_all_tex();
	if (s_upRing)
	{
		svcSleepThread(30000000LL); // let any DMA copy still reading the ring finish
		linearFree(s_upRing);
		s_upRing = nullptr; s_upUsed = 0; s_upCount = 0;
	}
}

void c3d_SetSpecialState(hwdspecialstate_t id, INT32 value) { (void)id; (void)value; }

void c3d_DrawModel(model_t*, INT32, float, float, INT32, FTransform*, float, float, UINT8, UINT8, FSurfaceInfo*) {}
void c3d_CreateModelVBOs(model_t*) {}

void c3d_SetTransform(FTransform* tr)
{
	g_gpu_stat[6]++;
	if (tr)
	{
		M4 m;
		if (tr->mirror)
			m = m4_scale(-tr->scalex, tr->scaley, -tr->scalez);
		else if (tr->flip)
			m = m4_scale(tr->scalex, -tr->scaley, -tr->scalez);
		else
			m = m4_scale(tr->scalex, tr->scaley, -tr->scalez);
		if (tr->roll)
			m = m4_mul(m, m4_rotate(tr->rollangle, 0.0f, 0.0f, 1.0f));
		m = m4_mul(m, m4_rotate(tr->anglex, 1.0f, 0.0f, 0.0f));
		m = m4_mul(m, m4_rotate(tr->angley + 270.0f, 0.0f, 1.0f, 0.0f));
		m = m4_mul(m, m4_translate(-tr->x, -tr->z, -tr->y));
		s_mv = m;
		s_fov = tr->fovxangle;
		s_splitscreen = (tr->splitscreen == 1);
		s_shearing = tr->shearing;
		s_shearY = tr->viewaiming;
		s_flipY = tr->flip;
		if (s_skyDrawing && !s_skyGotMV) { s_skyGotMV = true; s_skyMV = m; }
	}
	else
	{
		s_mv = m4_scale(1.0f, 1.0f, -1.0f);
		s_fov = kDefaultFov;
		s_shearing = false;
	}
	rebuild_projection();
}

INT32 c3d_GetTextureUsed(void) { return (INT32)s_texBytes; }

void c3d_PostImgRedraw(float points[SCREENVERTS][SCREENVERTS][2]) { (void)points; }
void c3d_FlushScreenTextures(void) {}
void c3d_StartScreenWipe(void) {}
void c3d_EndScreenWipe(void) {}
void c3d_DoScreenWipe(void) {}
void c3d_DrawIntermissionBG(void) {}
void c3d_MakeScreenTexture(void) {}
void c3d_MakeScreenFinalTexture(void) {}
void c3d_DrawScreenFinalTexture(int w, int h) { (void)w; (void)h; }

boolean c3d_CompileShaders(void) { return false; }
void c3d_CleanShaders(void) {}
void c3d_SetShader(int type) { (void)type; }
void c3d_UnSetShader(void) {}
void c3d_SetShaderInfo(hwdshaderinfo_t info, INT32 value) { (void)info; (void)value; }
void c3d_LoadCustomShader(int number, char* code, size_t size, boolean isfragment)
{
	(void)number; (void)code; (void)size; (void)isfragment;
}

} // namespace

// Vertices/indices are written by the CPU into linear memory: the
// data cache must be flushed before the GPU reads them, or the GPU sees garbage
// (zeros) and draws nothing. Called by presentation before C3D_FrameEnd.
extern "C" void I_3DS_GpuFlush3D(void)
{
	I_3DS_GpuSkipReport();
	// each frame, if linear memory drops below 2 MB, free old textures from there
	// (never those of this frame or the previous one) so the heap's safety net has room.
	{
		const size_t lf = linearSpaceFree();
		if (lf < 2048u * 1024u) evict_lru(2048u * 1024u - lf + 256u * 1024u, 1);
	}
	if (s_vbuf && s_vcount)
		GSPGPU_FlushDataCache(s_vbuf, (u32)(s_vcount * sizeof(FOutVector)));
	if (s_ibuf && s_icount)
		GSPGPU_FlushDataCache(s_ibuf, (u32)(s_icount * sizeof(u16)));
	if (s_sibuf && s_sicount && s_sibufFrame == s_vbufFrame)
		GSPGPU_FlushDataCache(s_sibuf, (u32)(s_sicount * sizeof(u16)));
	if (s_spool && s_spoolDirtyHi > s_spoolDirtyLo)
		GSPGPU_FlushDataCache(s_spool + s_spoolDirtyLo, (u32)((s_spoolDirtyHi - s_spoolDirtyLo) * sizeof(SVert)));
	s_spoolDirtyLo = (size_t)-1; s_spoolDirtyHi = 0;
}

// ---------------------------------------------------------------------------
// Static geometry: API for hw_main.cpp (RR3DS_Static*)
// ---------------------------------------------------------------------------
// Adds a polygon (fan of n vertices) to the pool with a fixed color.
// Returns the index of the first vertex, or -1 if it does not fit.
extern "C" int I_3DS_StaticAdd(const FOutVector* v, int n, unsigned int rgba)
{
	if (!s_spool || n < 3 || s_spoolUsed + (size_t)n > kStaticVerts)
		return -1;
	const size_t first = s_spoolUsed;
	SVert* d = s_spool + first;
	for (int i = 0; i < n; i++)
	{
		d[i].x = v[i].x; d[i].y = v[i].y; d[i].z = v[i].z;
		d[i].s = v[i].s; d[i].t = v[i].t; d[i].c = rgba;
	}
	s_spoolUsed += (size_t)n;
	if (first < s_spoolDirtyLo) s_spoolDirtyLo = first;
	if (s_spoolUsed > s_spoolDirtyHi) s_spoolDirtyHi = s_spoolUsed;
	return (int)first;
}

// shifts the texture coordinates of already-stored vertices
// (walls with a scrolling texture: same geometry, different offset).
extern "C" void I_3DS_StaticShiftST(int first, int n, float ds, float dt)
{
	if (!s_spool || first < 0 || n <= 0 || (size_t)(first + n) > s_spoolUsed)
		return;
	SVert* d = s_spool + first;
	for (int i = 0; i < n; i++) { d[i].s += ds; d[i].t += dt; }
	if ((size_t)first < s_spoolDirtyLo) s_spoolDirtyLo = (size_t)first;
	if ((size_t)(first + n) > s_spoolDirtyHi) s_spoolDirtyHi = (size_t)(first + n);
}

// rewrites y/t of already-stored vertices (walls whose only change is height).
extern "C" void I_3DS_StaticSetYT(int first, int n, const float* y, const float* t)
{
	if (!s_spool || first < 0 || n <= 0 || (size_t)(first + n) > s_spoolUsed)
		return;
	SVert* d = s_spool + first;
	for (int i = 0; i < n; i++) { d[i].y = y[i]; d[i].t = t[i]; }
	if ((size_t)first < s_spoolDirtyLo) s_spoolDirtyLo = (size_t)first;
	if ((size_t)(first + n) > s_spoolDirtyHi) s_spoolDirtyHi = (size_t)(first + n);
}

extern "C" void I_3DS_StaticSetY(int first, int n, float y)
{
	if (!s_spool || first < 0 || n <= 0 || (size_t)(first + n) > s_spoolUsed)
		return;
	SVert* d = s_spool + first;
	for (int i = 0; i < n; i++) d[i].y = y;
	if ((size_t)first < s_spoolDirtyLo) s_spoolDirtyLo = (size_t)first;
	if ((size_t)(first + n) > s_spoolDirtyHi) s_spoolDirtyHi = (size_t)(first + n);
}

extern "C" int  I_3DS_StaticUsed(void) { return (int)s_spoolUsed; }
extern "C" const void* I_3DS_StaticVerts(void) { return s_spool; } // map package generator
extern "C" void I_3DS_StaticTruncate(int used) { if (used >= 0 && (size_t)used <= s_spoolUsed) s_spoolUsed = (size_t)used; }

// Empties the pool. Not possible if draws that read the pool were already sent
// this frame (the GPU has not executed them yet): returns 0 and the caller must retry.
extern "C" int I_3DS_StaticReset(void)
{
	if (s_ready && begin_draw() && s_staticDrawFrame == s_vbufFrame)
		return 0;
	s_spoolUsed = 0;
	s_spoolDirtyLo = (size_t)-1; s_spoolDirtyHi = 0;
	return 1;
}

// Draws pool triangles. 1 = ok, 0 = the texture could not be uploaded,
// -1 = no space/state.
extern "C" int I_3DS_StaticDraw(GLMipmap_t* mip, GLMipmap_t* bright, FBITFIELD flags, const u16* idx, int n)
{
	DrvScope drvScope_(0);
	if (!s_spool || !s_sibuf || n < 3 || !begin_draw())
		return -1;
	if (s_sibufFrame != s_vbufFrame) { s_sibufFrame = s_vbufFrame; s_sicount = 0; }
	if (s_sicount + (size_t)n > kStaticIndices || !ensure_cmd_space())
	{
		g_gpu_stat[7]++;
		return -1;
	}

	const bool wantTex = mip && !(flags & PF_NoTexture);
	if (wantTex)
	{
		c3d_SetTexture(mip);
		if (!s_curTex)
			return 0;
		// brightmap (if lost, it is drawn without glow instead of failing)
		if (bright && (bright->flags & TF_BRIGHTMAP) && (bright->downloaded || bright->data))
			c3d_SetTexture(bright);
	}
	else
	{
		s_curTex = nullptr;
		s_curBright = nullptr;
	}

	if (!s_staticBound)
	{
		s_staticBound = true;
		C3D_BindProgram(&s_sprog);
		C3D_AttrInfo* attr = C3D_GetAttrInfo();
		AttrInfo_Init(attr);
		AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3);         // x,y,z
		AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);         // s,t
		AttrInfo_AddLoader(attr, 2, GPU_UNSIGNED_BYTE, 4); // r,g,b,a
		C3D_BufInfo* buf = C3D_GetBufInfo();
		BufInfo_Init(buf);
		BufInfo_Add(buf, s_spool, sizeof(SVert), 3, 0x210);
		upload_mtx(s_suProj, s_proj);
		upload_mtx(s_suMV, s_mv);
	}

	// Blending/depth as usual; the color comes from the vertex.
	s_stateValid = false;
	apply_state(nullptr, flags & ~PF_Modulated);
	s_stateValid = false;
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	reset_env12();
	bind_unit1(nullptr);
	if (wantTex && g_3ds_dbgflat != 1)
	{
		C3D_TexBind(0, s_curTex);
		// vertex tint + Software light (vertex alpha = light level)
		// + brightmap. Black fade (sectors with a colored fade are not cached).
		if (s_curBright) bind_unit1(s_curBright);
		setup_lit_env(0xFFFFFFFFu, 0, true, s_curBright != nullptr);
	}
	else
	{
		C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
		C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	}

	u16* ib = s_sibuf + s_sicount;
	memcpy(ib, idx, (size_t)n * sizeof(u16));
	s_sicount += (size_t)n;
	s_staticDrawFrame = s_vbufFrame;
	g_gpu_stat[0] += n / 3;
	C3D_DrawElements(GPU_TRIANGLES, n, C3D_UNSIGNED_SHORT, ib);
	return 1;
}

// Back to the normal path's program/buffers.
extern "C" void I_3DS_StaticEnd(void)
{
	if (!s_staticBound)
		return;
	s_staticBound = false;
	s_boundFrame = -1;   // begin_draw() binds the program, attributes and matrices again
	s_stateValid = false;
	s_curTex = nullptr;
}

// DEBUG (SELECT): red triangle in clip coordinates with matrices
// identity. If it is visible, the shader/vertices/flush path works and the
// problem is in the matrices or the game state.
extern "C" void I_3DS_GpuDebugTri(void)
{
	if (!begin_draw())
		return;
	M4 id = m4_identity();
	upload_mtx(s_uProj, id);
	upload_mtx(s_uMV, id);
	FOutVector v[3];
	memset(v, 0, sizeof(v));
	v[0].x = -0.5f; v[0].y = -0.5f; v[0].z = -0.5f;
	v[1].x =  0.5f; v[1].y = -0.5f; v[1].z = -0.5f;
	v[2].x =  0.0f; v[2].y =  0.5f; v[2].z = -0.5f;
	size_t first;
	if (!push_verts(v, 3, &first))
		return;
	FSurfaceInfo surf;
	memset(&surf, 0, sizeof(surf));
	surf.PolyColor.rgba = 0xFF0000FF; // R=255 (byte 0), A=255
	apply_state(&surf, PF_NoTexture | PF_Modulated | PF_NoDepthTest | PF_NoAlphaTest);
	C3D_DrawArrays(GPU_TRIANGLES, (int)first, 3);
	upload_mtx(s_uProj, s_proj);
	upload_mtx(s_uMV, s_mv);
	s_stateValid = false;
}

// [DRV]: time INSIDE the citro3d driver per frame (to know how much could be
// moved to a draw thread on core 1, like MKW 3DS). The pointers are wrapped
// pfn* (each call from hw_main/hwr2) + I_3DS_StaticDraw + I_3DS_SkyTexDraw.
// Sale en ringracers_perf.txt (line "   DRV:").
template <int ID, typename Sig> struct DrvWrap;
template <int ID, typename R, typename... A> struct DrvWrap<ID, R (*)(A...)>
{
	static R (*orig)(A...);
	static R call(A... a)
	{
		const u64 t0 = svcGetSystemTick();
		if constexpr (std::is_void_v<R>)
		{
			orig(a...);
			s_drvT[ID] += svcGetSystemTick() - t0; s_drvC[ID]++;
		}
		else
		{
			R r = orig(a...);
			s_drvT[ID] += svcGetSystemTick() - t0; s_drvC[ID]++;
			return r;
		}
	}
};
template <int ID, typename R, typename... A> R (*DrvWrap<ID, R (*)(A...)>::orig)(A...) = nullptr;
#define DRV_WRAP(ID, field, name) do { using W = DrvWrap<ID, decltype(hwdriver.field)>; 	W::orig = hwdriver.field; hwdriver.field = &W::call; s_drvName[ID] = name; } while (0)
extern "C" void rr3ds_drvstats(FILE *f, int frames)
{
	if (!f || frames <= 0) return;
	const double k = 1.0 / frames / (SYSCLOCK_ARM11 / 1000.0);
	unsigned long long tot = 0;
	for (int i = 0; i < kDrvN; i++) tot += s_drvT[i];
	fprintf(f, "   DRV: total=%.2fms/frame |", tot * k);
	for (int i = 0; i < kDrvN; i++)
		if (s_drvName[i] && s_drvC[i])
			fprintf(f, " %s=%.2fms/%u", s_drvName[i], s_drvT[i] * k, s_drvC[i] / frames);
	fprintf(f, "\n");
	memset(s_drvT, 0, sizeof s_drvT);
	memset(s_drvC, 0, sizeof s_drvC);
}

// Fills the HWD table (hardware/hw_drv.h) with this backend.
extern "C" void I_3DS_BindHWDriver(void)
{
	hwdriver.pfnInit                 = c3d_Init;
	hwdriver.pfnSetPalette           = c3d_SetPalette;
	hwdriver.pfnFinishUpdate         = c3d_FinishUpdate;
	hwdriver.pfnDraw2DLine           = c3d_Draw2DLine;
	hwdriver.pfnDrawPolygon          = c3d_DrawPolygon;
	hwdriver.pfnDrawIndexedTriangles = c3d_DrawIndexedTriangles;
	hwdriver.pfnRenderSkyDome        = c3d_RenderSkyDome;
	hwdriver.pfnSetBlend             = c3d_SetBlend;
	hwdriver.pfnClearBuffer          = c3d_ClearBuffer;
	hwdriver.pfnSetTexture           = c3d_SetTexture;
	hwdriver.pfnUpdateTexture        = c3d_UpdateTexture;
	hwdriver.pfnDeleteTexture        = c3d_DeleteTexture;
	hwdriver.pfnReadRect             = c3d_ReadRect;
	hwdriver.pfnGClipRect            = c3d_GClipRect;
	hwdriver.pfnClearMipMapCache     = c3d_ClearMipMapCache;
	hwdriver.pfnSetSpecialState      = c3d_SetSpecialState;
	hwdriver.pfnDrawModel            = c3d_DrawModel;
	hwdriver.pfnCreateModelVBOs      = c3d_CreateModelVBOs;
	hwdriver.pfnSetTransform         = c3d_SetTransform;
	hwdriver.pfnGetTextureUsed       = c3d_GetTextureUsed;
	hwdriver.pfnPostImgRedraw        = c3d_PostImgRedraw;
	hwdriver.pfnFlushScreenTextures  = c3d_FlushScreenTextures;
	hwdriver.pfnStartScreenWipe      = c3d_StartScreenWipe;
	hwdriver.pfnEndScreenWipe        = c3d_EndScreenWipe;
	hwdriver.pfnDoScreenWipe         = c3d_DoScreenWipe;
	hwdriver.pfnDrawIntermissionBG   = c3d_DrawIntermissionBG;
	hwdriver.pfnMakeScreenTexture    = c3d_MakeScreenTexture;
	hwdriver.pfnMakeScreenFinalTexture = c3d_MakeScreenFinalTexture;
	hwdriver.pfnDrawScreenFinalTexture = c3d_DrawScreenFinalTexture;
	hwdriver.pfnCompileShaders       = c3d_CompileShaders;
	hwdriver.pfnCleanShaders         = c3d_CleanShaders;
	hwdriver.pfnSetShader            = c3d_SetShader;
	hwdriver.pfnUnSetShader          = c3d_UnSetShader;
	hwdriver.pfnSetShaderInfo        = c3d_SetShaderInfo;
	hwdriver.pfnLoadCustomShader     = c3d_LoadCustomShader;
	hwdriver.pfnResetRenderState     = c3d_ResetRenderState;
	s_drvName[0] = "static"; s_drvName[1] = "sky";
	DRV_WRAP(2, pfnDrawPolygon, "poly");
	DRV_WRAP(3, pfnDrawIndexedTriangles, "idxtri");
	DRV_WRAP(4, pfnSetTexture, "settex");
	DRV_WRAP(5, pfnUpdateTexture, "updtex");
	DRV_WRAP(6, pfnSetBlend, "blend");
	DRV_WRAP(7, pfnClearBuffer, "clear");
	DRV_WRAP(8, pfnFinishUpdate, "finish");
	DRV_WRAP(9, pfnSetTransform, "transf");
	DRV_WRAP(10, pfnDrawModel, "model");
	DRV_WRAP(11, pfnRenderSkyDome, "skydome");
	DRV_WRAP(12, pfnGClipRect, "clip");
	DRV_WRAP(13, pfnSetSpecialState, "special");
	DRV_WRAP(14, pfnSetShader, "shader");
	DRV_WRAP(15, pfnDeleteTexture, "deltex");
}

#endif // __3DS__ && HWRENDER
