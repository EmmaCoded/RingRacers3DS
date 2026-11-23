// DR. ROBOTNIK'S RING RACERS (Ring Racers E, PC edition)
//-----------------------------------------------------------------------------
// 3D hardware backend (HWD table, "Legacy GL") on top of Direct3D 9 / 9Ex.
// Draws with the same device as the RHI backend (src/rhi/d3d9). Equivalent to r_opengl.cpp.
//
// Design:
// * Vertices are transformed on the CPU with the same matrices as r_opengl (modelview * GL
// projection) and sent to the GPU already in clip space. The vertex shader only corrects D3D9's half-pixel
// offset. This avoids depending on matrix constants in the shader or on driver clipping.
// * The near plane is clipped on the CPU (triangles crossing behind the camera are split).
// * The HLSL shaders reproduce the "Software-style" lighting of r_opengl.cpp's GLSL. The depth
// used by that formula (gl_FragCoord.z / gl_FragCoord.w) comes out exact from (z_clip + w_clip) / 2.
// * The alpha test is done in the pixel shader (independent of render states).
// * No 3D models, no fade/postimg screens through HWD (hwr2 does them with the RHI), no coronas.
// * Every draw restores ALL of its state, because the RHI shares the device.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------

#ifdef _WIN32

// Windows/DirectX headers BEFORE the game's (the game redefines "boolean").
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3dcommon.h>

#include <math.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../rhi/d3d9/d3d9_factory.hpp"
#include "../hw_drv.h"
#include "r_d3d9.h"

extern "C" void CONS_Printf(const char* fmt, ...);

namespace
{

// ------------------------------------------------------------------ HLSL
const char* kHlslCommon = R"HLSL(
float4 poly_color : register(c8);
float4 tint_color : register(c9);
float4 fade_color : register(c10);
float4 u_light    : register(c11); // x = lighting, y = fade_start, z = fade_end, w = leveltime
float4 u_d3d      : register(c12); // x = 1/viewport width, y = 1/viewport height
float4 u_color    : register(c13); // constant vertex color (glColor)
float4 u_alpha    : register(c14); // x = alpha test: 0 always, 1 = alpha > 0.5, 2 = alpha != 0

sampler2D s_tex    : register(s0);
sampler2D s_bright : register(s1);

struct VSIN
{
	float4 pos : POSITION0; // already in clip space (GL)
	float2 tc : TEXCOORD0;
	float dz : TEXCOORD2;
	float4 col : COLOR0;
};

struct VSOUT
{
	float4 pos : POSITION;
	float2 tc : TEXCOORD0;
	float4 col : TEXCOORD1;
	float dz : TEXCOORD2;
};

VSOUT vs_main(VSIN i)
{
	VSOUT o;
	float4 p = i.pos;
	p.xy += float2(-u_d3d.x, u_d3d.y) * p.w; // D3D9's half pixel cell
	o.pos = p;
	o.tc = i.tc;
	o.dz = i.dz;
#ifdef VCOLOR
	o.col = i.col;
#else
	o.col = u_color;
#endif
	return o;
}

void alpha_test(float a)
{
	if (u_alpha.x > 1.5)
	{
		if (a == 0.0)
			discard;
	}
	else if (u_alpha.x > 0.5)
	{
		if (a <= 0.5)
			discard;
	}
}
)HLSL";

const char* kHlslLighting = R"HLSL(
float R_DoomColormap(float light, float z)
{
	float lightnum = clamp(light / 17.0, 0.0, 15.0);
	float lightz = clamp(z / 16.0, 0.0, 127.0);
	float startmap = (15.0 - lightnum) * 4.0;
	float scale = 160.0 / (lightz + 1.0);
	return startmap - scale * 0.5;
}

float R_DoomLightingEquation(float light, float z)
{
	float colormap = floor(R_DoomColormap(light, z)) + 0.5;
	return clamp(colormap, 0.0, 31.0) / 32.0;
}

void software_tint(inout float4 final_color, float4 base_color, float brightmap_mix)
{
	if (lerp(tint_color.a, 0.0, brightmap_mix) > 0.0)
	{
		float color_bright = sqrt((base_color.r * base_color.r) + (base_color.g * base_color.g) + (base_color.b * base_color.b));
		float strength = sqrt(9.0 * lerp(tint_color.a, 0.0, brightmap_mix));
		final_color.r = clamp((color_bright * (tint_color.r * strength)) + (base_color.r * (1.0 - strength)), 0.0, 1.0);
		final_color.g = clamp((color_bright * (tint_color.g * strength)) + (base_color.g * (1.0 - strength)), 0.0, 1.0);
		final_color.b = clamp((color_bright * (tint_color.b * strength)) + (base_color.b * (1.0 - strength)), 0.0, 1.0);
	}
}

float fade_channel(float c, float f, float intensity)
{
	if (abs(c - f) <= intensity)
		return f;
	else if (c < f)
		return c + intensity;
	return c - intensity;
}

void software_fade(inout float4 final_color, float final_lighting, float z)
{
	float fade_start = u_light.y;
	float fade_end = u_light.z;
	float darkness = R_DoomLightingEquation(final_lighting, z);
	if (fade_start > 0.0 || fade_end < 31.0)
	{
		float fs = fade_start / 31.0;
		float fe = fade_end / 31.0;
		float fd = fe - fs;
		darkness = clamp((darkness - fs) * (1.0 / fd), 0.0, 1.0);
	}
	float colorBrightness = sqrt((final_color.r * final_color.r) + (final_color.g * final_color.g) + (final_color.b * final_color.b));
	float fogBrightness = sqrt((fade_color.r * fade_color.r) + (fade_color.g * fade_color.g) + (fade_color.b * fade_color.b));
	float colorIntensity = 0.0;
	if (colorBrightness < fogBrightness)
	{
		colorIntensity = 1.0 - min(final_color.r, min(final_color.g, final_color.b));
		colorIntensity = abs(colorIntensity - (1.0 - max(fade_color.r, max(fade_color.g, fade_color.b))));
	}
	else
	{
		colorIntensity = max(final_color.r, max(final_color.g, final_color.b));
		colorIntensity = abs(colorIntensity - min(fade_color.r, min(fade_color.g, fade_color.b)));
	}
	colorIntensity *= darkness;
	colorIntensity *= fade_color.a * 10.0;
	final_color.r = fade_channel(final_color.r, fade_color.r, colorIntensity);
	final_color.g = fade_channel(final_color.g, fade_color.g, colorIntensity);
	final_color.b = fade_channel(final_color.b, fade_color.b, colorIntensity);
}
)HLSL";

const char* kPsDefault = R"HLSL(
float4 ps_main(VSOUT i) : COLOR0
{
	float4 c = tex2D(s_tex, i.tc) * poly_color;
	alpha_test(c.a);
	return c;
}
)HLSL";

const char* kPsSoftware = R"HLSL(
float4 ps_main(VSOUT i) : COLOR0
{
	float4 texel = tex2D(s_tex, i.tc);
	float4 base_color = texel * poly_color;
	float4 final_color = base_color;
	float brightmap_mix = floor(tex2D(s_bright, i.tc).r);
	float light_gain = (255.0 - u_light.x) * brightmap_mix;
	float final_lighting = u_light.x + light_gain;
	software_tint(final_color, base_color, brightmap_mix);
	software_fade(final_color, final_lighting, i.dz);
	final_color.a = texel.a * poly_color.a;
	alpha_test(final_color.a);
	return final_color;
}
)HLSL";

const char* kPsWater = R"HLSL(
float4 ps_main(VSOUT i) : COLOR0
{
	float freq = 0.025;
	float amp = 0.025;
	float speed = 2.0;
	float pi = 3.14159;
	float z = i.dz / 2.0;
	float a = -pi * (z * freq) + (u_light.w * speed);
	float sdistort = sin(a) * amp;
	float cdistort = cos(a) * amp;
	float4 texel = tex2D(s_tex, float2(i.tc.x - sdistort, i.tc.y - cdistort));
	float4 base_color = texel * poly_color;
	float4 final_color = base_color;
	float brightmap_mix = floor(tex2D(s_bright, i.tc).r);
	float light_gain = (255.0 - u_light.x) * brightmap_mix;
	float final_lighting = u_light.x + light_gain;
	software_tint(final_color, base_color, brightmap_mix);
	software_fade(final_color, final_lighting, i.dz);
	final_color.a = texel.a * poly_color.a;
	alpha_test(final_color.a);
	return final_color;
}
)HLSL";

const char* kPsFog = R"HLSL(
float4 ps_main(VSOUT i) : COLOR0
{
	float4 base_color = i.col;
	float4 final_color = base_color;
	float final_lighting = u_light.x;
	software_tint(final_color, base_color, 0.0);
	software_fade(final_color, final_lighting, i.dz);
	alpha_test(final_color.a);
	return final_color;
}
)HLSL";

const char* kPsSky = R"HLSL(
float4 ps_main(VSOUT i) : COLOR0
{
	float4 c = tex2D(s_tex, i.tc) * i.col * poly_color;
	alpha_test(c.a);
	return c;
}
)HLSL";

// ------------------------------------------------------------------ programs
enum ProgramKind
{
	PK_DEFAULT,
	PK_SOFTWARE,
	PK_SPRITECLIP,
	PK_WATER,
	PK_FOG,
	PK_SKY,
	PK_COUNT
};

struct Program
{
	IDirect3DVertexShader9* vs = nullptr;
	IDirect3DPixelShader9* ps = nullptr;
};

typedef HRESULT(WINAPI* PFN_D3DCOMPILE_)(
	LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**
);

// ------------------------------------------------------------------ matrices (columns, like OpenGL)
struct M4
{
	float m[16]; // m[col * 4 + row]
};

M4 m4_identity()
{
	M4 r = {};
	r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
	return r;
}

M4 m4_mul(const M4& a, const M4& b) // a * b
{
	M4 r = {};
	for (int c = 0; c < 4; c++)
		for (int row = 0; row < 4; row++)
		{
			float s = 0.0f;
			for (int k = 0; k < 4; k++)
				s += a.m[k * 4 + row] * b.m[c * 4 + k];
			r.m[c * 4 + row] = s;
		}
	return r;
}

M4 m4_scale(float x, float y, float z)
{
	M4 r = m4_identity();
	r.m[0] = x;
	r.m[5] = y;
	r.m[10] = z;
	return r;
}

M4 m4_translate(float x, float y, float z)
{
	M4 r = m4_identity();
	r.m[12] = x;
	r.m[13] = y;
	r.m[14] = z;
	return r;
}

M4 m4_rotate(float deg, float x, float y, float z) // glRotatef
{
	const float rad = deg * 3.14159265358979f / 180.0f;
	const float c = cosf(rad), s = sinf(rad);
	const float len = sqrtf(x * x + y * y + z * z);
	if (len < 1e-8f)
		return m4_identity();
	x /= len;
	y /= len;
	z /= len;
	const float t = 1.0f - c;
	M4 r = m4_identity();
	r.m[0] = t * x * x + c;
	r.m[1] = t * x * y + s * z;
	r.m[2] = t * x * z - s * y;
	r.m[4] = t * x * y - s * z;
	r.m[5] = t * y * y + c;
	r.m[6] = t * y * z + s * x;
	r.m[8] = t * x * z + s * y;
	r.m[9] = t * y * z - s * x;
	r.m[10] = t * z * z + c;
	return r;
}

// ------------------------------------------------------------------ global state
struct Tex
{
	IDirect3DTexture9* tex = nullptr;
	GLMipmap_t* src = nullptr;
	uint32_t w = 0, h = 0, bytes = 0;
	uint32_t flags = 0;
};

// Already-transformed vertex (GL clip space) that is sent to the GPU.
struct TV
{
	float x, y, z, w;
	float s, t, dz;
	uint32_t col; // r,g,b,a in memory (UBYTE4N)
};

IDirect3DDevice9* g_dev = nullptr;
bool g_ex = false;
Program g_prog[PK_COUNT];
bool g_shaders_ready = false;
IDirect3DVertexDeclaration9* g_tv_decl = nullptr;

std::unordered_map<uint32_t, Tex> g_tex;
uint32_t g_next_tex_id = 1;
uint32_t g_tex_bytes = 0;
uint32_t g_cur_tex0 = 0; // 0 = no texture
uint32_t g_cur_tex1 = 0; // brightmap (0 = none)
IDirect3DTexture9* g_white = nullptr;
IDirect3DTexture9* g_black = nullptr;
RGBA_t g_palette[256];

D3DTEXTUREFILTERTYPE g_min_filter = D3DTEXF_POINT;
D3DTEXTUREFILTERTYPE g_mag_filter = D3DTEXF_LINEAR;

M4 g_mv = {};
M4 g_proj = {};
float g_fov = 90.0f;
float g_near = NZCLIP_PLANE;
const float kFar = 32768.0f;
bool g_splitscreen = false;
bool g_shearing = false;
float g_view_aiming = 0.0f;
bool g_flip = false;

INT32 g_screen_w = 0, g_screen_h = 0;
D3DVIEWPORT9 g_vp = {};
bool g_vp_set = false;

int g_cur_shader = SHADER_NONE;
float g_leveltime = 0.0f;
ProgramKind g_cur_kind = PK_DEFAULT;

float g_poly[4] = {1, 1, 1, 1};
float g_tint[4] = {0, 0, 0, 0};
float g_fade[4] = {0, 0, 0, 0};
float g_color[4] = {1, 1, 1, 1}; // glColor
float g_light[4] = {255.0f, 0.0f, 31.0f, 0.0f};

PFN_D3DCOMPILE_ get_compile()
{
	static PFN_D3DCOMPILE_ fn = nullptr;
	static bool tried = false;
	if (!tried)
	{
		tried = true;
		static const char* kDlls[] = {"d3dcompiler_47.dll", "d3dcompiler_46.dll", "d3dcompiler_43.dll"};
		for (const char* dll : kDlls)
		{
			HMODULE m = LoadLibraryA(dll);
			if (m && (fn = reinterpret_cast<PFN_D3DCOMPILE_>(GetProcAddress(m, "D3DCompile"))))
				break;
		}
	}
	return fn;
}

template <typename T>
void rel(T*& p)
{
	if (p)
	{
		p->Release();
		p = nullptr;
	}
}

bool compile_blob(const std::string& src, const char* entry, const char* target, const std::vector<D3D_SHADER_MACRO>& macros, ID3DBlob** out)
{
	PFN_D3DCOMPILE_ compile = get_compile();
	if (!compile)
		return false;
	ID3DBlob* errors = nullptr;
	HRESULT hr = compile(src.data(), src.size(), "hwd", macros.data(), nullptr, entry, target, 0, 0, out, &errors);
	if (FAILED(hr) || !*out)
	{
		CONS_Printf("[D3D9-3D] compile error (%s): %s\n", entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
		rel(errors);
		return false;
	}
	rel(errors);
	return true;
}

bool build_program(ProgramKind kind)
{
	const char* ps_src = kPsDefault;
	bool vcolor = false;
	switch (kind)
	{
	case PK_SOFTWARE:
	case PK_SPRITECLIP:
		ps_src = kPsSoftware;
		break;
	case PK_WATER:
		ps_src = kPsWater;
		break;
	case PK_FOG:
		ps_src = kPsFog;
		break;
	case PK_SKY:
		ps_src = kPsSky;
		vcolor = true;
		break;
	default:
		break;
	}
	std::string src = std::string(kHlslCommon) + kHlslLighting + ps_src;
	std::vector<D3D_SHADER_MACRO> macros;
	if (vcolor)
		macros.push_back({"VCOLOR", "1"});
	macros.push_back({nullptr, nullptr});

	ID3DBlob* vsb = nullptr;
	ID3DBlob* psb = nullptr;
	const bool ok = compile_blob(src, "vs_main", "vs_3_0", macros, &vsb) && compile_blob(src, "ps_main", "ps_3_0", macros, &psb);
	bool created = false;
	if (ok)
	{
		created = SUCCEEDED(g_dev->CreateVertexShader(static_cast<const DWORD*>(vsb->GetBufferPointer()), &g_prog[kind].vs)) &&
				  SUCCEEDED(g_dev->CreatePixelShader(static_cast<const DWORD*>(psb->GetBufferPointer()), &g_prog[kind].ps));
	}
	rel(vsb);
	rel(psb);
	return created;
}

ProgramKind kind_for_shader(int shader)
{
	switch (shader)
	{
	case SHADER_FLOOR:
	case SHADER_WALL:
	case SHADER_SPRITE:
	case SHADER_MODEL:
	case SHADER_MODEL_LIGHTING:
		return PK_SOFTWARE;
	case SHADER_SPRITECLIPHACK:
		return PK_SPRITECLIP;
	case SHADER_WATER:
		return PK_WATER;
	case SHADER_FOG:
		return PK_FOG;
	case SHADER_SKY:
		return PK_SKY;
	default:
		return PK_DEFAULT;
	}
}

IDirect3DTexture9* make_solid(uint32_t argb)
{
	IDirect3DTexture9* t = nullptr;
	if (FAILED(g_dev->CreateTexture(1, 1, 1, g_ex ? D3DUSAGE_DYNAMIC : 0, D3DFMT_A8R8G8B8, g_ex ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED, &t, nullptr)))
		return nullptr;
	D3DLOCKED_RECT lr;
	if (SUCCEEDED(t->LockRect(0, &lr, nullptr, 0)))
	{
		*static_cast<uint32_t*>(lr.pBits) = argb;
		t->UnlockRect(0);
	}
	return t;
}

void release_all_textures()
{
	for (auto& kv : g_tex)
	{
		if (kv.second.src)
			kv.second.src->downloaded = 0; // the game re-uploads it with UpdateTexture
		rel(kv.second.tex);
	}
	g_tex.clear();
	g_tex_bytes = 0;
	g_cur_tex0 = g_cur_tex1 = 0;
}

void on_before_reset()
{
	release_all_textures();
	rel(g_white);
	rel(g_black);
}

void on_after_reset()
{
	g_white = make_solid(0xFFFFFFFFu);
	g_black = make_solid(0xFF000000u);
	g_vp_set = false;
}

// ------------------------------------------------------------------ camera
void rebuild_projection()
{
	float fov = g_fov;
	float aspect = 1.0f;
	if (g_splitscreen)
	{
		fov = (float)(atan(tan(fov * 3.14159265358979 / 360.0) * 0.8) * 360.0 / 3.14159265358979);
		aspect = 2.0f;
	}
	const float radians = fov / 2.0f * 3.14159265358979f / 180.0f;
	const float sine = sinf(radians);
	const float dz = kFar - g_near;
	M4 p = {};
	if (fabsf(dz) > 1e-30f && fabsf(sine) > 1e-30f)
	{
		const float cot = cosf(radians) / sine;
		p.m[0] = cot / aspect;
		p.m[5] = cot;
		p.m[10] = -(kFar + g_near) / dz;
		p.m[11] = -1.0f;
		p.m[14] = -2.0f * g_near * kFar / dz;
	}
	else
		p = m4_identity();
	if (g_shearing)
	{
		float fdy = g_view_aiming * 2.0f;
		if (g_flip)
			fdy = -fdy;
		p = m4_mul(m4_translate(0.0f, -fdy / (float)BASEVIDHEIGHT, 0.0f), p);
	}
	g_proj = p;
}

// ------------------------------------------------------------------ transform and clipping (CPU)
struct Clip4
{
	float x, y, z, w;
};

// clip = projection * (mv * v)
Clip4 to_clip(const M4& mv, const FOutVector& v)
{
	const float p[4] = {v.x, v.y, v.z, 1.0f};
	float e[4] = {0, 0, 0, 0};
	for (int r = 0; r < 4; r++)
		for (int k = 0; k < 4; k++)
			e[r] += mv.m[k * 4 + r] * p[k];
	float c[4] = {0, 0, 0, 0};
	for (int r = 0; r < 4; r++)
		for (int k = 0; k < 4; k++)
			c[r] += g_proj.m[k * 4 + r] * e[k];
	return {c[0], c[1], c[2], c[3]};
}

FOutVector lerp_vert(const FOutVector& a, const FOutVector& b, float t)
{
	FOutVector r;
	r.x = a.x + (b.x - a.x) * t;
	r.y = a.y + (b.y - a.y) * t;
	r.z = a.z + (b.z - a.z) * t;
	r.s = a.s + (b.s - a.s) * t;
	r.t = a.t + (b.t - a.t) * t;
	return r;
}

TV make_tv(const Clip4& c, float s, float t, bool spriteclip, uint32_t col)
{
	float z = c.z;
	if (spriteclip)
		z -= 80.0f / z; // r_opengl: gl_Position.z -= 80.0 / gl_Position.z
	TV o;
	o.x = c.x;
	o.y = c.y;
	o.w = c.w;
	o.z = (z + c.w) * 0.5f; // z GL [-w, w] -> z D3D [0, w]
	o.dz = o.z;             // = r_opengl shader's gl_FragCoord.z / gl_FragCoord.w
	o.s = s;
	o.t = t;
	o.col = col;
	return o;
}

// Draws triangles (indices into `verts`): clips GL's near plane on the CPU, transforms and sends the
// triangle list to the GPU. `colors` (optional) is the per-vertex color (sky).
void draw_triangles(const M4& mv, const FOutVector* verts, UINT32 nverts, const UINT32* idx, FUINT nidx, const uint32_t* colors)
{
	const bool spriteclip = (g_cur_kind == PK_SPRITECLIP);

	static std::vector<Clip4> clip;
	static std::vector<float> dist;
	clip.resize(nverts);
	dist.resize(nverts);
	bool any_out = false;
	for (UINT32 i = 0; i < nverts; i++)
	{
		clip[i] = to_clip(mv, verts[i]);
		dist[i] = clip[i].z + clip[i].w; // GL near plane: z + w >= 0 is "inside"
		if (dist[i] < 0.0f)
			any_out = true;
	}

	static std::vector<TV> flat;
	flat.clear();
	flat.reserve(nidx);

	if (!any_out)
	{
		for (FUINT i = 0; i < nidx; i++)
		{
			const UINT32 k = idx[i];
			flat.push_back(make_tv(clip[k], verts[k].s, verts[k].t, spriteclip, colors ? colors[k] : 0xFFFFFFFFu));
		}
	}
	else
	{
		// Sutherland-Hodgman per triangle against the near plane. Clipping is linear in object space.
		for (FUINT t = 0; t + 2 < nidx; t += 3)
		{
			const UINT32 tri[3] = {idx[t], idx[t + 1], idx[t + 2]};
			const int inside = (dist[tri[0]] >= 0.0f) + (dist[tri[1]] >= 0.0f) + (dist[tri[2]] >= 0.0f);
			if (inside == 0)
				continue;
			if (inside == 3)
			{
				for (int k = 0; k < 3; k++)
					flat.push_back(make_tv(clip[tri[k]], verts[tri[k]].s, verts[tri[k]].t, spriteclip, colors ? colors[tri[k]] : 0xFFFFFFFFu));
				continue;
			}
			TV poly[4];
			int pn = 0;
			for (int k = 0; k < 3; k++)
			{
				const UINT32 cur = tri[k], nxt = tri[(k + 1) % 3];
				const bool cin = dist[cur] >= 0.0f, nin = dist[nxt] >= 0.0f;
				if (cin)
					poly[pn++] = make_tv(clip[cur], verts[cur].s, verts[cur].t, spriteclip, colors ? colors[cur] : 0xFFFFFFFFu);
				if (cin != nin)
				{
					const float tt = dist[cur] / (dist[cur] - dist[nxt]);
					const FOutVector nv = lerp_vert(verts[cur], verts[nxt], tt);
					poly[pn++] = make_tv(to_clip(mv, nv), nv.s, nv.t, spriteclip, colors ? colors[cur] : 0xFFFFFFFFu);
				}
			}
			for (int k = 1; k + 1 < pn; k++)
			{
				flat.push_back(poly[0]);
				flat.push_back(poly[k]);
				flat.push_back(poly[k + 1]);
			}
		}
	}
	if (flat.empty())
		return;
	g_dev->SetVertexDeclaration(g_tv_decl);
	g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(flat.size() / 3), flat.data(), sizeof(TV));
}

// ------------------------------------------------------------------ draw state
bool ensure_device();

// Sets up the state to draw with the given polygon flags. false = do not draw.
bool prepare(FSurfaceInfo* surf, FBITFIELD flags)
{
	if (!ensure_device())
		return false;
	if (flags & PF_Corona)
		return false; // coronas not supported

	srb2::rhi::d3d9_prepare_external_draw();

	if (!g_vp_set)
	{
		g_vp.X = 0;
		g_vp.Y = 0;
		g_vp.Width = g_screen_w > 0 ? (DWORD)g_screen_w : 1;
		g_vp.Height = g_screen_h > 0 ? (DWORD)g_screen_h : 1;
		g_vp.MinZ = 0.0f;
		g_vp.MaxZ = 1.0f;
		g_vp_set = true;
	}
	g_dev->SetViewport(&g_vp);

	// --- polygon colors ---
	if (surf)
	{
		if (flags & PF_Modulated)
		{
			g_color[0] = surf->PolyColor.s.red / 255.0f;
			g_color[1] = surf->PolyColor.s.green / 255.0f;
			g_color[2] = surf->PolyColor.s.blue / 255.0f;
			g_color[3] = surf->PolyColor.s.alpha / 255.0f;
		}
		if (flags & (PF_Modulated | PF_ColorMapped))
		{
			g_poly[0] = surf->PolyColor.s.red / 255.0f;
			g_poly[1] = surf->PolyColor.s.green / 255.0f;
			g_poly[2] = surf->PolyColor.s.blue / 255.0f;
			g_poly[3] = surf->PolyColor.s.alpha / 255.0f;
		}
		if (flags & PF_ColorMapped)
		{
			g_tint[0] = surf->TintColor.s.red / 255.0f;
			g_tint[1] = surf->TintColor.s.green / 255.0f;
			g_tint[2] = surf->TintColor.s.blue / 255.0f;
			g_tint[3] = surf->TintColor.s.alpha / 255.0f;
			g_fade[0] = surf->FadeColor.s.red / 255.0f;
			g_fade[1] = surf->FadeColor.s.green / 255.0f;
			g_fade[2] = surf->FadeColor.s.blue / 255.0f;
			g_fade[3] = surf->FadeColor.s.alpha / 255.0f;
		}
		g_light[0] = (float)surf->LightInfo.light_level;
		g_light[1] = (float)surf->LightInfo.fade_start;
		g_light[2] = (float)surf->LightInfo.fade_end;
	}
	else
	{
		g_light[0] = 255.0f;
		g_light[1] = 0.0f;
		g_light[2] = 31.0f;
	}
	g_light[3] = g_leveltime;

	// --- blend mode ---
	D3DBLEND src = D3DBLEND_ONE, dst = D3DBLEND_ZERO;
	D3DBLENDOP op = D3DBLENDOP_ADD;
	float alpha_mode = 1.0f; // default: alpha > 0.5
	const FBITFIELD blendbits = flags & (PF_Blending & ~(PF_Masked | PF_Occlude));
	if (flags & PF_Fog)
	{
		src = D3DBLEND_SRCALPHA;
		dst = D3DBLEND_SRCCOLOR;
		alpha_mode = 0.0f;
	}
	else
	{
		bool blending = true;
		switch (blendbits)
		{
		case PF_Translucent:
			src = D3DBLEND_SRCALPHA;
			dst = D3DBLEND_INVSRCALPHA;
			break;
		case PF_Additive:
		case PF_Subtractive:
		case PF_ReverseSubtract:
			src = D3DBLEND_SRCALPHA;
			dst = D3DBLEND_ONE;
			break;
		case PF_Environment:
			src = D3DBLEND_ONE;
			dst = D3DBLEND_INVSRCALPHA;
			break;
		case PF_Multiplicative:
			src = D3DBLEND_DESTCOLOR;
			dst = D3DBLEND_ZERO;
			break;
		default:
			blending = false;
			break;
		}
		if (!blending)
		{
			if (flags & PF_Masked)
			{
				src = D3DBLEND_SRCALPHA;
				dst = D3DBLEND_ZERO;
			}
		}
		else
			alpha_mode = 2.0f; // alpha != 0
	}
	if (blendbits == PF_Subtractive)
		op = D3DBLENDOP_SUBTRACT;
	else if (blendbits == PF_ReverseSubtract)
		op = D3DBLENDOP_REVSUBTRACT;
	if (flags & PF_NoAlphaTest)
		alpha_mode = 0.0f;

	const bool need_blend = !(src == D3DBLEND_ONE && dst == D3DBLEND_ZERO);
	g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, need_blend ? TRUE : FALSE);
	if (need_blend)
	{
		g_dev->SetRenderState(D3DRS_SRCBLEND, src);
		g_dev->SetRenderState(D3DRS_DESTBLEND, dst);
		g_dev->SetRenderState(D3DRS_BLENDOP, op);
	}
	g_dev->SetRenderState(D3DRS_COLORWRITEENABLE, (flags & PF_Invisible) ? 0 : 0xF);

	// --- depth and face ---
	g_dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
	g_dev->SetRenderState(D3DRS_ZFUNC, (flags & PF_NoDepthTest) ? D3DCMP_ALWAYS : D3DCMP_LESSEQUAL);
	g_dev->SetRenderState(D3DRS_ZWRITEENABLE, (flags & PF_Occlude) ? TRUE : FALSE);
	g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	g_dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
	g_dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
	g_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
	g_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	g_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	g_dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
	{
		// glPolygonOffset(-1, -1) for "decals"
		const float bias = (flags & PF_Decal) ? -0.000003f : 0.0f;
		const float slope = (flags & PF_Decal) ? -1.0f : 0.0f;
		g_dev->SetRenderState(D3DRS_DEPTHBIAS, *reinterpret_cast<const DWORD*>(&bias));
		g_dev->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, *reinterpret_cast<const DWORD*>(&slope));
	}

	// --- shaders ---
	ProgramKind kind = kind_for_shader(g_cur_shader);
	if (!g_prog[kind].vs || !g_prog[kind].ps)
		kind = PK_DEFAULT;
	g_cur_kind = kind;
	g_dev->SetVertexShader(g_prog[kind].vs);
	g_dev->SetPixelShader(g_prog[kind].ps);

	float all[7][4];
	memcpy(all[0], g_poly, 16);  // c8
	memcpy(all[1], g_tint, 16);  // c9
	memcpy(all[2], g_fade, 16);  // c10
	memcpy(all[3], g_light, 16); // c11
	all[4][0] = 1.0f / (float)std::max<DWORD>(g_vp.Width, 1);
	all[4][1] = 1.0f / (float)std::max<DWORD>(g_vp.Height, 1);
	all[4][2] = all[4][3] = 0.0f; // c12
	memcpy(all[5], g_color, 16);  // c13
	all[6][0] = alpha_mode;       // c14
	all[6][1] = all[6][2] = all[6][3] = 0.0f;
	g_dev->SetVertexShaderConstantF(8, &all[0][0], 7);
	g_dev->SetPixelShaderConstantF(8, &all[0][0], 7);

	// --- textures ---
	IDirect3DTexture9* t0 = g_white;
	bool nearest0 = false, wrapx = true, wrapy = true;
	if (!(flags & PF_NoTexture) && g_cur_tex0)
	{
		auto it = g_tex.find(g_cur_tex0);
		if (it != g_tex.end() && it->second.tex)
		{
			t0 = it->second.tex;
			const uint32_t tf = it->second.flags;
			nearest0 = (tf & TF_TRANSPARENT) != 0;
			wrapx = (tf & TF_WRAPX) != 0;
			wrapy = (tf & TF_WRAPY) != 0;
		}
	}
	if (flags & PF_ForceWrapX)
		wrapx = true;
	if (flags & PF_ForceWrapY)
		wrapy = true;
	if (flags & PF_RemoveYWrap)
		wrapy = false;
	g_dev->SetTexture(0, t0);
	g_dev->SetSamplerState(0, D3DSAMP_ADDRESSU, wrapx ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP);
	g_dev->SetSamplerState(0, D3DSAMP_ADDRESSV, wrapy ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP);
	g_dev->SetSamplerState(0, D3DSAMP_MINFILTER, nearest0 ? D3DTEXF_POINT : g_min_filter);
	g_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, nearest0 ? D3DTEXF_POINT : g_mag_filter);
	g_dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);

	IDirect3DTexture9* t1 = g_black;
	if (g_cur_tex1)
	{
		auto it = g_tex.find(g_cur_tex1);
		if (it != g_tex.end() && it->second.tex)
			t1 = it->second.tex;
	}
	g_dev->SetTexture(1, t1);
	g_dev->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	g_dev->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	g_dev->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_POINT);
	g_dev->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
	g_dev->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
	return true;
}

// ================================================================== HWD table functions
// The device is created when the window opens, AFTER VID_StartupOpenGL: it is picked up the first time it is used.
bool ensure_device()
{
	if (g_dev)
		return true;
	g_dev = static_cast<IDirect3DDevice9*>(srb2::rhi::d3d9_current_device());
	if (!g_dev)
		return false;
	g_ex = srb2::rhi::d3d9_current_is_ex();
	srb2::rhi::d3d9_set_reset_hooks(on_before_reset, on_after_reset);

	if (!g_white)
		g_white = make_solid(0xFFFFFFFFu);
	if (!g_black)
		g_black = make_solid(0xFF000000u);

	if (!g_shaders_ready)
	{
		bool ok = true;
		for (int k = 0; k < PK_COUNT; k++)
			ok = build_program(static_cast<ProgramKind>(k)) && ok;
		if (!ok)
			CONS_Printf("[D3D9-3D] some 3D shaders failed: using the default program instead\n");
		g_shaders_ready = true;
	}

	if (!g_tv_decl)
	{
		static const D3DVERTEXELEMENT9 elems[] = {
			{0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
			{0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
			{0, 24, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 2},
			{0, 28, D3DDECLTYPE_UBYTE4N, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
			D3DDECL_END()};
		g_dev->CreateVertexDeclaration(elems, &g_tv_decl);
	}

	g_mv = m4_scale(1.0f, 1.0f, -1.0f);
	rebuild_projection();
	CONS_Printf("[D3D9-3D] hardware 3D on Direct3D 9%s ready\n", g_ex ? "Ex" : "");
	return true;
}

boolean d3d9_Init(void)
{
	return true; // the device is acquired on first use (ensure_device)
}

void d3d9_SetPalette(RGBA_t* ppal)
{
	if (ppal)
		memcpy(g_palette, ppal, sizeof(g_palette));
	release_all_textures(); // indexed textures depend on the palette
}

void d3d9_FinishUpdate(INT32) {}

void d3d9_Draw2DLine(F2DCoord*, F2DCoord*, RGBA_t) {}

void d3d9_DrawPolygon(FSurfaceInfo* pSurf, FOutVector* pOutVerts, FUINT iNumPts, FBITFIELD PolyFlags)
{
	if (iNumPts < 3 || (PolyFlags & PF_WireFrame) || !prepare(pSurf, PolyFlags))
		return;
	static std::vector<UINT32> fan;
	fan.clear();
	for (FUINT i = 1; i + 1 < iNumPts; i++)
	{
		fan.push_back(0);
		fan.push_back(i);
		fan.push_back(i + 1);
	}
	draw_triangles(g_mv, pOutVerts, (UINT32)iNumPts, fan.data(), (FUINT)fan.size(), nullptr);
}

void d3d9_DrawIndexedTriangles(FSurfaceInfo* pSurf, FOutVector* pOutVerts, FUINT iNumPts, FBITFIELD PolyFlags, UINT32* IndexArray)
{
	if (iNumPts < 3 || !prepare(pSurf, PolyFlags))
		return;
	UINT32 maxi = 0;
	for (FUINT i = 0; i < iNumPts; i++)
		maxi = std::max<UINT32>(maxi, IndexArray[i]);
	draw_triangles(g_mv, pOutVerts, maxi + 1, IndexArray, iNumPts, nullptr);
}

void d3d9_RenderSkyDome(gl_sky_t* sky)
{
	if (!sky || !sky->data || !sky->loops)
		return;
	g_cur_shader = SHADER_SKY;
	if (!prepare(nullptr, PF_Occlude | PF_Modulated))
	{
		g_cur_shader = SHADER_NONE;
		return;
	}

	// dome transform (like r_opengl): vertical scale and 270 degree rotation
	M4 mv = m4_mul(g_mv, m4_scale(1.0f, (float)sky->height / 200.0f, 1.0f));
	mv = m4_mul(mv, m4_rotate(270.0f, 0.0f, 1.0f, 0.0f));

	// dome vertices as FOutVector + per-vertex color
	static std::vector<FOutVector> verts;
	static std::vector<uint32_t> colors;
	verts.resize(sky->vertex_count);
	colors.resize(sky->vertex_count);
	for (int i = 0; i < sky->vertex_count; i++)
	{
		verts[i].x = sky->data[i].x;
		verts[i].y = sky->data[i].y;
		verts[i].z = sky->data[i].z;
		verts[i].s = sky->data[i].u;
		verts[i].t = sky->data[i].v;
		const unsigned char rgba[4] = {sky->data[i].r, sky->data[i].g, sky->data[i].b, sky->data[i].a};
		memcpy(&colors[i], rgba, 4);
	}

	static std::vector<UINT32> idx;
	for (int j = 0; j < 2; j++)
	{
		for (int i = 0; i < sky->loopcount; i++)
		{
			const gl_skyloopdef_t* loop = &sky->loops[i];
			if (j == 0 ? loop->use_texture : !loop->use_texture)
				continue;
			if (loop->vertexcount < 3)
				continue;
			idx.clear();
			const int base = loop->vertexindex;
			if (loop->mode == HWD_SKYLOOP_FAN)
			{
				for (int k = 1; k + 1 < loop->vertexcount; k++)
				{
					idx.push_back(base);
					idx.push_back(base + k);
					idx.push_back(base + k + 1);
				}
			}
			else if (loop->mode == HWD_SKYLOOP_STRIP)
			{
				for (int k = 0; k + 2 < loop->vertexcount; k++)
				{
					idx.push_back(base + k);
					idx.push_back(base + k + 1);
					idx.push_back(base + k + 2);
				}
			}
			else
				continue;
			draw_triangles(mv, verts.data(), (UINT32)verts.size(), idx.data(), (FUINT)idx.size(), colors.data());
		}
	}
	g_cur_shader = SHADER_NONE;
}

void d3d9_SetBlend(FBITFIELD PolyFlags)
{
	// The real state is applied on every draw (prepare). PF_NoTexture removes the current texture.
	if (PolyFlags & PF_NoTexture)
		g_cur_tex0 = 0;
}

void d3d9_ResetRenderState(void)
{
	g_cur_tex0 = g_cur_tex1 = 0;
	g_cur_shader = SHADER_NONE;
	g_vp_set = false;
}

void d3d9_ClearBuffer(FBOOLEAN ColorMask, FBOOLEAN DepthMask, FRGBAFloat* ClearColor)
{
	if (!ensure_device())
		return;
	srb2::rhi::d3d9_prepare_external_draw();
	DWORD flags = 0;
	D3DCOLOR color = D3DCOLOR_ARGB(255, 0, 0, 0);
	if (ColorMask)
	{
		flags |= D3DCLEAR_TARGET;
		if (ClearColor)
			color = D3DCOLOR_COLORVALUE(ClearColor->red, ClearColor->green, ClearColor->blue, ClearColor->alpha);
	}
	if (DepthMask)
		flags |= D3DCLEAR_ZBUFFER;
	if (!flags)
		return;
	// D3D9 Clear respects the viewport; GL clears the whole buffer: full viewport while clearing.
	unsigned bw = 0, bh = 0;
	srb2::rhi::d3d9_backbuffer_size(&bw, &bh);
	D3DVIEWPORT9 full = {0, 0, bw ? bw : 1, bh ? bh : 1, 0.0f, 1.0f};
	D3DVIEWPORT9 old = {};
	g_dev->GetViewport(&old);
	g_dev->SetViewport(&full);
	g_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
	g_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
	g_dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
	g_dev->Clear(0, nullptr, flags, color, 1.0f, 0);
	g_dev->SetViewport(&old);
}

void d3d9_UpdateTexture(GLMipmap_t* pTexInfo)
{
	if (!ensure_device() || !pTexInfo)
		return;
	const uint32_t w = pTexInfo->width, h = pTexInfo->height;
	if (!w || !h)
		return;

	uint32_t id = pTexInfo->downloaded;
	Tex* t = nullptr;
	auto it = id ? g_tex.find(id) : g_tex.end();
	if (it != g_tex.end())
		t = &it->second;
	else
	{
		id = g_next_tex_id++;
		pTexInfo->downloaded = id;
		Tex nt;
		HRESULT hr = g_dev->CreateTexture(w, h, 1, g_ex ? D3DUSAGE_DYNAMIC : 0, D3DFMT_A8R8G8B8, g_ex ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED, &nt.tex, nullptr);
		if (FAILED(hr) || !nt.tex)
		{
			pTexInfo->downloaded = 0;
			return;
		}
		nt.w = w;
		nt.h = h;
		nt.bytes = w * h * 4;
		g_tex_bytes += nt.bytes;
		t = &(g_tex[id] = nt);
	}
	t->src = pTexInfo;

	D3DLOCKED_RECT lr;
	if (FAILED(t->tex->LockRect(0, &lr, nullptr, g_ex ? D3DLOCK_DISCARD : 0)))
		return;

	const unsigned char* img = static_cast<const unsigned char*>(pTexInfo->data);
	for (uint32_t j = 0; j < h; j++)
	{
		unsigned char* d = static_cast<unsigned char*>(lr.pBits) + j * lr.Pitch;
		for (uint32_t i = 0; i < w; i++)
		{
			uint8_t r = 0, g = 0, b = 0, a = 255;
			switch (pTexInfo->format)
			{
			case GL_TEXFMT_P_8:
			case GL_TEXFMT_AP_88:
			{
				const uint8_t idx = *img++;
				if (idx == HWR_PATCHES_CHROMAKEY_COLORINDEX && (pTexInfo->flags & TF_CHROMAKEYED))
				{
					r = g = b = a = 0;
					pTexInfo->flags |= TF_TRANSPARENT;
				}
				else
				{
					r = g_palette[idx].s.red;
					g = g_palette[idx].s.green;
					b = g_palette[idx].s.blue;
					a = g_palette[idx].s.alpha;
				}
				if (pTexInfo->format == GL_TEXFMT_AP_88)
				{
					const uint8_t ap = *img++;
					if (!(pTexInfo->flags & TF_CHROMAKEYED))
						a = ap;
				}
				break;
			}
			case GL_TEXFMT_RGBA:
				r = *img++;
				g = *img++;
				b = *img++;
				a = *img++;
				break;
			case GL_TEXFMT_ALPHA_INTENSITY_88:
				r = g = b = *img++;
				a = *img++;
				break;
			case GL_TEXFMT_ALPHA_8:
				r = g = b = 255; // fade mask: the color does not count
				a = *img++;
				break;
			default:
				break;
			}
			d[i * 4 + 0] = b;
			d[i * 4 + 1] = g;
			d[i * 4 + 2] = r;
			d[i * 4 + 3] = a;
		}
	}
	t->tex->UnlockRect(0);
	t->flags = pTexInfo->flags;
}

void d3d9_SetTexture(GLMipmap_t* pTexInfo)
{
	if (!pTexInfo)
	{
		g_cur_tex0 = 0;
		return;
	}
	if (!pTexInfo->downloaded || g_tex.find(pTexInfo->downloaded) == g_tex.end())
	{
		pTexInfo->downloaded = 0;
		d3d9_UpdateTexture(pTexInfo);
	}
	if (!pTexInfo->downloaded)
		return;
	if (pTexInfo->flags & TF_BRIGHTMAP)
		g_cur_tex1 = pTexInfo->downloaded;
	else
	{
		g_cur_tex0 = pTexInfo->downloaded;
		g_cur_tex1 = 0; // assigned later, if needed
	}
}

void d3d9_DeleteTexture(GLMipmap_t* pTexInfo)
{
	if (!pTexInfo || !pTexInfo->downloaded)
		return;
	auto it = g_tex.find(pTexInfo->downloaded);
	if (it != g_tex.end())
	{
		g_tex_bytes -= std::min(g_tex_bytes, it->second.bytes);
		if (g_cur_tex0 == pTexInfo->downloaded)
			g_cur_tex0 = 0;
		if (g_cur_tex1 == pTexInfo->downloaded)
			g_cur_tex1 = 0;
		rel(it->second.tex);
		g_tex.erase(it);
	}
	pTexInfo->downloaded = 0;
}

void d3d9_ClearMipMapCache(void)
{
	release_all_textures();
}

void d3d9_ReadRect(INT32 x, INT32 y, INT32 width, INT32 height, INT32 dst_stride, UINT16* dst_data)
{
	if (!ensure_device() || width <= 0 || height <= 0)
		return;
	IDirect3DSurface9* bb = nullptr;
	if (FAILED(g_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)))
		return;
	D3DSURFACE_DESC sd = {};
	bb->GetDesc(&sd);
	IDirect3DSurface9* sys = nullptr;
	if (SUCCEEDED(g_dev->CreateOffscreenPlainSurface(sd.Width, sd.Height, sd.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) && SUCCEEDED(g_dev->GetRenderTargetData(bb, sys)))
	{
		D3DLOCKED_RECT lr;
		if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
		{
			// GL: y from the bottom; the output is top to bottom => D3D row = H - y - height + r
			for (INT32 r = 0; r < height; r++)
			{
				const INT32 row = (INT32)sd.Height - y - height + r;
				if (row < 0 || row >= (INT32)sd.Height)
					continue;
				const unsigned char* s = static_cast<const unsigned char*>(lr.pBits) + row * lr.Pitch + x * 4;
				if (dst_stride == width * 3)
				{
					unsigned char* d = reinterpret_cast<unsigned char*>(dst_data) + r * dst_stride;
					for (INT32 i = 0; i < width; i++)
					{
						d[i * 3 + 0] = s[i * 4 + 2];
						d[i * 3 + 1] = s[i * 4 + 1];
						d[i * 3 + 2] = s[i * 4 + 0];
					}
				}
				else
				{
					for (INT32 i = 0; i < width; i++)
						dst_data[r * width + i] = (UINT16)(((s[i * 4 + 2] >> 3) << 11) | ((s[i * 4 + 1] >> 2) << 5) | (s[i * 4 + 0] >> 3));
				}
			}
			sys->UnlockRect();
		}
	}
	rel(sys);
	bb->Release();
}

void d3d9_GClipRect(INT32 minx, INT32 miny, INT32 maxx, INT32 maxy, float nearclip)
{
	// GL: viewport(minx, H - maxy, ...) from the bottom; in D3D (origin at top) that is (minx, miny).
	g_vp.X = (DWORD)std::max<INT32>(minx, 0);
	g_vp.Y = (DWORD)std::max<INT32>(miny, 0);
	g_vp.Width = (DWORD)std::max<INT32>(maxx - minx, 1);
	g_vp.Height = (DWORD)std::max<INT32>(maxy - miny, 1);
	g_vp.MinZ = 0.0f;
	g_vp.MaxZ = 1.0f;
	g_vp_set = true;
	g_near = nearclip;
	rebuild_projection();
}

void d3d9_SetSpecialState(hwdspecialstate_t IdState, INT32 Value)
{
	if (IdState != HWD_SET_TEXTUREFILTERMODE)
		return;
	switch (Value)
	{
	case HWD_SET_TEXTUREFILTER_TRILINEAR:
	case HWD_SET_TEXTUREFILTER_BILINEAR:
		g_min_filter = g_mag_filter = D3DTEXF_LINEAR;
		break;
	case HWD_SET_TEXTUREFILTER_POINTSAMPLED:
		g_min_filter = g_mag_filter = D3DTEXF_POINT;
		break;
	case HWD_SET_TEXTUREFILTER_MIXED1:
		g_min_filter = D3DTEXF_POINT;
		g_mag_filter = D3DTEXF_LINEAR;
		break;
	case HWD_SET_TEXTUREFILTER_MIXED2:
	case HWD_SET_TEXTUREFILTER_MIXED3:
		g_min_filter = D3DTEXF_LINEAR;
		g_mag_filter = D3DTEXF_POINT;
		break;
	default:
		g_mag_filter = D3DTEXF_LINEAR;
		g_min_filter = D3DTEXF_POINT;
		break;
	}
}

void d3d9_DrawModel(model_t*, INT32, float, float, INT32, FTransform*, float, float, UINT8, UINT8, FSurfaceInfo*) {}
void d3d9_CreateModelVBOs(model_t*) {}

void d3d9_SetTransform(FTransform* tr)
{
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
		g_mv = m;
		g_fov = tr->fovxangle;
		g_splitscreen = (tr->splitscreen == 1);
		g_shearing = tr->shearing;
		g_view_aiming = tr->viewaiming;
		g_flip = tr->flip;
	}
	else
	{
		g_mv = m4_scale(1.0f, 1.0f, -1.0f);
		g_fov = 90.0f;
		g_shearing = false;
	}
	rebuild_projection();
}

INT32 d3d9_GetTextureUsed(void)
{
	return (INT32)g_tex_bytes;
}

void d3d9_PostImgRedraw(float[SCREENVERTS][SCREENVERTS][2]) {}
void d3d9_FlushScreenTextures(void) {}
void d3d9_StartScreenWipe(void) {}
void d3d9_EndScreenWipe(void) {}
void d3d9_DoScreenWipe(void) {}
void d3d9_DrawIntermissionBG(void) {}
void d3d9_MakeScreenTexture(void) {}
void d3d9_MakeScreenFinalTexture(void) {}
void d3d9_DrawScreenFinalTexture(int, int) {}

boolean d3d9_CompileShaders(void)
{
	return g_shaders_ready ? true : false;
}

void d3d9_CleanShaders(void) {}

void d3d9_SetShader(int type)
{
	g_cur_shader = type;
}

void d3d9_UnSetShader(void)
{
	g_cur_shader = SHADER_NONE;
}

void d3d9_SetShaderInfo(hwdshaderinfo_t info, INT32 value)
{
	if (info == HWD_SHADERINFO_LEVELTIME)
		g_leveltime = (float)value / (float)TICRATE;
}

void d3d9_LoadCustomShader(int, char*, size_t, boolean) {}

} // namespace

// ------------------------------------------------------------------ public interface
void RR_D3D9_FillHWD(hwdriver_s* hwd)
{
	hwd->pfnInit = d3d9_Init;
	hwd->pfnFinishUpdate = nullptr;
	hwd->pfnDraw2DLine = d3d9_Draw2DLine;
	hwd->pfnDrawPolygon = d3d9_DrawPolygon;
	hwd->pfnDrawIndexedTriangles = d3d9_DrawIndexedTriangles;
	hwd->pfnRenderSkyDome = d3d9_RenderSkyDome;
	hwd->pfnSetBlend = d3d9_SetBlend;
	hwd->pfnClearBuffer = d3d9_ClearBuffer;
	hwd->pfnSetTexture = d3d9_SetTexture;
	hwd->pfnUpdateTexture = d3d9_UpdateTexture;
	hwd->pfnDeleteTexture = d3d9_DeleteTexture;
	hwd->pfnReadRect = d3d9_ReadRect;
	hwd->pfnGClipRect = d3d9_GClipRect;
	hwd->pfnClearMipMapCache = d3d9_ClearMipMapCache;
	hwd->pfnSetSpecialState = d3d9_SetSpecialState;
	hwd->pfnSetPalette = d3d9_SetPalette;
	hwd->pfnGetTextureUsed = d3d9_GetTextureUsed;
	hwd->pfnDrawModel = d3d9_DrawModel;
	hwd->pfnCreateModelVBOs = d3d9_CreateModelVBOs;
	hwd->pfnSetTransform = d3d9_SetTransform;
	hwd->pfnPostImgRedraw = d3d9_PostImgRedraw;
	hwd->pfnFlushScreenTextures = d3d9_FlushScreenTextures;
	hwd->pfnStartScreenWipe = d3d9_StartScreenWipe;
	hwd->pfnEndScreenWipe = d3d9_EndScreenWipe;
	hwd->pfnDoScreenWipe = d3d9_DoScreenWipe;
	hwd->pfnDrawIntermissionBG = d3d9_DrawIntermissionBG;
	hwd->pfnMakeScreenTexture = d3d9_MakeScreenTexture;
	hwd->pfnMakeScreenFinalTexture = d3d9_MakeScreenFinalTexture;
	hwd->pfnDrawScreenFinalTexture = d3d9_DrawScreenFinalTexture;
	hwd->pfnCompileShaders = d3d9_CompileShaders;
	hwd->pfnCleanShaders = d3d9_CleanShaders;
	hwd->pfnSetShader = d3d9_SetShader;
	hwd->pfnUnSetShader = d3d9_UnSetShader;
	hwd->pfnSetShaderInfo = d3d9_SetShaderInfo;
	hwd->pfnLoadCustomShader = d3d9_LoadCustomShader;
	hwd->pfnResetRenderState = d3d9_ResetRenderState;
	hwd->pfnDrawStaticPool = nullptr; // no static cache on D3D9
}

void RR_D3D9_Surface(INT32 w, INT32 h)
{
	ensure_device();
	g_screen_w = w;
	g_screen_h = h;
	g_vp_set = false;
	d3d9_ResetRenderState();
	g_mv = m4_scale(1.0f, 1.0f, -1.0f);
	rebuild_projection();
}

#endif // _WIN32
