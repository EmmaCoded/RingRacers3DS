// DR. ROBOTNIK'S RING RACERS — 3DS port
//-----------------------------------------------------------------------------
// CPU rasterizer for the 2D list (srb2::g_2d / hwr2::Twodee).
//
// On PC, ALL of Ring Racers' 2D (menus, HUD, fonts, fades) is queued in
// g_2d and drawn by hwr2 with GLSL shaders. On 3DS there is no hwr2 pipeline (CtrRhi
// ignored draw()), which is why the screen stayed black. This file walks
// the g_2d commands and paints them directly into an 8-bit buffer
// (palette indices), like the old SRB2 software renderer.
//
// Deliberately does not include <3ds.h> (avoids the 'Handle' clash with srb2::rhi).
//-----------------------------------------------------------------------------

#ifdef __3DS__

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <variant>
#include <vector>
#include <cstdint>
#include <cstdlib>

#include "../doomdef.h"
#include "../doomtype.h"
#include "../r_defs.h"
#include "../r_data.h"   // NearestColor
#include "../r_draw.h"   // R_GetBlendTable
#include "../v_video.h"  // g_2d, pLocalPalette
#include "../w_wad.h"
#include "../z_zone.h"
#include "../hwr2/twodee.hpp"
#include "i_3ds.h"

using namespace srb2;
using namespace srb2::hwr2;

// 2D profile per command type (printed to ringracers_perf.txt)
// 0 opaque quad, 1 quad with blend/colormap/scale, 2 color fill, 3 flat triangle,
// 4 tri color, 5 lines, 6 decode patch
extern "C" unsigned long long I_3DS_Ticks(void);
extern "C" unsigned long long g_2d_prof_t[8];
extern "C" int g_2d_prof_n[8];
extern "C" int g_2d_prof_px[8];

namespace
{

struct Target
{
	UINT8* px;
	int w;
	int h;
};

// rgb -> palette index cache (NearestColor is slow; fills repeat a lot)
std::unordered_map<UINT32, UINT8> g_nearest_cache;

UINT8 nearest_index(float r, float g, float b)
{
	const int ir = std::clamp((int)(r * 255.f + 0.5f), 0, 255);
	const int ig = std::clamp((int)(g * 255.f + 0.5f), 0, 255);
	const int ib = std::clamp((int)(b * 255.f + 0.5f), 0, 255);
	const UINT32 key = (ir << 16) | (ig << 8) | ib;
	auto it = g_nearest_cache.find(key);
	if (it != g_nearest_cache.end())
		return it->second;
	if (g_nearest_cache.size() > 4096)
		g_nearest_cache.clear();
	const UINT8 idx = NearestColor((UINT8)ir, (UINT8)ig, (UINT8)ib);
	g_nearest_cache.emplace(key, idx);
	return idx;
}

// ---------------------------------------------------------------------------
// in GPU mode index 255 is "transparent" (lets the 3D show through).
// Blending over 255 used to be approximated with 4x4 dithering (it looked
// "per-pixel"). Now there is an 8-bit alpha plane parallel to the buffer
// (g_3ds_2d_alpha): over the 3D the color is written with its opacity and the GPU
// real blending when presenting (gpu_present_3ds.c, RGBA8 texture).
// ---------------------------------------------------------------------------
} // namespace
extern "C" int g_3ds_gpu3d;
extern "C" UINT8* g_3ds_2d_alpha = nullptr;   // w*h, 255 = opaco
extern "C" int g_3ds_2d_alpha_n = 0;
extern "C" int g_3ds_2d_alpha_used = 0;       // 1 = there are pixels with alpha < 255
namespace
{
int g_mix_a = 0;          // opacity 0..255 over 255 pixels (3D)
int g_mix_col = -1;       // -1 = source color; otherwise a fixed index (black)
const UINT8* g_tbase = nullptr; // start of the buffer (for the alpha offset)
UINT8* g_alpha = nullptr;       // = g_3ds_2d_alpha in GPU mode, otherwise nullptr

inline void set_row(UINT8*, int) {}

// Opaque write: alpha goes back to 255 (only if there were translucent pixels this frame)
inline void opq(UINT8* d, int n)
{
	if (g_3ds_2d_alpha_used && g_alpha)
		std::memset(g_alpha + (d - g_tbase), 255, n);
}

// Blend one pixel with a table, respecting the transparent 255 of GPU mode
inline UINT8 mix(UINT8 c, UINT8 dst, const UINT8* tbl, const UINT8* dp)
{
	if (g_alpha)
	{
		UINT8* a = g_alpha + (dp - g_tbase);
		if (dst == 255)
		{
			if (g_mix_a <= 0)
				return 255;
			*a = (UINT8)g_mix_a;
			g_3ds_2d_alpha_used = 1;
			return g_mix_col >= 0 ? (UINT8)g_mix_col : c;
		}
		if (*a != 255 && g_mix_a > 0) // already translucent over the 3D: add coverage
			*a = (UINT8)(*a + ((255 - *a) * g_mix_a) / 255);
	}
	return tbl[(c << 8) | dst];
}

UINT8 nearest_index(float r, float g, float b);

// Computes g_mix_a/g_mix_col for the current command
void set_dither(BlendMode mode, int level, float r, float g, float b)
{
	g_mix_a = 0;
	g_mix_col = -1;
	const float opacity = 1.f - std::clamp(level, 0, 10) / 10.f;
	switch (mode)
	{
	case BlendMode::kReverseSubtractive: // destination - source: darkens
	{
		const float dark = std::clamp((r + g + b) / 3.f, 0.f, 1.f) * opacity;
		g_mix_a = (int)std::lround(dark * 255.f);
		g_mix_col = nearest_index(0.f, 0.f, 0.f);
		break;
	}
	case BlendMode::kAlphaTransparent:
	case BlendMode::kAdditive: // adds light: approximated as the source color with opacity
		g_mix_a = (int)std::lround(opacity * 255.f);
		break;
	default: // modulated, etc.: not visible over the 3D (left transparent)
		break;
	}
}

// Returns the 256x256 blend table (index = src<<8 | dst), or nullptr if opaque.
// *skip = true if the command is completely invisible.
const UINT8* blend_table(BlendMode mode, float alpha, bool* skip,
                         float r = 1.f, float g = 1.f, float b = 1.f)
{
	*skip = false;
	int level = (int)std::lround((1.f - std::clamp(alpha, 0.f, 1.f)) * 10.f); // 0 = opaco, 10 = invisible
	if (level >= 10)
	{
		*skip = true;
		return nullptr;
	}
	set_dither(mode, level, r, g, b);

	switch (mode)
	{
	case BlendMode::kAdditive:
		return R_GetBlendTable(AST_ADD, level);
	// Twodee uses the GL convention (see v_video.cpp): kReverseSubtractive == AST_SUBTRACT
	case BlendMode::kReverseSubtractive:
		return R_GetBlendTable(AST_SUBTRACT, level);
	case BlendMode::kSubtractive:
		return R_GetBlendTable(AST_REVERSESUBTRACT, level);
	case BlendMode::kModulate:
		return R_GetBlendTable(AST_MODULATE, 0);
	case BlendMode::kInvertDest:
		return nullptr; // unsupported: drawn opaque
	case BlendMode::kAlphaTransparent:
	default:
		if (level <= 0)
			return nullptr;
		return R_GetTranslucencyTable(level);
	}
}

inline void put(UINT8* d, UINT8 c, const UINT8* tbl)
{
	if (tbl) *d = mix(c, *d, tbl, d);
	else { *d = c; opq(d, 1); }
}

// ---------------------------------------------------------------------------
// Cache of patches decoded into rows (row-major).
// Drawing column by column writes 1 byte per cache line (stride 320) and
// the Old 3DS ARM11 has no L2: it was 90% of the 2D time. Here each patch
// is converted once to per-row pixels + a list of opaque spans per row.
// ---------------------------------------------------------------------------

struct Decoded
{
	std::vector<UINT8> px;          // W*H
	std::vector<uint16_t> runs;     // pairs (start, length)
	std::vector<uint32_t> rowstart; // H+1 offsets en runs
	const UINT8* columns;
	uint32_t sig;
	INT16 w, h;
};

// console crash (data abort in patch_sig, pointer 0xfab4ad45): a patch
// freed/corrupt. Validate pointers before reading; if invalid, do not draw.
static inline bool ptr_ok(const void* q)
{
	const uintptr_t a = (uintptr_t)q;
	return (a >= 0x08000000u && a < 0x10000000u)   // app heap
	    || (a >= 0x14000000u && a < 0x1C000000u)   // linear (FCRAM)
	    || (a >= 0x30000000u && a < 0x38000000u);  // linear (new mapping)
}

bool patch_ok(const patch_t* p)
{
	if (!ptr_ok(p) || !ptr_ok(p->columns) || !ptr_ok(p->columnofs))
		return false;
	const int W = p->width, H = p->height;
	if (W <= 0 || H <= 0 || W > 4096 || H > 4096)
		return false;
	const int cols[3] = {0, W / 2, W - 1};
	for (int c : cols)
		if ((UINT32)p->columnofs[c] > (1u << 22) || !ptr_ok(p->columns + p->columnofs[c]))
			return false;
	return true;
}

// Cheap content signature (in case a patch is freed and another reuses the address)
uint32_t patch_sig(const patch_t* p)
{
	uint32_t h = 2166136261u;
	const int W = p->width;
	const int cols[3] = {0, W / 2, W - 1};
	for (int c : cols)
	{
		const UINT8* col = p->columns + p->columnofs[c];
		for (int i = 0; i < 8; i++)
			h = (h ^ col[i]) * 16777619u;
	}
	return h;
}

std::unordered_map<const patch_t*, Decoded> g_patch_cache;
size_t g_patch_cache_bytes = 0;
constexpr size_t kPatchCacheMax = 3u * 1024u * 1024u;

const Decoded* decode_patch(const patch_t* p)
{
	if (!patch_ok(p))
	{
		g_patch_cache.erase(p);
		return nullptr;
	}
	auto it = g_patch_cache.find(p);
	if (it != g_patch_cache.end())
	{
		const Decoded& d = it->second;
		if (d.columns == p->columns && d.w == p->width && d.h == p->height && d.sig == patch_sig(p))
			return &d;
		g_2d_prof_n[6] += 1000; // marker: replacement by a reused pointer
		g_patch_cache_bytes -= d.px.size() + d.runs.size() * 2 + d.rowstart.size() * 4;
		g_patch_cache.erase(it);
	}

	const int W = p->width, H = p->height;
	if (g_patch_cache_bytes + (size_t)W * H * 2 > kPatchCacheMax)
	{
		g_patch_cache.clear();
		g_patch_cache_bytes = 0;
	}

	g_2d_prof_n[6]++;
	g_2d_prof_px[6] += W * H;
	std::vector<UINT8> opaque((size_t)W * H, 0);
	Decoded d;
	d.px.assign((size_t)W * H, 0);
	d.columns = p->columns;
	d.sig = patch_sig(p);
	d.w = p->width;
	d.h = p->height;

	for (int col = 0; col < W; col++)
	{
		const column_t* column = reinterpret_cast<const column_t*>(p->columns + p->columnofs[col]);
		int prevdelta = 0;
		for (int guard = 0; column->topdelta != 0xFF && guard < 1024; guard++)
		{
			int topdelta = column->topdelta;
			if (topdelta <= prevdelta) // "tall patches" hack
				topdelta += prevdelta;
			prevdelta = topdelta;
			const UINT8* src = reinterpret_cast<const UINT8*>(column) + 3;
			const int len = std::min((int)column->length, H - topdelta);
			for (int i = 0; i < len; i++)
			{
				const size_t o = (size_t)(topdelta + i) * W + col;
				d.px[o] = src[i];
				opaque[o] = 1;
			}
			column = reinterpret_cast<const column_t*>(reinterpret_cast<const UINT8*>(column) + column->length + 4);
		}
	}

	d.rowstart.resize(H + 1);
	for (int y = 0; y < H; y++)
	{
		d.rowstart[y] = (uint32_t)d.runs.size();
		const UINT8* o = opaque.data() + (size_t)y * W;
		int x = 0;
		while (x < W)
		{
			while (x < W && !o[x]) x++;
			const int s = x;
			while (x < W && o[x]) x++;
			if (x > s)
			{
				d.runs.push_back((uint16_t)s);
				d.runs.push_back((uint16_t)(x - s));
			}
		}
	}
	d.rowstart[H] = (uint32_t)d.runs.size();

	g_patch_cache_bytes += d.px.size() + d.runs.size() * 2 + d.rowstart.size() * 4;
	auto res = g_patch_cache.emplace(p, std::move(d));
	return &res.first->second;
}

// ---------------------------------------------------------------------------
// Quads (patches and color rectangles)
// ---------------------------------------------------------------------------

// Span of a patch quad: specialized versions (no per-pixel ifs)
typedef void (*SpanFn)(UINT8* d, int n, const UINT8* srow, int32_t cp, int32_t step, int W,
                       const UINT8* lut, const UINT8* tbl);

template <bool TBL, bool LUT, bool STEP1, bool FLIP>
void span_t(UINT8* d, int n, const UINT8* srow, int32_t cp, int32_t step, int W,
            const UINT8* lut, const UINT8* tbl)
{
	if (STEP1)
	{
		int col = cp >> 16;
		const UINT8* s = FLIP ? (srow + (W - 1 - col)) : (srow + col);
		for (int i = 0; i < n; i++)
		{
			UINT8 c = FLIP ? *s-- : *s++;
			if (LUT) c = lut[c];
			d[i] = TBL ? mix(c, d[i], tbl, d + i) : c;
		}
	}
	else
	{
		for (int i = 0; i < n; i++, cp += step)
		{
			int col = cp >> 16;
			UINT8 c = srow[FLIP ? (W - 1 - col) : col];
			if (LUT) c = lut[c];
			d[i] = TBL ? mix(c, d[i], tbl, d + i) : c;
		}
	}
}

#define RR_SPAN(a, b, c, d) &span_t<a, b, c, d>
const SpanFn k_spans[16] = {
	RR_SPAN(false, false, false, false), RR_SPAN(false, false, false, true),
	RR_SPAN(false, false, true, false),  RR_SPAN(false, false, true, true),
	RR_SPAN(false, true, false, false),  RR_SPAN(false, true, false, true),
	RR_SPAN(false, true, true, false),   RR_SPAN(false, true, true, true),
	RR_SPAN(true, false, false, false),  RR_SPAN(true, false, false, true),
	RR_SPAN(true, false, true, false),   RR_SPAN(true, false, true, true),
	RR_SPAN(true, true, false, false),   RR_SPAN(true, true, false, true),
	RR_SPAN(true, true, true, false),    RR_SPAN(true, true, true, true),
};
#undef RR_SPAN

inline SpanFn pick_span(bool tbl, bool lut, bool step1, bool flip)
{
	return k_spans[(tbl ? 8 : 0) | (lut ? 4 : 0) | (step1 ? 2 : 0) | (flip ? 1 : 0)];
}

void draw_quad(const Target& t, const Draw2dPatchQuad& q)
{
	bool skip;
	const UINT8* tbl = blend_table(q.blend, q.a, &skip, q.r, q.g, q.b);
	if (skip)
		return;

	float cx0 = 0.f, cy0 = 0.f, cx1 = (float)t.w, cy1 = (float)t.h;
	if (q.clip)
	{
		cx0 = std::max(cx0, q.clip_xmin);
		cy0 = std::max(cy0, q.clip_ymin);
		cx1 = std::min(cx1, q.clip_xmax);
		cy1 = std::min(cy1, q.clip_ymax);
	}

	// Pixel center: pixel x is covered if xmin <= x+0.5 < xmax
	const int x0 = std::max((int)std::ceil(std::max(q.xmin, cx0) - 0.5f), 0);
	const int x1 = std::min((int)std::ceil(std::min(q.xmax, cx1) - 0.5f), t.w);
	const int y0 = std::max((int)std::ceil(std::max(q.ymin, cy0) - 0.5f), 0);
	const int y1 = std::min((int)std::ceil(std::min(q.ymax, cy1) - 0.5f), t.h);
	if (x0 >= x1 || y0 >= y1)
		return;

	if (q.patch == nullptr)
	{
		const UINT8 c = nearest_index(q.r, q.g, q.b);
		for (int y = y0; y < y1; y++)
		{
			UINT8* row = t.px + y * t.w;
			set_row(row, y);
			if (!tbl)
			{
				std::memset(row + x0, c, x1 - x0);
				opq(row + x0, x1 - x0);
			}
			else
				for (int x = x0; x < x1; x++)
					put(row + x, c, tbl);
		}
		return;
	}

	const patch_t* p = q.patch;
	if (p->width <= 0 || p->height <= 0 || p->columnofs == nullptr || p->columns == nullptr)
		return;

	const float qw = q.xmax - q.xmin;
	const float qh = q.ymax - q.ymin;
	if (qw <= 0.f || qh <= 0.f)
		return;

	const Decoded* dp = decode_patch(p);
	if (dp == nullptr)
		return;
	const Decoded& D = *dp;

	const UINT8* cmap = q.colormap;
	// Tint (q.r/g/b other than white): approximate - ignored except for pure black
	const bool tint_black = (q.r < 0.01f && q.g < 0.01f && q.b < 0.01f);

	// Horizontal and vertical step in texels per pixel, 16.16 fixed point
	const int32_t colstep = (int32_t)(((float)p->width / qw) * 65536.f);
	const int32_t colbase = (int32_t)((((float)x0 + 0.5f) - q.xmin) * ((float)p->width / qw) * 65536.f);
	const int32_t rowstep = (int32_t)(((float)p->height / qh) * 65536.f);
	int32_t rowpos = (int32_t)((((float)y0 + 0.5f) - q.ymin) * ((float)p->height / qh) * 65536.f);
	if (colstep <= 0 || rowstep <= 0)
		return;

	const bool plain = !cmap && !tint_black && !tbl && colstep == 65536 && !q.flip;
	const int W = p->width;

	// colormap + black tint combined into a single 256 table
	static UINT8 s_lut[256];
	const bool use_lut = cmap || tint_black;
	if (use_lut)
		for (int i = 0; i < 256; i++)
			s_lut[i] = tint_black ? 31 : cmap[i];
	const UINT8* lut = use_lut ? s_lut : nullptr;
	const SpanFn span_fn = pick_span(tbl != nullptr, use_lut, colstep == 65536, q.flip);

	for (int y = y0; y < y1; y++, rowpos += rowstep)
	{
		int ty = rowpos >> 16;
		if (ty < 0 || ty >= p->height)
			continue;
		if (q.vflip)
			ty = p->height - 1 - ty;

		const UINT8* srow = D.px.data() + ty * W;
		UINT8* drow = t.px + y * t.w;
		set_row(drow, y);
		const uint16_t* run = D.runs.data() + D.rowstart[ty];
		const uint16_t* runend = D.runs.data() + D.rowstart[ty + 1];

		for (; run < runend; run += 2)
		{
			// range of texel columns [cs, ce) in screen order
			int cs = run[0], ce = run[0] + run[1];
			if (q.flip)
			{
				const int a = W - ce, b = W - cs;
				cs = a; ce = b;
			}
			// x such that (colbase + (x-x0)*colstep)>>16 is in [cs, ce)
			int64_t ns = ((int64_t)cs << 16) - colbase;
			int64_t ne = ((int64_t)ce << 16) - colbase;
			int xa = x0 + (int)(ns <= 0 ? 0 : (ns + colstep - 1) / colstep);
			int xb = x0 + (int)(ne <= 0 ? 0 : (ne + colstep - 1) / colstep);
			if (xa < x0) xa = x0;
			if (xb > x1) xb = x1;
			if (xa >= xb)
				continue;

			int32_t cp = colbase + (xa - x0) * colstep;
			if (plain)
			{
				std::memcpy(drow + xa, srow + (cp >> 16), xb - xa);
				opq(drow + xa, xb - xa);
				continue;
			}
			span_fn(drow + xa, xb - xa, srow, cp, colstep, W, lut, tbl);
			if (!tbl)
				opq(drow + xa, xb - xa);
		}
	}
}

// ---------------------------------------------------------------------------
// Triangles / lines (diagonals, background flats, minimap)
// ---------------------------------------------------------------------------

struct FlatRef
{
	const UINT8* data = nullptr;
	int size = 0;
};

FlatRef get_flat(lumpnum_t lump)
{
	FlatRef f;
	if (lump == LUMPERROR)
		return f;
	const size_t len = W_LumpLength(lump);
	int size = (int)std::sqrt((double)len);
	if (size <= 0)
		return f;
	f.data = static_cast<const UINT8*>(W_CacheLumpNum(lump, PU_CACHE));
	f.size = size;
	return f;
}

inline float edge(float ax, float ay, float bx, float by, float px, float py)
{
	return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

void draw_tri(const Target& t, const TwodeeVertex& a, const TwodeeVertex& b, const TwodeeVertex& c,
              const FlatRef& flat, UINT8 solid, const UINT8* tbl)
{
	const float area = edge(a.x, a.y, b.x, b.y, c.x, c.y);
	if (std::fabs(area) < 1e-6f)
		return;

	const int minx = std::max((int)std::floor(std::min({a.x, b.x, c.x})), 0);
	const int maxx = std::min((int)std::ceil(std::max({a.x, b.x, c.x})), t.w);
	const int miny = std::max((int)std::floor(std::min({a.y, b.y, c.y})), 0);
	const int maxy = std::min((int)std::ceil(std::max({a.y, b.y, c.y})), t.h);
	if (minx >= maxx || miny >= maxy)
		return;

	const float inv = 1.f / area;
	// Slopes of the barycentric weights with respect to x (constants)
	const float d0 = -(c.y - b.y) * inv;
	const float d1 = -(a.y - c.y) * inv;
	const float d2 = -(d0 + d1);

	const bool textured = flat.data != nullptr;
	const int fsize = flat.size;
	const bool pow2 = textured && (fsize & (fsize - 1)) == 0;
	const int fmask = fsize - 1;
	const float du = textured ? (d0 * a.u + d1 * b.u + d2 * c.u) * fsize : 0.f;
	const float dv = textured ? (d0 * a.v + d1 * b.v + d2 * c.v) * fsize : 0.f;

	for (int y = miny; y < maxy; y++)
	{
		const float py = y + 0.5f;
		const float px0 = minx + 0.5f;
		// weights at the first pixel of the row
		const float w0s = edge(b.x, b.y, c.x, c.y, px0, py) * inv;
		const float w1s = edge(c.x, c.y, a.x, a.y, px0, py) * inv;
		const float w2s = 1.f - w0s - w1s;

		// Range [lo, hi] of steps k (x = minx + k) where all 3 weights are >= 0
		float lo = 0.f, hi = (float)(maxx - minx - 1);
		auto clip = [&](float w, float d) {
			if (d > 0.f)       lo = std::max(lo, -w / d);
			else if (d < 0.f)  hi = std::min(hi, -w / d);
			else if (w < 0.f)  hi = -1.f;
		};
		clip(w0s, d0);
		clip(w1s, d1);
		clip(w2s, d2);
		if (lo > hi)
			continue;
		const int k0 = (int)std::ceil(lo - 1e-4f);
		const int k1 = (int)std::floor(hi + 1e-4f);
		if (k0 > k1)
			continue;
		const int xs = std::max(minx + k0, minx);
		const int xe = std::min(minx + k1 + 1, maxx);
		if (xs >= xe)
			continue;

		UINT8* d = t.px + y * t.w + xs;
		const int n = xe - xs;
		set_row(t.px + y * t.w, y);

		if (!textured)
		{
			if (!tbl)
			{
				std::memset(d, solid, n);
				opq(d, n);
			}
			else
			{
				for (int i = 0; i < n; i++)
					d[i] = mix(solid, d[i], tbl, d + i);
			}
			continue;
		}

		// UV of the span's first pixel
		const float k = (float)(xs - minx);
		const float w0 = w0s + d0 * k, w1 = w1s + d1 * k, w2 = 1.f - w0 - w1;
		float u = (w0 * a.u + w1 * b.u + w2 * c.u) * fsize;
		float v = (w0 * a.v + w1 * b.v + w2 * c.v) * fsize;

		if (pow2)
		{
			int32_t fu = (int32_t)std::floor(u * 65536.f);
			int32_t fv = (int32_t)std::floor(v * 65536.f);
			const int32_t fdu = (int32_t)(du * 65536.f);
			const int32_t fdv = (int32_t)(dv * 65536.f);
			for (int i = 0; i < n; i++, fu += fdu, fv += fdv)
			{
				const UINT8 col = flat.data[((fv >> 16) & fmask) * fsize + ((fu >> 16) & fmask)];
				if (col == 247) // flats' transparent index
					continue;
				if (tbl) d[i] = mix(col, d[i], tbl, d + i);
				else { d[i] = col; opq(d + i, 1); }
			}
		}
		else
		{
			for (int i = 0; i < n; i++, u += du, v += dv)
			{
				int tx = ((int)std::floor(u)) % fsize;
				int ty = ((int)std::floor(v)) % fsize;
				if (tx < 0) tx += fsize;
				if (ty < 0) ty += fsize;
				const UINT8 col = flat.data[ty * fsize + tx];
				if (col == 247)
					continue;
				if (tbl) d[i] = mix(col, d[i], tbl, d + i);
				else { d[i] = col; opq(d + i, 1); }
			}
		}
	}
}

void draw_line(const Target& t, const TwodeeVertex& a, const TwodeeVertex& b, UINT8 c, const UINT8* tbl)
{
	int x0 = (int)a.x, y0 = (int)a.y, x1 = (int)b.x, y1 = (int)b.y;
	const int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
	const int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
	int err = dx + dy;
	for (int guard = 0; guard < 4096; guard++)
	{
		if (x0 >= 0 && x0 < t.w && y0 >= 0 && y0 < t.h)
		{
			set_row(t.px + y0 * t.w, y0);
			put(t.px + y0 * t.w + x0, c, tbl);
		}
		if (x0 == x1 && y0 == y1)
			break;
		const int e2 = 2 * err;
		if (e2 >= dy) { err += dy; x0 += sx; }
		if (e2 <= dx) { err += dx; y0 += sy; }
	}
}

void draw_verts(const Target& t, const Draw2dList& list, const Draw2dVertices& cmd)
{
	if (cmd.elements == 0)
		return;
	const TwodeeVertex& first = list.vertices[list.indices[cmd.begin_index]];

	bool skip;
	const UINT8* tbl = blend_table(cmd.blend, first.a, &skip, first.r, first.g, first.b);
	if (skip)
		return;

	const UINT8 solid = nearest_index(first.r, first.g, first.b);
	const FlatRef flat = get_flat(cmd.flat_lump);

	const std::size_t end = std::min(cmd.begin_index + cmd.elements, list.indices.size());
	if (cmd.lines)
	{
		for (std::size_t i = cmd.begin_index; i + 1 < end; i += 2)
			draw_line(t, list.vertices[list.indices[i]], list.vertices[list.indices[i + 1]], solid, tbl);
	}
	else
	{
		for (std::size_t i = cmd.begin_index; i + 2 < end; i += 3)
		{
			draw_tri(t, list.vertices[list.indices[i]], list.vertices[list.indices[i + 1]],
			         list.vertices[list.indices[i + 2]], flat, solid, tbl);
		}
	}
}

} // namespace

// 1 if there are queued 2D commands (for the fade)
extern "C" int I_3DS_TwodeePending(void)
{
	for (const Draw2dList& list : g_2d)
		if (!list.cmds.empty())
			return 1;
	return 0;
}

// ---------------------------------------------------------------------------
// GPU menu blends. k_menudraw registers the patches (letters,
// MENUBG2); the first group followed by add/subtract quads of those patches is sent
// to the GPU and splits the 2D into layer A (before) and layer B (after, in dstB).
// ---------------------------------------------------------------------------
extern "C" int I_3DS_GpuBlendTex(const void* key, unsigned sig, int W, int H,
                                  const unsigned char* px, const unsigned char* opaque, const unsigned* pal);
extern "C" void I_3DS_GpuBlendQuad(int t, float x0, float y0, float x1, float y1, int flip, int vflip, int blend, float a);
namespace
{
const void* g_gm_keys[8];
int g_gm_nkeys = 0;
}
extern "C" void I_3DS_GpuBlendPatch(const void* p)
{
	for (int i = 0; i < g_gm_nkeys; i++)
		if (g_gm_keys[i] == p)
			return;
	if (g_gm_nkeys < 8)
		g_gm_keys[g_gm_nkeys++] = p;
}

namespace
{
bool gm_try(const Draw2dPatchQuad& q)
{
	if (!q.patch || q.colormap || q.clip || q.a <= 0.f)
		return false;
	int blend;
	switch (q.blend)
	{
	case BlendMode::kAdditive: blend = 1; break;
	case BlendMode::kReverseSubtractive: blend = 2; break;
	case BlendMode::kSubtractive: blend = 3; break;
	default: return false;
	}
	if (q.r < 0.99f || q.g < 0.99f || q.b < 0.99f)
		return false;
	bool reg = false;
	for (int i = 0; i < g_gm_nkeys; i++)
		if (g_gm_keys[i] == q.patch)
			reg = true;
	if (!reg)
		return false;
	const Decoded* D = decode_patch(q.patch);
	if (!D)
	{ static int n; if (n++ < 20) CONS_Printf("[GPUBLENDFAIL] decode_patch failed (%p)\n", q.patch); return false; }
	int t = I_3DS_GpuBlendTex(q.patch, D->sig, D->w, D->h, nullptr, nullptr, nullptr);
	if (t < 0)
	{
		static unsigned pal[256];
		for (int i = 0; i < 256; i++)
			pal[i] = ((unsigned)pLocalPalette[i].s.red << 24) | ((unsigned)pLocalPalette[i].s.green << 16)
				| ((unsigned)pLocalPalette[i].s.blue << 8);
		std::vector<UINT8> op((size_t)D->w * D->h, 0);
		for (int y = 0; y < D->h; y++)
			for (uint32_t r = D->rowstart[y]; r < D->rowstart[y + 1]; r += 2)
				std::memset(op.data() + (size_t)y * D->w + D->runs[r], 1, D->runs[r + 1]);
		t = I_3DS_GpuBlendTex(q.patch, D->sig, D->w, D->h, D->px.data(), op.data(), pal);
		if (t < 0)
		{ static int n; if (n++ < 20) CONS_Printf("[GPUBLENDFAIL] GPU texture not created %dx%d\n", D->w, D->h); return false; }
	}
	I_3DS_GpuBlendQuad(t, q.xmin, q.ymin, q.xmax, q.ymax, q.flip ? 1 : 0, q.vflip ? 1 : 0, blend, std::min(q.a, 1.f));
	return true;
}
} // namespace

// Paints the whole pending 2D list onto 'dst' (w*h, 1 byte per pixel) and empties it.
extern "C" int I_3DS_RasterizeTwodee2(UINT8* dst, UINT8* dstB, INT32 w, INT32 h);
void I_3DS_RasterizeTwodee(UINT8* dst, INT32 w, INT32 h)
{
	I_3DS_RasterizeTwodee2(dst, nullptr, w, h);
}

// Same, but with dstB != null: returns 1 if it was split into layers (B used).
extern "C" int I_3DS_RasterizeTwodee2(UINT8* dst, UINT8* dstB, INT32 w, INT32 h)
{
	int status = 0; // 0 layer A, 1 inside the GPU group, 2 layer B
	if (dst != nullptr && w > 0 && h > 0)
	{
		Target t {dst, w, h};
		g_tbase = dst;
		g_alpha = nullptr;
		if (g_3ds_gpu3d)
		{
			if (g_3ds_2d_alpha_n < w * h)
			{
				free(g_3ds_2d_alpha);
				g_3ds_2d_alpha = (UINT8*)malloc((size_t)w * h);
				g_3ds_2d_alpha_n = g_3ds_2d_alpha ? w * h : 0;
				if (g_3ds_2d_alpha) std::memset(g_3ds_2d_alpha, 255, (size_t)w * h);
				g_3ds_2d_alpha_used = 0;
			}
			g_alpha = g_3ds_2d_alpha;
		}
		for (const Draw2dList& list : g_2d)
		{
			for (const Draw2dCmd& cmd : list.cmds)
			{
				const unsigned long long t0 = I_3DS_Ticks();
				int cat;
				if (const auto* q = std::get_if<Draw2dPatchQuad>(&cmd))
				{
					if (!q->patch)
						cat = 2;
					else
					{
						const bool simple = !q->colormap && q->a >= 0.95f && q->blend == BlendMode::kAlphaTransparent
							&& std::fabs((q->xmax - q->xmin) - q->patch->width) < 0.01f;
						cat = simple ? 0 : 1;
					}
					g_2d_prof_px[cat] += (int)std::max(0.f, (q->xmax - q->xmin) * (q->ymax - q->ymin));
					if (dstB && status < 2 && gm_try(*q))
					{
						status = 1; // to the GPU
						cat = 7;
					}
					else
					{
						if (status == 1) // the group ended: the rest goes to layer B
						{
							status = 2;
							t.px = dstB;
							g_tbase = dstB;
						}
						draw_quad(t, *q);
					}
				}
				else if (const auto* v = std::get_if<Draw2dVertices>(&cmd))
				{
					cat = v->lines ? 5 : (v->flat_lump != LUMPERROR ? 3 : 4);
					if (status == 1)
					{
						status = 2;
						t.px = dstB;
						g_tbase = dstB;
					}
					draw_verts(t, list, *v);
				}
				else
					continue;
				g_2d_prof_t[cat] += I_3DS_Ticks() - t0;
				g_2d_prof_n[cat]++;
			}
		}
	}

	g_2d.reset_keep(); // keeps the memory (previously = Twodee())
	g_gm_nkeys = 0; // registered again every frame
	return status > 0;
}

// Discards what was queued without drawing (before releasing patches/colormaps).
void I_3DS_DiscardTwodee(void)
{
	g_2d.reset_keep(); // keeps the memory (previously = Twodee())
	g_patch_cache.clear();
	g_patch_cache_bytes = 0;
}

#endif // __3DS__
