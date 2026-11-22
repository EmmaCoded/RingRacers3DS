// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2025 by Kart Krew.
// Copyright (C) 2020 by Sonic Team Junior.
// Copyright (C) 2000 by DooM Legacy Team.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file hw_main.c
/// \brief hardware renderer, using the standard HardWareRender driver DLL for SRB2

#include <algorithm>
#include "../rr_config.h"  // single-file configuration
#include <math.h>

// RR3DS = 3DS port code that also compiles on PC (preparer / Ring Racers E).
#if defined(__3DS__) || defined(RR_PCGEN)
#define RR3DS 1
#endif
#ifdef __3DS__
#define RR3DS_DIR "sdmc:/3ds/RingRacers3ds"
#define RR3DS_MKDIR(p) mkdir((p), 0777)
#elif defined(_WIN32)
#include <direct.h>
#define RR3DS_DIR "prepared"
#define RR3DS_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define RR3DS_DIR "prepared"
#define RR3DS_MKDIR(p) mkdir((p), 0777)
#endif

#include "../doomstat.h"

#ifdef HWRENDER
#include "hw_glob.h"
#include "hw_light.h"
#include "hw_drv.h"
#include "hw_batching.h"

#include "../i_video.h" // for rendermode == render_glide
#include "../v_video.h"
#include "../p_local.h"
#include "../p_setup.h"
#include "../r_fps.h"
#include "../r_local.h"
#include "../r_patch.h"
#include "../r_picformats.h"
#include "../r_bsp.h"
#include "../d_clisrv.h"
#include "../d_netcmd.h" // D_MapChange
#include "../w_wad.h"
#include "../z_zone.h"
#include "../r_splats.h"
#include "../g_game.h"
#include "../st_stuff.h"
#include "../i_system.h"
#include "../m_cheat.h"
#include "../f_finale.h"
#include "../r_things.h" // R_GetShadowZ
#include "../d_main.h"
#include "../p_slopes.h"
#include "hw_md2.h"

// SRB2Kart
#include "../k_hitlag.h" // HITLAGJITTERS
#include "../r_fps.h"
#include "../r_plane.h" // R_FlatDimensionsFromLumpSize

/// FINALLY some real clipping that doesn't make walls dissappear AND speeds the game up
/// (that was the original comment from SRB2CB, sadly it is a lie and actually slows game down)
/// on the bright side it fixes some weird issues with translucent walls
/// \note	SRB2CB port.
///      	SRB2CB itself ported this from PrBoom+
#include "hw_clip.h"
#if defined(RR3DS) && !defined(__3DS__)
#include "rr_gfxapi.h" // RRGfx_IsAngle (flat sky with ANGLE)
#endif

#define R_FAKEFLOORS
#define HWPRECIP
//#define POLYSKY

// ==========================================================================
// the hardware driver object
// ==========================================================================
struct hwdriver_s hwdriver;

// ==========================================================================
//                                                                     PROTOS
// ==========================================================================


static void HWR_AddSprites(sector_t *sec);
static void HWR_ProjectSprite(mobj_t *thing);
#ifdef HWPRECIP
static void HWR_AddPrecipitationSprites(void);
static void HWR_ProjectPrecipitationSprite(precipmobj_t *thing);
#endif
static void HWR_ProjectBoundingBox(mobj_t *thing);
static void HWR_RollTransform(FTransform *tr, angle_t roll);

void HWR_AddTransparentFloor(levelflat_t *levelflat, extrasubsector_t *xsub, boolean isceiling, fixed_t fixedheight, INT32 lightlevel, INT32 alpha, sector_t *FOFSector, FBITFIELD blend, boolean fogplane, extracolormap_t *planecolormap);
void HWR_AddTransparentPolyobjectFloor(levelflat_t *levelflat, polyobj_t *polysector, boolean isceiling, fixed_t fixedheight,
                             INT32 lightlevel, INT32 alpha, sector_t *FOFSector, FBITFIELD blend, extracolormap_t *planecolormap);

boolean drawsky = true;

// ==========================================================================
//                                                               VIEW GLOBALS
// ==========================================================================
// Fineangles in the SCREENWIDTH wide window.
#define FIELDOFVIEW ANGLE_90
#define ABS(x) ((x) < 0 ? -(x) : (x))

static angle_t gl_clipangle;

// The viewangletox[viewangle + FINEANGLES/4] lookup
// maps the visible view angles to screen X coordinates,
// flattening the arc to a flat projection plane.
// There will be many angles mapped to the same X.
static INT32 gl_viewangletox[FINEANGLES/2];

// The xtoviewangleangle[] table maps a screen pixel
// to the lowest viewangle that maps back to x ranges
// from clipangle to -clipangle.
static angle_t gl_xtoviewangle[MAXVIDWIDTH+1];

// ==========================================================================
//                                                                    GLOBALS
// ==========================================================================

// uncomment to remove the plane rendering
#define DOPLANES
//#define DOWALLS

// test of drawing sky by polygons like in software with visplane, unfortunately
// this doesn't work since we must have z for pixel and z for texture (not like now with z = oow)
//#define POLYSKY

// test change fov when looking up/down but bsp projection messup :(
//#define NOCRAPPYMLOOK

// base values set at SetViewSize
static float gl_basecentery;
static float gl_basecenterx;

float gl_baseviewwindowy, gl_basewindowcentery;
float gl_baseviewwindowx, gl_basewindowcenterx;
float gl_viewwidth, gl_viewheight; // viewport clipping boundaries (screen coords)
float gl_viewwindowx;

static float gl_centerx, gl_centery;
static float gl_viewwindowy; // top left corner of view window
static float gl_windowcenterx; // center of view window, for projection
static float gl_windowcentery;

static float gl_pspritexscale, gl_pspriteyscale;

static seg_t *gl_curline;
static side_t *gl_sidedef;
static line_t *gl_linedef;
static sector_t *gl_frontsector;
static sector_t *gl_backsector;

// --------------------------------------------------------------------------
//                                              STUFF FOR THE PROJECTION CODE
// --------------------------------------------------------------------------

FTransform atransform;
// duplicates of the main code, set after R_SetupFrame() passed them into sharedstruct,
// copied here for local use
static fixed_t dup_viewx, dup_viewy, dup_viewz;
static angle_t dup_viewangle;

static float gl_viewx, gl_viewy, gl_viewz;
float gl_viewsin, gl_viewcos;

// Maybe not necessary with the new T&L code (needs to be checked!)
static float gl_viewludsin, gl_viewludcos; // look up down kik test
static float gl_fovlud;

static angle_t gl_aimingangle;
static void HWR_SetTransformAiming(FTransform *trans, player_t *player, boolean skybox);

// Render stats
precise_t ps_hw_skyboxtime = 0;
precise_t ps_hw_nodesorttime = 0;
precise_t ps_hw_nodedrawtime = 0;
precise_t ps_hw_spritesorttime = 0;
precise_t ps_hw_spritedrawtime = 0;

// Render stats for batching
int ps_hw_numpolys = 0;
int ps_hw_numverts = 0;
int ps_hw_numcalls = 0;
int ps_hw_numshaders = 0;
int ps_hw_numtextures = 0;
int ps_hw_numpolyflags = 0;
int ps_hw_numcolors = 0;
precise_t ps_hw_batchsorttime = 0;
precise_t ps_hw_batchdrawtime = 0;

boolean gl_init = false;
boolean gl_maploaded = false;
boolean gl_sessioncommandsadded = false;
boolean gl_shadersavailable = true;

// ==========================================================================
// Lighting
// ==========================================================================

// PC = 3DS map preparer only if prepared/rr_genmap exists. Before, ALL of PC
// (Ring Racers E) was left without shaders or light-based darkening (Legacy GL with flat brightness).
#if defined(RR3DS) && !defined(__3DS__)
static boolean RR3DS_PcPreparer(void)
{
	static int st = -1;
	if (st < 0) { FILE *g = fopen(RR3DS_DIR "/rr_genmap", "rb"); st = g ? 1 : 0; if (g) fclose(g); }
	return st == 1;
}
#define RR3DS_LIGHT3DS (RR3DS_PcPreparer())
#elif defined(__3DS__)
#define RR3DS_LIGHT3DS 1
#else
#define RR3DS_LIGHT3DS 0
#endif

// Legacy GL changes that CHANGE THE IMAGE (distance 12288, flat sky, no
// horizon lines, half contrast on sprites without shaders): on E only with Low-End (design decision).
// Drawing only (not simulation): also valid in replays and when toggling the option mid-race.
#if defined(__3DS__)
#define RRE_GLLOWEND 1
#elif defined(RR3DS)
#define RRE_GLLOWEND (RR3DS_PcPreparer() || rr_lowend || cv_rrlowend.value)
#else
#define RRE_GLLOWEND 0
#endif

static boolean HWR_UseShader(void)
{
	if (RR3DS_LIGHT3DS)
		return false; // same as 3DS (no shaders) so the preparer bakes the same lighting
	return (cv_glshaders.value && gl_shadersavailable);
}
#if defined(RR3DS) && !defined(__3DS__)
extern "C" boolean HWR_UseShaderPC(void) { return HWR_UseShader(); } // rr_pcgen.cpp
#endif

boolean HWR_OverrideObjectLightLevel(mobj_t *thing, INT32 *lightlevel)
{
	if (R_ThingIsFullBright(thing))
		*lightlevel = 255;
	else if (R_ThingIsFullDark(thing))
		*lightlevel = 0;
	else if (thing->renderflags & RF_ABSOLUTELIGHTLEVEL)
		*lightlevel = R_ThingLightLevel(thing);
	else
		return false;

	return true;
}

void HWR_ObjectLightLevelPost(gl_vissprite_t *spr, const sector_t *sector, INT32 *lightlevel, boolean model)
{
	const boolean semibright = R_ThingIsSemiBright(spr->mobj);
	const boolean papersprite = R_ThingIsPaperSprite(spr->mobj);

	*lightlevel += R_ThingLightLevel(spr->mobj);

	if (maplighting.directional == true && P_SectorUsesDirectionalLighting(sector))
	{
		if (model == false) // this is implemented by shader
		{
			fixed_t extralight = R_GetSpriteDirectionalLighting(
				papersprite
				? spr->angle + (spr->flip ? -ANGLE_90 : ANGLE_90)
				: R_PointToAngle(spr->mobj->x, spr->mobj->y) // fixme
			);

			// Less change in contrast in dark sectors
			extralight = FixedMul(extralight, std::min<INT32>(std::max<INT32>(0, *lightlevel), 255) * FRACUNIT / 255);

			if (papersprite)
			{
				// Papersprite contrast should match walls
				*lightlevel += FixedFloor(extralight + (FRACUNIT / 2)) / FRACUNIT;
			}
			else
			{
				// simple OGL approximation
				fixed_t tr = R_PointToDist(spr->mobj->x, spr->mobj->y);
				fixed_t xscale = FixedDiv((vid.width / 2) << FRACBITS, tr);

				// Less change in contrast at further distances, to counteract DOOM diminished light
				fixed_t n = FixedDiv(FixedMul(xscale, LIGHTRESOLUTIONFIX), ((MAXLIGHTSCALE-1) << LIGHTSCALESHIFT));
				extralight = FixedMul(extralight, std::min<INT32>(n, FRACUNIT));

				// Contrast is stronger for normal sprites, stronger than wall lighting is at the same distance
#ifdef RR3DS
				// without shaders, low light (e.g. 200 -> 75) looks almost
				// black with the fallback formula (like the rings).
				// Measured on Balconies: Sonic with his back to the light = 75. Half
				// contrast: 200 -> ~138, visible shading but not black.
				if (RRE_GLLOWEND)
					*lightlevel += FixedFloor(extralight + (FRACUNIT / 2)) / FRACUNIT;
				else
					*lightlevel += FixedFloor((extralight * 2) + (FRACUNIT / 2)) / FRACUNIT;
#else
				*lightlevel += FixedFloor((extralight * 2) + (FRACUNIT / 2)) / FRACUNIT;
#endif
			}
		}

		// Semibright objects will be made slightly brighter to compensate contrast
		if (semibright)
		{
			*lightlevel += 16;
		}
	}

	if (semibright)
	{
		*lightlevel = 192 + (*lightlevel >> 1);
	}
}

void HWR_Lighting(FSurfaceInfo *Surface, INT32 light_level, extracolormap_t *colormap, const boolean directional)
{
	RGBA_t poly_color, tint_color, fade_color;

	poly_color.rgba = 0xFFFFFFFF;
	tint_color.rgba = (colormap != NULL) ? (UINT32)colormap->rgba : GL_DEFAULTMIX;
	fade_color.rgba = (colormap != NULL) ? (UINT32)colormap->fadergba : GL_DEFAULTFOG;

	// Crappy backup coloring if you can't do shaders
	if (!HWR_UseShader())
	{
		// be careful, this may get negative for high lightlevel values.
		float tint_alpha, fade_alpha;
		float red, green, blue;

		red = (float)poly_color.s.red;
		green = (float)poly_color.s.green;
		blue = (float)poly_color.s.blue;

		// 48 is just an arbritrary value that looked relatively okay.
		// 3DS: the original formula with a black colormap from
		// alpha 0x19 gave tint_alpha=0.94 -> almost black sprites (col=030303).
		// We use the real alpha (0x19/255 = 10%), like the shader does.
		if (RR3DS_LIGHT3DS)
			tint_alpha = (float)tint_color.s.alpha / 255.0f;
		else
			tint_alpha = (float)(sqrt(tint_color.s.alpha) * 48) / 255.0f;

		// 8 is roughly the brightness of the "close" color in Software, and 16 the brightness of the "far" color.
		// 8 is too bright for dark levels, and 16 is too dark for bright levels.
		// 12 is the compromise value. It doesn't look especially good anywhere, but it's the most balanced.
		// (Also, as far as I can tell, fade_color's alpha is actually not used in Software, so we only use light level.)
		// (clamp: with light >255 the root of a negative gave NaN -> color 0)
		fade_alpha = (float)(sqrt(std::max<INT32>(0, 255-light_level)) * 12) / 255.0f;

		// Clamp the alpha values
		tint_alpha = std::min(std::max(tint_alpha, 0.0f), 1.0f);
		fade_alpha = std::min(std::max(fade_alpha, 0.0f), 1.0f);

		red = (tint_color.s.red * tint_alpha) + (red * (1.0f - tint_alpha));
		green = (tint_color.s.green * tint_alpha) + (green * (1.0f - tint_alpha));
		blue = (tint_color.s.blue * tint_alpha) + (blue * (1.0f - tint_alpha));

		// 3DS: the darkening (fade) is done by the GPU per vertex according to the
		// distance, like Software mode (world.v.pica / static.v.pica). There
		// (and in the PC preparer) only the colormap tint remains.
		if (!RR3DS_LIGHT3DS)
		{
			red = (fade_color.s.red * fade_alpha) + (red * (1.0f - fade_alpha));
			green = (fade_color.s.green * fade_alpha) + (green * (1.0f - fade_alpha));
			blue = (fade_color.s.blue * fade_alpha) + (blue * (1.0f - fade_alpha));
		}

		poly_color.s.red = (UINT8)red;
		poly_color.s.green = (UINT8)green;
		poly_color.s.blue = (UINT8)blue;
	}

	// Clamp the light level, since it can sometimes go out of the 0-255 range from animations
	light_level = std::min<INT32>(std::max<INT32>(light_level, 0), 255);

	Surface->PolyColor.rgba = poly_color.rgba;
	Surface->TintColor.rgba = tint_color.rgba;
	Surface->FadeColor.rgba = fade_color.rgba;

	Surface->LightInfo.light_level = light_level;
	Surface->LightInfo.fade_start = (colormap != NULL) ? colormap->fadestart : 0;
	Surface->LightInfo.fade_end = (colormap != NULL) ? colormap->fadeend : 31;

	Surface->LightInfo.directional = (maplighting.directional == true && directional == true);
}

UINT8 HWR_FogBlockAlpha(INT32 light, extracolormap_t *colormap) // Let's see if this can work
{
	RGBA_t realcolor, surfcolor;
	INT32 alpha;

	realcolor.rgba = (colormap != NULL) ? colormap->rgba : GL_DEFAULTMIX;

	if (cv_glshaders.value && gl_shadersavailable)
	{
		surfcolor.s.alpha = (255 - light);
	}
	else
	{
		light = light - (255 - light);

		// Don't go out of bounds
		if (light < 0)
			light = 0;
		else if (light > 255)
			light = 255;

		alpha = (realcolor.s.alpha*255)/25;

		// at 255 brightness, alpha is between 0 and 127, at 0 brightness alpha will always be 255
		surfcolor.s.alpha = (alpha*light) / (2*256) + 255-light;
	}

	return surfcolor.s.alpha;
}

#ifndef __3DS__
// fog block without shaders, like Software: what is behind fades to the fog colour by the light
// (colour = fog * d premultiplied, alpha = 1 - d; r_opengl blends fog as GL_ONE, GL_SRC_ALPHA).
// The 3DS has its own fog blend (r_citro3d), so it keeps the old colour.
static void HWR_FogNoShaderColor(FSurfaceInfo *Surf, INT32 light, extracolormap_t *colormap)
{
	RGBA_t fade;
	fade.rgba = (colormap != NULL) ? (UINT32)colormap->fadergba : GL_DEFAULTFOG;
	const float strength = std::min(1.0f, fade.s.alpha * 10.0f / 255.0f);
	const float d = std::min(std::max(255 - light, 0), 255) / 255.0f * strength;
	Surf->PolyColor.s.red = (UINT8)(fade.s.red * d);
	Surf->PolyColor.s.green = (UINT8)(fade.s.green * d);
	Surf->PolyColor.s.blue = (UINT8)(fade.s.blue * d);
	Surf->PolyColor.s.alpha = (UINT8)(255.0f * (1.0f - d));
}
#endif

static FUINT HWR_CalcWallLight(FUINT lightnum, seg_t *seg)
{
	INT16 finallight = lightnum;

	if (seg != NULL && P_ApplyLightOffsetFine(lightnum, seg->frontsector))
	{
		finallight += seg->hwLightOffset;

		if (finallight > 255) finallight = 255;
		if (finallight < 0) finallight = 0;
	}

	return (FUINT)finallight;
}

static FUINT HWR_CalcSlopeLight(FUINT lightnum, pslope_t *slope, const sector_t *sector, const boolean fof)
{
	INT16 finallight = lightnum;

	if (slope != NULL && sector != NULL && P_ApplyLightOffsetFine(lightnum, sector))
	{
		finallight += (fof ? -slope->hwLightOffset : slope->hwLightOffset);

		if (finallight > 255) finallight = 255;
		if (finallight < 0) finallight = 0;
	}

	return (FUINT)finallight;
}

// ==========================================================================
//                                   FLOOR/CEILING GENERATION FROM SUBSECTORS
// ==========================================================================

#ifdef DOPLANES

// -----------------+
// HWR_RenderPlane  : Render a floor or ceiling convex polygon
// -----------------+
#ifdef RR3DS
// DEBUG mode 7 (sdmc:/3ds/rr_dbgflat = "7"): colors each polygon according to
// where it comes from. red=floor, cyan=ceiling, magenta=FOF floor/ceiling,
// green=wall, dark green=translucent wall/FOF, blue=sprite,
// yellow=polyobject, orange=shadow.
#ifdef __3DS__
extern "C" unsigned I_3DS_UpTotalUs(void); extern "C" unsigned I_3DS_UpPartUs(int); // diag
#else
static inline unsigned I_3DS_UpTotalUs(void) { return 0; }
static inline unsigned I_3DS_UpPartUs(int) { return 0; }
#endif
extern "C" int g_3ds_dbgflat;
static inline void dbg3ds_tint(FSurfaceInfo *s, FBITFIELD *f, UINT8 r, UINT8 g, UINT8 b)
{
	if (g_3ds_dbgflat != 7 || !s) return;
	s->PolyColor.s.red = r; s->PolyColor.s.green = g; s->PolyColor.s.blue = b;
	if (f) *f |= PF_Modulated;
}
#define DBG3DS_TINT(s,f,r,g,b) dbg3ds_tint(s,f,r,g,b)

// ==========================================================================
// STATIC GEOMETRY - world cache in GPU memory
// --------------------------------------------------------------------------
// The first time a subsector floor/ceiling or a wall (seg) is drawn
// it is "captured": the polygons emitted by the normal path are copied into a persistent
// vertex pool (with the lighting already baked in as per-vertex color).
// In later frames, if the "signature" (heights, textures, light, offsets...)
// has not changed, all HWR computation is skipped and only indices are added.
// At the end of the BSP everything is drawn grouped by (texture, flags).
// Anything that changes often (>=4 times) or is translucent stays on the normal path.
// Disable: create sdmc:/3ds/rr_nostatic
// ==========================================================================
#include <vector>
#include <string>
#include <csignal>
#include <unistd.h> // _exit
#ifdef _WIN32
// no windows.h (clashes with min/max/boolean): only what is needed from kernel32
#ifndef _WINDOWS_
extern "C" __declspec(dllimport) unsigned short __stdcall RtlCaptureStackBackTrace(unsigned long, unsigned long, void **, unsigned long *);
extern "C" __declspec(dllimport) void *__stdcall GetModuleHandleA(const char *);
#endif
#endif
#include <unordered_map>
#include <new>
extern "C" {
int  I_3DS_StaticAdd(const FOutVector* v, int n, unsigned int rgba);
int  I_3DS_StaticUsed(void);
void I_3DS_StaticTruncate(int used);
int  I_3DS_StaticReset(void);
int  I_3DS_StaticDraw(GLMipmap_t* mip, GLMipmap_t* bright, FBITFIELD flags, const UINT16* idx, int n);
void I_3DS_StaticEnd(void);
void I_3DS_StaticShiftST(int first, int n, float ds, float dt);
void I_3DS_StaticSetYT(int first, int n, const float *y, const float *t);
void I_3DS_StaticSetY(int first, int n, float y);
}
// sky (skybox) to texture (r_citro3d.cpp). Does nothing outside the 3DS.
#ifdef __3DS__
extern "C" {
int  I_3DS_SkyTexOn(void);
int  I_3DS_SkyTexDrawing(void);
int  I_3DS_SkyTexNeedRedraw(void);
int  I_3DS_SkyTexBegin(void);
void I_3DS_SkyTexEnd(void);
void I_3DS_SkyTexDraw(void);
void I_3DS_SkyTexInvalidate(void);
void I_3DS_SkyTexWhy(int* noData, int* behind, int* turn, float* ox, float* oy);
}
#else
static inline int  I_3DS_SkyTexOn(void) { return 0; }
static inline int  I_3DS_SkyTexDrawing(void) { return 0; }
static inline int  I_3DS_SkyTexNeedRedraw(void) { return 1; }
static inline int  I_3DS_SkyTexBegin(void) { return 0; }
static inline void I_3DS_SkyTexEnd(void) {}
static inline void I_3DS_SkyTexDraw(void) {}
static inline void I_3DS_SkyTexInvalidate(void) {}
static inline void I_3DS_SkyTexWhy(int* a, int* b, int* c, float* d, float* e) { *a = *b = *c = 0; *d = *e = 0; }
#endif
static boolean rr3ds_skyQuad = false; // this frame the sky comes from the texture
static int rr3ds_skyDrawn = 0, rr3ds_skyReused = 0;
extern GLMapTexture_t *rr3ds_lastgrtex; // hw_cache.cpp
GLMipmap_t *rr3ds_lastmip = NULL; // set by HWR_SetCurrentTexture (hw_batching.cpp)
GLMipmap_t *rr3ds_lastbright = NULL; // brightmap of rr3ds_lastmip (or NULL)
struct rr3ds_texreq_t { int kind; INT32 a, b; void *p; }; // same as in hw_cache.cpp
extern rr3ds_texreq_t rr3ds_texreq; // hw_cache.cpp
extern mobj_t *rr3ds_sprmobj; // hw_cache.cpp

static int RR3DS_ViewNow(void);
namespace rr3ds_static
{
enum { ST_NONE = 0, ST_VALID = 1, ST_NEVER = 2 };
struct SItem  { UINT32 sig; UINT32 gen; UINT32 firstPoly; UINT16 numPolys; UINT8 state; UINT8 changes; UINT8 why; UINT8 hasOff; fixed_t to, ro;
	UINT32 sigH; UINT8 kmask; UINT8 patchOk; UINT32 fofMask;
	UINT16 stable; UINT8 strikes; }; // "never" due to changes -> retry if the signature stays still // heights tracked separately (simple walls)
struct SPoly  { UINT16 base; UINT8 n; UINT8 pad; UINT16 group; float sx, sy; };
struct SGroup { GLMipmap_t *mip; FBITFIELD flags; std::vector<UINT16> idx; rr3ds_texreq_t req; GLMipmap_t *bright; };

// these large vectors go to LINEAR MEMORY (~4.6 MB to spare) and not to the heap:
// on 64 MB the heap is full after loading 765Stadium (1.7 MB free, largest block
// 1 KB) and items (~700 KB contiguous) gave bad_alloc.
} // namespace rr3ds_static 
extern "C" void *linearAlloc(size_t size);
extern "C" void linearFree(void *mem);
template <class T> struct RRLinAlloc
{
	typedef T value_type;
	RRLinAlloc() noexcept {}
	template <class U> RRLinAlloc(const RRLinAlloc<U>&) noexcept {}
	static bool IsLin(const void *p) { const uintptr_t a = (uintptr_t)p; return (a >= 0x14000000u && a < 0x1C000000u) || (a >= 0x30000000u && a < 0x40000000u); }
	T *allocate(size_t n)
	{
		void *p = linearAlloc(n * sizeof(T));
		if (!p) p = malloc(n * sizeof(T));
		if (!p) throw std::bad_alloc();
		return (T *)p;
	}
	void deallocate(T *p, size_t) noexcept { if (IsLin(p)) linearFree(p); else free(p); }
	template <class U> bool operator==(const RRLinAlloc<U>&) const noexcept { return true; }
	template <class U> bool operator!=(const RRLinAlloc<U>&) const noexcept { return false; }
};
namespace rr3ds_static {
static std::vector<SItem, RRLinAlloc<SItem>>  items;     // [numsubsectors*2 (floor/ceiling) + numsegs]
enum { RR3DS_MAXPOLYS = 16384, RR3DS_MAXGROUPS = 512 };
static std::vector<SPoly, RRLinAlloc<SPoly>>  polys;
static std::vector<SGroup, RRLinAlloc<SGroup>> groups;
static std::unordered_map<UINT64, UINT16> groupMap;
static std::vector<UINT32> secSig, secStamp;
static UINT32 gen = 1, stamp = 1;
// per wall, what has already been decided (type + signature). Reused if the FULL signature
// of its sectors (once per sector and pass) and its side/line data did not change.
// Gives EXACTLY the same sig/sigH/curSimple as recomputing (the full signature contains everything).
struct SegMemo { UINT32 fSig, bSig, sKey, sig, sigH; UINT32 fs, bs; UINT8 ok, simple; };
static std::vector<SegMemo, RRLinAlloc<SegMemo>> segMemo;
static int stMemoHit = 0, stMemoMiss = 0;
static boolean enabled = true, needReset = true;
static size_t segBase = 0;

static boolean capActive = false, capFail = false, capNever = false;
static size_t capItem = 0, capPoly0 = 0;
static int capVert0 = 0;

static int stHits = 0, stCapt = 0, stNever = 0, stResets = 0, stDrawFail = 0, stFrames = 0;
// why an item is "never": 1 changes a lot, 2 translucent wall, 3 flags (fog/horizon/no depth), 4 no texture, 5 >255 vertices
static int stWhyPlane = 0, stWhyWall = 0;
static int stWhy[6] = {0}; static UINT8 capWhy = 0; static int stAnim = 0;

static inline UINT32 H(UINT32 h, UINT32 v) { h ^= v; return h * 16777619u; }
static inline UINT32 HP(UINT32 h, const void *p) { return H(h, (UINT32)(uintptr_t)p); }

static UINT32 SlopeSig(UINT32 h, const pslope_t *s)
{
	if (!s) return H(h, 0x51u);
	h = H(h, (UINT32)s->o.z);
	h = H(h, (UINT32)s->normal.x); h = H(h, (UINT32)s->normal.y); h = H(h, (UINT32)s->normal.z);
	return h;
}

static UINT32 FlatSig(UINT32 h, INT32 pic)
{
	if (pic < 0 || (size_t)pic >= numlevelflats) return H(h, (UINT32)pic);
	const levelflat_t *lf = &levelflats[pic];
	h = H(h, lf->type);
	// ANIMATED flats (RBTCPR01, RBCSLI02, road edges...): signature uses the BASE frame;
	// the current frame is chosen at draw time (RR3DS_StaticFrameDraw, kind 2). Before, each
	// frame change turned the floor and all neighboring walls into "never".
	if (lf->speed && lf->type == LEVELFLAT_TEXTURE && lf->u.texture.basenum != -1)
		h = H(h, (UINT32)lf->u.texture.basenum);
	else if (lf->speed && lf->type == LEVELFLAT_FLAT && lf->u.flat.baselumpnum != LUMPERROR)
		h = H(h, (UINT32)lf->u.flat.baselumpnum);
	else
		h = H(h, (UINT32)lf->u.texture.num); // (or u.flat.lumpnum: same position)
	return h;
}

// current offset of the wall being processed (for sliding walls)
static boolean curHasOff = false; static fixed_t curTo = 0, curRo = 0;
static int stShift = 0;
// simple wall (2 sides, no slopes/lights/FOFs): patchable heights
static boolean curSimple = false; static UINT32 curSigH = 0; static UINT8 curKind = 0; static float curPlaneY = 0.0f;
static int stPatch = 0, stReshape = 0, stRetry = 0; // stRetry = "never" items that return to the cache
// [BLAME2] named culprits: items that are never cached because they change
static std::unordered_map<UINT64, UINT32> culp2;
// expensive diagnostics (CULP2/3/4, [SUB], [CHG]) only if sdmc:/3ds/rr_diag contains "1".
// Without that file the game (and the bench) runs clean.
static int diag = -1;
static inline boolean Diag(void)
{
	if (diag < 0)
	{
		diag = 0;
		diag = RRCfg_On("diag"); // rr_config.txt
		RR_DIAGPRINT("[DIAG] diagnostics %s\n", diag ? "ON" : "OFF");
	}
	return diag != 0;
}
} // namespace rr3ds_static (paused)
extern "C" int RR3DS_DiagOn(void) { return rr3ds_static::Diag() ? 1 : 0; } // for p_tick.c
namespace rr3ds_static {
static inline boolean IsNeverChg(size_t item) { return item < items.size() && items[item].state == ST_NEVER && items[item].why == 1; }
static inline void Culp2(UINT32 sec, UINT32 texA, UINT32 kind) { culp2[((UINT64)sec << 32) | ((UINT64)(kind & 0xF) << 28) | (texA & 0x0FFFFFFFu)]++; }
// [BLAME3] "never" floors: which field changes from one frame to the next (bits, see report)
struct PSnap { INT32 pic, ll, fll; const void *cm; UINT32 fl, slz, sln; fixed_t xo, yo, h; angle_t an; UINT32 sf; const void *sl; UINT32 sig, sigH, view; const void *ecm; UINT8 ok; };
static std::vector<PSnap> psnap;
static std::unordered_map<UINT32, std::pair<UINT32, UINT32>> culp3; // sector -> (bits, times)
// [BLAME4] "never" walls: which part of the signature changes. bits 1 front_sector 2 back_sector 4 side 8 line 16 heights
struct WSnap { UINT32 c[14]; UINT8 ok; }; // 14 parts
static std::vector<WSnap> wsnap;
static std::unordered_map<UINT64, std::pair<UINT32, UINT32>> culp4; // (front<<32|back) -> (bits, times)
static void Culp4(size_t seg, UINT32 fsec, UINT32 bsec, const UINT32 *c)
{
	if (wsnap.size() != numsegs) wsnap.assign(numsegs, WSnap{});
	if (seg >= wsnap.size()) return;
	WSnap &o = wsnap[seg];
	if (o.ok)
	{
		UINT32 b = 0;
		for (int i = 0; i < 14; i++) b |= (o.c[i] != c[i]) ? (1u << i) : 0u;
		auto &e = culp4[((UINT64)fsec << 32) | bsec]; e.first |= b; e.second++;
	}
	for (int i = 0; i < 14; i++) o.c[i] = c[i];
	o.ok = 1;
}
static void Culp3(size_t sub, const sector_t *s, INT32 fll, const void *cm, UINT32 fl, fixed_t h, UINT32 sig, UINT32 sigH)
{
	if (psnap.size() != numsubsectors) psnap.assign(numsubsectors, PSnap{});
	if (sub >= psnap.size()) return;
	PSnap n; n.pic = s->floorpic; n.ll = s->lightlevel; n.fll = fll; n.cm = cm; n.fl = fl;
	n.sl = s->f_slope; n.slz = s->f_slope ? (UINT32)s->f_slope->o.z : 0; n.sln = s->f_slope ? (UINT32)(s->f_slope->normal.x ^ s->f_slope->normal.y ^ s->f_slope->normal.z) : 0;
	n.xo = s->floor_xoffs ^ s->floor_yoffs; n.yo = 0; n.an = s->floorpic_angle; n.sf = (UINT32)s->flags; n.h = h; n.ok = 1;
	n.sig = sig; n.sigH = sigH; n.view = (UINT32)::RR3DS_ViewNow(); n.ecm = s->extra_colormap;
	PSnap &o = psnap[sub];
	if (o.ok)
	{
		UINT32 b = 0;
		b |= (o.pic != n.pic) ? 1u : 0u; b |= (o.ll != n.ll) ? 2u : 0u; b |= (o.fll != n.fll) ? 4u : 0u; b |= (o.cm != n.cm) ? 8u : 0u;
		b |= (o.fl != n.fl) ? 16u : 0u; b |= (o.slz != n.slz) ? 32u : 0u; b |= (o.sln != n.sln) ? 64u : 0u; b |= (o.xo != n.xo) ? 128u : 0u;
		b |= (o.an != n.an) ? 256u : 0u; b |= (o.sf != n.sf) ? 512u : 0u; b |= (o.h != n.h) ? 1024u : 0u; b |= (o.sl != n.sl) ? 2048u : 0u;
		b |= (o.sig != n.sig) ? 4096u : 0u; b |= (o.sigH != n.sigH) ? 8192u : 0u; b |= (o.view != n.view) ? 16384u : 0u; b |= (o.ecm != n.ecm) ? 32768u : 0u;
		auto &e = culp3[(UINT32)(s - sectors)]; e.first |= b; e.second++;
	}
	o = n;
}
} // namespace rr3ds_static (paused)
static boolean RR3DS_PatchSegHeights(const rr3ds_static::SItem &it);
static boolean RR3DS_PatchHeights(size_t item, const rr3ds_static::SItem &it);
namespace rr3ds_static {
static UINT32 SideSig(UINT32 h, const side_t *sd, boolean withOffsets = true)
{
	if (!sd) return H(h, 0x5Du);
	if (withOffsets) h = H(h, (UINT32)sd->textureoffset);
	h = H(h, (UINT32)sd->rowoffset); // vertical: can clip the geometry (midtexture), stays in the signature
	// BASE texture (without the animation frame): animated walls
	// stay cached and the current frame is chosen at draw time (RR3DS_StaticFrameDraw).
	h = H(h, (UINT32)sd->toptexture);
	h = H(h, (UINT32)sd->midtexture);
	h = H(h, (UINT32)sd->bottomtexture);
	h = H(h, (UINT32)sd->repeatcnt);
	return h;
}

// mode 0 = base (floors/ceilings), 1 = one-sided wall (lights, no FOFs), 2 = full
static UINT32 SectorSigRaw(const sector_t *s, int mode = 2)
{
	UINT32 h = 2166136261u;
	// +16 = floor-only data, +32 = ceiling-only data (for planes: a ceiling that
	// moves or scrolls no longer turns the floor of the same sector into "never", and vice versa)
	const boolean onlyF = (mode & 16) != 0, onlyC = (mode & 32) != 0;
	if (!(mode & 4)) { if (!onlyC) h = H(h, (UINT32)s->floorheight); if (!onlyF) h = H(h, (UINT32)s->ceilingheight); }
	const boolean noFofH = (mode & 8) != 0; // without FOF heights or light list
	mode &= 3;
	if (onlyF || onlyC)
	{
		const boolean f = onlyF;
		h = H(h, (UINT32)(f ? s->floorpic : s->ceilingpic)); h = FlatSig(h, f ? s->floorpic : s->ceilingpic);
		h = H(h, (UINT32)s->lightlevel);
		h = H(h, (UINT32)(f ? s->floorlightlevel : s->ceilinglightlevel));
		h = H(h, (UINT32)(f ? s->floorlightabsolute : s->ceilinglightabsolute));
		const INT32 ls = f ? s->floorlightsec : s->ceilinglightsec; h = H(h, (UINT32)ls);
		if (ls >= 0 && (size_t)ls < numsectors) h = H(h, (UINT32)sectors[ls].lightlevel);
		h = HP(h, s->extra_colormap);
		h = H(h, (UINT32)(f ? s->floor_xoffs : s->ceiling_xoffs)); h = H(h, (UINT32)(f ? s->floor_yoffs : s->ceiling_yoffs));
		h = H(h, (UINT32)(f ? s->floorpic_angle : s->ceilingpic_angle));
		h = H(h, (UINT32)s->flags);
		h = SlopeSig(h, f ? s->f_slope : s->c_slope);
		return h;
	}
	h = H(h, (UINT32)s->floorpic);    h = H(h, (UINT32)s->ceilingpic);
	h = FlatSig(h, s->floorpic);      h = FlatSig(h, s->ceilingpic);
	h = H(h, (UINT32)s->lightlevel);
	h = H(h, (UINT32)s->floorlightlevel); h = H(h, (UINT32)s->ceilinglightlevel);
	h = H(h, (UINT32)s->floorlightabsolute | ((UINT32)s->ceilinglightabsolute << 1));
	h = H(h, (UINT32)s->floorlightsec); h = H(h, (UINT32)s->ceilinglightsec);
	if (s->floorlightsec >= 0 && (size_t)s->floorlightsec < numsectors) h = H(h, (UINT32)sectors[s->floorlightsec].lightlevel);
	if (s->ceilinglightsec >= 0 && (size_t)s->ceilinglightsec < numsectors) h = H(h, (UINT32)sectors[s->ceilinglightsec].lightlevel);
	h = HP(h, s->extra_colormap);
	h = H(h, (UINT32)s->floor_xoffs); h = H(h, (UINT32)s->floor_yoffs);
	h = H(h, (UINT32)s->ceiling_xoffs); h = H(h, (UINT32)s->ceiling_yoffs);
	h = H(h, (UINT32)s->floorpic_angle); h = H(h, (UINT32)s->ceilingpic_angle);
	h = H(h, (UINT32)s->flags);
	h = SlopeSig(h, s->f_slope); h = SlopeSig(h, s->c_slope);
	if (mode == 0) return h; // floors/ceilings: light and colormap go separately in their signature
	// only the light bands that change something visible (different light/colormap)
	// or that clip the wall (CUTSOLIDS/CUTEXTRA). The others split the wall
	// into identical pieces: moving them does not change what is seen.
	{
		UINT32 pl = 0xFFFFFFFFu; const extracolormap_t *pc = (const extracolormap_t *)(uintptr_t)1;
		for (INT32 i = 0; !noFofH && i < s->numlights && s->lightlist; i++)
		{
			const lightlist_t *l = &s->lightlist[i];
			const boolean cut = (l->flags & (FOF_CUTSOLIDS|FOF_CUTEXTRA)) != 0;
			boolean vis = false;
			if (!(l->flags & FOF_NOSHADE))
			{
				const UINT32 ll = l->lightlevel ? (UINT32)*l->lightlevel : 0xFFFFu;
				const extracolormap_t *lc = l->extra_colormap ? *l->extra_colormap : NULL;
				if (ll != pl || lc != pc) { vis = true; pl = ll; pc = lc; }
			}
			if (!cut && !vis) continue;
			h = H(h, (UINT32)l->height); h = H(h, (UINT32)l->flags);
			h = H(h, pl); h = HP(h, pc);
			h = SlopeSig(h, l->slope);
			if (cut && l->caster) h = H(h, l->caster->bottomheight ? (UINT32)*l->caster->bottomheight : 0);
		}
	}
	if (mode == 1) return h; // one-sided wall: FOFs do not draw walls here
	for (const ffloor_t *r = s->ffloors; r; r = r->next)
	{
		h = H(h, (UINT32)r->fofflags); h = H(h, (UINT32)r->alpha); h = H(h, (UINT32)r->blend);
		if (!noFofH)
		{
			h = H(h, r->topheight ? (UINT32)*r->topheight : 0);
			h = H(h, r->bottomheight ? (UINT32)*r->bottomheight : 0);
		}
		h = H(h, r->toppic ? (UINT32)*r->toppic : 0);
		h = H(h, r->bottompic ? (UINT32)*r->bottompic : 0);
		h = H(h, r->toplightlevel ? (UINT32)*r->toplightlevel : 0);
		h = SlopeSig(h, r->t_slope ? *r->t_slope : NULL);
		h = SlopeSig(h, r->b_slope ? *r->b_slope : NULL);
		if (r->master && r->master->sidenum[0] != 0xffff)
			h = SideSig(h, &sides[r->master->sidenum[0]]);
	}
	return h;
}

// Signature of a sector, once per render pass.
static std::vector<UINT32> secSigB, secStampB;
// signature without FOFs or light list (for sector floors/ceilings).
[[maybe_unused]] static UINT32 SectorSigBase(const sector_t *s)
{
	const size_t i = (size_t)(s - sectors);
	if (i >= secSigB.size())
		return SectorSigRaw(s, 0);
	if (secStampB[i] != stamp) { secStampB[i] = stamp; secSigB[i] = SectorSigRaw(s, 0); }
	return secSigB[i];
}
static std::vector<UINT32> secSigW, secStampW;
// signature for one-sided walls (no FOFs)
static UINT32 SectorSigWall1(const sector_t *s)
{
	const size_t i = (size_t)(s - sectors);
	if (i >= secSigW.size())
		return SectorSigRaw(s, 1);
	if (secStampW[i] != stamp) { secStampW[i] = stamp; secSigW[i] = SectorSigRaw(s, 1); }
	return secSigW[i];
}
static std::vector<UINT32> secSigN, secStampN;
// full signature WITHOUT floor/ceiling heights (simple walls: heights are patched)
static UINT32 SectorSigNoH(const sector_t *s)
{
	const size_t i = (size_t)(s - sectors);
	if (i >= secSigN.size())
		return SectorSigRaw(s, 2 | 4);
	if (secStampN[i] != stamp) { secStampN[i] = stamp; secSigN[i] = SectorSigRaw(s, 2 | 4); }
	return secSigN[i];
}
static UINT32 SectorSig(const sector_t *s)
{
	const size_t i = (size_t)(s - sectors);
	if (i >= secSig.size())
		return SectorSigRaw(s);
	if (secStamp[i] != stamp) { secStamp[i] = stamp; secSig[i] = SectorSigRaw(s); }
	return secSig[i];
}

static inline void Emit(const SItem &it)
{
	for (UINT32 p = it.firstPoly; p < it.firstPoly + it.numPolys; p++)
	{
		const SPoly &sp = polys[p];
		std::vector<UINT16> &ix = groups[sp.group].idx;
		for (UINT16 k = 1; k + 1 < sp.n; k++)
		{
			ix.push_back(sp.base);
			ix.push_back((UINT16)(sp.base + k));
			ix.push_back((UINT16)(sp.base + k + 1));
		}
	}
}

// true = already drawn from the cache (skip the normal path).
// false = draw normally (if applicable, it is being captured).
static boolean Use(size_t item, UINT32 sig)
{
	if (!enabled || item >= items.size() || capActive)
		return false;
	SItem &it = items[item];
	if (it.state == ST_NEVER)
	{
		// [BLAME3] showed "never" floors whose signature NO LONGER changes (marked at the start of
		// the race). If the signature stays the same 60<<strikes times in a row, cache again.
		if (it.why == 1)
		{
			if (it.sig == sig && it.sigH == curSigH)
			{
				const UINT32 need = 60u << (it.strikes < 4 ? it.strikes : 4);
				if (++it.stable >= need)
				{
					if (it.strikes < 255) it.strikes++;
					it.stable = 0; it.changes = 0; stRetry++;
					goto recapture;
				}
			}
			else { it.sig = sig; it.sigH = curSigH; it.stable = 0; }
		}
		stWhy[it.why < 6 ? it.why : 0]++;
		if (it.why == 1) { if (item < segBase) stWhyPlane++; else stWhyWall++; }
		return false;
	}
	if (it.state == ST_VALID && it.gen == gen)
	{
		boolean hOk = true;
		if (it.sig == sig && it.sigH != curSigH)
		{
			hOk = false;
			// only floor/ceiling heights changed: patch y/t without recomputing
			if (it.patchOk && curSimple && RR3DS_PatchHeights(item, it)) { it.sigH = curSigH; stPatch++; hOk = true; }
			else if (it.patchOk && curSimple) { stReshape++; goto recapture; } // a piece appears/disappears: recapture without counting a change
		}
		if (it.sig == sig && hOk)
		{
			if (it.hasOff && curHasOff && (it.to != curTo || it.ro != curRo))
			{
				const float dto = (float)(curTo - it.to), dro = (float)(curRo - it.ro);
				for (UINT32 p = it.firstPoly; p < it.firstPoly + it.numPolys; p++)
				{
					const SPoly &sp = polys[p];
					if (sp.sx != 0.0f || sp.sy != 0.0f)
						I_3DS_StaticShiftST(sp.base, sp.n, dto * sp.sx, dro * sp.sy);
				}
				it.to = curTo; it.ro = curRo; stShift++;
			}
			Emit(it); stHits++; return true;
		}
		if (++it.changes >= 4) { it.state = ST_NEVER; it.why = 1; stNever++; return false; }
	}
recapture:
	it.state = ST_NONE;
	it.sig = sig;
	it.sigH = curSigH; it.kmask = 0; it.fofMask = 0; it.patchOk = curSimple ? 1 : 0;
	capActive = true; capFail = false; capNever = false; capWhy = 0;
	capItem = item; capPoly0 = polys.size(); capVert0 = I_3DS_StaticUsed();
	return false;
}

static void EndCapture(void)
{
	if (!capActive)
		return;
	capActive = false;
	SItem &it = items[capItem];
	if (capNever || capFail)
	{
		polys.resize(capPoly0);
		I_3DS_StaticTruncate(capVert0);
		if (capNever) { it.state = ST_NEVER; it.why = capWhy; stNever++; }
		else needReset = true; // pool full: empty and recapture
		return;
	}
	it.state = ST_VALID; it.gen = gen;
	it.hasOff = curHasOff ? 1 : 0; it.to = curTo; it.ro = curRo;
	it.firstPoly = (UINT32)capPoly0;
	it.numPolys = (UINT16)(polys.size() - capPoly0);
	stCapt++;
}

static void Never(void) { if (capActive) { capNever = true; if (!capWhy) capWhy = 2; } }

static boolean SectorOk(const sector_t *s) { return !s || s->heightsec == -1; }
} // namespace rr3ds_static

// FAST PATH per zone (subsector). The normal walk stores, per floor/ceiling/wall,
// whether it came out whole from the cache (K_HIT) or if there was nothing to draw (K_EMPTY). In following frames,
// if the zone's sectors did not change (height, light, floor texture, colormap),
// the zone is drawn directly from the cache: no signatures, no angles, no per-wall clipping.
// 1 of every 16 frames each zone goes through the normal path (rare changes: wall textures).
// Only with precomputed visibility (.rrv): without per-wall clipping, the .rrv is what bounds drawing.
// rr_config: fast_sub=0 turns it off.
namespace rr3ds_fast {
	enum { K_NONE = 0, K_HIT = 1, K_EMPTY = 2 };
	static const UINT32 kMask = 0x3FFFFFFFu;
	struct Lite { UINT32 seen, chg, hash; };
	static std::vector<UINT32, RRLinAlloc<UINT32>> rec; // same as rr3ds_static::items
	static std::vector<Lite, RRLinAlloc<Lite>> lite;    // per sector
	static UINT32 stamp = 1, mainFrame = 0;
	static int on = 0;
	static UINT32 nFast = 0, nSlow = 0, nEmit = 0, nSegSlow = 0, nFrames = 0;
	static UINT64 tAll = 0, tSpr = 0, tSegSlow = 0; static UINT32 nSamp = 0; // diag: 1 of every 4 frames
	static void LevelInit(size_t nItems)
	{
		static int cfg = -1;
		if (cfg < 0) cfg = RRCfg_Int("fast_sub", 1);
		on = 0;
		decltype(rec)().swap(rec); decltype(lite)().swap(lite);
		if (!cfg || !rr3ds_static::enabled || !nItems) return;
		try { rec.assign(nItems, 0); lite.assign(numsectors, Lite{ 0, 0, 0 }); on = 1; }
		catch (const std::bad_alloc &) { decltype(rec)().swap(rec); decltype(lite)().swap(lite); RR_DIAGPRINT("[FASTPATH] out of memory: fast path OFF\n"); }
	}
	static inline void Set(size_t item, UINT32 kind) { if (item < rec.size()) rec[item] = (kind << 30) | (stamp & kMask); }
	static inline void Clr(size_t item) { if (item < rec.size()) rec[item] = 0; }
	// last 'stamp' at which something cheap to check changed in the sector
	static inline UINT32 SecChg(const sector_t *s)
	{
		Lite &l = lite[s - sectors];
		if (l.seen != stamp)
		{
			using rr3ds_static::H;
			l.seen = stamp;
			UINT32 h = H(H(H(H(2166136261u, (UINT32)s->floorheight), (UINT32)s->ceilingheight), (UINT32)s->floorpic), (UINT32)s->ceilingpic);
			h = H(H(H(h, (UINT32)(UINT16)s->lightlevel), (UINT32)(UINT16)s->floorlightlevel), (UINT32)(UINT16)s->ceilinglightlevel);
			h = H(H(H(h, (UINT32)(uintptr_t)s->extra_colormap), (UINT32)(uintptr_t)s->ffloors), (UINT32)s->numlights);
			h = H(H(H(h, (UINT32)(uintptr_t)s->f_slope), (UINT32)(uintptr_t)s->c_slope), (UINT32)s->heightsec);
			if (h != l.hash || !l.chg) { l.hash = h; l.chg = stamp; }
		}
		return l.chg;
	}
	static void Frame(void) // once per main view
	{
		mainFrame++;
		if (++nFrames < 90) return;
		if (on && rr3ds_static::Diag())
			RR_DIAGPRINT("[FASTPATH] 90 frames: fast zones=%u slow=%u per frame | cached walls=%u, regular path=%u\n",
				(unsigned)(nFast / 90), (unsigned)(nSlow / 90), (unsigned)(nEmit / 90), (unsigned)(nSegSlow / 90));
		if (on && rr3ds_static::Diag() && nSamp)
		{
			const double u = 1000000.0 / (double)I_GetPrecisePrecision() / nSamp;
			RR_DIAGPRINT("[FASTPATH] us/frame (sample %u): fast path total=%.0f | sprites=%.0f | walls on regular path=%.0f\n",
				(unsigned)nSamp, tAll * u, tSpr * u, tSegSlow * u);
		}
		tAll = tSpr = tSegSlow = 0; nSamp = 0;
		nFast = nSlow = nEmit = nSegSlow = nFrames = 0;
	}
}

// Called by HWR_ProcessPolygon (hw_batching.cpp) for every polygon.
void RR3DS_StaticCapturePoly(FSurfaceInfo *surf, FOutVector *v, FUINT n, FBITFIELD flags, boolean horizon)
{
	using namespace rr3ds_static;
	if (!capActive)
		return;
	if (horizon || n < 3 || n > 255 || (flags & (PF_Fog | PF_NoDepthTest))
		|| (flags & PF_EnvironmentTrans)) // note: PF_Blending includes PF_Invert == PF_Occlude
	{
		capNever = true;
		if (!capWhy) capWhy = (n > 255) ? 5 : 3;
		return;
	}
	// zero-area polygon (e.g. height-0 wall due to slope rounding): skipped.
	// 3DS generated them and PC did not (5 walls 765NSPB): invisible anyway.
	{
		double ax = 0, ay = 0, az = 0;
		for (FUINT i = 1; i + 1 < n; i++)
		{
			const double ux = v[i].x - v[0].x, uy = v[i].y - v[0].y, uz = v[i].z - v[0].z;
			const double wx = v[i+1].x - v[0].x, wy = v[i+1].y - v[0].y, wz = v[i+1].z - v[0].z;
			ax += uy*wz - uz*wy; ay += uz*wx - ux*wz; az += ux*wy - uy*wx;
		}
		if (ax*ax + ay*ay + az*az < 1.0) return; // area < 0.5 (0.001 slits)
	}
	GLMipmap_t *mip = (flags & PF_NoTexture) ? NULL : rr3ds_lastmip;
	if (!mip && !(flags & PF_NoTexture)) { capNever = true; if (!capWhy) capWhy = 4; return; }
	// rgb = tint; the alpha byte carries the LIGHT LEVEL (static.v.pica does
	// distance darkening). Color fade (colored fog): it is not
	// caches (the normal path does it correctly).
	if (surf && (flags & PF_Modulated) && (surf->FadeColor.rgba & 0x00FFFFFFu))
	{
		capNever = true;
		if (!capWhy) capWhy = 3;
		return;
	}
#ifndef __3DS__
	// PC: the cache draws with per-vertex light (alpha) and black fade by default; with
	// shaders the tint goes in a uniform (not in PolyColor). Anything that does not fit there: normal path.
	if (surf && !RR3DS_LIGHT3DS
		&& (surf->TintColor.s.alpha != 0 || surf->FadeColor.rgba != GL_DEFAULTFOG
		|| surf->LightInfo.fade_start != 0 || surf->LightInfo.fade_end != 31
		|| surf->LightInfo.directional || surf->PolyColor.s.alpha != 0xFF || (flags & PF_Ripple)))
	{
		capNever = true;
		if (!capWhy) capWhy = 3;
		return;
	}
#endif
	const UINT32 col = (surf && (flags & (PF_Modulated | PF_NoTexture)))
		? ((surf->PolyColor.rgba & 0x00FFFFFFu) | ((UINT32)(surf->LightInfo.light_level & 0xFF) << 24))
		: 0xFFFFFFFFu;
	// FIXED memory (Old 3DS 64 MB): polys/groups reserved when loading the map; never
	// grow (when growing, the vector requested a contiguous block of double the size and on fragmented 64 MB
	// there was none -> std::bad_alloc mid-race). Full = flush and recapture.
	if (polys.size() >= polys.capacity() || groups.size() >= groups.capacity()) { capFail = true; return; }
	const int base = I_3DS_StaticAdd(v, (int)n, col);
	if (base < 0) { capFail = true; return; }
	// wall textures are grouped by BASE texture (animated ones included).
	const boolean byBase = (mip && rr3ds_texreq.kind == 1 && rr3ds_texreq.b >= 0 && rr3ds_texreq.b < numtextures
		&& R_GetTextureNum(rr3ds_texreq.b) == rr3ds_texreq.a);
	const UINT64 key = byBase ? (((UINT64)(UINT32)(rr3ds_texreq.b + 1) << 32) | (UINT32)flags)
		: (((UINT64)(uintptr_t)mip << 32) | (UINT32)flags);
	UINT16 g;
	auto f = groupMap.find(key);
	if (f == groupMap.end())
	{
		if (groups.size() >= 65535) { capFail = true; return; }
		g = (UINT16)groups.size();
		groups.push_back(SGroup{ mip, flags, {}, rr3ds_texreq, mip ? rr3ds_lastbright : NULL });
		groupMap.emplace(key, g);
	}
	else g = f->second;
	float sx = 0.0f, sy = 0.0f;
	if (rr3ds_texreq.kind == 1 && rr3ds_lastgrtex && mip == &rr3ds_lastgrtex->mipmap)
		{ sx = rr3ds_lastgrtex->scaleX; sy = rr3ds_lastgrtex->scaleY; }
	else if (byBase && rr3ds_lastgrtex)
		{ sx = rr3ds_lastgrtex->scaleX; sy = rr3ds_lastgrtex->scaleY; }
	polys.push_back(SPoly{ (UINT16)base, (UINT8)n, curKind, g, sx, sy });
	if (curSimple)
	{
		SItem &ci = items[capItem];
		if (curKind >= 8) // FOF wall (from the back sector), bit = FOF order
		{
			const UINT32 b = 1u << (curKind - 8);
			if (n != 4 || sy == 0.0f || (ci.fofMask & b)) ci.patchOk = 0;
			ci.fofMask |= b;
		}
		else
		{
			if (curKind == 0 || (curKind != 6 && n != 4) || (curKind <= 3 && sy == 0.0f) || (curKind != 6 && (ci.kmask & (1u << curKind)))) ci.patchOk = 0;
			ci.kmask |= (UINT8)(1u << curKind);
		}
	}
}


// diagnostics: which sector/side fields change over time.
namespace rr3ds_chg {
	struct SecSnap { fixed_t fh, ch, fx, fy, cx, cy; INT16 light; INT32 fpic, cpic; void *cmap; INT32 nl; angle_t fa, ca; };
	struct SideSnap { fixed_t to, ro; INT32 t, m, b; };
	static std::vector<SecSnap> sec; static std::vector<SideSnap> sid;
	static int frames = 0;
	static SecSnap Snap(const sector_t *s) { return SecSnap{ s->floorheight, s->ceilingheight, s->floor_xoffs, s->floor_yoffs, s->ceiling_xoffs, s->ceiling_yoffs, s->lightlevel, s->floorpic, s->ceilingpic, s->extra_colormap, s->numlights, s->floorpic_angle, s->ceilingpic_angle }; }
	static SideSnap SnapS(const side_t *d) { return SideSnap{ d->textureoffset, d->rowoffset, d->toptexture, d->midtexture, d->bottomtexture }; }
	void Tick(void)
	{
		if (sec.size() != numsectors || sid.size() != numsides)
		{
			sec.resize(numsectors); sid.resize(numsides);
			for (size_t i = 0; i < numsectors; i++) sec[i] = Snap(&sectors[i]);
			for (size_t i = 0; i < numsides; i++) sid[i] = SnapS(&sides[i]);
			frames = 0; return;
		}
		if (++frames < 60) return;
		frames = 0;
		int c[12] = {0};
		for (size_t i = 0; i < numsectors; i++)
		{
			SecSnap n = Snap(&sectors[i]), &o = sec[i];
			c[0] += n.fh != o.fh; c[1] += n.ch != o.ch; c[2] += (n.fx != o.fx || n.fy != o.fy); c[3] += (n.cx != o.cx || n.cy != o.cy);
			c[4] += n.light != o.light; c[5] += (n.fpic != o.fpic || n.cpic != o.cpic); c[6] += n.cmap != o.cmap; c[7] += n.nl != o.nl;
			c[8] += (n.fa != o.fa || n.ca != o.ca);
			o = n;
		}
		for (size_t i = 0; i < numsides; i++)
		{
			SideSnap n = SnapS(&sides[i]), &o = sid[i];
			c[9] += (n.to != o.to || n.ro != o.ro); c[10] += (n.t != o.t || n.m != o.m || n.b != o.b);
			o = n;
		}
		RR_DIAGPRINT("[CHG] 60f sectors: floor_h=%d ceil_h=%d floor_off=%d ceil_off=%d light=%d flat=%d colormap=%d lights=%d angle=%d | sides: off=%d texture=%d\n",
			c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9], c[10]);
		RR_DIAGPRINT("[EST] never(changes) planes=%d walls=%d\n", rr3ds_static::stWhyPlane, rr3ds_static::stWhyWall);
			{
				// [BLAME2] once per map (3rd report): top 25 by times/60f, with texture name
				static int rep2 = 0; static INT16 map2 = -1;
				if (gamemap != map2) { map2 = gamemap; rep2 = 0; rr3ds_static::culp2.clear(); }
				if (++rep2 == 3)
				{
					std::vector<std::pair<UINT32, UINT64>> v;
					for (auto &kv : rr3ds_static::culp2) v.push_back({kv.second, kv.first});
					std::sort(v.begin(), v.end(), [](const std::pair<UINT32, UINT64> &a, const std::pair<UINT32, UINT64> &b){ return a.first > b.first; });
					for (size_t i = 0; i < v.size() && i < 25; i++)
					{
						const UINT32 sec = (UINT32)(v[i].second >> 32), kind = (UINT32)(v[i].second >> 28) & 0xF;
						const INT32 tx = (INT32)(v[i].second & 0x0FFFFFFFu);
						char nm[9] = "?";
						if (kind == 1) { if (tx > 0 && tx < numtextures && textures[tx]) { memcpy(nm, textures[tx]->name, 8); nm[8] = 0; } else strcpy(nm, "(none)"); }
						else if (tx >= 0 && (size_t)tx < numlevelflats) { memcpy(nm, levelflats[tx].name, 8); nm[8] = 0; }
						const sector_t *sc = sec < numsectors ? &sectors[sec] : NULL;
						RR_DIAGPRINT("[BLAME2] %s sector %u tag %d texture=%s count=%u\n", kind == 1 ? "wall" : (kind == 2 ? "floor" : "ceiling"),
							(unsigned)sec, (sc && sc->tags.count) ? (int)sc->tags.tags[0] : 0, nm, (unsigned)v[i].first);
					}
					RR_DIAGPRINT("[BLAME2] total distinct=%u\n", (unsigned)v.size());
					// [BLAME3] bits: 1 flat 2 sector_light 4 floor_light 8 colormap 16 blend 32 slope_z 64 slope_normal 128 offset 256 angle 512
					std::vector<std::pair<UINT32, UINT32>> w;
					for (auto &kv : rr3ds_static::culp3) w.push_back({kv.second.second, kv.first});
					std::sort(w.begin(), w.end(), [](const std::pair<UINT32, UINT32> &a, const std::pair<UINT32, UINT32> &b){ return a.first > b.first; });
					for (size_t i = 0; i < w.size() && i < 10; i++)
						RR_DIAGPRINT("[BLAME3] floor sector %u bits=%u count=%u\n", (unsigned)w[i].second, (unsigned)rr3ds_static::culp3[w[i].second].first, (unsigned)w[i].first);
					// [BLAME4] bits: 1 front_sector 2 back_sector 4 side 8 line 16 heights
					std::vector<std::pair<UINT32, UINT64>> w4;
					for (auto &kv : rr3ds_static::culp4) w4.push_back({kv.second.second, kv.first});
					std::sort(w4.begin(), w4.end(), [](const std::pair<UINT32, UINT64> &a, const std::pair<UINT32, UINT64> &b){ return a.first > b.first; });
					for (size_t i = 0; i < w4.size() && i < 30; i++)
						RR_DIAGPRINT("[BLAME4] wall front %u back %u bits=%u count=%u\n", (unsigned)(w4[i].second >> 32), (unsigned)(w4[i].second & 0xFFFFFFFFu),
							(unsigned)rr3ds_static::culp4[w4[i].second].first, (unsigned)w4[i].first);
				}
				rr3ds_static::culp2.clear(); rr3ds_static::culp3.clear(); rr3ds_static::culp4.clear();
			}
		rr3ds_static::stWhyPlane = rr3ds_static::stWhyWall = 0;
	}
}
namespace rr3ds_gen {
	static boolean busy = false; // generating a map package (see RR3DS_GenStep)
	// X offset of each side at map START. Scrolling walls
	// (765MQB1/MQB2/LFWL1/_FTVB) change textureoffset every tic; when generating, this
	// fixed value is used so the .rr3 does not depend on timing (PC and 3DS match).
	static std::vector<fixed_t> baseOff;
	static fixed_t SideOff(const side_t *sd)
	{
		const size_t i = (size_t)(sd - sides);
		return (busy && i < baseOff.size()) ? baseOff[i] : sd->textureoffset;
	}
}
// HWR_LoadLevel
static void RR3DS_StaticLevelInit(void)
{
	// offsets at map LOAD time (before, at the start of generation: PC and 3DS at different tics)
	rr3ds_gen::baseOff.resize(numsides);
	for (size_t i = 0; i < numsides; i++) rr3ds_gen::baseOff[i] = sides[i].textureoffset;
	using namespace rr3ds_static;
	enabled = !RRCfg_On("nostatic"); // rr_config.txt
#ifndef __3DS__
	// PC: the static cache is used by the preparer (rr_genmap).
	// and in Low-End with OpenGL (drawn by r_opengl.cpp DrawStaticPool, like on 3DS).
	if (!RR3DS_PcPreparer() && !(rr_lowend && rendermode == render_opengl))
		enabled = false;
#endif
	segBase = numsubsectors * 2;
	try { items.assign(segBase + numsegs, SItem{ 0, 0, 0, 0, ST_NONE, 0, 0, 0, 0, 0, 0, 0, 0, 0 }); }
	catch (const std::bad_alloc &) // out of memory -> no cache (slow but does not die)
	{
		decltype(items)().swap(items); enabled = false;
		RR_DIAGPRINT("[EST] OUT OF MEMORY for the cache (%u items): cache OFF\n", (unsigned)(segBase + numsegs));
	}
	secSig.assign(numsectors, 0);
	secStamp.assign(numsectors, 0);
	try { segMemo.assign(numsegs, SegMemo{}); } // ~36 B x segs, linear memory
	catch (const std::bad_alloc &) { decltype(segMemo)().swap(segMemo); RR_DIAGPRINT("[EST] out of memory for segMemo: A1 OFF\n"); }
	rr3ds_fast::LevelInit(items.size());
	secSigB.assign(numsectors, 0);
	secStampB.assign(numsectors, 0);
	secSigW.assign(numsectors, 0);
	secStampW.assign(numsectors, 0);
	secSigN.assign(numsectors, 0);
	secStampN.assign(numsectors, 0);
	// fixed reservation (see AddPoly). ~16 B per poly; the maximum seen was ~11.6k.
	try {
	if (polys.capacity() < RR3DS_MAXPOLYS) { decltype(polys)().swap(polys); polys.reserve(RR3DS_MAXPOLYS); }
	if (groups.capacity() < RR3DS_MAXGROUPS) { decltype(groups)().swap(groups); groups.reserve(RR3DS_MAXGROUPS); }
	} catch (const std::bad_alloc &) { enabled = false; RR_DIAGPRINT("[EST] OUT OF MEMORY for polys/groups: cache OFF\n"); }
	groupMap.reserve(RR3DS_MAXGROUPS);
	needReset = true;
	RR_DIAGPRINT("[EST] map: %u subsectors, %u segs, cache %s\n",
		(unsigned)numsubsectors, (unsigned)numsegs, enabled ? "ON" : "OFF");
}

// on 3DS, textures with holes do NOT have TF_TRANSPARENT (ringracers_3ds_defs.pk3) and go
// through HWR_ProjectWall (PF_Masked). In the PC preparer they do have it -> transparent wall ("never").
// When GENERATING on PC, TF_TRANSPARENT is ignored to give the same result as 3DS (all 135 walls).
#if defined(RR_PCGEN) && !defined(__3DS__)
#define RR3DS_TEXTRANSP(t) (((t)->mipmap.flags & TF_TRANSPARENT) && !rr3ds_gen::busy)
#else
#define RR3DS_TEXTRANSP(t) ((t)->mipmap.flags & TF_TRANSPARENT)
#endif
static void RR3DS_GenStep(void);

// Before HWR_RenderBSPNode
static void RR3DS_StaticFrameBegin(void)
{
	using namespace rr3ds_static;
	stamp++;
	if (needReset && I_3DS_StaticReset())
	{
		needReset = false;
		gen++;
		polys.clear();
		groups.clear();
		groupMap.clear();
		stResets++;
	}
}

static inline bool RR3DS_LfValid(const void *p)
{
	const levelflat_t *lf = (const levelflat_t *)p;
	return levelflats && lf >= levelflats && lf < levelflats + numlevelflats;
}

// After the BSP (and HWR_RenderBatches): draws what is in the cache.
static void RR3DS_StaticFrameDraw(void)
{
	using namespace rr3ds_static;
	EndCapture();
	boolean any = false;
	for (SGroup &g : groups)
	{
		if (g.idx.empty())
			continue;
		any = true;
		// animated texture: use the current frame.
		if (g.req.kind == 1 && g.req.b >= 0 && g.req.b < numtextures && g.mip && !(g.flags & PF_NoTexture))
		{
			const INT32 cur = R_GetTextureNum(g.req.b);
			if (cur != g.req.a && cur >= 0 && cur < numtextures)
			{
				GLMapTexture_t *t = HWR_GetTexture(cur, g.req.b);
				if (t) { g.mip = &t->mipmap; g.req.a = cur; stAnim++; }
			}
		}
		// animated flat: use the current frame
		// the stored pointer can go stale if levelflats is reallocated
		// (console crash in v1.0.80): only use it if it still lies inside the array.
		if (g.req.kind == 2 && g.req.p && !RR3DS_LfValid(g.req.p))
		{
			g.req.p = NULL;
			needReset = true; // recapture everything with new pointers
		}
		if (g.req.kind == 2 && g.req.p && g.mip && !(g.flags & PF_NoTexture))
		{
			levelflat_t *lf = (levelflat_t *)g.req.p;
			if (lf->speed && (lf->type == LEVELFLAT_TEXTURE || lf->type == LEVELFLAT_FLAT))
			{
				const INT32 cur = lf->type == LEVELFLAT_TEXTURE ? lf->u.texture.num : (INT32)lf->u.flat.lumpnum;
				if (cur != g.req.a)
				{
					rr3ds_lastmip = NULL;
					HWR_GetLevelFlat(lf, (boolean)g.req.b);
					if (rr3ds_lastmip) { g.mip = rr3ds_lastmip; g.req.a = cur; stAnim++; }
				}
			}
		}
		// If the RAM copy was purged and the GPU evicted it, regenerate it
		// through the same path that requested it (otherwise: empty/white frame).
		if (g.mip && !(g.flags & PF_NoTexture) && !g.mip->downloaded && !g.mip->data)
		{
			switch (g.req.kind)
			{
				case 1: HWR_GetTexture(g.req.a, g.req.b); break;
				case 2: if (g.req.p) HWR_GetLevelFlat((levelflat_t *)g.req.p, (boolean)g.req.b); break;
				case 3: HWR_GetRawFlat((lumpnum_t)g.req.a, (boolean)g.req.b); break;
				default: break;
			}
		}
		const int r = I_3DS_StaticDraw(g.mip, g.bright, g.flags, g.idx.data(), (int)g.idx.size());
		if (r == 0) { needReset = true; stDrawFail++; } // texture lost: recapture everything
		else if (r < 0) stDrawFail++;
		g.idx.clear();
	}
	if (any)
		I_3DS_StaticEnd();

	if (++stFrames >= 30)
	{
		RR_DIAGPRINT("[EST] 30f: used=%d captured=%d never=%d emptied=%d failures=%d | pool=%d verts, %u polys, %u groups\n",
			stHits, stCapt, stNever, stResets, stDrawFail, I_3DS_StaticUsed(),
			(unsigned)polys.size(), (unsigned)groups.size());
		RR_DIAGPRINT("[EST] never used/30f: changes=%d translucent=%d flags=%d notex=%d large=%d other=%d\n",
			stWhy[1], stWhy[2], stWhy[3], stWhy[4], stWhy[5], stWhy[0]);
		RR_DIAGPRINT("[EST] animated frame changes/30f: %d  scrolled walls/30f: %d  patched heights/30f: %d  shape recaptures/30f: %d  never-retries/30f: %d\n", stAnim, stShift, stPatch, stReshape, stRetry); stAnim = 0; stShift = 0; stPatch = 0; stReshape = 0; stRetry = 0;
		for (int w = 0; w < 6; w++) stWhy[w] = 0;
		stHits = stCapt = stNever = stResets = stDrawFail = stFrames = 0;
	}
}
#else
#define DBG3DS_TINT(s,f,r,g,b)
#endif

static void HWR_RenderPlane(subsector_t *subsector, extrasubsector_t *xsub, boolean isceiling, fixed_t fixedheight, FBITFIELD PolyFlags, INT32 lightlevel, levelflat_t *levelflat, sector_t *FOFsector, UINT8 alpha, extracolormap_t *planecolormap)
{
	polyvertex_t *  pv;
	float           height; //constant y for all points on the convex flat polygon
	FOutVector      *v3d;
	INT32             nrPlaneVerts;   //verts original define of convex flat polygon
	INT32             i;
	float           flatxref,flatyref;
	float fflatwidth = 64.0f, fflatheight = 64.0f;
	INT32 flatflag = 63;
	boolean texflat = false;
	float scrollx = 0.0f, scrolly = 0.0f, anglef = 0.0f;
	angle_t angle = 0;
	FSurfaceInfo    Surf;
	float tempxsow, tempytow;
	pslope_t *slope = NULL;

	static FOutVector *planeVerts = NULL;
	static UINT16 numAllocedPlaneVerts = 0;

	INT32 shader = SHADER_DEFAULT;

	// no convex poly were generated for this subsector
	if (!xsub->planepoly)
		return;

	// Get the slope pointer to simplify future code
	if (FOFsector)
	{
		if (FOFsector->f_slope && !isceiling)
			slope = FOFsector->f_slope;
		else if (FOFsector->c_slope && isceiling)
			slope = FOFsector->c_slope;
	}
	else
	{
		if (gl_frontsector->f_slope && !isceiling)
			slope = gl_frontsector->f_slope;
		else if (gl_frontsector->c_slope && isceiling)
			slope = gl_frontsector->c_slope;
	}

	// Set fixedheight to the slope's height from our viewpoint, if we have a slope
	if (slope)
		fixedheight = P_GetSlopeZAt(slope, viewx, viewy);

	height = FIXED_TO_FLOAT(fixedheight);

	pv  = xsub->planepoly->pts;
	nrPlaneVerts = xsub->planepoly->numpts;

	if (nrPlaneVerts < 3)   //not even a triangle ?
		return;

	// Allocate plane-vertex buffer if we need to
	if (!planeVerts || nrPlaneVerts > numAllocedPlaneVerts)
	{
		numAllocedPlaneVerts = (UINT16)nrPlaneVerts;
		Z_Free(planeVerts);
		Z_Malloc(numAllocedPlaneVerts * sizeof (FOutVector), PU_LEVEL, &planeVerts);
	}

	// set texture for polygon
	if (levelflat != NULL)
	{
		if (levelflat->type == LEVELFLAT_FLAT)
		{
			size_t len = W_LumpLength(levelflat->u.flat.lumpnum);
			size_t sflatsize = R_FlatDimensionsFromLumpSize(len);
			fflatwidth = fflatheight = (double)sflatsize;
			flatflag = sflatsize-1;
		}
		else
		{
			if (levelflat->type == LEVELFLAT_TEXTURE)
			{
				fflatwidth = textures[levelflat->u.texture.num]->width;
				fflatheight = textures[levelflat->u.texture.num]->height;
			}
			else if (levelflat->type == LEVELFLAT_PATCH || levelflat->type == LEVELFLAT_PNG)
			{
				fflatwidth = levelflat->width;
				fflatheight = levelflat->height;
			}
			texflat = true;
		}
	}
	else // set no texture
		HWR_SetCurrentTexture(NULL);

	// reference point for flat texture coord for each vertex around the polygon
	flatxref = (float)(((fixed_t)pv->x & (~flatflag)) / fflatwidth);
	flatyref = (float)(((fixed_t)pv->y & (~flatflag)) / fflatheight);

	// transform
	if (FOFsector != NULL)
	{
		if (!isceiling) // it's a floor
		{
			scrollx = FIXED_TO_FLOAT(FOFsector->floor_xoffs)/fflatwidth;
			scrolly = FIXED_TO_FLOAT(FOFsector->floor_yoffs)/fflatheight;
			angle = FOFsector->floorpic_angle;
		}
		else // it's a ceiling
		{
			scrollx = FIXED_TO_FLOAT(FOFsector->ceiling_xoffs)/fflatwidth;
			scrolly = FIXED_TO_FLOAT(FOFsector->ceiling_yoffs)/fflatheight;
			angle = FOFsector->ceilingpic_angle;
		}
	}
	else if (gl_frontsector)
	{
		if (!isceiling) // it's a floor
		{
			scrollx = FIXED_TO_FLOAT(gl_frontsector->floor_xoffs)/fflatwidth;
			scrolly = FIXED_TO_FLOAT(gl_frontsector->floor_yoffs)/fflatheight;
			angle = gl_frontsector->floorpic_angle;
		}
		else // it's a ceiling
		{
			scrollx = FIXED_TO_FLOAT(gl_frontsector->ceiling_xoffs)/fflatwidth;
			scrolly = FIXED_TO_FLOAT(gl_frontsector->ceiling_yoffs)/fflatheight;
			angle = gl_frontsector->ceilingpic_angle;
		}
	}


	if (angle) // Only needs to be done if there's an altered angle
	{
		tempxsow = flatxref;
		tempytow = flatyref;

		anglef = ANG2RAD(InvAngle(angle));

		flatxref = (tempxsow * cos(anglef)) - (tempytow * sin(anglef));
		flatyref = (tempxsow * sin(anglef)) + (tempytow * cos(anglef));
	}

#define SETUP3DVERT(vert, vx, vy) {\
		/* Hurdler: add scrolling texture on floor/ceiling */\
		if (texflat)\
		{\
			vert->s = (float)((vx) / fflatwidth) + scrollx;\
			vert->t = -(float)((vy) / fflatheight) + scrolly;\
		}\
		else\
		{\
			vert->s = (float)(((vx) / fflatwidth) - flatxref + scrollx);\
			vert->t = (float)(flatyref - ((vy) / fflatheight) + scrolly);\
		}\
\
		/* Need to rotate before translate */\
		if (angle) /* Only needs to be done if there's an altered angle */\
		{\
			tempxsow = vert->s;\
			tempytow = vert->t;\
			vert->s = (tempxsow * cos(anglef)) - (tempytow * sin(anglef));\
			vert->t = (tempxsow * sin(anglef)) + (tempytow * cos(anglef));\
		}\
\
		vert->x = (vx);\
		vert->y = height;\
		vert->z = (vy);\
\
		if (slope)\
		{\
			fixedheight = P_GetSlopeZAt(slope, FLOAT_TO_FIXED((vx)), FLOAT_TO_FIXED((vy)));\
			vert->y = FIXED_TO_FLOAT(fixedheight);\
		}\
}

	for (i = 0, v3d = planeVerts; i < nrPlaneVerts; i++,v3d++,pv++)
		SETUP3DVERT(v3d, pv->x, pv->y);

	lightlevel = HWR_CalcSlopeLight(lightlevel, slope, gl_frontsector, (FOFsector != NULL));
	HWR_Lighting(&Surf, lightlevel, planecolormap, P_SectorUsesDirectionalLighting(gl_frontsector));

	if (PolyFlags & PF_EnvironmentTrans)
	{
		Surf.PolyColor.s.alpha = (UINT8)alpha;
		PolyFlags |= PF_Modulated;
	}
	else
		PolyFlags |= PF_Masked|PF_Modulated;

#ifndef __3DS__
	if ((PolyFlags & PF_Fog) && !HWR_UseShader())
		HWR_FogNoShaderColor(&Surf, lightlevel, planecolormap);
#endif

	if (HWR_UseShader())
	{
		if (PolyFlags & PF_Fog)
			shader = SHADER_FOG;
		else if (PolyFlags & PF_Ripple)
			shader = SHADER_WATER;
		else
			shader = SHADER_FLOOR;

		PolyFlags |= PF_ColorMapped;
	}

	if (FOFsector) DBG3DS_TINT(&Surf, &PolyFlags, 255, 0, 255);
	else if (isceiling) DBG3DS_TINT(&Surf, &PolyFlags, 0, 255, 255);
	else DBG3DS_TINT(&Surf, &PolyFlags, 255, 0, 0);
	HWR_ProcessPolygon(&Surf, planeVerts, nrPlaneVerts, PolyFlags, shader, false);

#ifdef RR3DS
	// 3DS: horizon lines raise the floor up to the camera height
	// (a giant plane that covered the track). Disabled (on E only with Low-End).
	if (!RRE_GLLOWEND && subsector)
#else
	if (subsector)
#endif
	{
		// Horizon lines
		FOutVector horizonpts[6];
		float dist, vx, vy;
		float x1, y1, xd, yd;
		UINT8 numplanes, j;
		vertex_t v; // For determining the closest distance from the line to the camera, to split render planes for minimum distortion;

		const float renderdist = 27000.0f; // How far out to properly render the plane
		const float farrenderdist = 32768.0f; // From here, raise plane to horizon level to fill in the line with some texture distortion

		seg_t *line = &segs[subsector->firstline];

		for (i = 0; i < subsector->numlines; i++, line++)
		{
			if (!line->glseg && line->linedef->special == HORIZONSPECIAL && R_PointOnSegSide(dup_viewx, dup_viewy, line) == 0)
			{
				P_ClosestPointOnLine(viewx, viewy, line->linedef, &v);
				dist = FIXED_TO_FLOAT(R_PointToDist(v.x, v.y));

				x1 = ((polyvertex_t *)line->pv1)->x;
				y1 = ((polyvertex_t *)line->pv1)->y;
				xd = ((polyvertex_t *)line->pv2)->x - x1;
				yd = ((polyvertex_t *)line->pv2)->y - y1;

				// Based on the seg length and the distance from the line, split horizon into multiple poly sets to reduce distortion
				dist = sqrtf((xd*xd) + (yd*yd)) / dist / 16.0f;
				if (dist > 100.0f)
					numplanes = 100;
				else
					numplanes = (UINT8)dist + 1;

				for (j = 0; j < numplanes; j++)
				{
					// Left side
					vx = x1 + xd * j / numplanes;
					vy = y1 + yd * j / numplanes;
					SETUP3DVERT((&horizonpts[1]), vx, vy);

					dist = sqrtf(powf(vx - gl_viewx, 2) + powf(vy - gl_viewy, 2));
					vx = (vx - gl_viewx) * renderdist / dist + gl_viewx;
					vy = (vy - gl_viewy) * renderdist / dist + gl_viewy;
					SETUP3DVERT((&horizonpts[0]), vx, vy);

					// Right side
					vx = x1 + xd * (j+1) / numplanes;
					vy = y1 + yd * (j+1) / numplanes;
					SETUP3DVERT((&horizonpts[2]), vx, vy);

					dist = sqrtf(powf(vx - gl_viewx, 2) + powf(vy - gl_viewy, 2));
					vx = (vx - gl_viewx) * renderdist / dist + gl_viewx;
					vy = (vy - gl_viewy) * renderdist / dist + gl_viewy;
					SETUP3DVERT((&horizonpts[3]), vx, vy);

					// Horizon fills
					vx = (horizonpts[0].x - gl_viewx) * farrenderdist / renderdist + gl_viewx;
					vy = (horizonpts[0].z - gl_viewy) * farrenderdist / renderdist + gl_viewy;
					SETUP3DVERT((&horizonpts[5]), vx, vy);
					horizonpts[5].y = gl_viewz;

					vx = (horizonpts[3].x - gl_viewx) * farrenderdist / renderdist + gl_viewx;
					vy = (horizonpts[3].z - gl_viewy) * farrenderdist / renderdist + gl_viewy;
					SETUP3DVERT((&horizonpts[4]), vx, vy);
					horizonpts[4].y = gl_viewz;

					// Draw
					HWR_ProcessPolygon(&Surf, horizonpts, 6, PolyFlags, shader, true);
				}
			}
		}
	}

#ifdef ALAM_LIGHTING
	// add here code for dynamic lighting on planes
	HWR_PlaneLighting(planeVerts, nrPlaneVerts);
#endif
}

#ifdef POLYSKY
// this don't draw anything it only update the z-buffer so there isn't problem with
// wall/things upper that sky (map12)
static void HWR_RenderSkyPlane(extrasubsector_t *xsub, fixed_t fixedheight)
{
	polyvertex_t *pv;
	float height; //constant y for all points on the convex flat polygon
	FOutVector *v3d;
	INT32 nrPlaneVerts;   //verts original define of convex flat polygon
	INT32 i;

	// no convex poly were generated for this subsector
	if (!xsub->planepoly)
		return;

	height = FIXED_TO_FLOAT(fixedheight);

	pv  = xsub->planepoly->pts;
	nrPlaneVerts = xsub->planepoly->numpts;

	if (nrPlaneVerts < 3) // not even a triangle?
		return;

	if (nrPlaneVerts > MAXPLANEVERTICES) // FIXME: exceeds plVerts size
	{
		CONS_Debug(DBG_RENDER, "polygon size of %d exceeds max value of %d vertices\n", nrPlaneVerts, MAXPLANEVERTICES);
		return;
	}

	// transform
	v3d = planeVerts;
	for (i = 0; i < nrPlaneVerts; i++,v3d++,pv++)
	{
		v3d->s = 0.0f;
		v3d->t = 0.0f;
		v3d->x = pv->x;
		v3d->y = height;
		v3d->z = pv->y;
	}

	HWD.pfnDrawPolygon(NULL, planeVerts, nrPlaneVerts, PF_Invisible|PF_NoTexture|PF_Occlude);
}
#endif //polysky

#endif //doplanes

FBITFIELD HWR_GetBlendModeFlag(INT32 ast)
{
	switch (ast)
	{
		//case AST_COPY: -- intentionally defaults to translucent
		case AST_OVERLAY:
			return PF_Masked;
		case AST_ADD:
			return PF_Additive;
		case AST_SUBTRACT:
			return PF_ReverseSubtract;
		case AST_REVERSESUBTRACT:
			return PF_Subtractive;
		case AST_MODULATE:
			return PF_Multiplicative;
		default:
			return PF_Translucent;
	}

	return 0;
}

UINT8 HWR_GetTranstableAlpha(INT32 transtablenum)
{
	transtablenum = std::max<INT32>(std::min<INT32>(transtablenum, (INT32)tr_trans90), 0);

	switch (transtablenum)
	{
		case 0          : return 0xff;
		case tr_trans10 : return 0xe6;
		case tr_trans20 : return 0xcc;
		case tr_trans30 : return 0xb3;
		case tr_trans40 : return 0x99;
		case tr_trans50 : return 0x80;
		case tr_trans60 : return 0x66;
		case tr_trans70 : return 0x4c;
		case tr_trans80 : return 0x33;
		case tr_trans90 : return 0x19;
	}

	return 0xff;
}

FBITFIELD HWR_SurfaceBlend(INT32 style, INT32 transtablenum, FSurfaceInfo *pSurf)
{
	pSurf->PolyColor.s.alpha = 0xff;

	if (style == AST_MODULATE)
		return PF_Multiplicative;

	if (!transtablenum && !style)
		return PF_Masked;

	pSurf->PolyColor.s.alpha = HWR_GetTranstableAlpha(transtablenum);
	return HWR_GetBlendModeFlag(style);
}

FBITFIELD HWR_TranstableToAlpha(INT32 transtablenum, FSurfaceInfo *pSurf)
{
	if (!transtablenum)
	{
		pSurf->PolyColor.s.alpha = 0x00;
		return PF_Masked;
	}

	pSurf->PolyColor.s.alpha = HWR_GetTranstableAlpha(transtablenum);
	return PF_Translucent;
}

static void HWR_AddTransparentWall(FOutVector *wallVerts, FSurfaceInfo *pSurf, INT32 texnum, INT32 basetexnum, FBITFIELD blend, boolean fogwall, INT32 lightlevel, extracolormap_t *wallcolormap);

// ==========================================================================
// Wall generation from subsector segs
// ==========================================================================

/*
   wallVerts order is :
		3--2
		| /|
		|/ |
		0--1
*/

//
// HWR_ProjectWall
//
static void HWR_ProjectWall(FOutVector *wallVerts, FSurfaceInfo *pSurf, FBITFIELD blendmode, INT32 lightlevel, extracolormap_t *wallcolormap)
{
	INT32 shader = SHADER_DEFAULT;

	HWR_Lighting(pSurf, lightlevel, wallcolormap, P_SectorUsesDirectionalLighting(gl_frontsector));

	if (HWR_UseShader())
	{
		shader = SHADER_WALL;
		blendmode |= PF_ColorMapped;
	}

	DBG3DS_TINT(pSurf, &blendmode, 0, 255, 0);
	HWR_ProcessPolygon(pSurf, wallVerts, 4, blendmode|PF_Modulated|PF_Occlude, shader, false);
}

// ==========================================================================
//                                                          BSP, CULL, ETC..
// ==========================================================================

//
// HWR_SplitWall
//
static void HWR_SplitWall(sector_t *sector, FOutVector *wallVerts, INT32 texnum, INT32 basetexnum, FSurfaceInfo* Surf, INT32 cutflag, ffloor_t *pfloor, FBITFIELD polyflags)
{
	/* SoM: split up and light walls according to the
	 lightlist. This may also include leaving out parts
	 of the wall that can't be seen */

	float realtop, realbot, top, bot;
	float pegt, pegb, pegmul;
	float height = 0.0f, bheight = 0.0f;

	float endrealtop, endrealbot, endtop, endbot;
	float endpegt, endpegb, endpegmul;
	float endheight = 0.0f, endbheight = 0.0f;

	// compiler complains when P_GetSlopeZAt is used in FLOAT_TO_FIXED directly
	// use this as a temp var to store P_GetSlopeZAt's return value each time
	fixed_t temp;

	fixed_t v1x = FLOAT_TO_FIXED(wallVerts[0].x);
	fixed_t v1y = FLOAT_TO_FIXED(wallVerts[0].z); // not a typo
	fixed_t v2x = FLOAT_TO_FIXED(wallVerts[1].x);
	fixed_t v2y = FLOAT_TO_FIXED(wallVerts[1].z); // not a typo

	INT32 solid, i;
	lightlist_t *  list = sector->lightlist;
	const UINT8 alpha = Surf->PolyColor.s.alpha;
	FUINT lightnum = sector->lightlevel;
	extracolormap_t *colormap = NULL;

	realtop = top = wallVerts[3].y;
	realbot = bot = wallVerts[0].y;
	pegt = wallVerts[3].t;
	pegb = wallVerts[0].t;
	pegmul = (pegb - pegt) / (top - bot);

	endrealtop = endtop = wallVerts[2].y;
	endrealbot = endbot = wallVerts[1].y;
	endpegt = wallVerts[2].t;
	endpegb = wallVerts[1].t;
	endpegmul = (endpegb - endpegt) / (endtop - endbot);

	for (i = 0; i < sector->numlights; i++)
	{
		if (endtop < endrealbot && top < realbot)
			return;

		if (!(list[i].flags & FOF_NOSHADE))
		{
			if (pfloor && (pfloor->fofflags & FOF_FOG))
			{
				lightnum = pfloor->master->frontsector->lightlevel;
				colormap = pfloor->master->frontsector->extra_colormap;
			}
			else
			{
				lightnum = *list[i].lightlevel;
				colormap = *list[i].extra_colormap;
			}
		}

		solid = false;

		if ((sector->lightlist[i].flags & FOF_CUTSOLIDS) && !(cutflag & FOF_EXTRA))
			solid = true;
		else if ((sector->lightlist[i].flags & FOF_CUTEXTRA) && (cutflag & FOF_EXTRA))
		{
			if (sector->lightlist[i].flags & FOF_EXTRA)
			{
				if ((sector->lightlist[i].flags & (FOF_FOG|FOF_SWIMMABLE)) == (cutflag & (FOF_FOG|FOF_SWIMMABLE))) // Only merge with your own types
					solid = true;
			}
			else
				solid = true;
		}
		else
			solid = false;

		temp = P_GetLightZAt(&list[i], v1x, v1y);
		height = FIXED_TO_FLOAT(temp);
		temp = P_GetLightZAt(&list[i], v2x, v2y);
		endheight = FIXED_TO_FLOAT(temp);
		if (solid)
		{
			temp = P_GetFFloorBottomZAt(list[i].caster, v1x, v1y);
			bheight = FIXED_TO_FLOAT(temp);
			temp = P_GetFFloorBottomZAt(list[i].caster, v2x, v2y);
			endbheight = FIXED_TO_FLOAT(temp);
		}

		if (endheight >= endtop && height >= top)
		{
			if (solid && top > bheight)
				top = bheight;
			if (solid && endtop > endbheight)
				endtop = endbheight;
		}

		if (i + 1 < sector->numlights)
		{
			temp = P_GetLightZAt(&list[i+1], v1x, v1y);
			bheight = FIXED_TO_FLOAT(temp);
			temp = P_GetLightZAt(&list[i+1], v2x, v2y);
			endbheight = FIXED_TO_FLOAT(temp);
		}
		else
		{
			bheight = realbot;
			endbheight = endrealbot;
		}

		if (endbheight >= endtop && bheight >= top)
			continue;

		//Found a break;
		bot = bheight;

		if (bot < realbot)
			bot = realbot;

		endbot = endbheight;

		if (endbot < endrealbot)
			endbot = endrealbot;

		Surf->PolyColor.s.alpha = alpha;

		wallVerts[3].t = pegt + ((realtop - top) * pegmul);
		wallVerts[2].t = endpegt + ((endrealtop - endtop) * endpegmul);
		wallVerts[0].t = pegt + ((realtop - bot) * pegmul);
		wallVerts[1].t = endpegt + ((endrealtop - endbot) * endpegmul);

		// set top/bottom coords
		wallVerts[3].y = top;
		wallVerts[2].y = endtop;
		wallVerts[0].y = bot;
		wallVerts[1].y = endbot;

		if (polyflags & PF_Fog)
			HWR_AddTransparentWall(wallVerts, Surf, texnum, basetexnum, polyflags, true, HWR_CalcWallLight(lightnum, gl_curline), colormap);
		else if (polyflags & PF_EnvironmentTrans)
			HWR_AddTransparentWall(wallVerts, Surf, texnum, basetexnum, polyflags, false, HWR_CalcWallLight(lightnum, gl_curline), colormap);
		else
			HWR_ProjectWall(wallVerts, Surf, polyflags, HWR_CalcWallLight(lightnum, gl_curline), colormap);

		top = bot;
		endtop = endbot;
	}

	bot = realbot;
	endbot = endrealbot;
	if (endtop <= endrealbot && top <= realbot)
		return;

	Surf->PolyColor.s.alpha = alpha;

	wallVerts[3].t = pegt + ((realtop - top) * pegmul);
	wallVerts[2].t = endpegt + ((endrealtop - endtop) * endpegmul);
	wallVerts[0].t = pegt + ((realtop - bot) * pegmul);
	wallVerts[1].t = endpegt + ((endrealtop - endbot) * endpegmul);

	// set top/bottom coords
	wallVerts[3].y = top;
	wallVerts[2].y = endtop;
	wallVerts[0].y = bot;
	wallVerts[1].y = endbot;

	if (polyflags & PF_Fog)
		HWR_AddTransparentWall(wallVerts, Surf, texnum, basetexnum, polyflags, true, HWR_CalcWallLight(lightnum, gl_curline), colormap);
	else if (polyflags & PF_EnvironmentTrans)
		HWR_AddTransparentWall(wallVerts, Surf, texnum, basetexnum, polyflags, false, HWR_CalcWallLight(lightnum, gl_curline), colormap);
	else
		HWR_ProjectWall(wallVerts, Surf, polyflags, HWR_CalcWallLight(lightnum, gl_curline), colormap);
}

// HWR_DrawSkyWall
// Draw walls into the depth buffer so that anything behind is culled properly
static void HWR_DrawSkyWall(FOutVector *wallVerts, FSurfaceInfo *Surf)
{
	HWR_SetCurrentTexture(NULL);
	// no texture
	wallVerts[3].t = wallVerts[2].t = 0;
	wallVerts[0].t = wallVerts[1].t = 0;
	wallVerts[0].s = wallVerts[3].s = 0;
	wallVerts[2].s = wallVerts[1].s = 0;
	// this no longer sets top/bottom coords, this should be done before caling the function
	HWR_ProjectWall(wallVerts, Surf, PF_Invisible|PF_NoTexture, 255, NULL);
	// PF_Invisible so it's not drawn into the colour buffer
	// PF_NoTexture for no texture
	// PF_Occlude is set in HWR_ProjectWall to draw into the depth buffer
}

//
// HWR_ProcessSeg
// A portion or all of a wall segment will be drawn, from startfrac to endfrac,
//  where 0 is the start of the segment, 1 the end of the segment
// Anything between means the wall segment has been clipped with solidsegs,
//  reducing wall overdraw to a minimum
//
#ifdef RR3DS
#define RR3DS_KIND(k) (rr3ds_static::curKind = (k))
#else
#define RR3DS_KIND(k) ((void)0)
#endif
static void HWR_ProcessSeg(void) // Sort of like GLWall::Process in GZDoom
{
	RR3DS_KIND(0);
	FOutVector wallVerts[4];
	v2d_t vs, ve; // start, end vertices of 2d line (view from above)

	fixed_t worldtop, worldbottom;
	fixed_t worldhigh = 0, worldlow = 0;
	fixed_t worldtopslope, worldbottomslope;
	fixed_t worldhighslope = 0, worldlowslope = 0;
	fixed_t v1x, v1y, v2x, v2y;

	GLMapTexture_t *grTex = NULL;
	float cliplow = 0.0f, cliphigh = 0.0f;
	INT32 gl_midtexture;
	fixed_t h, l; // 3D sides and 2s middle textures
	fixed_t hS, lS;

	FUINT lightnum = 255;
	extracolormap_t *colormap;
	FSurfaceInfo Surf;

	boolean tripwire;

	gl_sidedef = gl_curline->sidedef;
	gl_linedef = gl_curline->linedef;

	vs.x = ((polyvertex_t *)gl_curline->pv1)->x;
	vs.y = ((polyvertex_t *)gl_curline->pv1)->y;
	ve.x = ((polyvertex_t *)gl_curline->pv2)->x;
	ve.y = ((polyvertex_t *)gl_curline->pv2)->y;

	v1x = FLOAT_TO_FIXED(vs.x);
	v1y = FLOAT_TO_FIXED(vs.y);
	v2x = FLOAT_TO_FIXED(ve.x);
	v2y = FLOAT_TO_FIXED(ve.y);

#define SLOPEPARAMS(slope, end1, end2, normalheight) \
	end1 = P_GetZAt(slope, v1x, v1y, normalheight); \
	end2 = P_GetZAt(slope, v2x, v2y, normalheight);

	SLOPEPARAMS(gl_frontsector->c_slope, worldtop,    worldtopslope,    gl_frontsector->ceilingheight)
	SLOPEPARAMS(gl_frontsector->f_slope, worldbottom, worldbottomslope, gl_frontsector->floorheight)

	// remember vertices ordering
	//  3--2
	//  | /|
	//  |/ |
	//  0--1
	// make a wall polygon (with 2 triangles), using the floor/ceiling heights,
	// and the 2d map coords of start/end vertices
	wallVerts[0].x = wallVerts[3].x = vs.x;
	wallVerts[0].z = wallVerts[3].z = vs.y;
	wallVerts[2].x = wallVerts[1].x = ve.x;
	wallVerts[2].z = wallVerts[1].z = ve.y;

	// x offset the texture
	{
#ifdef RR3DS
		fixed_t texturehpeg = rr3ds_gen::SideOff(gl_sidedef) + gl_curline->offset;
#else
		fixed_t texturehpeg = gl_sidedef->textureoffset + gl_curline->offset;
#endif
		cliplow = (float)texturehpeg;
		cliphigh = (float)(texturehpeg + (gl_curline->flength*FRACUNIT));
	}

	tripwire = P_IsLineTripWire(gl_linedef);

	if (tripwire == false)
	{
		lightnum = HWR_CalcWallLight(gl_frontsector->lightlevel, gl_curline);
	}

	colormap = gl_frontsector->extra_colormap;

	if (gl_frontsector)
		Surf.PolyColor.s.alpha = 255;

	if (gl_backsector)
	{
		INT32 gl_toptexture = 0, gl_bottomtexture = 0;
		INT32 gl_basetoptexture = 0, gl_basebottomtexture = 0;
		// two sided line
		boolean bothceilingssky = false; // turned on if both back and front ceilings are sky
		boolean bothfloorssky = false; // likewise, but for floors

		SLOPEPARAMS(gl_backsector->c_slope, worldhigh, worldhighslope, gl_backsector->ceilingheight)
		SLOPEPARAMS(gl_backsector->f_slope, worldlow,  worldlowslope,  gl_backsector->floorheight)

		// hack to allow height changes in outdoor areas
		// This is what gets rid of the upper textures if there should be sky
		if (gl_frontsector->ceilingpic == skyflatnum
			&& gl_backsector->ceilingpic  == skyflatnum)
		{
			bothceilingssky = true;
		}

		// likewise, but for floors and upper textures
		if (gl_frontsector->floorpic == skyflatnum
			&& gl_backsector->floorpic == skyflatnum)
		{
			bothfloorssky = true;
		}

		if (!bothceilingssky)
		{
			gl_basetoptexture = gl_sidedef->toptexture;
			gl_toptexture = R_GetTextureNum(gl_basetoptexture);
		}
		if (!bothfloorssky)
		{
			gl_basebottomtexture = gl_sidedef->bottomtexture;
			gl_bottomtexture = R_GetTextureNum(gl_basebottomtexture);
		}

		// check TOP TEXTURE
		if ((worldhighslope < worldtopslope || worldhigh < worldtop) && gl_toptexture)
		{
			RR3DS_KIND(1);
			{
				fixed_t texturevpegtop; // top

				grTex = HWR_GetTexture(gl_toptexture, gl_basetoptexture);

				// PEGGING
				if (gl_linedef->flags & ML_DONTPEGTOP)
					texturevpegtop = 0;
				else if (gl_linedef->flags & ML_SKEWTD)
					texturevpegtop = worldhigh + textureheight[gl_sidedef->toptexture] - worldtop;
				else
					texturevpegtop = gl_backsector->ceilingheight + textureheight[gl_sidedef->toptexture] - gl_frontsector->ceilingheight;

				texturevpegtop += gl_sidedef->rowoffset;

				// This is so that it doesn't overflow and screw up the wall, it doesn't need to go higher than the texture's height anyway
				texturevpegtop %= (textures[gl_toptexture]->height)<<FRACBITS;

				wallVerts[3].t = wallVerts[2].t = texturevpegtop * grTex->scaleY;
				wallVerts[0].t = wallVerts[1].t = (texturevpegtop + gl_frontsector->ceilingheight - gl_backsector->ceilingheight) * grTex->scaleY;
				wallVerts[0].s = wallVerts[3].s = cliplow * grTex->scaleX;
				wallVerts[2].s = wallVerts[1].s = cliphigh * grTex->scaleX;

				// Adjust t value for sloped walls
				if (!(gl_linedef->flags & ML_SKEWTD))
				{
					// Unskewed
					wallVerts[3].t -= (worldtop - gl_frontsector->ceilingheight) * grTex->scaleY;
					wallVerts[2].t -= (worldtopslope - gl_frontsector->ceilingheight) * grTex->scaleY;
					wallVerts[0].t -= (worldhigh - gl_backsector->ceilingheight) * grTex->scaleY;
					wallVerts[1].t -= (worldhighslope - gl_backsector->ceilingheight) * grTex->scaleY;
				}
				else if (gl_linedef->flags & ML_DONTPEGTOP)
				{
					// Skewed by top
					wallVerts[0].t = (texturevpegtop + worldtop - worldhigh) * grTex->scaleY;
					wallVerts[1].t = (texturevpegtop + worldtopslope - worldhighslope) * grTex->scaleY;
				}
				else
				{
					// Skewed by bottom
					wallVerts[0].t = wallVerts[1].t = (texturevpegtop + worldtop - worldhigh) * grTex->scaleY;
					wallVerts[3].t = wallVerts[0].t - (worldtop - worldhigh) * grTex->scaleY;
					wallVerts[2].t = wallVerts[1].t - (worldtopslope - worldhighslope) * grTex->scaleY;
				}
			}

			// set top/bottom coords
			wallVerts[3].y = FIXED_TO_FLOAT(worldtop);
			wallVerts[0].y = FIXED_TO_FLOAT(worldhigh);
			wallVerts[2].y = FIXED_TO_FLOAT(worldtopslope);
			wallVerts[1].y = FIXED_TO_FLOAT(worldhighslope);

			{
				FBITFIELD polyflags = PF_Masked;

				if (RR3DS_TEXTRANSP(grTex))
					polyflags = PF_Environment;

				if (gl_frontsector->numlights)
					HWR_SplitWall(gl_frontsector, wallVerts, gl_toptexture, gl_basetoptexture, &Surf, FOF_CUTLEVEL, NULL, polyflags);
				else if (RR3DS_TEXTRANSP(grTex))
					HWR_AddTransparentWall(wallVerts, &Surf, gl_toptexture, gl_basetoptexture, polyflags, false, lightnum, colormap);
				else
					HWR_ProjectWall(wallVerts, &Surf, polyflags, lightnum, colormap);
			}
		}

		// check BOTTOM TEXTURE
		if ((
			worldlowslope > worldbottomslope ||
            worldlow > worldbottom) && gl_bottomtexture) //only if VISIBLE!!!
		{
			RR3DS_KIND(2);
			{
				fixed_t texturevpegbottom = 0; // bottom

				grTex = HWR_GetTexture(gl_bottomtexture, gl_basebottomtexture);

				// PEGGING
				if (!(gl_linedef->flags & ML_DONTPEGBOTTOM))
					texturevpegbottom = 0;
				else if (gl_linedef->flags & ML_SKEWTD)
					texturevpegbottom = worldbottom - worldlow;
				else
					texturevpegbottom = gl_frontsector->floorheight - gl_backsector->floorheight;

				texturevpegbottom += gl_sidedef->rowoffset;

				// This is so that it doesn't overflow and screw up the wall, it doesn't need to go higher than the texture's height anyway
				texturevpegbottom %= (textures[gl_bottomtexture]->height)<<FRACBITS;

				wallVerts[3].t = wallVerts[2].t = texturevpegbottom * grTex->scaleY;
				wallVerts[0].t = wallVerts[1].t = (texturevpegbottom + gl_backsector->floorheight - gl_frontsector->floorheight) * grTex->scaleY;
				wallVerts[0].s = wallVerts[3].s = cliplow * grTex->scaleX;
				wallVerts[2].s = wallVerts[1].s = cliphigh * grTex->scaleX;

				// Adjust t value for sloped walls
				if (!(gl_linedef->flags & ML_SKEWTD))
				{
					// Unskewed
					wallVerts[0].t -= (worldbottom - gl_frontsector->floorheight) * grTex->scaleY;
					wallVerts[1].t -= (worldbottomslope - gl_frontsector->floorheight) * grTex->scaleY;
					wallVerts[3].t -= (worldlow - gl_backsector->floorheight) * grTex->scaleY;
					wallVerts[2].t -= (worldlowslope - gl_backsector->floorheight) * grTex->scaleY;
				}
				else if (gl_linedef->flags & ML_DONTPEGBOTTOM)
				{
					// Skewed by bottom
					wallVerts[0].t = wallVerts[1].t = (texturevpegbottom + worldlow - worldbottom) * grTex->scaleY;
					//wallVerts[3].t = wallVerts[0].t - (worldlow - worldbottom) * grTex->scaleY; // no need, [3] is already this
					wallVerts[2].t = wallVerts[1].t - (worldlowslope - worldbottomslope) * grTex->scaleY;
				}
				else
				{
					// Skewed by top
					wallVerts[0].t = (texturevpegbottom + worldlow - worldbottom) * grTex->scaleY;
					wallVerts[1].t = (texturevpegbottom + worldlowslope - worldbottomslope) * grTex->scaleY;
				}
			}

			// set top/bottom coords
			wallVerts[3].y = FIXED_TO_FLOAT(worldlow);
			wallVerts[0].y = FIXED_TO_FLOAT(worldbottom);
			wallVerts[2].y = FIXED_TO_FLOAT(worldlowslope);
			wallVerts[1].y = FIXED_TO_FLOAT(worldbottomslope);

			{
				FBITFIELD polyflags = PF_Masked;

				if (RR3DS_TEXTRANSP(grTex))
					polyflags = PF_Environment;

				if (gl_frontsector->numlights)
					HWR_SplitWall(gl_frontsector, wallVerts, gl_bottomtexture, gl_basebottomtexture, &Surf, FOF_CUTLEVEL, NULL, polyflags);
				else if (RR3DS_TEXTRANSP(grTex))
					HWR_AddTransparentWall(wallVerts, &Surf, gl_bottomtexture, gl_basebottomtexture, polyflags, false, lightnum, colormap);
				else
					HWR_ProjectWall(wallVerts, &Surf, polyflags, lightnum, colormap);
			}
		}
		gl_midtexture = R_GetTextureNum(gl_sidedef->midtexture);
		if (gl_midtexture)
		{
			RR3DS_KIND(3);
			FBITFIELD blendmode;
			sector_t *front, *back;
			fixed_t  popentop, popenbottom, polytop, polybottom, lowcut, highcut;
			fixed_t     texturevpeg = 0;
			INT32 repeats;

			if (gl_linedef->frontsector->heightsec != -1)
				front = &sectors[gl_linedef->frontsector->heightsec];
			else
				front = gl_linedef->frontsector;

			if (gl_linedef->backsector->heightsec != -1)
				back = &sectors[gl_linedef->backsector->heightsec];
			else
				back = gl_linedef->backsector;

			if (gl_sidedef->repeatcnt)
				repeats = 1 + gl_sidedef->repeatcnt;
			else if (gl_linedef->flags & ML_WRAPMIDTEX)
			{
				fixed_t high, low;

				if (front->ceilingheight > back->ceilingheight)
					high = back->ceilingheight;
				else
					high = front->ceilingheight;

				if (front->floorheight > back->floorheight)
					low = front->floorheight;
				else
					low = back->floorheight;

				repeats = (high - low)/textureheight[gl_sidedef->midtexture];
				if ((high-low)%textureheight[gl_sidedef->midtexture])
					repeats++; // tile an extra time to fill the gap -- Monster Iestyn
			}
			else
				repeats = 1;

			// SoM: a little note: This code re-arranging will
			// fix the bug in Nimrod map02. popentop and popenbottom
			// record the limits the texture can be displayed in.
			// polytop and polybottom, are the ideal (i.e. unclipped)
			// heights of the polygon, and h & l, are the final (clipped)
			// poly coords.

			// NOTE: With polyobjects, whenever you need to check the properties of the polyobject sector it belongs to,
			// you must use the linedef's backsector to be correct
			// From CB
			if (gl_curline->polyseg)
			{
				popentop = back->ceilingheight;
				popenbottom = back->floorheight;
			}
			else
            {
				popentop = std::min(worldtop, worldhigh);
				popenbottom = std::max(worldbottom, worldlow);
			}

			if (gl_linedef->flags & ML_NOSKEW)
			{
				if (gl_linedef->flags & ML_MIDPEG)
				{
					polybottom = std::max(front->floorheight, back->floorheight) + gl_sidedef->rowoffset;
					polytop = polybottom + textureheight[gl_midtexture]*repeats;
				}
				else
				{
					polytop = std::min(front->ceilingheight, back->ceilingheight) + gl_sidedef->rowoffset;
					polybottom = polytop - textureheight[gl_midtexture]*repeats;
				}
			}
			else if (gl_linedef->flags & ML_MIDPEG)
			{
				polybottom = popenbottom + gl_sidedef->rowoffset;
				polytop = polybottom + textureheight[gl_midtexture]*repeats;
			}
			else
			{
				polytop = popentop + gl_sidedef->rowoffset;
				polybottom = polytop - textureheight[gl_midtexture]*repeats;
			}
			// CB
			// NOTE: With polyobjects, whenever you need to check the properties of the polyobject sector it belongs to,
			// you must use the linedef's backsector to be correct
			if (gl_curline->polyseg)
			{
				lowcut = polybottom;
				highcut = polytop;
			}
			else
			{
				// The cut-off values of a linedef can always be constant, since every line has an absoulute front and or back sector
				lowcut = popenbottom;
				highcut = popentop;
			}

			h = std::min(highcut, polytop);
			l = std::max(polybottom, lowcut);

			{
				// PEGGING
				if (gl_linedef->flags & ML_MIDPEG)
					texturevpeg = textureheight[gl_sidedef->midtexture]*repeats - h + polybottom;
				else
					texturevpeg = polytop - h;

				grTex = HWR_GetTexture(gl_midtexture, gl_sidedef->midtexture);

				// Check if we should flip tripwire texture vertically for unpegged tripwires
				if (R_ShouldFlipTripWire(gl_linedef))
				{
					// Flip texture coordinates vertically
					wallVerts[0].t = wallVerts[1].t = texturevpeg * grTex->scaleY;
					wallVerts[3].t = wallVerts[2].t = (h - l + texturevpeg) * grTex->scaleY;
				}
				else
				{
					wallVerts[3].t = wallVerts[2].t = texturevpeg * grTex->scaleY;
					wallVerts[0].t = wallVerts[1].t = (h - l + texturevpeg) * grTex->scaleY;
				}
				wallVerts[0].s = wallVerts[3].s = cliplow * grTex->scaleX;
				wallVerts[2].s = wallVerts[1].s = cliphigh * grTex->scaleX;
			}

			// set top/bottom coords
			// Take the texture peg into account, rather than changing the offsets past
			// where the polygon might not be.
			wallVerts[2].y = wallVerts[3].y = FIXED_TO_FLOAT(h);
			wallVerts[0].y = wallVerts[1].y = FIXED_TO_FLOAT(l);

			// Correct to account for slopes
			{
				fixed_t midtextureslant;

				if (gl_linedef->flags & ML_NOSKEW)
					midtextureslant = 0;
				else if (gl_linedef->flags & ML_MIDPEG)
					midtextureslant = worldlow < worldbottom
							  ? worldbottomslope-worldbottom
							  : worldlowslope-worldlow;
				else
					midtextureslant = worldtop < worldhigh
							  ? worldtopslope-worldtop
							  : worldhighslope-worldhigh;

				polytop += midtextureslant;
				polybottom += midtextureslant;

				highcut += worldtop < worldhigh
						 ? worldtopslope-worldtop
						 : worldhighslope-worldhigh;
				lowcut += worldlow < worldbottom
						? worldbottomslope-worldbottom
						: worldlowslope-worldlow;

				// Texture stuff
				h = std::min(highcut, polytop);
				l = std::max(polybottom, lowcut);

				{
					// PEGGING
					if (gl_linedef->flags & ML_MIDPEG)
						texturevpeg = textureheight[gl_sidedef->midtexture]*repeats - h + polybottom;
					else
						texturevpeg = polytop - h;

					// Apply tripwire flipping for slope correction as well
					if (R_ShouldFlipTripWire(gl_linedef))
					{
						// Flip texture coordinates vertically
						wallVerts[1].t = texturevpeg * grTex->scaleY;
						wallVerts[2].t = (h - l + texturevpeg) * grTex->scaleY;
					}
					else
					{
						wallVerts[2].t = texturevpeg * grTex->scaleY;
						wallVerts[1].t = (h - l + texturevpeg) * grTex->scaleY;
					}
				}

				wallVerts[2].y = FIXED_TO_FLOAT(h);
				wallVerts[1].y = FIXED_TO_FLOAT(l);
			}

			// set alpha for transparent walls
			// ooops ! this do not work at all because render order we should render it in backtofront order
			if (gl_linedef->blendmode && gl_linedef->blendmode != AST_FOG)
			{
				if (gl_linedef->alpha >= 0 && gl_linedef->alpha < FRACUNIT)
					blendmode = HWR_SurfaceBlend(gl_linedef->blendmode, R_GetLinedefTransTable(gl_linedef->alpha), &Surf);
				else
					blendmode = HWR_GetBlendModeFlag(gl_linedef->blendmode);
			}
			else if (gl_linedef->alpha >= 0 && gl_linedef->alpha < FRACUNIT)
				blendmode = HWR_TranstableToAlpha(R_GetLinedefTransTable(gl_linedef->alpha), &Surf);
			else
				blendmode = PF_Masked;

			if (gl_curline->polyseg && gl_curline->polyseg->translucency > 0)
			{
				if (gl_curline->polyseg->translucency >= NUMTRANSMAPS) // wall not drawn
				{
					Surf.PolyColor.s.alpha = 0x00; // This shouldn't draw anything regardless of blendmode
					blendmode = PF_Masked;
				}
				else
					blendmode = HWR_TranstableToAlpha(gl_curline->polyseg->translucency, &Surf);
			}

			// Render midtextures on two-sided lines with a z-buffer offset.
			// This will cause the midtexture appear on top, if a FOF overlaps with it.
			blendmode |= PF_Decal;

			if (tripwire == false && gl_frontsector->numlights)
			{
				if (!(blendmode & PF_Masked))
					HWR_SplitWall(gl_frontsector, wallVerts, gl_midtexture, gl_sidedef->midtexture, &Surf, FOF_TRANSLUCENT, NULL, blendmode); // vanilla just uses PF_Masked here - if we run into any issues, maybe change to that
				else
				{
					HWR_SplitWall(gl_frontsector, wallVerts, gl_midtexture, gl_sidedef->midtexture, &Surf, FOF_CUTLEVEL, NULL, blendmode); // vanilla just uses PF_Masked here - if we run into any issues, maybe change to that
				}
			}
			else if (!(blendmode & PF_Masked))
				HWR_AddTransparentWall(wallVerts, &Surf, gl_midtexture, gl_sidedef->midtexture, blendmode, false, lightnum, colormap);
			else
				HWR_ProjectWall(wallVerts, &Surf, blendmode, lightnum, colormap);
		}

		// Sky culling
		// No longer so much a mess as before!
		if (!gl_curline->polyseg) // Don't do it for polyobjects
		{
			if (gl_frontsector->ceilingpic == skyflatnum)
			{
				if (gl_backsector->ceilingpic != skyflatnum) // don't cull if back sector is also sky
				{
					RR3DS_KIND(4);
					wallVerts[2].y = wallVerts[3].y = FIXED_TO_FLOAT(INT32_MAX); // draw to top of map space
					wallVerts[0].y = FIXED_TO_FLOAT(worldtop);
					wallVerts[1].y = FIXED_TO_FLOAT(worldtopslope);
					HWR_DrawSkyWall(wallVerts, &Surf);
				}
			}

			if (gl_frontsector->floorpic == skyflatnum)
			{
				if (gl_backsector->floorpic != skyflatnum) // don't cull if back sector is also sky
				{
					RR3DS_KIND(5);
					wallVerts[3].y = FIXED_TO_FLOAT(worldbottom);
					wallVerts[2].y = FIXED_TO_FLOAT(worldbottomslope);
					wallVerts[0].y = wallVerts[1].y = FIXED_TO_FLOAT(INT32_MIN); // draw to bottom of map space
					HWR_DrawSkyWall(wallVerts, &Surf);
				}
			}
		}
		RR3DS_KIND(0);
	}
	else
	{
		// Single sided line... Deal only with the middletexture (if one exists)
		gl_midtexture = R_GetTextureNum(gl_sidedef->midtexture);
		if (gl_midtexture && gl_linedef->special != 41) // (Ignore horizon line for OGL)
		{
			{
				fixed_t     texturevpeg;
				// PEGGING
				if ((gl_linedef->flags & (ML_DONTPEGBOTTOM|ML_NOSKEW)) == (ML_DONTPEGBOTTOM|ML_NOSKEW))
					texturevpeg = gl_frontsector->floorheight + textureheight[gl_sidedef->midtexture] - gl_frontsector->ceilingheight + gl_sidedef->rowoffset;
				else if (gl_linedef->flags & ML_DONTPEGBOTTOM)
					texturevpeg = worldbottom + textureheight[gl_sidedef->midtexture] - worldtop + gl_sidedef->rowoffset;
				else
					// top of texture at top
					texturevpeg = gl_sidedef->rowoffset;

				grTex = HWR_GetTexture(gl_midtexture, gl_sidedef->midtexture);

				// Check if we should flip tripwire texture vertically for single-sided lines too
				if (R_ShouldFlipTripWire(gl_linedef))
				{
					// Flip texture coordinates vertically
					wallVerts[0].t = wallVerts[1].t = texturevpeg * grTex->scaleY;
					wallVerts[3].t = wallVerts[2].t = (texturevpeg + gl_frontsector->ceilingheight - gl_frontsector->floorheight) * grTex->scaleY;
				}
				else
				{
					wallVerts[3].t = wallVerts[2].t = texturevpeg * grTex->scaleY;
					wallVerts[0].t = wallVerts[1].t = (texturevpeg + gl_frontsector->ceilingheight - gl_frontsector->floorheight) * grTex->scaleY;
				}
				wallVerts[0].s = wallVerts[3].s = cliplow * grTex->scaleX;
				wallVerts[2].s = wallVerts[1].s = cliphigh * grTex->scaleX;

				// Texture correction for slopes
				if (gl_linedef->flags & ML_NOSKEW) {
					wallVerts[3].t += (gl_frontsector->ceilingheight - worldtop) * grTex->scaleY;
					wallVerts[2].t += (gl_frontsector->ceilingheight - worldtopslope) * grTex->scaleY;
					wallVerts[0].t += (gl_frontsector->floorheight - worldbottom) * grTex->scaleY;
					wallVerts[1].t += (gl_frontsector->floorheight - worldbottomslope) * grTex->scaleY;
				} else if (gl_linedef->flags & ML_DONTPEGBOTTOM) {
					wallVerts[3].t = wallVerts[0].t + (worldbottom-worldtop) * grTex->scaleY;
					wallVerts[2].t = wallVerts[1].t + (worldbottomslope-worldtopslope) * grTex->scaleY;
				} else {
					wallVerts[0].t = wallVerts[3].t - (worldbottom-worldtop) * grTex->scaleY;
					wallVerts[1].t = wallVerts[2].t - (worldbottomslope-worldtopslope) * grTex->scaleY;
				}
			}

			//Set textures properly on single sided walls that are sloped
			wallVerts[3].y = FIXED_TO_FLOAT(worldtop);
			wallVerts[0].y = FIXED_TO_FLOAT(worldbottom);
			wallVerts[2].y = FIXED_TO_FLOAT(worldtopslope);
			wallVerts[1].y = FIXED_TO_FLOAT(worldbottomslope);

			// I don't think that solid walls can use translucent linedef types...
			if (gl_frontsector->numlights)
				HWR_SplitWall(gl_frontsector, wallVerts, gl_midtexture, gl_sidedef->midtexture, &Surf, FOF_CUTLEVEL, NULL, 0);
			else
			{
				FBITFIELD blendmode = PF_Masked;
				if (RR3DS_TEXTRANSP(grTex))
					blendmode = PF_Environment;

				// I don't think that solid walls can use translucent linedef types...
				if (gl_frontsector->numlights)
					HWR_SplitWall(gl_frontsector, wallVerts, gl_midtexture, gl_sidedef->midtexture, &Surf, FOF_CUTLEVEL, NULL, blendmode);
				else
				{
					if (RR3DS_TEXTRANSP(grTex))
						HWR_AddTransparentWall(wallVerts, &Surf, gl_midtexture, gl_sidedef->midtexture, blendmode, false, lightnum, colormap);
					else
						HWR_ProjectWall(wallVerts, &Surf, blendmode, lightnum, colormap);
				}
			}
		}

		if (!gl_curline->polyseg)
		{
			if (gl_frontsector->ceilingpic == skyflatnum) // It's a single-sided line with sky for its sector
			{
				wallVerts[2].y = wallVerts[3].y = FIXED_TO_FLOAT(INT32_MAX); // draw to top of map space
				wallVerts[0].y = FIXED_TO_FLOAT(worldtop);
				wallVerts[1].y = FIXED_TO_FLOAT(worldtopslope);
				HWR_DrawSkyWall(wallVerts, &Surf);
			}
			if (gl_frontsector->floorpic == skyflatnum)
			{
				wallVerts[3].y = FIXED_TO_FLOAT(worldbottom);
				wallVerts[2].y = FIXED_TO_FLOAT(worldbottomslope);
				wallVerts[0].y = wallVerts[1].y = FIXED_TO_FLOAT(INT32_MIN); // draw to bottom of map space
				HWR_DrawSkyWall(wallVerts, &Surf);
			}
		}
	}


	//Hurdler: 3d-floors test
	if (gl_frontsector && gl_backsector && !Tag_Compare(&gl_frontsector->tags, &gl_backsector->tags) && (gl_backsector->ffloors || gl_frontsector->ffloors))
	{
		ffloor_t * rover;
		fixed_t    highcut = 0, lowcut = 0;
		fixed_t lowcutslope, highcutslope;

		// Used for height comparisons and etc across FOFs and slopes
		fixed_t high1, highslope1, low1, lowslope1;

		INT32 texnum, basetexnum;
		line_t * newline = NULL; // Multi-Property FOF

		lowcut = std::max(worldbottom, worldlow);
		highcut = std::min(worldtop, worldhigh);
		lowcutslope = std::max(worldbottomslope, worldlowslope);
		highcutslope = std::min(worldtopslope, worldhighslope);

		if (gl_backsector->ffloors)
		{
			INT32 rr_ord = -1; (void)rr_ord;
			for (rover = gl_backsector->ffloors; rover; rover = rover->next)
			{
				rr_ord++;
				RR3DS_KIND(0);
				boolean bothsides = false;
				// Skip if it exists on both sectors.
				ffloor_t * r2;
				for (r2 = gl_frontsector->ffloors; r2; r2 = r2->next)
					if (rover->master == r2->master)
					{
						bothsides = true;
						break;
					}

				if (bothsides) continue;

				if (!(rover->fofflags & FOF_EXISTS) || !(rover->fofflags & FOF_RENDERSIDES))
					continue;
				if (!(rover->fofflags & FOF_ALLSIDES) && rover->fofflags & FOF_INVERTSIDES)
					continue;

				SLOPEPARAMS(*rover->t_slope, high1, highslope1, *rover->topheight)
				SLOPEPARAMS(*rover->b_slope, low1,  lowslope1,  *rover->bottomheight)

				if ((high1 < lowcut && highslope1 < lowcutslope) || (low1 > highcut && lowslope1 > highcutslope))
					continue;
				RR3DS_KIND(rr_ord < 24 ? 8 + rr_ord : 0);

				basetexnum = sides[rover->master->sidenum[0]].midtexture;

				if (rover->master->flags & ML_TFERLINE)
				{
					size_t linenum = gl_curline->linedef-gl_backsector->lines[0];
					newline = rover->master->frontsector->lines[0] + linenum;
					basetexnum = sides[newline->sidenum[0]].midtexture;
				}

				texnum = R_GetTextureNum(basetexnum);

				h  = P_GetFFloorTopZAt   (rover, v1x, v1y);
				hS = P_GetFFloorTopZAt   (rover, v2x, v2y);
				l  = P_GetFFloorBottomZAt(rover, v1x, v1y);
				lS = P_GetFFloorBottomZAt(rover, v2x, v2y);
				// Adjust the heights so the FOF does not overlap with top and bottom textures.
				if (h >= highcut && hS >= highcutslope)
				{
					h = highcut;
					hS = highcutslope;
				}
				if (l <= lowcut && lS <= lowcutslope)
				{
					l = lowcut;
					lS = lowcutslope;
				}
				//Hurdler: HW code starts here
				//FIXME: check if peging is correct
				// set top/bottom coords

				wallVerts[3].y = FIXED_TO_FLOAT(h);
				wallVerts[2].y = FIXED_TO_FLOAT(hS);
				wallVerts[0].y = FIXED_TO_FLOAT(l);
				wallVerts[1].y = FIXED_TO_FLOAT(lS);
				if (rover->fofflags & FOF_FOG)
				{
					wallVerts[3].t = wallVerts[2].t = 0;
					wallVerts[0].t = wallVerts[1].t = 0;
					wallVerts[0].s = wallVerts[3].s = 0;
					wallVerts[2].s = wallVerts[1].s = 0;
				}
				else
				{
					fixed_t texturevpeg;
					boolean attachtobottom = false;
					boolean slopeskew = false; // skew FOF walls with slopes?

					// Wow, how was this missing from OpenGL for so long?
					// ...Oh well, anyway, Lower Unpegged now changes pegging of FOFs like in software
					// -- Monster Iestyn 26/06/18
					if (newline)
					{
						texturevpeg = sides[newline->sidenum[0]].rowoffset;
						attachtobottom = !!(newline->flags & ML_DONTPEGBOTTOM);
						slopeskew = !!(newline->flags & ML_SKEWTD);
					}
					else
					{
						texturevpeg = sides[rover->master->sidenum[0]].rowoffset;
						attachtobottom = !!(gl_linedef->flags & ML_DONTPEGBOTTOM);
						slopeskew = !!(rover->master->flags & ML_SKEWTD);
					}

					grTex = HWR_GetTexture(texnum, basetexnum);

					if (!slopeskew) // no skewing
					{
						if (attachtobottom)
							texturevpeg -= *rover->topheight - *rover->bottomheight;
						wallVerts[3].t = (*rover->topheight - h + texturevpeg) * grTex->scaleY;
						wallVerts[2].t = (*rover->topheight - hS + texturevpeg) * grTex->scaleY;
						wallVerts[0].t = (*rover->topheight - l + texturevpeg) * grTex->scaleY;
						wallVerts[1].t = (*rover->topheight - lS + texturevpeg) * grTex->scaleY;
					}
					else
					{
						if (!attachtobottom) // skew by top
						{
							wallVerts[3].t = wallVerts[2].t = texturevpeg * grTex->scaleY;
							wallVerts[0].t = (h - l + texturevpeg) * grTex->scaleY;
							wallVerts[1].t = (hS - lS + texturevpeg) * grTex->scaleY;
						}
						else // skew by bottom
						{
							wallVerts[0].t = wallVerts[1].t = texturevpeg * grTex->scaleY;
							wallVerts[3].t = wallVerts[0].t - (h - l) * grTex->scaleY;
							wallVerts[2].t = wallVerts[1].t - (hS - lS) * grTex->scaleY;
						}
					}

					wallVerts[0].s = wallVerts[3].s = cliplow * grTex->scaleX;
					wallVerts[2].s = wallVerts[1].s = cliphigh * grTex->scaleX;
				}
				if (rover->fofflags & FOF_FOG)
				{
					FBITFIELD blendmode;

					blendmode = PF_Fog|PF_NoTexture;

					lightnum = HWR_CalcWallLight(rover->master->frontsector->lightlevel, gl_curline);
					colormap = rover->master->frontsector->extra_colormap;

					Surf.PolyColor.s.alpha = HWR_FogBlockAlpha(rover->master->frontsector->lightlevel, rover->master->frontsector->extra_colormap);

					if (gl_frontsector->numlights)
						HWR_SplitWall(gl_frontsector, wallVerts, 0, 0, &Surf, rover->fofflags, rover, blendmode);
					else
						HWR_AddTransparentWall(wallVerts, &Surf, 0, 0, blendmode, true, lightnum, colormap);
				}
				else
				{
					FBITFIELD blendmode = PF_Masked;

					if (rover->fofflags & FOF_TRANSLUCENT)
					{
						if (rover->alpha < 256 || rover->blend)
						{
							blendmode = HWR_GetBlendModeFlag(rover->blend);
							Surf.PolyColor.s.alpha = std::max<INT32>(0, std::min<INT32>(rover->alpha, 255));
						}
					}

					if (gl_frontsector->numlights)
						HWR_SplitWall(gl_frontsector, wallVerts, texnum, basetexnum, &Surf, rover->fofflags, rover, blendmode);
					else
					{
						if (blendmode != PF_Masked)
							HWR_AddTransparentWall(wallVerts, &Surf, texnum, basetexnum, blendmode, false, lightnum, colormap);
						else
							HWR_ProjectWall(wallVerts, &Surf, PF_Masked, lightnum, colormap);
					}
				}
			}
		}

		RR3DS_KIND(0);
		if (gl_frontsector->ffloors) // Putting this seperate should allow 2 FOF sectors to be connected without too many errors? I think?
		{
			for (rover = gl_frontsector->ffloors; rover; rover = rover->next)
			{
				boolean bothsides = false;
				// Skip if it exists on both sectors.
				ffloor_t * r2;
				for (r2 = gl_backsector->ffloors; r2; r2 = r2->next)
					if (rover->master == r2->master)
					{
						bothsides = true;
						break;
					}

				if (bothsides) continue;

				if (!(rover->fofflags & FOF_EXISTS) || !(rover->fofflags & FOF_RENDERSIDES))
					continue;
				if (!(rover->fofflags & FOF_ALLSIDES || rover->fofflags & FOF_INVERTSIDES))
					continue;

				SLOPEPARAMS(*rover->t_slope, high1, highslope1, *rover->topheight)
				SLOPEPARAMS(*rover->b_slope, low1,  lowslope1,  *rover->bottomheight)

				if ((high1 < lowcut && highslope1 < lowcutslope) || (low1 > highcut && lowslope1 > highcutslope))
					continue;

				basetexnum = sides[rover->master->sidenum[0]].midtexture;

				if (rover->master->flags & ML_TFERLINE)
				{
					size_t linenum = gl_curline->linedef-gl_backsector->lines[0];
					newline = rover->master->frontsector->lines[0] + linenum;
					basetexnum = sides[newline->sidenum[0]].midtexture;
				}

				texnum = R_GetTextureNum(basetexnum);

				h  = P_GetFFloorTopZAt   (rover, v1x, v1y);
				hS = P_GetFFloorTopZAt   (rover, v2x, v2y);
				l  = P_GetFFloorBottomZAt(rover, v1x, v1y);
				lS = P_GetFFloorBottomZAt(rover, v2x, v2y);
				// Adjust the heights so the FOF does not overlap with top and bottom textures.
				if (h >= highcut && hS >= highcutslope)
				{
					h = highcut;
					hS = highcutslope;
				}
				if (l <= lowcut && lS <= lowcutslope)
				{
					l = lowcut;
					lS = lowcutslope;
				}
				//Hurdler: HW code starts here
				//FIXME: check if peging is correct
				// set top/bottom coords

				wallVerts[3].y = FIXED_TO_FLOAT(h);
				wallVerts[2].y = FIXED_TO_FLOAT(hS);
				wallVerts[0].y = FIXED_TO_FLOAT(l);
				wallVerts[1].y = FIXED_TO_FLOAT(lS);
				if (rover->fofflags & FOF_FOG)
				{
					wallVerts[3].t = wallVerts[2].t = 0;
					wallVerts[0].t = wallVerts[1].t = 0;
					wallVerts[0].s = wallVerts[3].s = 0;
					wallVerts[2].s = wallVerts[1].s = 0;
				}
				else
				{
					grTex = HWR_GetTexture(texnum, basetexnum);

					if (newline)
					{
						wallVerts[3].t = wallVerts[2].t = (*rover->topheight - h + sides[newline->sidenum[0]].rowoffset) * grTex->scaleY;
						wallVerts[0].t = wallVerts[1].t = (h - l + (*rover->topheight - h + sides[newline->sidenum[0]].rowoffset)) * grTex->scaleY;
					}
					else
					{
						wallVerts[3].t = wallVerts[2].t = (*rover->topheight - h + sides[rover->master->sidenum[0]].rowoffset) * grTex->scaleY;
						wallVerts[0].t = wallVerts[1].t = (h - l + (*rover->topheight - h + sides[rover->master->sidenum[0]].rowoffset)) * grTex->scaleY;
					}

					wallVerts[0].s = wallVerts[3].s = cliplow * grTex->scaleX;
					wallVerts[2].s = wallVerts[1].s = cliphigh * grTex->scaleX;
				}

				if (rover->fofflags & FOF_FOG)
				{
					FBITFIELD blendmode;

					blendmode = PF_Fog|PF_NoTexture;

					lightnum = HWR_CalcWallLight(rover->master->frontsector->lightlevel, gl_curline);
					colormap = rover->master->frontsector->extra_colormap;

					Surf.PolyColor.s.alpha = HWR_FogBlockAlpha(rover->master->frontsector->lightlevel, rover->master->frontsector->extra_colormap);

					if (gl_backsector->numlights)
						HWR_SplitWall(gl_backsector, wallVerts, 0, 0, &Surf, rover->fofflags, rover, blendmode);
					else
						HWR_AddTransparentWall(wallVerts, &Surf, 0, 0, blendmode, true, lightnum, colormap);
				}
				else
				{
					FBITFIELD blendmode = PF_Masked;

					if (rover->fofflags & FOF_TRANSLUCENT)
					{
						if (rover->alpha < 256 || rover->blend)
						{
							blendmode = HWR_GetBlendModeFlag(rover->blend);
							Surf.PolyColor.s.alpha = std::max<INT32>(0, std::min<INT32>(rover->alpha, 255));
						}
					}

					if (gl_backsector->numlights)
						HWR_SplitWall(gl_backsector, wallVerts, texnum, basetexnum, &Surf, rover->fofflags, rover, blendmode);
					else
					{
						if (blendmode != PF_Masked)
							HWR_AddTransparentWall(wallVerts, &Surf, texnum, basetexnum, blendmode, false, lightnum, colormap);
						else
							HWR_ProjectWall(wallVerts, &Surf, PF_Masked, lightnum, colormap);
					}
				}
			}
		}
	}
#undef SLOPEPARAMS
//Hurdler: end of 3d-floors test
}

// From PrBoom:
//
// e6y: Check whether the player can look beyond this line
//
boolean checkforemptylines = true;
// Don't modify anything here, just check
// Kalaron: Modified for sloped linedefs
static boolean CheckClip(seg_t * seg, sector_t * afrontsector, sector_t * abacksector)
{
	fixed_t frontf1,frontf2, frontc1, frontc2; // front floor/ceiling ends
	fixed_t backf1, backf2, backc1, backc2; // back floor ceiling ends
	boolean bothceilingssky = false, bothfloorssky = false;

	if (abacksector->ceilingpic == skyflatnum && afrontsector->ceilingpic == skyflatnum)
		bothceilingssky = true;
	if (abacksector->floorpic == skyflatnum && afrontsector->floorpic == skyflatnum)
		bothfloorssky = true;

	// GZDoom method of sloped line clipping

	if (afrontsector->f_slope || afrontsector->c_slope || abacksector->f_slope || abacksector->c_slope)
	{
		fixed_t v1x, v1y, v2x, v2y; // the seg's vertexes as fixed_t
		v1x = FLOAT_TO_FIXED(((polyvertex_t *)gl_curline->pv1)->x);
		v1y = FLOAT_TO_FIXED(((polyvertex_t *)gl_curline->pv1)->y);
		v2x = FLOAT_TO_FIXED(((polyvertex_t *)gl_curline->pv2)->x);
		v2y = FLOAT_TO_FIXED(((polyvertex_t *)gl_curline->pv2)->y);
#define SLOPEPARAMS(slope, end1, end2, normalheight) \
		end1 = P_GetZAt(slope, v1x, v1y, normalheight); \
		end2 = P_GetZAt(slope, v2x, v2y, normalheight);

		SLOPEPARAMS(afrontsector->f_slope, frontf1, frontf2, afrontsector->  floorheight)
		SLOPEPARAMS(afrontsector->c_slope, frontc1, frontc2, afrontsector->ceilingheight)
		SLOPEPARAMS( abacksector->f_slope,  backf1,  backf2,  abacksector->  floorheight)
		SLOPEPARAMS( abacksector->c_slope,  backc1,  backc2,  abacksector->ceilingheight)
#undef SLOPEPARAMS
	}
	else
	{
		frontf1 = frontf2 = afrontsector->  floorheight;
		frontc1 = frontc2 = afrontsector->ceilingheight;
		backf1  =  backf2 =  abacksector->  floorheight;
		backc1  =  backc2 =  abacksector->ceilingheight;
	}
	// properly render skies (consider door "open" if both ceilings are sky)
	// same for floors
	if (!bothceilingssky && !bothfloorssky)
	{
		// now check for closed sectors!
		if ((backc1 <= frontf1 && backc2 <= frontf2)
			|| (backf1 >= frontc1 && backf2 >= frontc2))
		{
			checkforemptylines = false;
			return true;
		}

		if (backc1 <= backf1 && backc2 <= backf2)
		{
			// preserve a kind of transparent door/lift special effect:
			if (((backc1 >= frontc1 && backc2 >= frontc2) || seg->sidedef->toptexture)
			&& ((backf1 <= frontf1 && backf2 <= frontf2) || seg->sidedef->bottomtexture))
			{
				checkforemptylines = false;
				return true;
			}
		}
	}

	if (!bothceilingssky) {
		if (backc1 != frontc1 || backc2 != frontc2)
		{
			checkforemptylines = false;
			return false;
		}
	}

	if (!bothfloorssky) {
		if (backf1 != frontf1 || backf2 != frontf2)
		{
			checkforemptylines = false;
			return false;
		}
	}

	return false;
}

// -----------------+
// HWR_AddLine      : Clips the given segment and adds any visible pieces to the line list.
// Notes            : gl_cursectorlight is set to the current subsector -> sector -> light value
//                  : (it may be mixed with the wall's own flat colour in the future ...)
#ifdef RR3DS
// 3DS: [SUB] breakdown inside the BSP, only 1 frame in 4 (sampling).
namespace rr3ds_sub {
	enum { S_FAKE, S_PLANES, S_POLY, S_SPR, S_LINES, S_N };
	static UINT64 accV[2][S_N];
	static UINT64 nSubV[2], nLinesV[2], bspTotV[2], skyBgV[2];
	static UINT64 segT[2], nFront[2], nProc[2], nMiss[2], nAngReuse[2];
	static UINT64 sigT[2], useT[2], missT[2]; // breakdown of process time
	static int framesV[2];
	static int on = 0, frameNo = 0, view = 0;
	static UINT64 last;
	static inline void Start(void) { if (on) last = I_GetPreciseTime(); }
	static inline void Mark(int seg) { if (on) { UINT64 n = I_GetPreciseTime(); accV[view][seg] += n - last; last = n; } }
}
#define SUB_START() rr3ds_sub::Start()
#define SUB_MARK(x) rr3ds_sub::Mark(rr3ds_sub::x)
#else
#define SUB_START()
#define SUB_MARK(x)
#endif
// -----------------+
// Wraps HWR_ProcessSeg with the static geometry cache.
#ifdef RR3DS
// sector data that only depends on the view (heights, light, colormap):
// computed once per sector and view, not per subsector (~3 per sector).
namespace rr3ds_seccache {
	struct E { UINT32 stamp; fixed_t cullF, cullC, locF, locC; INT32 fll, cll; extracolormap_t *fcm, *ccm; };
	static std::vector<E> e;
	static UINT32 stamp = 1;
	void LevelReset(void) { e.assign(numsectors, E{0, 0, 0, 0, 0, 0, 0, NULL, NULL}); stamp = 1; }
}
#endif
#ifdef RR3DS
// recomputes y/t of a simple wall (no slopes) with the current heights,
// same as HWR_ProcessSeg. False if which pieces are visible changes (needs recapture).
// real heights at both ends of the seg (with static slopes)
static void RR3DS_WorldH(fixed_t &wt, fixed_t &wts, fixed_t &wb, fixed_t &wbs, fixed_t &wh, fixed_t &whs, fixed_t &wl, fixed_t &wls)
{
	const sector_t *fs = gl_frontsector, *bs = gl_backsector;
	const fixed_t v1x = FLOAT_TO_FIXED(((polyvertex_t *)gl_curline->pv1)->x), v1y = FLOAT_TO_FIXED(((polyvertex_t *)gl_curline->pv1)->y);
	const fixed_t v2x = FLOAT_TO_FIXED(((polyvertex_t *)gl_curline->pv2)->x), v2y = FLOAT_TO_FIXED(((polyvertex_t *)gl_curline->pv2)->y);
	wt = P_GetZAt(fs->c_slope, v1x, v1y, fs->ceilingheight); wts = P_GetZAt(fs->c_slope, v2x, v2y, fs->ceilingheight);
	wb = P_GetZAt(fs->f_slope, v1x, v1y, fs->floorheight);   wbs = P_GetZAt(fs->f_slope, v2x, v2y, fs->floorheight);
	wh = P_GetZAt(bs->c_slope, v1x, v1y, bs->ceilingheight); whs = P_GetZAt(bs->c_slope, v2x, v2y, bs->ceilingheight);
	wl = P_GetZAt(bs->f_slope, v1x, v1y, bs->floorheight);   wls = P_GetZAt(bs->f_slope, v2x, v2y, bs->floorheight);
}
static UINT8 RR3DS_SimpleMask(void)
{
	const sector_t *fs = gl_frontsector, *bs = gl_backsector;
	const side_t *sd = gl_curline->sidedef;
	UINT8 m = 0;
	const boolean csky = fs->ceilingpic == skyflatnum && bs->ceilingpic == skyflatnum;
	const boolean fsky = fs->floorpic == skyflatnum && bs->floorpic == skyflatnum;
	fixed_t wt, wts, wb, wbs, wh, whs, wl, wls; RR3DS_WorldH(wt, wts, wb, wbs, wh, whs, wl, wls);
	if (!csky && R_GetTextureNum(sd->toptexture) && (whs < wts || wh < wt)) m |= 1u << 1;
	if (!fsky && R_GetTextureNum(sd->bottomtexture) && (wls > wbs || wl > wb)) m |= 1u << 2;
	if (R_GetTextureNum(sd->midtexture)) m |= 1u << 3;
	if (!gl_curline->polyseg)
	{
		if (fs->ceilingpic == skyflatnum && bs->ceilingpic != skyflatnum) m |= 1u << 4;
		if (fs->floorpic == skyflatnum && bs->floorpic != skyflatnum) m |= 1u << 5;
	}
	return m;
}
// which FOFs of the back sector draw a wall now (same as HWR_ProcessSeg, no slopes)
static UINT32 RR3DS_FofMask(void)
{
	const sector_t *fs = gl_frontsector, *bs = gl_backsector;
	if (!bs->ffloors || Tag_Compare(&fs->tags, &bs->tags)) return 0;
	const fixed_t lowcut = std::max(fs->floorheight, bs->floorheight), highcut = std::min(fs->ceilingheight, bs->ceilingheight);
	UINT32 m = 0; int k = 0;
	for (const ffloor_t *r = bs->ffloors; r; r = r->next, k++)
	{
		if (!(r->fofflags & FOF_EXISTS) || !(r->fofflags & FOF_RENDERSIDES)) continue;
		if (!(r->fofflags & FOF_ALLSIDES) && (r->fofflags & FOF_INVERTSIDES)) continue;
		if (*r->topheight < lowcut || *r->bottomheight > highcut) continue;
		if (k < 24) m |= 1u << k;
	}
	return m;
}
static boolean RR3DS_PatchSegHeights(const rr3ds_static::SItem &it)
{
	using namespace rr3ds_static;
	if (RR3DS_SimpleMask() != it.kmask || RR3DS_FofMask() != it.fofMask)
		return false;
	const sector_t *fs = gl_frontsector, *bs = gl_backsector;
	const side_t *sd = gl_curline->sidedef;
	const line_t *ld = gl_curline->linedef;
	const fixed_t wt = fs->ceilingheight, wb = fs->floorheight, wh = bs->ceilingheight, wl = bs->floorheight;
	fixed_t xwt, xwts, xwb, xwbs, xwh, xwhs, xwl, xwls; RR3DS_WorldH(xwt, xwts, xwb, xwbs, xwh, xwhs, xwl, xwls);
	const boolean anySl = fs->f_slope || fs->c_slope || bs->f_slope || bs->c_slope;
	for (UINT32 p = it.firstPoly; p < it.firstPoly + it.numPolys; p++)
	{
		const SPoly &sp = polys[p];
		float y[4], t[4];
		const float sy = sp.sy;
		switch (sp.pad)
		{
			case 1: // top (with static slopes, same as HWR_ProcessSeg)
			{
				const INT32 tex = R_GetTextureNum(sd->toptexture);
				fixed_t v;
				if (ld->flags & ML_DONTPEGTOP) v = 0;
				else if (ld->flags & ML_SKEWTD) v = xwh + textureheight[sd->toptexture] - xwt;
				else v = bs->ceilingheight + textureheight[sd->toptexture] - fs->ceilingheight;
				v += sd->rowoffset;
				v %= (textures[tex]->height) << FRACBITS;
				t[3] = t[2] = v * sy; t[0] = t[1] = (v + fs->ceilingheight - bs->ceilingheight) * sy;
				if (!(ld->flags & ML_SKEWTD))
				{
					t[3] -= (xwt - fs->ceilingheight) * sy; t[2] -= (xwts - fs->ceilingheight) * sy;
					t[0] -= (xwh - bs->ceilingheight) * sy; t[1] -= (xwhs - bs->ceilingheight) * sy;
				}
				else if (ld->flags & ML_DONTPEGTOP)
				{
					t[0] = (v + xwt - xwh) * sy; t[1] = (v + xwts - xwhs) * sy;
				}
				else
				{
					t[0] = t[1] = (v + xwt - xwh) * sy;
					t[3] = t[0] - (xwt - xwh) * sy; t[2] = t[1] - (xwts - xwhs) * sy;
				}
				y[3] = FIXED_TO_FLOAT(xwt); y[0] = FIXED_TO_FLOAT(xwh); y[2] = FIXED_TO_FLOAT(xwts); y[1] = FIXED_TO_FLOAT(xwhs);
				break;
			}
			case 2: // bottom (with static slopes)
			{
				const INT32 tex = R_GetTextureNum(sd->bottomtexture);
				fixed_t v;
				if (!(ld->flags & ML_DONTPEGBOTTOM)) v = 0;
				else if (ld->flags & ML_SKEWTD) v = xwb - xwl;
				else v = fs->floorheight - bs->floorheight;
				v += sd->rowoffset;
				v %= (textures[tex]->height) << FRACBITS;
				t[3] = t[2] = v * sy; t[0] = t[1] = (v + bs->floorheight - fs->floorheight) * sy;
				if (!(ld->flags & ML_SKEWTD))
				{
					t[0] -= (xwb - fs->floorheight) * sy; t[1] -= (xwbs - fs->floorheight) * sy;
					t[3] -= (xwl - bs->floorheight) * sy; t[2] -= (xwls - bs->floorheight) * sy;
				}
				else if (ld->flags & ML_DONTPEGBOTTOM)
				{
					t[0] = t[1] = (v + xwl - xwb) * sy;
					t[2] = t[1] - (xwls - xwbs) * sy;
				}
				else
				{
					t[0] = (v + xwl - xwb) * sy; t[1] = (v + xwls - xwbs) * sy;
				}
				y[3] = FIXED_TO_FLOAT(xwl); y[0] = FIXED_TO_FLOAT(xwb); y[2] = FIXED_TO_FLOAT(xwls); y[1] = FIXED_TO_FLOAT(xwbs);
				break;
			}
			case 3: // middle
			{
				if (anySl) return false;
				const INT32 mid = R_GetTextureNum(sd->midtexture);
				const sector_t *front = ld->frontsector, *back = ld->backsector;
				const INT32 repeats = sd->repeatcnt ? 1 + sd->repeatcnt : 1;
				const fixed_t popentop = std::min(wt, wh), popenbottom = std::max(wb, wl);
				fixed_t polytop, polybottom;
				if (ld->flags & ML_NOSKEW)
				{
					if (ld->flags & ML_MIDPEG)
					{
						polybottom = std::max(front->floorheight, back->floorheight) + sd->rowoffset;
						polytop = polybottom + textureheight[mid]*repeats;
					}
					else
					{
						polytop = std::min(front->ceilingheight, back->ceilingheight) + sd->rowoffset;
						polybottom = polytop - textureheight[mid]*repeats;
					}
				}
				else if (ld->flags & ML_MIDPEG)
				{
					polybottom = popenbottom + sd->rowoffset;
					polytop = polybottom + textureheight[mid]*repeats;
				}
				else
				{
					polytop = popentop + sd->rowoffset;
					polybottom = polytop - textureheight[mid]*repeats;
				}
				const fixed_t h = std::min(popentop, polytop), l = std::max(polybottom, popenbottom);
				const fixed_t v = (ld->flags & ML_MIDPEG) ? textureheight[sd->midtexture]*repeats - h + polybottom : polytop - h;
				t[3] = t[2] = v * sy; t[0] = t[1] = (h - l + v) * sy;
				y[2] = y[3] = FIXED_TO_FLOAT(h); y[0] = y[1] = FIXED_TO_FLOAT(l);
				break;
			}
			case 4: // sky above
				if (anySl) return false;
				y[2] = y[3] = FIXED_TO_FLOAT(INT32_MAX); y[0] = y[1] = FIXED_TO_FLOAT(wt);
				t[0] = t[1] = t[2] = t[3] = 0.0f;
				break;
			case 5: // sky below
				if (anySl) return false;
				y[3] = y[2] = FIXED_TO_FLOAT(wb); y[0] = y[1] = FIXED_TO_FLOAT(INT32_MIN);
				t[0] = t[1] = t[2] = t[3] = 0.0f;
				break;
			default:
			{
				if (sp.pad < 8 || anySl) return false;
				// FOF wall of the back sector (order sp.pad-8)
				const ffloor_t *r = bs->ffloors; for (int k = 8; r && k < sp.pad; k++) r = r->next;
				if (!r) return false;
				const fixed_t lowcut = std::max(wb, wl), highcut = std::min(wt, wh);
				const fixed_t top = *r->topheight, bot = *r->bottomheight;
				fixed_t hh = top, ll = bot;
				if (hh >= highcut) hh = highcut;
				if (ll <= lowcut) ll = lowcut;
				fixed_t v = sides[r->master->sidenum[0]].rowoffset;
				const boolean attach = (ld->flags & ML_DONTPEGBOTTOM) != 0;
				if (!(r->master->flags & ML_SKEWTD))
				{
					if (attach) v -= top - bot;
					t[3] = t[2] = (top - hh + v) * sy; t[0] = t[1] = (top - ll + v) * sy;
				}
				else if (!attach) { t[3] = t[2] = v * sy; t[0] = t[1] = (hh - ll + v) * sy; }
				else { t[0] = t[1] = v * sy; t[3] = t[2] = t[0] - (hh - ll) * sy; }
				y[3] = y[2] = FIXED_TO_FLOAT(hh); y[0] = y[1] = FIXED_TO_FLOAT(ll);
				break;
			}
		}
		I_3DS_StaticSetYT(sp.base, 4, y, t);
	}
	return true;
}
// plane without slope: only the height changes -> all y equal to the new one
static boolean RR3DS_PatchHeights(size_t item, const rr3ds_static::SItem &it)
{
	using namespace rr3ds_static;
	if (item >= segBase)
		return RR3DS_PatchSegHeights(it);
	if (it.kmask != (1u << 6))
		return false;
	for (UINT32 p = it.firstPoly; p < it.firstPoly + it.numPolys; p++)
		I_3DS_StaticSetY(polys[p].base, polys[p].n, curPlaneY);
	return true;
}
#endif
static void RR3DS_ProcessSegCachedImpl(void);
static void RR3DS_ProcessSegCached(void)
{
#ifdef RR3DS
	if (rr3ds_sub::on)
	{
		const UINT64 t0 = I_GetPreciseTime();
		RR3DS_ProcessSegCachedImpl();
		rr3ds_sub::segT[rr3ds_sub::view] += I_GetPreciseTime() - t0;
		rr3ds_sub::nProc[rr3ds_sub::view]++;
		return;
	}
#endif
	RR3DS_ProcessSegCachedImpl();
}
static int RR3DS_ViewNow(void) { return rr3ds_sub::view; }
static void RR3DS_ProcessSegCachedImpl(void)
{
#ifdef RR3DS
	using namespace rr3ds_static;
	seg_t *line = gl_curline;
	if (enabled && line >= segs && line < segs + numsegs && !line->polyseg
		&& SectorOk(line->frontsector) && SectorOk(line->backsector)
		&& gl_frontsector >= sectors && gl_frontsector < sectors + numsectors
		&& (!gl_backsector || (gl_backsector >= sectors && gl_backsector < sectors + numsectors)))
	{
		const UINT64 rrT0 = rr3ds_sub::on ? I_GetPreciseTime() : 0;
		// simple wall -> signature without heights; heights go in curSigH and are patched
		const sector_t *fs = gl_frontsector, *bs = gl_backsector;
		// reuse what was decided if nothing about this wall changed
		const size_t segIdx = (size_t)(line - segs);
		const UINT32 mF = SectorSig(fs), mB = bs ? SectorSig(bs) : 0xBAC0u;
		UINT32 mK = 2166136261u;
		if (const side_t *sd = line->sidedef)
		{
			mK = H(mK, (UINT32)sd->rowoffset); mK = H(mK, (UINT32)sd->toptexture); mK = H(mK, (UINT32)sd->midtexture);
			mK = H(mK, (UINT32)sd->bottomtexture); mK = H(mK, (UINT32)sd->repeatcnt);
		}
		else mK = H(mK, 0x5Du);
		if (const line_t *ld = line->linedef) { mK = H(mK, (UINT32)ld->flags); mK = H(mK, (UINT32)ld->alpha); mK = H(mK, (UINT32)ld->blendmode); }
		const UINT32 mFs = (UINT32)(fs - sectors), mBs = bs ? (UINT32)(bs - sectors) : 0xFFFFFFFFu;
		SegMemo *mm = segIdx < segMemo.size() ? &segMemo[segIdx] : NULL;
		UINT32 sig;
		if (mm && mm->ok && mm->fSig == mF && mm->bSig == mB && mm->sKey == mK && mm->fs == mFs && mm->bs == mBs)
		{
			sig = mm->sig; curSigH = mm->sigH; curSimple = mm->simple ? true : false; stMemoHit++;
		}
		else
		{
			// STATIC slopes allowed if there are only walls above/below (whack-a-mole moles)
			const boolean anySl = bs && (fs->f_slope || fs->c_slope || bs->f_slope || bs->c_slope);
			const auto dynSl = [](const pslope_t *p) { return p && (p->flags & SL_DYNAMIC); };
			const boolean slOk = !anySl || (line->sidedef && !line->sidedef->midtexture && !bs->ffloors && !bs->numlights
				&& !dynSl(fs->f_slope) && !dynSl(fs->c_slope) && !dynSl(bs->f_slope) && !dynSl(bs->c_slope)
				&& (fs->ceilingpic == skyflatnum) == (bs->ceilingpic == skyflatnum)
				&& (fs->floorpic == skyflatnum) == (bs->floorpic == skyflatnum));
			curSimple = (bs && line->linedef && line->sidedef && slOk
				&& !fs->numlights && !fs->ffloors
				&& !(line->linedef->flags & ML_WRAPMIDTEX) && !P_IsLineTripWire(line->linedef));
			// FOFs in the back sector (drums): allowed if they are flat and simple
			boolean bFof = false;
			if (curSimple && (bs->ffloors || bs->numlights))
			{
				bFof = true; int k = 0;
				for (const ffloor_t *r = bs->ffloors; r; r = r->next, k++)
					if (k >= 24 || (r->t_slope && *r->t_slope) || (r->b_slope && *r->b_slope) || !r->topheight || !r->bottomheight
						|| !r->master || (r->master->flags & ML_TFERLINE) || r->master->sidenum[0] == 0xffff)
						{ curSimple = false; break; }
			}
			if (curSimple)
			{
				sig = H(SectorSigNoH(fs), bFof ? SectorSigRaw(bs, 2 | 4 | 8) : SectorSigNoH(bs));
				curSigH = H(H(H(H(2166136261u, (UINT32)fs->floorheight), (UINT32)fs->ceilingheight), (UINT32)bs->floorheight), (UINT32)bs->ceilingheight);
				for (const ffloor_t *r = bs->ffloors; r; r = r->next)
					curSigH = H(H(curSigH, (UINT32)*r->topheight), (UINT32)*r->bottomheight);
			}
			else
			{
				curSigH = 0;
				sig = gl_backsector ? SectorSig(gl_frontsector) : SectorSigWall1(gl_frontsector);
				sig = H(sig, gl_backsector ? SectorSig(gl_backsector) : 0xBAC0u);
			}
			sig = SideSig(sig, line->sidedef, false); // without scrolling (corrected when drawing)
			if (line->linedef)
			{
				sig = H(sig, line->linedef->flags);
				sig = H(sig, (UINT32)line->linedef->alpha);
				sig = H(sig, line->linedef->blendmode);
			}
			if (mm) { mm->fSig = mF; mm->bSig = mB; mm->sKey = mK; mm->fs = mFs; mm->bs = mBs; mm->sig = sig; mm->sigH = curSigH; mm->simple = curSimple ? 1 : 0; mm->ok = 1; }
			stMemoMiss++;
		}
		curHasOff = (line->sidedef != NULL);
		curTo = line->sidedef ? line->sidedef->textureoffset : 0;
		curRo = 0; // horizontal only
		const UINT64 rrT1 = rr3ds_sub::on ? I_GetPreciseTime() : 0;
		const boolean usedC = Use(segBase + (size_t)(line - segs), sig);
		if (usedC) rr3ds_fast::Set(segBase + (size_t)(line - segs), rr3ds_fast::K_HIT); else rr3ds_fast::Clr(segBase + (size_t)(line - segs));
		if (rr3ds_sub::on) { const UINT64 rrT2 = I_GetPreciseTime(); rr3ds_sub::sigT[rr3ds_sub::view] += rrT1 - rrT0; rr3ds_sub::useT[rr3ds_sub::view] += rrT2 - rrT1; }
		if (usedC) { curHasOff = false; curSimple = false; return; }
		if (rr3ds_sub::on) rr3ds_sub::nMiss[rr3ds_sub::view]++;
		if (Diag() && IsNeverChg(segBase + (size_t)(line - segs)) && line->sidedef)
		{
			const side_t *sd = line->sidedef;
			const INT32 tx = sd->midtexture ? sd->midtexture : (sd->bottomtexture ? sd->bottomtexture : sd->toptexture);
			Culp2((UINT32)(gl_frontsector - sectors), (UINT32)tx, 1);
			// [BLAME4]
			UINT32 cc[14];
			cc[0] = SectorSig(gl_frontsector);
			cc[1] = gl_backsector ? SectorSig(gl_backsector) : 0u;
			cc[2] = SideSig(2166136261u, line->sidedef, false);
			cc[3] = line->linedef ? H(H(H(2166136261u, line->linedef->flags), (UINT32)line->linedef->alpha), line->linedef->blendmode) : 0u;
			cc[4] = curSigH;
			// breakdown: 32 view, 64/128/256/512 front (base, heights, lights, FOFs), 1024..8192 back the same
			cc[5] = (UINT32)rr3ds_sub::view;
			const sector_t *ps[2] = { gl_frontsector, gl_backsector };
			for (int q = 0; q < 2; q++)
			{
				const sector_t *z = ps[q];
				cc[6 + q*4] = z ? SectorSigRaw(z, 0 | 4) : 0u;
				cc[7 + q*4] = z ? H(H(2166136261u, (UINT32)z->floorheight), (UINT32)z->ceilingheight) : 0u;
				cc[8 + q*4] = z ? SectorSigRaw(z, 1 | 4) : 0u;
				cc[9 + q*4] = z ? SectorSigRaw(z, 2 | 4) : 0u;
			}
			Culp4((size_t)(line - segs), (UINT32)(gl_frontsector - sectors), gl_backsector ? (UINT32)(gl_backsector - sectors) : 0xFFFFu, cc);
		}
		const UINT64 rrT3 = rr3ds_sub::on ? I_GetPreciseTime() : 0;
		const unsigned rrUp0 = rr3ds_sub::on ? I_3DS_UpTotalUs() : 0; // diag
		unsigned rrP0[10] = {0}; if (rr3ds_sub::on) for (int q = 0; q < 10; q++) rrP0[q] = I_3DS_UpPartUs(q);
		HWR_ProcessSeg();
		EndCapture();
		if (rr3ds_sub::on)
		{
			const UINT64 rrD = I_GetPreciseTime() - rrT3;
			rr3ds_sub::missT[rr3ds_sub::view] += rrD;
			// diag: which kind of wall is slow in SKY (max 24 lines per race)
			static int slowN = 0;
			const UINT32 rrUs = (UINT32)(rrD * 1000000 / I_GetPrecisePrecision());
			if (Diag() && rr3ds_sub::view == 1 && slowN < 24 && rrUs > 500 && line->sidedef && line->linedef)
			{
				slowN++;
				const side_t *sd = line->sidedef;
				RR_DIAGPRINT("[SLOW] seg=%u us=%u lflags=%X tex t/m/b=%d/%d/%d fs ffl=%d lights=%d bs ffl=%d lights=%d alpha=%d blend=%d\n",
					(unsigned)(line - segs), (unsigned)rrUs, (unsigned)line->linedef->flags,
					(int)sd->toptexture, (int)sd->midtexture, (int)sd->bottomtexture,
					gl_frontsector->ffloors ? 1 : 0, (int)gl_frontsector->numlights,
					(gl_backsector && gl_backsector->ffloors) ? 1 : 0, gl_backsector ? (int)gl_backsector->numlights : 0,
					(int)line->linedef->alpha, (int)line->linedef->blendmode);
				if (sd->midtexture > 0 && sd->midtexture < numtextures)
					RR_DIAGPRINT("[SLOW] mid=%.8s %dx%d uploads_us=%u conv=%u evict=%u reserve=%u dma=%u lin=%u etc=%u rgba=%u pal=%u fread=%u\n", textures[sd->midtexture]->name, (int)textures[sd->midtexture]->width, (int)textures[sd->midtexture]->height, I_3DS_UpTotalUs() - rrUp0, I_3DS_UpPartUs(0)-rrP0[0], I_3DS_UpPartUs(1)-rrP0[1], I_3DS_UpPartUs(2)-rrP0[2], I_3DS_UpPartUs(3)-rrP0[3], I_3DS_UpPartUs(4)-rrP0[4], I_3DS_UpPartUs(6)-rrP0[6], I_3DS_UpPartUs(7)-rrP0[7], I_3DS_UpPartUs(8)-rrP0[8], I_3DS_UpPartUs(9)-rrP0[9]);
			}
		}
		if (curSimple) // if what was captured does not match what was predicted, do not patch
		{
			SItem &ci = items[segBase + (size_t)(line - segs)];
			if (ci.state == ST_VALID && (ci.kmask != RR3DS_SimpleMask() || ci.fofMask != RR3DS_FofMask())) ci.patchOk = 0;
		}
		curHasOff = false; curSimple = false; curKind = 0;
		return;
	}
#endif
	HWR_ProcessSeg();
}

#ifdef RR3DS
// float version of R_PointToAngle64 (which uses 64-bit division by
// software: very slow on the ARM11). Same result: tantoangle[slope].
static inline UINT32 RR3DS_SlopeF(float num, float den)
{
	if (den < (512.0f / 65536.0f))
		return SLOPERANGE;
	const float a = num * (float)SLOPERANGE / den;
	return a < (float)SLOPERANGE ? (UINT32)a : SLOPERANGE;
}
static inline angle_t RR3DS_AngF(float x, float y)
{
	x -= FIXED_TO_FLOAT(viewx); y -= FIXED_TO_FLOAT(viewy); // same as R_PointToAngle64
	if (x == 0.0f && y == 0.0f) return 0;
	if (x >= 0)
	{
		if (y >= 0) return (x > y) ? tantoangle[RR3DS_SlopeF(y, x)] : ANGLE_90 - tantoangle[RR3DS_SlopeF(x, y)];
		y = -y;
		return (x > y) ? 0 - tantoangle[RR3DS_SlopeF(y, x)] : ANGLE_270 + tantoangle[RR3DS_SlopeF(x, y)];
	}
	x = -x;
	if (y >= 0) return (x > y) ? ANGLE_180 - tantoangle[RR3DS_SlopeF(y, x)] : ANGLE_90 + tantoangle[RR3DS_SlopeF(x, y)];
	y = -y;
	return (x > y) ? ANGLE_180 + tantoangle[RR3DS_SlopeF(y, x)] : ANGLE_270 - tantoangle[RR3DS_SlopeF(x, y)];
}
#endif

static void HWR_AddLine(seg_t * line)
{
	angle_t angle1, angle2;

	// SoM: Backsector needs to be run through R_FakeFlat
	static sector_t tempsec;

	fixed_t v1x, v1y, v2x, v2y; // the seg's vertexes as fixed_t
	if (line->polyseg && !(line->polyseg->flags & POF_RENDERSIDES))
		return;

	gl_curline = line;

#ifdef RR3DS
	(void)v1x; (void)v1y; (void)v2x; (void)v2y;
	// fast back-face culling (the seg's side relative to the camera,
	// like R_PointOnSegSide) before computing the 2 angles.
	{
		const polyvertex_t *a = (polyvertex_t *)gl_curline->pv1, *b = (polyvertex_t *)gl_curline->pv2;
		const float vx = FIXED_TO_FLOAT(viewx), vy = FIXED_TO_FLOAT(viewy);
		if ((vy - a->y) * (b->x - a->x) > (b->y - a->y) * (vx - a->x))
			return;
	}
	if (rr3ds_sub::on) rr3ds_sub::nFront[rr3ds_sub::view]++;
	{
		// a seg's v1 is usually the previous seg's v2: reuse its angle.
		static float lx = 0.0f, ly = 0.0f; static angle_t la = 0; static UINT32 lstamp = 0;
		const polyvertex_t *a = (polyvertex_t *)gl_curline->pv1, *b = (polyvertex_t *)gl_curline->pv2;
		if (lstamp == rr3ds_seccache::stamp && a->x == lx && a->y == ly)
		{
			angle1 = la;
			if (rr3ds_sub::on) rr3ds_sub::nAngReuse[rr3ds_sub::view]++;
		}
		else
			angle1 = RR3DS_AngF(a->x, a->y);
		angle2 = RR3DS_AngF(b->x, b->y);
		lx = b->x; ly = b->y; la = angle2; lstamp = rr3ds_seccache::stamp;
	}
#else
	v1x = FLOAT_TO_FIXED(((polyvertex_t *)gl_curline->pv1)->x);
	v1y = FLOAT_TO_FIXED(((polyvertex_t *)gl_curline->pv1)->y);
	v2x = FLOAT_TO_FIXED(((polyvertex_t *)gl_curline->pv2)->x);
	v2y = FLOAT_TO_FIXED(((polyvertex_t *)gl_curline->pv2)->y);

	// OPTIMIZE: quickly reject orthogonal back sides.
	angle1 = R_PointToAngle64(v1x, v1y);
	angle2 = R_PointToAngle64(v2x, v2y);
#endif

	 // PrBoom: Back side, i.e. backface culling - read: endAngle >= startAngle!
	if (angle2 - angle1 < ANGLE_180)
		return;

	// PrBoom: use REAL clipping math YAYYYYYYY!!!

	if (!gld_clipper_SafeCheckRange(angle2, angle1))
	{
		return;
	}

	checkforemptylines = true;

	gl_backsector = line->backsector;

	if (!line->backsector)
	{
		gld_clipper_SafeAddClipRange(angle2, angle1);
	}
	else
	{
		boolean bothceilingssky = false, bothfloorssky = false;

		gl_backsector = R_FakeFlat(gl_backsector, &tempsec, NULL, NULL, true);

		if (gl_backsector->ceilingpic == skyflatnum && gl_frontsector->ceilingpic == skyflatnum)
			bothceilingssky = true;
		if (gl_backsector->floorpic == skyflatnum && gl_frontsector->floorpic == skyflatnum)
			bothfloorssky = true;

		if (bothceilingssky && bothfloorssky) // everything's sky? let's save us a bit of time then
		{
			if (!line->polyseg &&
				!line->sidedef->midtexture
				&& ((!gl_frontsector->ffloors && !gl_backsector->ffloors)
					|| Tag_Compare(&gl_frontsector->tags, &gl_backsector->tags)))
			{
#ifdef RR3DS
				if (line >= segs && line < segs + numsegs) rr3ds_fast::Set(rr3ds_static::segBase + (size_t)(line - segs), rr3ds_fast::K_EMPTY);
#endif
				return; // line is empty, don't even bother
			}
			// treat like wide open window instead
			RR3DS_ProcessSegCached();
			return;
		}

		if (CheckClip(line, gl_frontsector, gl_backsector))
		{
			gld_clipper_SafeAddClipRange(angle2, angle1);
			checkforemptylines = false;
		}
		// Reject empty lines used for triggers and special events.
		// Identical floor and ceiling on both sides,
		//  identical light levels on both sides,
		//  and no middle texture.
		if (checkforemptylines && R_IsEmptyLine(line, gl_frontsector, gl_backsector))
		{
#ifdef RR3DS
			if (line >= segs && line < segs + numsegs) rr3ds_fast::Set(rr3ds_static::segBase + (size_t)(line - segs), rr3ds_fast::K_EMPTY);
#endif
			return;
		}
	}

	RR3DS_ProcessSegCached();
	return;
}

// HWR_CheckBBox
// Checks BSP node/subtree bounding box.
// Returns true
//  if some part of the bbox might be visible.
//
// modified to use local variables

static boolean HWR_CheckBBox(fixed_t *bspcoord)
{
	INT32 boxpos;
	fixed_t px1, py1, px2, py2;
	angle_t angle1, angle2;

	// Find the corners of the box
	// that define the edges from current viewpoint.
	if (dup_viewx <= bspcoord[BOXLEFT])
		boxpos = 0;
	else if (dup_viewx < bspcoord[BOXRIGHT])
		boxpos = 1;
	else
		boxpos = 2;

	if (dup_viewy >= bspcoord[BOXTOP])
		boxpos |= 0;
	else if (dup_viewy > bspcoord[BOXBOTTOM])
		boxpos |= 1<<2;
	else
		boxpos |= 2<<2;

	if (boxpos == 5)
		return true;

#ifdef RR3DS
	// (perf): discard BSP branches farther than RR3DS_WORLDDIST
	// (distance from the closest point of the box to the camera).
	{
		// configurable distance for measuring: sdmc:/3ds/RingRacers3ds/rr_dist = number (e.g. 8192). No file = 12288.
		static INT32 RR3DS_WORLDDIST = -1;
		if (RR3DS_WORLDDIST < 0)
		{
			RR3DS_WORLDDIST = 12288;
			{ int v = RRCfg_Int("dist", 0); if (v >= 1024 && v <= 32768) RR3DS_WORLDDIST = v; } // rr_config.txt
			RR_DIAGPRINT("[DIST] draw distance = %d\n", (int)RR3DS_WORLDDIST);
		}
		INT32 vx = dup_viewx >> FRACBITS, vy = dup_viewy >> FRACBITS;
		INT32 dx = 0, dy = 0;
		if (vx < (bspcoord[BOXLEFT] >> FRACBITS)) dx = (bspcoord[BOXLEFT] >> FRACBITS) - vx;
		else if (vx > (bspcoord[BOXRIGHT] >> FRACBITS)) dx = vx - (bspcoord[BOXRIGHT] >> FRACBITS);
		if (vy < (bspcoord[BOXBOTTOM] >> FRACBITS)) dy = (bspcoord[BOXBOTTOM] >> FRACBITS) - vy;
		else if (vy > (bspcoord[BOXTOP] >> FRACBITS)) dy = vy - (bspcoord[BOXTOP] >> FRACBITS);
		if (RRE_GLLOWEND && (INT64)dx * dx + (INT64)dy * dy > (INT64)RR3DS_WORLDDIST * RR3DS_WORLDDIST)
			return false;
	}
#endif

	px1 = bspcoord[checkcoord[boxpos][0]];
	py1 = bspcoord[checkcoord[boxpos][1]];
	px2 = bspcoord[checkcoord[boxpos][2]];
	py2 = bspcoord[checkcoord[boxpos][3]];

#ifdef RR3DS
	angle1 = RR3DS_AngF(FIXED_TO_FLOAT(px1), FIXED_TO_FLOAT(py1));
	angle2 = RR3DS_AngF(FIXED_TO_FLOAT(px2), FIXED_TO_FLOAT(py2));
#else
	angle1 = R_PointToAngle64(px1, py1);
	angle2 = R_PointToAngle64(px2, py2);
#endif
	return gld_clipper_SafeCheckRange(angle2, angle1);
}

//
// HWR_AddPolyObjectSegs
//
// haleyjd 02/19/06
// Adds all segs in all polyobjects in the given subsector.
// Modified for hardware rendering.
//
static inline void HWR_AddPolyObjectSegs(void)
{
	size_t i, j;
	seg_t *gl_fakeline = (seg_t *)Z_Calloc(sizeof(seg_t), PU_STATIC, NULL);
	polyvertex_t *pv1 = (polyvertex_t *)Z_Calloc(sizeof(polyvertex_t), PU_STATIC, NULL);
	polyvertex_t *pv2 = (polyvertex_t *)Z_Calloc(sizeof(polyvertex_t), PU_STATIC, NULL);

	// Sort through all the polyobjects
	for (i = 0; i < numpolys; ++i)
	{
		// Render the polyobject's lines
		for (j = 0; j < po_ptrs[i]->segCount; ++j)
		{
			// Copy the info of a polyobject's seg, then convert it to OpenGL floating point
			M_Memcpy(gl_fakeline, po_ptrs[i]->segs[j], sizeof(seg_t));

			// Now convert the line to float and add it to be rendered
			pv1->x = FIXED_TO_FLOAT(gl_fakeline->v1->x);
			pv1->y = FIXED_TO_FLOAT(gl_fakeline->v1->y);
			pv2->x = FIXED_TO_FLOAT(gl_fakeline->v2->x);
			pv2->y = FIXED_TO_FLOAT(gl_fakeline->v2->y);

			gl_fakeline->pv1 = pv1;
			gl_fakeline->pv2 = pv2;

			HWR_AddLine(gl_fakeline);
		}
	}

	// Free temporary data no longer needed
	Z_Free(pv2);
	Z_Free(pv1);
	Z_Free(gl_fakeline);
}

static void HWR_RenderPolyObjectPlane(polyobj_t *polysector, boolean isceiling, fixed_t fixedheight,
									FBITFIELD blendmode, UINT8 lightlevel, levelflat_t *levelflat, sector_t *FOFsector,
									UINT8 alpha, extracolormap_t *planecolormap)
{
	FSurfaceInfo Surf;
	FOutVector *v3d;
	INT32 shader = SHADER_DEFAULT;

	size_t nrPlaneVerts = polysector->numVertices;
	INT32 i;

	float height = FIXED_TO_FLOAT(fixedheight); // constant y for all points on the convex flat polygon
	float flatxref, flatyref;
	float fflatwidth = 64.0f, fflatheight = 64.0f;
	INT32 flatflag = 63;

	boolean texflat = false;

	float scrollx = 0.0f, scrolly = 0.0f;
	angle_t angle = 0;
	fixed_t tempxs, tempyt;

	static FOutVector *planeVerts = NULL;
	static UINT16 numAllocedPlaneVerts = 0;

	if (nrPlaneVerts < 3)   // Not even a triangle?
		return;
	else if (nrPlaneVerts > (size_t)UINT16_MAX) // FIXME: exceeds plVerts size
	{
		CONS_Debug(DBG_RENDER, "polygon size of %s exceeds max value of %d vertices\n", sizeu1(nrPlaneVerts), UINT16_MAX);
		return;
	}

	// Allocate plane-vertex buffer if we need to
	if (!planeVerts || nrPlaneVerts > numAllocedPlaneVerts)
	{
		numAllocedPlaneVerts = (UINT16)nrPlaneVerts;
		Z_Free(planeVerts);
		Z_Malloc(numAllocedPlaneVerts * sizeof (FOutVector), PU_LEVEL, &planeVerts);
	}

	// set texture for polygon
	if (levelflat != NULL)
	{
		if (levelflat->type == LEVELFLAT_FLAT)
		{
			size_t len = W_LumpLength(levelflat->u.flat.lumpnum);
			size_t sflatsize = R_FlatDimensionsFromLumpSize(len);
			fflatwidth = fflatheight = (double)sflatsize;
			flatflag = sflatsize-1;
		}
		else
		{
			if (levelflat->type == LEVELFLAT_TEXTURE)
			{
				fflatwidth = textures[levelflat->u.texture.num]->width;
				fflatheight = textures[levelflat->u.texture.num]->height;
			}
			else if (levelflat->type == LEVELFLAT_PATCH || levelflat->type == LEVELFLAT_PNG)
			{
				fflatwidth = levelflat->width;
				fflatheight = levelflat->height;
			}
			texflat = true;
		}
	}
	else // set no texture
		HWR_SetCurrentTexture(NULL);

	// reference point for flat texture coord for each vertex around the polygon
	flatxref = FIXED_TO_FLOAT(polysector->origVerts[0].x);
	flatyref = FIXED_TO_FLOAT(polysector->origVerts[0].y);

	flatxref = (float)(((fixed_t)flatxref & (~flatflag)) / fflatwidth);
	flatyref = (float)(((fixed_t)flatyref & (~flatflag)) / fflatheight);

	// transform
	v3d = planeVerts;

	if (FOFsector != NULL)
	{
		if (!isceiling) // it's a floor
		{
			scrollx = FIXED_TO_FLOAT(FOFsector->floor_xoffs)/fflatwidth;
			scrolly = FIXED_TO_FLOAT(FOFsector->floor_yoffs)/fflatheight;
			angle = FOFsector->floorpic_angle;
		}
		else // it's a ceiling
		{
			scrollx = FIXED_TO_FLOAT(FOFsector->ceiling_xoffs)/fflatwidth;
			scrolly = FIXED_TO_FLOAT(FOFsector->ceiling_yoffs)/fflatheight;
			angle = FOFsector->ceilingpic_angle;
		}
	}
	else if (gl_frontsector)
	{
		if (!isceiling) // it's a floor
		{
			scrollx = FIXED_TO_FLOAT(gl_frontsector->floor_xoffs)/fflatwidth;
			scrolly = FIXED_TO_FLOAT(gl_frontsector->floor_yoffs)/fflatheight;
			angle = gl_frontsector->floorpic_angle;
		}
		else // it's a ceiling
		{
			scrollx = FIXED_TO_FLOAT(gl_frontsector->ceiling_xoffs)/fflatwidth;
			scrolly = FIXED_TO_FLOAT(gl_frontsector->ceiling_yoffs)/fflatheight;
			angle = gl_frontsector->ceilingpic_angle;
		}
	}

	if (angle) // Only needs to be done if there's an altered angle
	{
		angle = (InvAngle(angle))>>ANGLETOFINESHIFT;

		// This needs to be done so that it scrolls in a different direction after rotation like software
		/*tempxs = FLOAT_TO_FIXED(scrollx);
		tempyt = FLOAT_TO_FIXED(scrolly);
		scrollx = (FIXED_TO_FLOAT(FixedMul(tempxs, FINECOSINE(angle)) - FixedMul(tempyt, FINESINE(angle))));
		scrolly = (FIXED_TO_FLOAT(FixedMul(tempxs, FINESINE(angle)) + FixedMul(tempyt, FINECOSINE(angle))));*/

		// This needs to be done so everything aligns after rotation
		// It would be done so that rotation is done, THEN the translation, but I couldn't get it to rotate AND scroll like software does
		tempxs = FLOAT_TO_FIXED(flatxref);
		tempyt = FLOAT_TO_FIXED(flatyref);
		flatxref = (FIXED_TO_FLOAT(FixedMul(tempxs, FINECOSINE(angle)) - FixedMul(tempyt, FINESINE(angle))));
		flatyref = (FIXED_TO_FLOAT(FixedMul(tempxs, FINESINE(angle)) + FixedMul(tempyt, FINECOSINE(angle))));
	}

	for (i = 0; i < (INT32)nrPlaneVerts; i++,v3d++)
	{
		// Go from the polysector's original vertex locations
		// Means the flat is offset based on the original vertex locations
		if (texflat)
		{
			v3d->s = (float)(FIXED_TO_FLOAT(polysector->origVerts[i].x) / fflatwidth) + scrollx;
			v3d->t = -(float)(FIXED_TO_FLOAT(polysector->origVerts[i].y) / fflatheight) + scrolly;
		}
		else
		{
			v3d->s = (float)((FIXED_TO_FLOAT(polysector->origVerts[i].x) / fflatwidth) - flatxref + scrollx);
			v3d->t = (float)(flatyref - (FIXED_TO_FLOAT(polysector->origVerts[i].y) / fflatheight) + scrolly);
		}

		// Need to rotate before translate
		if (angle) // Only needs to be done if there's an altered angle
		{
			tempxs = FLOAT_TO_FIXED(v3d->s);
			tempyt = FLOAT_TO_FIXED(v3d->t);
			v3d->s = (FIXED_TO_FLOAT(FixedMul(tempxs, FINECOSINE(angle)) - FixedMul(tempyt, FINESINE(angle))));
			v3d->t = (FIXED_TO_FLOAT(FixedMul(tempxs, FINESINE(angle)) + FixedMul(tempyt, FINECOSINE(angle))));
		}

		v3d->x = FIXED_TO_FLOAT(polysector->vertices[i]->x);
		v3d->y = height;
		v3d->z = FIXED_TO_FLOAT(polysector->vertices[i]->y);
	}

	HWR_Lighting(&Surf, lightlevel, planecolormap, P_SectorUsesDirectionalLighting((FOFsector != NULL) ? FOFsector : gl_frontsector));

	if (blendmode & PF_Translucent)
	{
		Surf.PolyColor.s.alpha = (UINT8)alpha;
		blendmode |= PF_Modulated|PF_Occlude;
	}
	else
		blendmode |= PF_Masked|PF_Modulated;

	if (HWR_UseShader())
	{
		shader = SHADER_FLOOR;
		blendmode |= PF_ColorMapped;
	}

	DBG3DS_TINT(&Surf, &blendmode, 255, 255, 0);
	HWR_ProcessPolygon(&Surf, planeVerts, nrPlaneVerts, blendmode, shader, false);
}

static void HWR_AddPolyObjectPlanes(void)
{
	size_t i;
	sector_t *polyobjsector;
	INT32 light = 0;

	// Polyobject Planes need their own function for drawing because they don't have extrasubsectors by themselves
	// It should be okay because polyobjects should always be convex anyway

	for (i  = 0; i < numpolys; i++)
	{
		polyobjsector = po_ptrs[i]->lines[0]->backsector; // the in-level polyobject sector

		if (!(po_ptrs[i]->flags & POF_RENDERPLANES)) // Only render planes when you should
			continue;

		if (po_ptrs[i]->translucency >= NUMTRANSMAPS)
			continue;

		if (polyobjsector->floorheight <= gl_frontsector->ceilingheight
			&& polyobjsector->floorheight >= gl_frontsector->floorheight
			&& (viewz < polyobjsector->floorheight))
		{
			light = R_GetPlaneLight(gl_frontsector, polyobjsector->floorheight, true);
			if (po_ptrs[i]->translucency > 0)
			{
				FSurfaceInfo Surf;
				FBITFIELD blendmode;
				memset(&Surf, 0x00, sizeof(Surf));
				blendmode = HWR_TranstableToAlpha(po_ptrs[i]->translucency, &Surf);
				HWR_AddTransparentPolyobjectFloor(&levelflats[polyobjsector->floorpic], po_ptrs[i], false, polyobjsector->floorheight,
													(light == -1 ? gl_frontsector->lightlevel : *gl_frontsector->lightlist[light].lightlevel), Surf.PolyColor.s.alpha, polyobjsector, blendmode, (light == -1 ? gl_frontsector->extra_colormap : *gl_frontsector->lightlist[light].extra_colormap));
			}
			else
			{
				HWR_GetLevelFlat(&levelflats[polyobjsector->floorpic], R_NoEncore(polyobjsector, &levelflats[polyobjsector->floorpic], false));
				HWR_RenderPolyObjectPlane(po_ptrs[i], false, polyobjsector->floorheight, PF_Occlude,
										(light == -1 ? gl_frontsector->lightlevel : *gl_frontsector->lightlist[light].lightlevel), &levelflats[polyobjsector->floorpic],
										polyobjsector, 255, (light == -1 ? gl_frontsector->extra_colormap : *gl_frontsector->lightlist[light].extra_colormap));
			}
		}

		if (polyobjsector->ceilingheight >= gl_frontsector->floorheight
			&& polyobjsector->ceilingheight <= gl_frontsector->ceilingheight
			&& (viewz > polyobjsector->ceilingheight))
		{
			light = R_GetPlaneLight(gl_frontsector, polyobjsector->ceilingheight, true);
			if (po_ptrs[i]->translucency > 0)
			{
				FSurfaceInfo Surf;
				FBITFIELD blendmode;
				memset(&Surf, 0x00, sizeof(Surf));
				blendmode = HWR_TranstableToAlpha(po_ptrs[i]->translucency, &Surf);
				HWR_AddTransparentPolyobjectFloor(&levelflats[polyobjsector->ceilingpic], po_ptrs[i], true, polyobjsector->ceilingheight,
				                                  (light == -1 ? gl_frontsector->lightlevel : *gl_frontsector->lightlist[light].lightlevel), Surf.PolyColor.s.alpha, polyobjsector, blendmode, (light == -1 ? gl_frontsector->extra_colormap : *gl_frontsector->lightlist[light].extra_colormap));
			}
			else
			{
				HWR_GetLevelFlat(&levelflats[polyobjsector->ceilingpic], R_NoEncore(polyobjsector, &levelflats[polyobjsector->ceilingpic], true));
				HWR_RenderPolyObjectPlane(po_ptrs[i], true, polyobjsector->ceilingheight, PF_Occlude,
				                          (light == -1 ? gl_frontsector->lightlevel : *gl_frontsector->lightlist[light].lightlevel), &levelflats[polyobjsector->ceilingpic],
				                          polyobjsector, 255, (light == -1 ? gl_frontsector->extra_colormap : *gl_frontsector->lightlist[light].extra_colormap));
			}
		}
	}
}

static FBITFIELD HWR_RippleBlend(sector_t *sector, ffloor_t *rover, boolean ceiling)
{
	return R_IsRipplePlane(sector, rover, ceiling) ? PF_Ripple : 0;
}

// -----------------+
// HWR_Subsector    : Determine floor/ceiling planes.
//                  : Add sprites of things in sector.
//                  : Draw one or more line segments.
// Notes            : Sets gl_cursectorlight to the light of the parent sector, to modulate wall textures
// -----------------+

#ifdef RR3DS
// ===========================================================================
// PRECOMPUTED VISIBILITY in-game - for now COUNT ONLY.
// Reads <MAP>.rrv (PC preparer) and measures how many zones the BSP draws are
// OUTSIDE the camera zone's list (= what would be saved by culling).
// [PVS] log every 90 frames. Does not change the rendering.
// ===========================================================================
namespace rr3ds_pvs {
	static std::vector<UINT8> blob;          // whole .rrv (compressed)
	static std::vector<UINT32> off;          // start of each zone in the blob
	static std::vector<UINT8> bits;          // current zone's list (1 bit per zone)
	static std::vector<UINT8> nodeBits;      // BSP nodes with at least one listed zone below
	static size_t curSub = (size_t)-1;
	static boolean ok = false, counting = false, cull = false;
	static UINT32 nDraw = 0, nOut = 0, nFrames = 0, nChanges = 0, nHigh = 0, nListSum = 0, nNoData = 0;
	static const fixed_t kHigh = 464 * FRACUNIT; // generator samples up to floor+400 (+64 margin)

	static void Load(UINT32 fingerprint, UINT32 geoFingerprint)
	{
		ok = false; cull = false; curSub = (size_t)-1;
		decltype(blob)().swap(blob); decltype(off)().swap(off);
		char path[200];
		snprintf(path, sizeof path, RR3DS_DIR "/cache/maps/%s.rrv", G_BuildMapName(gamemap));
		FILE *f = fopen(path, "rb");
		if (!f) { RR_DIAGPRINT("[PVS] no %s (regular drawing)\n", path); return; }
		fseek(f, 0, SEEK_END); const long sz = ftell(f); fseek(f, 0, SEEK_SET);
		if (sz > 20) { blob.resize((size_t)sz); if (fread(blob.data(), 1, (size_t)sz, f) != (size_t)sz) blob.clear(); }
		fclose(f);
		UINT32 h[5];
		if (blob.size() < 28) { RR_DIAGPRINT("[PVS] %s broken\n", path); return; }
		memcpy(h, blob.data(), 20);
		if (memcmp(blob.data(), "RRV1", 4) || !((h[1] == 2 && h[2] == fingerprint) || (h[1] == 3 && h[2] == geoFingerprint)) || h[3] != numsubsectors
			|| memcmp(blob.data() + blob.size() - 8, "RRVF", 4))
		{
			RR_DIAGPRINT("[PVS] %s does not match the map (fingerprint/version): not used | file: ver=%u fingerprint=%08x zones=%u | map: fingerprint=%08x zones=%u\n", path, (unsigned)h[1], (unsigned)h[2], (unsigned)h[3], (unsigned)fingerprint, (unsigned)numsubsectors);
			decltype(blob)().swap(blob); return;
		}
		off.resize(numsubsectors);
		size_t p = 20;
		for (size_t s = 0; s < numsubsectors; s++)
		{
			if (p + 2 > blob.size()) { RR_DIAGPRINT("[PVS] %s truncated at zone %u\n", path, (unsigned)s); decltype(blob)().swap(blob); return; }
			off[s] = (UINT32)p;
			UINT16 nr; memcpy(&nr, &blob[p], 2); p += 2;
			for (UINT32 r = 0; r < 2u * nr; r++) { while (p < blob.size() && (blob[p] & 0x80)) p++; p++; }
		}
		bits.assign((numsubsectors + 7) / 8, 0);
		nodeBits.assign((numnodes + 7) / 8, 0);
		ok = true;
#ifdef __3DS__
		// culling of zones outside the list (3DS only, drawing only). rr_config.txt: pvs_cull=0 turns it off.
		cull = RRCfg_Int("pvs_cull", 1) != 0;
#endif
		RR_DIAGPRINT("[PVS] loaded %s (%u KB) cull=%d\n", path, (unsigned)(blob.size() >> 10), (int)cull);
	}
	// marks the nodes that have a visible zone below them (the camera's included)
	static bool MarkNode(UINT32 n, int depth)
	{
		if (n & NF_SUBSECTOR)
		{
			const size_t z = n & ~NF_SUBSECTOR;
			return z < numsubsectors && (z == curSub || (bits[z >> 3] & (1 << (z & 7))));
		}
		if (n >= numnodes || depth > 200) return true;
		const bool a = MarkNode(nodes[n].children[0], depth + 1), b = MarkNode(nodes[n].children[1], depth + 1);
		if (a || b) nodeBits[n >> 3] |= (UINT8)(1 << (n & 7));
		return a || b;
	}
	static inline bool NodeSkip(UINT32 n)
	{
		return cull && counting && n < numnodes && !(nodeBits[n >> 3] & (1 << (n & 7)));
	}
	static void Decode(size_t s)
	{
		std::fill(bits.begin(), bits.end(), 0);
		size_t p = off[s]; UINT16 nr; memcpy(&nr, &blob[p], 2); p += 2;
		size_t pos = 0; UINT32 cnt = 0;
		auto rd = [&]() { UINT32 v = 0; int sh = 0; while (p < blob.size()) { const UINT8 b = blob[p++]; v |= (UINT32)(b & 0x7F) << sh; sh += 7; if (!(b & 0x80)) break; } return v; };
		for (UINT32 r = 0; r < nr; r++)
		{
			pos += rd(); UINT32 n = rd();
			for (; n && pos < numsubsectors; n--, pos++) { bits[pos >> 3] |= (UINT8)(1 << (pos & 7)); cnt++; }
		}
		nListSum += cnt;
		std::fill(nodeBits.begin(), nodeBits.end(), 0);
		if (numnodes) MarkNode((UINT32)numnodes - 1, 0);
	}
	// before the BSP of the main view
	static void Begin(void)
	{
		counting = false;
		if (!ok) { nNoData++; return; }
		subsector_t *ss = R_PointInSubsector(viewx, viewy);
		if (!ss) return;
		const size_t s = (size_t)(ss - subsectors);
		if (s >= numsubsectors) return;
		if (viewz - P_GetSectorFloorZAt(ss->sector, viewx, viewy) > kHigh) { nHigh++; return; }
		if (s != curSub) { curSub = s; Decode(s); nChanges++; }
		counting = true;
	}
	static inline void Note(size_t num)
	{
		if (!counting || num >= numsubsectors) return;
		nDraw++;
		if (!(bits[num >> 3] & (1 << (num & 7)))) nOut++;
	}
	// true = this zone is not visible from the camera zone: do not draw it (the camera's own zone is always drawn)
	static inline bool Skip(size_t num)
	{
		return cull && counting && num < numsubsectors && num != curSub && !(bits[num >> 3] & (1 << (num & 7)));
	}
	static void End(void)
	{
		counting = false;
		if (++nFrames < 90) return;
		if (ok)
			RR_DIAGPRINT("[PVS] 90 frames: zones drawn=%u/frame, OUTSIDE the list=%u/frame (%.0f%% savable) | mean list=%u zones (%.0f%% of the map) | zone changes=%u, high camera=%u\n",
				nDraw / 90, nOut / 90, nDraw ? 100.0 * nOut / nDraw : 0.0,
				nChanges ? nListSum / nChanges : 0, (nChanges && numsubsectors) ? 100.0 * (nListSum / nChanges) / numsubsectors : 0.0,
				nChanges, nHigh);
		nDraw = nOut = nFrames = nChanges = nHigh = nListSum = nNoData = 0;
	}
}
#endif

#ifdef RR3DS
namespace rr3ds_fast {
static inline const rr3ds_static::SItem *Hit(size_t item, UINT32 chg)
{
	const UINT32 r = rec[item];
	if ((r >> 30) != K_HIT || (r & kMask) < chg) return NULL;
	const rr3ds_static::SItem &it = rr3ds_static::items[item];
	return (it.state == rr3ds_static::ST_VALID && it.gen == rr3ds_static::gen) ? &it : NULL;
}
// true = zone drawn from the cache (the normal path is not needed)
static boolean Try(size_t num)
{
	using namespace rr3ds_static;
	if (on <= 0 || !enabled || capActive || rr3ds_gen::busy || !(rr3ds_pvs::cull && rr3ds_pvs::counting))
		return false;
	if (((mainFrame + (UINT32)num) & 15u) == 0) { nSlow++; return false; } // turn for a full check
	subsector_t *sub = &subsectors[num];
	sector_t *fs = sub->sector;
	if (fs->heightsec != -1 || fs->ffloors || fs->numlights || sub->polyList) { nSlow++; return false; }
	const UINT32 chgF = SecChg(fs);
	const SItem *fi = NULL, *ci = NULL;
	if (sub->validcount != validcount)
	{
		if (fs->floorpic != skyflatnum && (fs->f_slope ? P_GetSectorFloorZAt(fs, viewx, viewy) : fs->floorheight) < dup_viewz
			&& !(fi = Hit(num * 2, chgF))) { nSlow++; return false; }
		if (fs->ceilingpic != skyflatnum && (fs->c_slope ? P_GetSectorCeilingZAt(fs, viewx, viewy) : fs->ceilingheight) > dup_viewz
			&& !(ci = Hit(num * 2 + 1, chgF))) { nSlow++; return false; }
	}
	nFast++;
	const boolean med = rr3ds_sub::on && rr3ds_sub::view == 0;
	const UINT64 t0 = med ? I_GetPreciseTime() : 0;
	gl_frontsector = fs;
	if (fi) Emit(*fi);
	if (ci) Emit(*ci);
	if (fs->ceilingpic == skyflatnum || fs->floorpic == skyflatnum)
		drawsky = true;
	INT32 count = sub->numlines;
	seg_t *line = &segs[sub->firstline];
	if (count > 0)
	{
		if (med) { const UINT64 a0 = I_GetPreciseTime(); HWR_AddSprites(fs); tSpr += I_GetPreciseTime() - a0; }
		else HWR_AddSprites(fs);
		fs->validcount = validcount;
		const float vx = FIXED_TO_FLOAT(viewx), vy = FIXED_TO_FLOAT(viewy);
		for (; count--; line++)
		{
			if (line->glseg || line->polyseg) continue;
			const polyvertex_t *a = (polyvertex_t *)line->pv1, *b = (polyvertex_t *)line->pv2;
			if ((vy - a->y) * (b->x - a->x) > (b->y - a->y) * (vx - a->x))
				continue; // back face
			const size_t item = segBase + (size_t)(line - segs);
			const UINT32 r = rec[item];
			if (r >> 30)
			{
				UINT32 chg = chgF;
				if (line->backsector) { const UINT32 c = SecChg(line->backsector); if (c > chg) chg = c; }
				if ((r & kMask) >= chg)
				{
					if ((r >> 30) == K_EMPTY) continue;
					const SItem &it = items[item];
					if (it.state == ST_VALID && it.gen == gen
						&& !(it.hasOff && line->sidedef && it.to != line->sidedef->textureoffset))
					{ Emit(it); nEmit++; continue; }
				}
			}
			nSegSlow++;
			gl_frontsector = fs;
			if (med) { const UINT64 a0 = I_GetPreciseTime(); HWR_AddLine(line); tSegSlow += I_GetPreciseTime() - a0; }
			else HWR_AddLine(line); // wall with no data or changed: normal path for it only
		}
	}
	sub->validcount = validcount;
	if (med) tAll += I_GetPreciseTime() - t0;
	return true;
}
}
#endif

static void HWR_Subsector(size_t num)
{
	INT16 count;
	seg_t *line;
	subsector_t *sub;
	static sector_t tempsec; //SoM: 4/7/2000
	INT32 floorlightlevel;
	INT32 ceilinglightlevel;
	INT32 locFloorHeight, locCeilingHeight;
	INT32 cullFloorHeight, cullCeilingHeight;
	INT32 light = 0;
	extracolormap_t *floorcolormap;
	extracolormap_t *ceilingcolormap;
	ffloor_t *rover;

#ifdef PARANOIA //no risk while developing, enough debugging nights!
	if (num >= addsubsector)
		I_Error("HWR_Subsector: ss %s with numss = %s, addss = %s\n",
			sizeu1(num), sizeu2(numsubsectors), sizeu3(addsubsector));

	/*if (num >= numsubsectors)
		I_Error("HWR_Subsector: ss %i with numss = %i",
		        num,
		        numsubsectors);*/
#endif

#ifdef RR3DS
	if (rr3ds_sub::on) rr3ds_sub::nSubV[rr3ds_sub::view]++;
	rr3ds_pvs::Note(num); // (A0)
	if (rr3ds_pvs::Skip(num)) // precomputed visibility (.rrv)
		return;
	if (num < numsubsectors && rr3ds_fast::Try(num)) // fast path
		return;
#endif
	SUB_START();
	if (num < numsubsectors)
	{
		// subsector
		sub = &subsectors[num];
		// sector
		gl_frontsector = sub->sector;
		// how many linedefs
		count = sub->numlines;
		// first line seg
		line = &segs[sub->firstline];
	}
	else
	{
		// there are no segs but only planes
		sub = &subsectors[0];
		gl_frontsector = sub->sector;
		count = 0;
		line = NULL;
	}

#ifdef RR3DS
	rr3ds_seccache::E *rr_sc = NULL;
	if (gl_frontsector->heightsec == -1 && gl_frontsector >= sectors && (size_t)(gl_frontsector - sectors) < rr3ds_seccache::e.size())
	{
		rr_sc = &rr3ds_seccache::e[gl_frontsector - sectors];
		if (rr_sc->stamp == rr3ds_seccache::stamp)
		{
			floorlightlevel = rr_sc->fll; ceilinglightlevel = rr_sc->cll;
			floorcolormap = rr_sc->fcm; ceilingcolormap = rr_sc->ccm;
			cullFloorHeight = rr_sc->cullF; cullCeilingHeight = rr_sc->cullC;
			locFloorHeight = rr_sc->locF; locCeilingHeight = rr_sc->locC;
			goto rr_sc_done;
		}
	}
#endif
	//SoM: 4/7/2000: Test to make Boom water work in Hardware mode.
	gl_frontsector = R_FakeFlat(gl_frontsector, &tempsec, &floorlightlevel,
								&ceilinglightlevel, false);
	//FIXME: Use floorlightlevel and ceilinglightlevel insted of lightlevel.

	floorcolormap = ceilingcolormap = gl_frontsector->extra_colormap;

	// ------------------------------------------------------------------------
	// sector lighting, DISABLED because it's done in HWR_StoreWallRange
	// ------------------------------------------------------------------------
	/// \todo store a RGBA instead of just intensity, allow coloured sector lighting
	//light = (FUBYTE)(sub->sector->lightlevel & 0xFF) / 255.0f;
	//gl_cursectorlight.red   = light;
	//gl_cursectorlight.green = light;
	//gl_cursectorlight.blue  = light;
	//gl_cursectorlight.alpha = light;

// ----- end special tricks -----
	cullFloorHeight   = P_GetSectorFloorZAt  (gl_frontsector, viewx, viewy);
	cullCeilingHeight = P_GetSectorCeilingZAt(gl_frontsector, viewx, viewy);
	locFloorHeight    = P_GetSectorFloorZAt  (gl_frontsector, gl_frontsector->soundorg.x, gl_frontsector->soundorg.y);
	locCeilingHeight  = P_GetSectorCeilingZAt(gl_frontsector, gl_frontsector->soundorg.x, gl_frontsector->soundorg.y);

	if (gl_frontsector->ffloors)
	{
		boolean anyMoved = gl_frontsector->moved;

		if (anyMoved == false)
		{
			for (rover = gl_frontsector->ffloors; rover; rover = rover->next)
			{
				sector_t *controlSec = &sectors[rover->secnum];
				if (controlSec->moved == true)
				{
					anyMoved = true;
					break;
				}
			}
		}

		if (anyMoved == true)
		{
			gl_frontsector->numlights = sub->sector->numlights = 0;
			R_Prep3DFloors(gl_frontsector);
			sub->sector->lightlist = gl_frontsector->lightlist;
			sub->sector->numlights = gl_frontsector->numlights;
			sub->sector->moved = gl_frontsector->moved = false;
		}

		light = R_GetPlaneLight(gl_frontsector, locFloorHeight, false);
		if (gl_frontsector->floorlightsec == -1 && !gl_frontsector->floorlightabsolute)
			floorlightlevel = std::max<INT32>(0, std::min<INT32>(255, *gl_frontsector->lightlist[light].lightlevel + gl_frontsector->floorlightlevel));
		floorcolormap = *gl_frontsector->lightlist[light].extra_colormap;

		light = R_GetPlaneLight(gl_frontsector, locCeilingHeight, false);
		if (gl_frontsector->ceilinglightsec == -1 && !gl_frontsector->ceilinglightabsolute)
			ceilinglightlevel = std::max<INT32>(0, std::min<INT32>(255, *gl_frontsector->lightlist[light].lightlevel + gl_frontsector->ceilinglightlevel));
		ceilingcolormap = *gl_frontsector->lightlist[light].extra_colormap;
	}

#ifdef RR3DS
	if (rr_sc)
	{
		rr_sc->stamp = rr3ds_seccache::stamp;
		rr_sc->fll = floorlightlevel; rr_sc->cll = ceilinglightlevel;
		rr_sc->fcm = floorcolormap; rr_sc->ccm = ceilingcolormap;
		rr_sc->cullF = cullFloorHeight; rr_sc->cullC = cullCeilingHeight;
		rr_sc->locF = locFloorHeight; rr_sc->locC = locCeilingHeight;
	}
rr_sc_done:
#endif
	sub->sector->extra_colormap = gl_frontsector->extra_colormap;
	SUB_MARK(S_FAKE);

	// render floor ?
#ifdef DOPLANES
	// yeah, easy backface cull! :)
	if (cullFloorHeight < dup_viewz)
	{
		if (gl_frontsector->floorpic != skyflatnum)
		{
			if (sub->validcount != validcount)
			{
#ifdef RR3DS
				const fixed_t rr_fh = locFloorHeight == cullFloorHeight ? locFloorHeight : gl_frontsector->floorheight;
				const FBITFIELD rr_fl = PF_Occlude | HWR_RippleBlend(gl_frontsector, NULL, false);
				UINT32 rr_sig = 0;
				boolean rr_cached = false;
				if (num < numsubsectors && rr3ds_static::SectorOk(sub->sector))
				{
					using namespace rr3ds_static;
					if (!gl_frontsector->f_slope && !sub->sector->f_slope) // flat: height stored separately (patched)
					{
						rr_sig = H(H(H(SectorSigRaw(sub->sector, 0 | 4 | 16), rr_fl), (UINT32)floorlightlevel), (UINT32)(uintptr_t)floorcolormap);
						curSimple = true; curKind = 6; curSigH = H(2166136261u, (UINT32)rr_fh); curPlaneY = FIXED_TO_FLOAT(rr_fh);
					}
					else {
						rr_sig = H(H(H(H(SectorSigRaw(sub->sector, 0 | 16), gl_frontsector->f_slope ? 0 : (UINT32)rr_fh), rr_fl), (UINT32)floorlightlevel), (UINT32)(uintptr_t)floorcolormap); curSimple = false; curKind = 0; curSigH = 0; } // slope: clear cur
					rr_cached = Use(num * 2, rr_sig);
					if (rr_cached) rr3ds_fast::Set(num * 2, rr3ds_fast::K_HIT); else rr3ds_fast::Clr(num * 2);
					if (!rr_cached && Diag() && IsNeverChg(num * 2)) { Culp2((UINT32)(sub->sector - sectors), (UINT32)gl_frontsector->floorpic, 2);
						Culp3((size_t)num, sub->sector, (INT32)floorlightlevel, floorcolormap, (UINT32)rr_fl, rr_fh, rr_sig, curSigH); }
					if (rr_cached) { curSimple = false; curKind = 0; curSigH = 0; }
				}
				if (!rr_cached)
				{
#endif
				HWR_GetLevelFlat(&levelflats[gl_frontsector->floorpic], R_NoEncore(gl_frontsector, &levelflats[gl_frontsector->floorpic], false));
				HWR_RenderPlane(sub, &extrasubsectors[num], false,
					// Hack to make things continue to work around slopes.
					locFloorHeight == cullFloorHeight ? locFloorHeight : gl_frontsector->floorheight,
					// We now return you to your regularly scheduled rendering.
					PF_Occlude | HWR_RippleBlend(gl_frontsector, NULL, false),
					floorlightlevel, &levelflats[gl_frontsector->floorpic], NULL, 255, floorcolormap);
#ifdef RR3DS
				rr3ds_static::EndCapture();
				rr3ds_static::curSimple = false; rr3ds_static::curKind = 0; rr3ds_static::curSigH = 0;
				}
#endif
			}
		}
		else
		{
#ifdef POLYSKY
			HWR_RenderSkyPlane(&extrasubsectors[num], locFloorHeight);
#endif
		}
	}

	if (cullCeilingHeight > dup_viewz)
	{
		if (gl_frontsector->ceilingpic != skyflatnum)
		{
			if (sub->validcount != validcount)
			{
#ifdef RR3DS
				const fixed_t rr_ch = locCeilingHeight == cullCeilingHeight ? locCeilingHeight : gl_frontsector->ceilingheight;
				const FBITFIELD rr_cl = PF_Occlude | HWR_RippleBlend(gl_frontsector, NULL, true);
				UINT32 rr_sig = 0;
				boolean rr_cached = false;
				if (num < numsubsectors && rr3ds_static::SectorOk(sub->sector))
				{
					using namespace rr3ds_static;
					if (!gl_frontsector->c_slope && !sub->sector->c_slope) // flat: height stored separately (patched)
					{
						rr_sig = H(H(H(SectorSigRaw(sub->sector, 0 | 4 | 32), rr_cl), (UINT32)ceilinglightlevel), (UINT32)(uintptr_t)ceilingcolormap);
						curSimple = true; curKind = 6; curSigH = H(2166136261u, (UINT32)rr_ch); curPlaneY = FIXED_TO_FLOAT(rr_ch);
					}
					else {
						rr_sig = H(H(H(H(SectorSigRaw(sub->sector, 0 | 32), gl_frontsector->c_slope ? 0 : (UINT32)rr_ch), rr_cl), (UINT32)ceilinglightlevel), (UINT32)(uintptr_t)ceilingcolormap); curSimple = false; curKind = 0; curSigH = 0; } // slope: clear
					rr_cached = Use(num * 2 + 1, rr_sig);
					if (rr_cached) rr3ds_fast::Set(num * 2 + 1, rr3ds_fast::K_HIT); else rr3ds_fast::Clr(num * 2 + 1);
					if (!rr_cached && Diag() && IsNeverChg(num * 2 + 1)) Culp2((UINT32)(sub->sector - sectors), (UINT32)gl_frontsector->ceilingpic, 3);
					if (rr_cached) { curSimple = false; curKind = 0; curSigH = 0; }
				}
				if (!rr_cached)
				{
#endif
				HWR_GetLevelFlat(&levelflats[gl_frontsector->ceilingpic], R_NoEncore(gl_frontsector, &levelflats[gl_frontsector->ceilingpic], true));
				HWR_RenderPlane(sub, &extrasubsectors[num], true,
					// Hack to make things continue to work around slopes.
					locCeilingHeight == cullCeilingHeight ? locCeilingHeight : gl_frontsector->ceilingheight,
					// We now return you to your regularly scheduled rendering.
					PF_Occlude | HWR_RippleBlend(gl_frontsector, NULL, true),
					ceilinglightlevel, &levelflats[gl_frontsector->ceilingpic], NULL, 255, ceilingcolormap);
#ifdef RR3DS
				rr3ds_static::EndCapture();
				rr3ds_static::curSimple = false; rr3ds_static::curKind = 0; rr3ds_static::curSigH = 0;
				}
#endif
			}
		}
		else
		{
#ifdef POLYSKY
			HWR_RenderSkyPlane(&extrasubsectors[num], locCeilingHeight);
#endif
		}
	}

#ifndef POLYSKY
	// Moved here because before, when above the ceiling and the floor does not have the sky flat, it doesn't draw the sky
	if (gl_frontsector->ceilingpic == skyflatnum || gl_frontsector->floorpic == skyflatnum)
		drawsky = true;
#endif

#ifdef R_FAKEFLOORS
	if (gl_frontsector->ffloors)
	{
		/// \todo fix light, xoffs, yoffs, extracolormap ?
		for (rover = gl_frontsector->ffloors;
			rover; rover = rover->next)
		{
			fixed_t cullHeight, centerHeight;

            // bottom plane
			cullHeight   = P_GetFFloorBottomZAt(rover, viewx, viewy);
			centerHeight = P_GetFFloorBottomZAt(rover, gl_frontsector->soundorg.x, gl_frontsector->soundorg.y);

			if (!(rover->fofflags & FOF_EXISTS) || !(rover->fofflags & FOF_RENDERPLANES))
				continue;
			if (sub->validcount == validcount)
				continue;

			if (centerHeight <= locCeilingHeight &&
			    centerHeight >= locFloorHeight &&
			    ((dup_viewz < cullHeight && (rover->fofflags & FOF_BOTHPLANES || !(rover->fofflags & FOF_INVERTPLANES))) ||
			     (dup_viewz > cullHeight && (rover->fofflags & FOF_BOTHPLANES || rover->fofflags & FOF_INVERTPLANES))))
			{
				if (rover->fofflags & FOF_FOG)
				{
					UINT8 alpha;

					light = R_GetPlaneLight(gl_frontsector, centerHeight, dup_viewz < cullHeight ? true : false);
					alpha = HWR_FogBlockAlpha(*gl_frontsector->lightlist[light].lightlevel, rover->master->frontsector->extra_colormap);

					HWR_AddTransparentFloor(0,
					                       &extrasubsectors[num],
										   false,
					                       *rover->bottomheight,
					                       *gl_frontsector->lightlist[light].lightlevel,
					                       alpha, rover->master->frontsector, PF_Fog|PF_NoTexture,
										   true, rover->master->frontsector->extra_colormap);
				}
				else if (rover->fofflags & FOF_TRANSLUCENT
					&& (rover->alpha < 256 || rover->blend)) // SoM: Flags are more efficient
				{
					FBITFIELD blendmode = HWR_GetBlendModeFlag(rover->blend) | HWR_RippleBlend(gl_frontsector, rover, false);

					light = R_GetPlaneLight(gl_frontsector, centerHeight, dup_viewz < cullHeight ? true : false);

					HWR_AddTransparentFloor(&levelflats[*rover->bottompic],
					                       &extrasubsectors[num],
										   false,
					                       *rover->bottomheight,
					                       *gl_frontsector->lightlist[light].lightlevel,
										   std::max<INT32>(0, std::min<INT32>(rover->alpha, 255)), rover->master->frontsector, blendmode,
					                       false, *gl_frontsector->lightlist[light].extra_colormap);
				}
				else
				{
					HWR_GetLevelFlat(&levelflats[*rover->bottompic], R_NoEncore(gl_frontsector, &levelflats[*rover->bottompic], false));
					light = R_GetPlaneLight(gl_frontsector, centerHeight, dup_viewz < cullHeight ? true : false);
					HWR_RenderPlane(sub, &extrasubsectors[num], false, *rover->bottomheight, HWR_RippleBlend(gl_frontsector, rover, false) | PF_Occlude, *gl_frontsector->lightlist[light].lightlevel, &levelflats[*rover->bottompic],
					                rover->master->frontsector, 255, *gl_frontsector->lightlist[light].extra_colormap);
				}
			}

			// top plane
			cullHeight   = P_GetFFloorTopZAt(rover, viewx, viewy);
			centerHeight = P_GetFFloorTopZAt(rover, gl_frontsector->soundorg.x, gl_frontsector->soundorg.y);

			if (centerHeight >= locFloorHeight &&
			    centerHeight <= locCeilingHeight &&
			    ((dup_viewz > cullHeight && (rover->fofflags & FOF_BOTHPLANES || !(rover->fofflags & FOF_INVERTPLANES))) ||
			     (dup_viewz < cullHeight && (rover->fofflags & FOF_BOTHPLANES || rover->fofflags & FOF_INVERTPLANES))))
			{
				if (rover->fofflags & FOF_FOG)
				{
					UINT8 alpha;

					light = R_GetPlaneLight(gl_frontsector, centerHeight, dup_viewz < cullHeight ? true : false);
					alpha = HWR_FogBlockAlpha(*gl_frontsector->lightlist[light].lightlevel, rover->master->frontsector->extra_colormap);

					HWR_AddTransparentFloor(0,
					                       &extrasubsectors[num],
										   true,
					                       *rover->topheight,
					                       *gl_frontsector->lightlist[light].lightlevel,
					                       alpha, rover->master->frontsector, PF_Fog|PF_NoTexture,
										   true, rover->master->frontsector->extra_colormap);
				}
				else if (rover->fofflags & FOF_TRANSLUCENT
					&& (rover->alpha < 256 || rover->blend)) // SoM: Flags are more efficient
				{
					FBITFIELD blendmode = HWR_GetBlendModeFlag(rover->blend) | HWR_RippleBlend(gl_frontsector, rover, false);

					light = R_GetPlaneLight(gl_frontsector, centerHeight, dup_viewz < cullHeight ? true : false);

					HWR_AddTransparentFloor(&levelflats[*rover->toppic],
					                        &extrasubsectors[num],
											true,
					                        *rover->topheight,
					                        *gl_frontsector->lightlist[light].lightlevel,
											std::max<INT32>(0, std::min<INT32>(rover->alpha, 255)), rover->master->frontsector, blendmode,
					                        false, *gl_frontsector->lightlist[light].extra_colormap);
				}
				else
				{
					HWR_GetLevelFlat(&levelflats[*rover->toppic], R_NoEncore(gl_frontsector, &levelflats[*rover->toppic], true));
					light = R_GetPlaneLight(gl_frontsector, centerHeight, dup_viewz < cullHeight ? true : false);
					HWR_RenderPlane(sub, &extrasubsectors[num], true, *rover->topheight, HWR_RippleBlend(gl_frontsector, rover, true) | PF_Occlude, *gl_frontsector->lightlist[light].lightlevel, &levelflats[*rover->toppic],
					                  rover->master->frontsector, 255, *gl_frontsector->lightlist[light].extra_colormap);
				}
			}
		}
	}
#endif
#endif //doplanes
	SUB_MARK(S_PLANES);

	// Draw all the polyobjects in this subsector
	if (sub->polyList)
	{
		polyobj_t *po = sub->polyList;

		numpolys = 0;

		// Count all the polyobjects, reset the list, and recount them
		while (po)
		{
			++numpolys;
			po = (polyobj_t *)(po->link.next);
		}

		// for render stats
		ps_numpolyobjects += numpolys;

		// Sort polyobjects
		R_SortPolyObjects(sub);

		// Draw polyobject lines.
#ifdef RR3DS
		if (!rr3ds_gen::busy)
#endif
		HWR_AddPolyObjectSegs();

		if (sub->validcount != validcount) // This validcount situation seems to let us know that the floors have already been drawn.
		{
			// Draw polyobject planes
			HWR_AddPolyObjectPlanes();
		}
	}

// Hurder ici se passe les choses INT32�essantes!
// on vient de tracer le sol et le plafond
// on trace �pr�ent d'abord les sprites et ensuite les murs
// hurdler: faux: on ajoute seulement les sprites, le murs sont trac� d'abord
	SUB_MARK(S_POLY);
	if (line)
	{
		// draw sprites first, coz they are clipped to the solidsegs of
		// subsectors more 'in front'
#ifdef RR3DS
		if (!rr3ds_gen::busy) // generating the package: no sprites
#endif
		HWR_AddSprites(gl_frontsector);
		SUB_MARK(S_SPR);
#ifdef RR3DS
		if (rr3ds_sub::on) rr3ds_sub::nLinesV[rr3ds_sub::view] += count;
#endif

		//Hurdler: at this point validcount must be the same, but is not because
		//         gl_frontsector doesn't point anymore to sub->sector due to
		//         the call gl_frontsector = R_FakeFlat(...)
		//         if it's not done, the sprite is drawn more than once,
		//         what looks really bad with translucency or dynamic light,
		//         without talking about the overdraw of course.
		sub->sector->validcount = validcount;/// \todo fix that in a better way

		while (count--)
		{

			if (!line->glseg && !line->polyseg) // ignore segs that belong to polyobjects
				HWR_AddLine(line);
			line++;
		}
		SUB_MARK(S_LINES);
	}

	sub->validcount = validcount;
}

//
// Renders all subsectors below a given node,
//  traversing subtree recursively.
// Just call with BSP root.

#ifdef coolhack
//t;b;l;r
static fixed_t hackbbox[4];
//BOXTOP,
//BOXBOTTOM,
//BOXLEFT,
//BOXRIGHT
static boolean HWR_CheckHackBBox(fixed_t *bb)
{
	if (bb[BOXTOP] < hackbbox[BOXBOTTOM]) //y up
		return false;
	if (bb[BOXBOTTOM] > hackbbox[BOXTOP])
		return false;
	if (bb[BOXLEFT] > hackbbox[BOXRIGHT])
		return false;
	if (bb[BOXRIGHT] < hackbbox[BOXLEFT])
		return false;
	return true;
}
#endif

// BP: big hack for a test in lighning ref : 1249753487AB
fixed_t *hwbbox;

#ifdef RR3DS
// 3DS: in the skybox view we only walk the room's zone
// of the skybox (sectors connected to the viewpoint's), not the whole map.
namespace rr3ds_skycull {
	static boolean active = false;
	static sector_t *cachedSec = NULL;
	static boolean valid = false;
	static fixed_t box[4];
	static std::vector<UINT8> seen;
	static std::vector<sector_t*> stack;
	static void Build(sector_t *start)
	{
		cachedSec = start; valid = false;
		if (!start) return;
		seen.assign(numsectors, 0);
		stack.clear(); stack.push_back(start); seen[start - sectors] = 1;
		box[BOXTOP] = box[BOXRIGHT] = INT32_MIN; box[BOXBOTTOM] = box[BOXLEFT] = INT32_MAX;
		size_t count = 0;
		while (!stack.empty())
		{
			sector_t *sec = stack.back(); stack.pop_back(); count++;
			for (size_t i = 0; i < sec->linecount; i++)
			{
				line_t *ld = sec->lines[i];
				vertex_t *vs[2] = { ld->v1, ld->v2 };
				for (int k = 0; k < 2; k++)
				{
					if (vs[k]->y > box[BOXTOP]) box[BOXTOP] = vs[k]->y;
					if (vs[k]->y < box[BOXBOTTOM]) box[BOXBOTTOM] = vs[k]->y;
					if (vs[k]->x > box[BOXRIGHT]) box[BOXRIGHT] = vs[k]->x;
					if (vs[k]->x < box[BOXLEFT]) box[BOXLEFT] = vs[k]->x;
				}
				sector_t *o = (ld->frontsector == sec) ? ld->backsector : ld->frontsector;
				if (o && !seen[o - sectors]) { seen[o - sectors] = 1; stack.push_back(o); }
			}
		}
		// If the "room" is almost the entire map, we do not cull (safe).
		valid = (count < numsectors / 2) && box[BOXTOP] > box[BOXBOTTOM];
		RR_DIAGPRINT("[SKYCULL] sectors=%u of %u, cull=%s\n", (unsigned)count, (unsigned)numsectors, valid ? "yes" : "no");
	}
	static void Begin(player_t *player)
	{
		mobj_t *vp = player->skybox.viewpoint;
		sector_t *sec = (vp && vp->subsector) ? vp->subsector->sector : NULL;
		if (sec != cachedSec) Build(sec);
		active = valid;
	}
	static inline boolean Outside(const fixed_t *b)
	{
		return b[BOXLEFT] > box[BOXRIGHT] || b[BOXRIGHT] < box[BOXLEFT]
			|| b[BOXBOTTOM] > box[BOXTOP] || b[BOXTOP] < box[BOXBOTTOM];
	}
	void LevelReset(void) { cachedSec = NULL; valid = false; active = false; }
}
#endif


static void HWR_RenderBSPNode(INT32 bspnum)
{
	/*//GZDoom code
	if(bspnum == -1)
	{
		HWR_Subsector(subsectors);
		return;
	}
	while(!((size_t)bspnum&(~NF_SUBSECTOR))) // Keep going until found a subsector
	{
		node_t *bsp = &nodes[bspnum];

		// Decide which side the view point is on
		INT32 side = R_PointOnSide(dup_viewx, dup_viewy, bsp);

		// Recursively divide front space (toward the viewer)
		HWR_RenderBSPNode(bsp->children[side]);

		// Possibly divide back space (away from viewer)
		side ^= 1;

		if (!HWR_CheckBBox(bsp->bbox[side]))
			return;

		bspnum = bsp->children[side];
	}

	HWR_Subsector(bspnum-1);
*/
	node_t *bsp = &nodes[bspnum];

	// Decide which side the view point is on
	INT32 side;

#ifdef RR3DS
	if (!(bspnum & NF_SUBSECTOR) && rr3ds_pvs::NodeSkip((UINT32)bspnum)) // branch with no visible zones (.rrv)
		return;
#endif
	ps_numbspcalls++;

	// Found a subsector?
	if (bspnum & NF_SUBSECTOR)
	{
		if (bspnum == -1)
		{
			//*(gl_drawsubsector_p++) = 0;
			HWR_Subsector(0);
		}
		else
		{
			//*(gl_drawsubsector_p++) = bspnum&(~NF_SUBSECTOR);
			HWR_Subsector(bspnum&(~NF_SUBSECTOR));
		}
		return;
	}

	// Decide which side the view point is on.
	side = R_PointOnSide(dup_viewx, dup_viewy, bsp);

	// BP: big hack for a test in lighning ref : 1249753487AB
	hwbbox = bsp->bbox[side];

	// Recursively divide front space.
#ifdef RR3DS
	if (!rr3ds_skycull::active || !rr3ds_skycull::Outside(bsp->bbox[side]))
#endif
	HWR_RenderBSPNode(bsp->children[side]);

	// Possibly divide back space.
#ifdef RR3DS
	if (rr3ds_skycull::active && rr3ds_skycull::Outside(bsp->bbox[side^1]))
		return;
#endif
	if (HWR_CheckBBox(bsp->bbox[side^1]))
	{
		// BP: big hack for a test in lighning ref : 1249753487AB
		hwbbox = bsp->bbox[side^1];
		HWR_RenderBSPNode(bsp->children[side^1]);
	}
}

/*
//
// Clear 'stack' of subsectors to draw
//
static void HWR_ClearDrawSubsectors(void)
{
	gl_drawsubsector_p = gl_drawsubsectors;
}

//
// Draw subsectors pushed on the drawsubsectors 'stack', back to front
//
static void HWR_RenderSubsectors(void)
{
	while (gl_drawsubsector_p > gl_drawsubsectors)
	{
		HWR_RenderBSPNode(
		lastsubsec->nextsubsec = bspnum & (~NF_SUBSECTOR);
	}
}
*/

// ==========================================================================
//                                                              FROM R_MAIN.C
// ==========================================================================

//BP : exactely the same as R_InitTextureMapping
void HWR_InitTextureMapping(void)
{
	angle_t i;
	INT32 x;
	INT32 t;
	fixed_t focallength;
	fixed_t grcenterx;
	fixed_t grcenterxfrac;
	INT32 grviewwidth;

#define clipanglefov (FIELDOFVIEW>>ANGLETOFINESHIFT)

	grviewwidth = vid.width;
	grcenterx = grviewwidth/2;
	grcenterxfrac = grcenterx<<FRACBITS;

	// Use tangent table to generate viewangletox:
	//  viewangletox will give the next greatest x
	//  after the view angle.
	//
	// Calc focallength
	//  so FIELDOFVIEW angles covers SCREENWIDTH.
	focallength = FixedDiv(grcenterxfrac,
		FINETANGENT(FINEANGLES/4+clipanglefov/2));

	for (i = 0; i < FINEANGLES/2; i++)
	{
		if (FINETANGENT(i) > FRACUNIT*2)
			t = -1;
		else if (FINETANGENT(i) < -FRACUNIT*2)
			t = grviewwidth+1;
		else
		{
			t = FixedMul(FINETANGENT(i), focallength);
			t = (grcenterxfrac - t+FRACUNIT-1)>>FRACBITS;

			if (t < -1)
				t = -1;
			else if (t > grviewwidth+1)
				t = grviewwidth+1;
		}
		gl_viewangletox[i] = t;
	}

	// Scan viewangletox[] to generate xtoviewangle[]:
	//  xtoviewangle will give the smallest view angle
	//  that maps to x.
	for (x = 0; x <= grviewwidth; x++)
	{
		i = 0;
		while (gl_viewangletox[i]>x)
			i++;
		gl_xtoviewangle[x] = (i<<ANGLETOFINESHIFT) - ANGLE_90;
	}

	// Take out the fencepost cases from viewangletox.
	for (i = 0; i < FINEANGLES/2; i++)
	{
		if (gl_viewangletox[i] == -1)
			gl_viewangletox[i] = 0;
		else if (gl_viewangletox[i] == grviewwidth+1)
			gl_viewangletox[i]  = grviewwidth;
	}

	gl_clipangle = gl_xtoviewangle[0];
}

// ==========================================================================
// gl_things.c
// ==========================================================================

// sprites are drawn after all wall and planes are rendered, so that
// sprite translucency effects apply on the rendered view (instead of the background sky!!)

static UINT32 gl_visspritecount;
static gl_vissprite_t *gl_visspritechunks[MAXVISSPRITES >> VISSPRITECHUNKBITS] = {NULL};

// --------------------------------------------------------------------------
// HWR_ClearSprites
// Called at frame start.
// --------------------------------------------------------------------------
static void HWR_ClearSprites(void)
{
	gl_visspritecount = 0;
}

// --------------------------------------------------------------------------
// HWR_NewVisSprite
// --------------------------------------------------------------------------
static gl_vissprite_t gl_overflowsprite;

static gl_vissprite_t *HWR_GetVisSprite(UINT32 num)
{
		UINT32 chunk = num >> VISSPRITECHUNKBITS;

		// Allocate chunk if necessary
		if (!gl_visspritechunks[chunk])
			Z_Malloc(sizeof(gl_vissprite_t) * VISSPRITESPERCHUNK, PU_LEVEL, &gl_visspritechunks[chunk]);

		return gl_visspritechunks[chunk] + (num & VISSPRITEINDEXMASK);
}

static gl_vissprite_t *HWR_NewVisSprite(void)
{
	if (gl_visspritecount == MAXVISSPRITES)
		return &gl_overflowsprite;

	gl_vissprite_t *v = HWR_GetVisSprite(gl_visspritecount++);
	v->rr_roll = 0.0f;
	return v;
}

// A hack solution for transparent surfaces appearing on top of linkdraw sprites.
// Keep a list of linkdraw sprites and draw their shapes to the z-buffer after all other
// sprite drawing is done. (effectively the z-buffer drawing of linkdraw sprites is delayed)
// NOTE: This will no longer be necessary once full translucent sorting is implemented, where
// translucent sprites and surfaces are sorted together.

typedef struct
{
	FOutVector verts[4];
	gl_vissprite_t *spr;
} zbuffersprite_t;

// this list is used to store data about linkdraw sprites
zbuffersprite_t linkdrawlist[MAXVISSPRITES];
UINT32 linkdrawcount = 0;

// add the necessary data to the list for delayed z-buffer drawing
static void HWR_LinkDrawHackAdd(FOutVector *verts, gl_vissprite_t *spr)
{
	if (linkdrawcount < MAXVISSPRITES)
	{
		memcpy(linkdrawlist[linkdrawcount].verts, verts, sizeof(FOutVector) * 4);
		linkdrawlist[linkdrawcount].spr = spr;
		linkdrawcount++;
	}
}

// process and clear the list of sprites for delayed z-buffer drawing
static void HWR_LinkDrawHackFinish(void)
{
	UINT32 i;
	FSurfaceInfo surf;
	surf.PolyColor.rgba = 0xFFFFFFFF;
	surf.TintColor.rgba = 0xFFFFFFFF;
	surf.FadeColor.rgba = 0xFFFFFFFF;
	surf.LightInfo.light_level = 0;
	surf.LightInfo.fade_start = 0;
	surf.LightInfo.fade_end = 31;
	for (i = 0; i < linkdrawcount; i++)
	{
		// draw sprite shape, only to z-buffer
		HWR_GetPatch(linkdrawlist[i].spr->gpatch);
		HWR_ProcessPolygon(&surf, linkdrawlist[i].verts, 4, PF_Translucent|PF_Occlude|PF_Invisible, 0, false);
	}
	// reset list
	linkdrawcount = 0;
}

//
// HWR_DoCulling
// Hardware version of R_DoCulling
// (see r_main.c)
static boolean HWR_DoCulling(line_t *cullheight, line_t *viewcullheight, float vz, float bottomh, float toph)
{
	float cullplane;

	if (!cullheight)
		return false;

	cullplane = FIXED_TO_FLOAT(cullheight->frontsector->floorheight);
	if (cullheight->args[1]) // Group culling
	{
		if (!viewcullheight)
			return false;

		// Make sure this is part of the same group
		if (viewcullheight->frontsector == cullheight->frontsector)
		{
			// OK, we can cull
			if (vz > cullplane && toph < cullplane) // Cull if below plane
				return true;

			if (bottomh > cullplane && vz <= cullplane) // Cull if above plane
				return true;
		}
	}
	else // Quick culling
	{
		if (vz > cullplane && toph < cullplane) // Cull if below plane
			return true;

		if (bottomh > cullplane && vz <= cullplane) // Cull if above plane
			return true;
	}

	return false;
}

static void HWR_DrawDropShadow(mobj_t *thing, fixed_t scale)
{
	patch_t *gpatch;
	FOutVector shadowVerts[4];
	FSurfaceInfo sSurf;
	float fscale; float fx; float fy; float offset;
	float ph;
	extracolormap_t *colormap = NULL;
	FBITFIELD blendmode = PF_ReverseSubtract;
	INT32 shader = SHADER_DEFAULT;
	UINT8 i;
	SINT8 flip = P_MobjFlip(thing);

	INT32 light;
	fixed_t scalemul;
	fixed_t groundz;
	fixed_t slopez;
	pslope_t *groundslope;

	// uncapped/interpolation
	interpmobjstate_t interp = {0};

	if (R_UsingFrameInterpolation() && !paused)
	{
		R_InterpolateMobjState(thing, rendertimefrac, &interp);
	}
	else
	{
		R_InterpolateMobjState(thing, FRACUNIT, &interp);
	}

	// hitlag vibrating
	if (thing->hitlag > 0 && (thing->eflags & MFE_DAMAGEHITLAG))
	{
		fixed_t jitters = HITLAGJITTERS;
		if (R_UsingFrameInterpolation() && !paused)
			jitters += (rendertimefrac / HITLAGDIV);
		fixed_t mul = thing->hitlag * jitters;

		// perhaps there could be a way to interp this too?
		if (leveltime & 1)
		{
			mul = -mul;
		}

		interp.x += FixedMul(thing->momx, mul);
		interp.y += FixedMul(thing->momy, mul);
		interp.z += FixedMul(thing->momz, mul);
	}

	// sprite offset
	interp.x += thing->sprxoff;
	interp.y += thing->spryoff;
	interp.z += thing->sprzoff;

	groundz = R_GetShadowZ(thing, &groundslope);

	gpatch = (patch_t *)W_CachePatchName("DSHADOW", PU_SPRITE);
	if (!(gpatch && ((GLPatch_t *)gpatch->hardware)->mipmap->format)) return;
	HWR_GetPatch(gpatch);

	scalemul = FixedMul(scale, (thing->radius * 2) / gpatch->height);

	ph = (float)gpatch->height;

	fscale = FIXED_TO_FLOAT(scalemul);
	fx = FIXED_TO_FLOAT(interp.x);
	fy = FIXED_TO_FLOAT(interp.y);

	if (fscale > 0.0)
	{
		offset = (ph / 2) * fscale;
	}
	else
	{
		return;
	}

	//  3--2
	//  | /|
	//  |/ |
	//  0--1

	shadowVerts[2].x = shadowVerts[3].x = fx + offset;
	shadowVerts[1].x = shadowVerts[0].x = fx - offset;
	shadowVerts[1].z = shadowVerts[2].z = fy - offset;
	shadowVerts[0].z = shadowVerts[3].z = fy + offset;

	for (i = 0; i < 4; i++)
	{
		float oldx = shadowVerts[i].x;
		float oldy = shadowVerts[i].z;
		shadowVerts[i].x = fx + ((oldx - fx) * gl_viewcos) - ((oldy - fy) * gl_viewsin);
		shadowVerts[i].z = fy + ((oldx - fx) * gl_viewsin) + ((oldy - fy) * gl_viewcos);
	}

	if (groundslope)
	{
		for (i = 0; i < 4; i++)
		{
			slopez = P_GetSlopeZAt(groundslope, FLOAT_TO_FIXED(shadowVerts[i].x), FLOAT_TO_FIXED(shadowVerts[i].z));
			shadowVerts[i].y = FIXED_TO_FLOAT(slopez) + flip * 0.05f;
		}
	}
	else
	{
		for (i = 0; i < 4; i++)
			shadowVerts[i].y = FIXED_TO_FLOAT(groundz) + flip * 0.05f;
	}

	shadowVerts[0].s = shadowVerts[3].s = 0;
	shadowVerts[2].s = shadowVerts[1].s = ((GLPatch_t *)gpatch->hardware)->max_s;

	shadowVerts[3].t = shadowVerts[2].t = 0;
	shadowVerts[0].t = shadowVerts[1].t = ((GLPatch_t *)gpatch->hardware)->max_t;

	if (!(thing->renderflags & RF_NOCOLORMAPS))
	{
		if (thing->subsector->sector->numlights)
		{
			// Always use the light at the top instead of whatever I was doing before
			light = R_GetPlaneLight(thing->subsector->sector, groundz, false);

			if (*thing->subsector->sector->lightlist[light].extra_colormap)
				colormap = *thing->subsector->sector->lightlist[light].extra_colormap;
		}
		else if (thing->subsector->sector->extra_colormap)
			colormap = thing->subsector->sector->extra_colormap;
	}

	HWR_Lighting(&sSurf, 255, colormap, P_SectorUsesDirectionalLighting(thing->subsector->sector));
	sSurf.PolyColor.s.alpha = 255;

	if (thing->whiteshadow == true)
	{
		blendmode = PF_Additive;
	}

	if (HWR_UseShader())
	{
		shader = SHADER_SPRITE;
		blendmode |= PF_ColorMapped;
	}

	DBG3DS_TINT(&sSurf, &blendmode, 255, 128, 0);
	HWR_ProcessPolygon(&sSurf, shadowVerts, 4, blendmode|PF_Modulated, shader, false);
}

// This is expecting a pointer to an array containing 4 wallVerts for a sprite
static void HWR_RotateSpritePolyToAim(gl_vissprite_t *spr, FOutVector *wallVerts, const boolean precip)
{
	if (cv_glspritebillboarding.value
		&& spr && spr->mobj && !R_ThingIsPaperSprite(spr->mobj)
		&& wallVerts)
	{
		// uncapped/interpolation
		interpmobjstate_t interp = {0};

		// do interpolation
		if (R_UsingFrameInterpolation() && !paused)
		{
			if (spr->precip)
			{
				R_InterpolatePrecipMobjState((precipmobj_t *)spr->mobj, rendertimefrac, &interp);
			}
			else
			{
				R_InterpolateMobjState(spr->mobj, rendertimefrac, &interp);
			}
		}
		else
		{
			if (spr->precip)
			{
				R_InterpolatePrecipMobjState((precipmobj_t *)spr->mobj, FRACUNIT, &interp);
			}
			else
			{
				R_InterpolateMobjState(spr->mobj, FRACUNIT, &interp);
			}
		}

		float basey = FIXED_TO_FLOAT(interp.z);
		float lowy = wallVerts[0].y;
		if (!precip && P_MobjFlip(spr->mobj) == -1) // precip doesn't have eflags so they can't flip
		{
			basey = FIXED_TO_FLOAT(interp.z + spr->mobj->height);
		}
		// Rotate sprites to fully billboard with the camera
		// X, Y, AND Z need to be manipulated for the polys to rotate around the
		// origin, because of how the origin setting works I believe that should
		// be mobj->z or mobj->z + mobj->height
		wallVerts[2].y = wallVerts[3].y = (spr->gzt - basey) * gl_viewludsin + basey;
		wallVerts[0].y = wallVerts[1].y = (lowy - basey) * gl_viewludsin + basey;
		// translate back to be around 0 before translating back
		wallVerts[3].x += ((spr->gzt - basey) * gl_viewludcos) * gl_viewcos;
		wallVerts[2].x += ((spr->gzt - basey) * gl_viewludcos) * gl_viewcos;

		wallVerts[0].x += ((lowy - basey) * gl_viewludcos) * gl_viewcos;
		wallVerts[1].x += ((lowy - basey) * gl_viewludcos) * gl_viewcos;

		wallVerts[3].z += ((spr->gzt - basey) * gl_viewludcos) * gl_viewsin;
		wallVerts[2].z += ((spr->gzt - basey) * gl_viewludcos) * gl_viewsin;

		wallVerts[0].z += ((lowy - basey) * gl_viewludcos) * gl_viewsin;
		wallVerts[1].z += ((lowy - basey) * gl_viewludcos) * gl_viewsin;
	}
}

static void HWR_SplitSprite(gl_vissprite_t *spr)
{
	FOutVector wallVerts[4];
	FOutVector baseWallVerts[4]; // This is what the verts should end up as
	patch_t *gpatch;
	FSurfaceInfo Surf;
	extracolormap_t *colormap = NULL;
	INT32 lightlevel;
	boolean lightset = true;
	FBITFIELD blend = 0;
	FBITFIELD occlusion;
	INT32 shader = SHADER_DEFAULT;
	boolean use_linkdraw_hack = false;
	UINT8 alpha;

	INT32 i;
	float realtop, realbot, top, bot;
	float ttop, tbot, tmult;
	float bheight;
	float realheight, heightmult;
	const sector_t *sector = spr->mobj->subsector->sector;
	const lightlist_t *list = sector->lightlist;
	float endrealtop, endrealbot, endtop, endbot;
	float endbheight;
	float endrealheight;
	fixed_t temp;
	fixed_t v1x, v1y, v2x, v2y;

	gpatch = spr->gpatch;

	// cache the patch in the graphics card memory
	//12/12/99: Hurdler: same comment as above (for md2)
	//Hurdler: 25/04/2000: now support colormap in hardware mode
	rr3ds_sprmobj = spr->mobj; HWR_GetMappedPatch(gpatch, spr->colormap); rr3ds_sprmobj = NULL;

	baseWallVerts[0].x = baseWallVerts[3].x = spr->x1;
	baseWallVerts[2].x = baseWallVerts[1].x = spr->x2;
	baseWallVerts[0].z = baseWallVerts[3].z = spr->z1;
	baseWallVerts[1].z = baseWallVerts[2].z = spr->z2;

	baseWallVerts[2].y = baseWallVerts[3].y = spr->gzt;
	baseWallVerts[0].y = baseWallVerts[1].y = spr->gz;

	v1x = FLOAT_TO_FIXED(spr->x1);
	v1y = FLOAT_TO_FIXED(spr->z1);
	v2x = FLOAT_TO_FIXED(spr->x2);
	v2y = FLOAT_TO_FIXED(spr->z2);

	if (spr->flip)
	{
		baseWallVerts[0].s = baseWallVerts[3].s = ((GLPatch_t *)gpatch->hardware)->max_s;
		baseWallVerts[2].s = baseWallVerts[1].s = 0;
	}
	else
	{
		baseWallVerts[0].s = baseWallVerts[3].s = 0;
		baseWallVerts[2].s = baseWallVerts[1].s = ((GLPatch_t *)gpatch->hardware)->max_s;
	}

	// flip the texture coords (look familiar?)
	if (spr->vflip)
	{
		baseWallVerts[3].t = baseWallVerts[2].t = ((GLPatch_t *)gpatch->hardware)->max_t;
		baseWallVerts[0].t = baseWallVerts[1].t = 0;
	}
	else
	{
		baseWallVerts[3].t = baseWallVerts[2].t = 0;
		baseWallVerts[0].t = baseWallVerts[1].t = ((GLPatch_t *)gpatch->hardware)->max_t;
	}

	// if it has a dispoffset, push it a little towards the camera
	if (spr->dispoffset) {
		float co = -gl_viewcos*(0.05f*spr->dispoffset);
		float si = -gl_viewsin*(0.05f*spr->dispoffset);
		baseWallVerts[0].z = baseWallVerts[3].z = baseWallVerts[0].z+si;
		baseWallVerts[1].z = baseWallVerts[2].z = baseWallVerts[1].z+si;
		baseWallVerts[0].x = baseWallVerts[3].x = baseWallVerts[0].x+co;
		baseWallVerts[1].x = baseWallVerts[2].x = baseWallVerts[1].x+co;
	}

	// Let dispoffset work first since this adjust each vertex
	HWR_RotateSpritePolyToAim(spr, baseWallVerts, false);

	realtop = top = baseWallVerts[3].y;
	realbot = bot = baseWallVerts[0].y;
	ttop = baseWallVerts[3].t;
	tbot = baseWallVerts[0].t;
	tmult = (tbot - ttop) / (top - bot);

	endrealtop = endtop = baseWallVerts[2].y;
	endrealbot = endbot = baseWallVerts[1].y;

	// copy the contents of baseWallVerts into the drawn wallVerts array
	// baseWallVerts is used to know the final shape to easily get the vertex
	// co-ordinates
	memcpy(wallVerts, baseWallVerts, sizeof(baseWallVerts));

	// if sprite has linkdraw, then dont write to z-buffer (by not using PF_Occlude)
	// this will result in sprites drawn afterwards to be drawn on top like intended when using linkdraw.
	if ((spr->mobj->flags2 & MF2_LINKDRAW) && spr->mobj->tracer)
		occlusion = 0;
	else
		occlusion = PF_Occlude;

	// Determine the blendmode and translucency value
	{
		UINT32 blendmode, trans;
		if (spr->mobj->renderflags & RF_BLENDMASK)
			blendmode = (spr->mobj->renderflags & RF_BLENDMASK) >> RF_BLENDSHIFT;
		else
			blendmode = (spr->mobj->frame & FF_BLENDMASK) >> FF_BLENDSHIFT;
		if (blendmode)
			blendmode++; // realign to constants

		if (spr->mobj->renderflags & RF_TRANSMASK)
			trans = (spr->mobj->renderflags & RF_TRANSMASK) >> RF_TRANSSHIFT;
		else
			trans = (spr->mobj->frame & FF_TRANSMASK) >> FF_TRANSSHIFT;
		if (trans >= NUMTRANSMAPS)
			return; // cap

		blend = HWR_SurfaceBlend(blendmode, trans, &Surf);

		// if sprite has PF_ALWAYSONTOP, draw on top of everything.
		if (cv_debugrender_spriteclip.value || spr->mobj->renderflags & RF_ALWAYSONTOP)
			blend |= PF_NoDepthTest;

		if (!trans && !blendmode)
		{
			// BP: i agree that is little better in environement but it don't
			//     work properly under glide nor with fogcolor to ffffff :(
			// Hurdler: PF_Environement would be cool, but we need to fix
			//          the issue with the fog before
			blend |= occlusion;
			if (!occlusion) use_linkdraw_hack = true;
		}
	}

	if (HWR_UseShader())
	{
		shader = (R_ThingIsPaperSprite(spr->mobj) || R_ThingIsFloorSprite(spr->mobj)) ? SHADER_SPRITE : SHADER_SPRITECLIPHACK;
		blend |= PF_ColorMapped;
	}

	alpha = Surf.PolyColor.s.alpha;

	// Start with the lightlevel and colormap from the top of the sprite
	lightlevel = *list[sector->numlights - 1].lightlevel;
	if (!R_ThingIsFullBright(spr->mobj) && !(spr->mobj->renderflags & RF_NOCOLORMAPS))
		colormap = *list[sector->numlights - 1].extra_colormap;

	i = 0;
	temp = FLOAT_TO_FIXED(realtop);

	lightset = HWR_OverrideObjectLightLevel(spr->mobj, &lightlevel);

	for (i = 1; i < sector->numlights; i++)
	{
		fixed_t h = P_GetLightZAt(&sector->lightlist[i], spr->mobj->x, spr->mobj->y);
		if (h <= temp)
		{
			if (!lightset)
				lightlevel = *list[i-1].lightlevel > 255 ? 255 : *list[i-1].lightlevel;
			if (!R_ThingIsFullBright(spr->mobj) && !(spr->mobj->renderflags & RF_NOCOLORMAPS))
				colormap = *list[i-1].extra_colormap;
			break;
		}
	}

	if (!lightset)
		HWR_ObjectLightLevelPost(spr, sector, &lightlevel, false);

	for (i = 0; i < sector->numlights; i++)
	{
		if (endtop < endrealbot && top < realbot)
			return;

		// even if we aren't changing colormap or lightlevel, we still need to continue drawing down the sprite
		if (!(list[i].flags & FOF_NOSHADE) && (list[i].flags & FOF_CUTSPRITES))
		{
			if (!lightset)
			{
				lightlevel = *list[i].lightlevel > 255 ? 255 : *list[i].lightlevel;
				HWR_ObjectLightLevelPost(spr, sector, &lightlevel, false);
			}

			if (!R_ThingIsFullBright(spr->mobj) && !(spr->mobj->renderflags & RF_NOCOLORMAPS))
				colormap = *list[i].extra_colormap;
		}

		if (i + 1 < sector->numlights)
		{
			temp = P_GetLightZAt(&list[i+1], v1x, v1y);
			bheight = FIXED_TO_FLOAT(temp);
			temp = P_GetLightZAt(&list[i+1], v2x, v2y);
			endbheight = FIXED_TO_FLOAT(temp);
		}
		else
		{
			bheight = realbot;
			endbheight = endrealbot;
		}

		if (endbheight >= endtop && bheight >= top)
			continue;

		bot = bheight;

		if (bot < realbot)
			bot = realbot;

		endbot = endbheight;

		if (endbot < endrealbot)
			endbot = endrealbot;

		wallVerts[3].t = ttop + ((realtop - top) * tmult);
		wallVerts[2].t = ttop + ((endrealtop - endtop) * tmult);
		wallVerts[0].t = ttop + ((realtop - bot) * tmult);
		wallVerts[1].t = ttop + ((endrealtop - endbot) * tmult);

		wallVerts[3].y = top;
		wallVerts[2].y = endtop;
		wallVerts[0].y = bot;
		wallVerts[1].y = endbot;

		// The x and y only need to be adjusted in the case that it's not a papersprite
		if (cv_glspritebillboarding.value
			&& spr->mobj && !R_ThingIsPaperSprite(spr->mobj))
		{
			// Get the x and z of the vertices so billboarding draws correctly
			realheight = realbot - realtop;
			endrealheight = endrealbot - endrealtop;
			heightmult = (realtop - top) / realheight;
			wallVerts[3].x = baseWallVerts[3].x + (baseWallVerts[3].x - baseWallVerts[0].x) * heightmult;
			wallVerts[3].z = baseWallVerts[3].z + (baseWallVerts[3].z - baseWallVerts[0].z) * heightmult;

			heightmult = (endrealtop - endtop) / endrealheight;
			wallVerts[2].x = baseWallVerts[2].x + (baseWallVerts[2].x - baseWallVerts[1].x) * heightmult;
			wallVerts[2].z = baseWallVerts[2].z + (baseWallVerts[2].z - baseWallVerts[1].z) * heightmult;

			heightmult = (realtop - bot) / realheight;
			wallVerts[0].x = baseWallVerts[3].x + (baseWallVerts[3].x - baseWallVerts[0].x) * heightmult;
			wallVerts[0].z = baseWallVerts[3].z + (baseWallVerts[3].z - baseWallVerts[0].z) * heightmult;

			heightmult = (endrealtop - endbot) / endrealheight;
			wallVerts[1].x = baseWallVerts[2].x + (baseWallVerts[2].x - baseWallVerts[1].x) * heightmult;
			wallVerts[1].z = baseWallVerts[2].z + (baseWallVerts[2].z - baseWallVerts[1].z) * heightmult;
		}

		HWR_Lighting(&Surf, lightlevel, colormap, P_SectorUsesDirectionalLighting(sector) && !R_ThingIsFullBright(spr->mobj));

		Surf.PolyColor.s.alpha = alpha;

		{ DBG3DS_TINT(&Surf, NULL, 0, 0, 255); HWR_ProcessPolygon(&Surf, wallVerts, 4, blend|PF_Modulated, shader, false); }

		if (use_linkdraw_hack)
			HWR_LinkDrawHackAdd(wallVerts, spr);

		top = bot;
		endtop = endbot;
	}

	bot = realbot;
	endbot = endrealbot;
	if (endtop <= endrealbot && top <= realbot)
		return;

	// If we're ever down here, somehow the above loop hasn't draw all the light levels of sprite
	wallVerts[3].t = ttop + ((realtop - top) * tmult);
	wallVerts[2].t = ttop + ((endrealtop - endtop) * tmult);
	wallVerts[0].t = ttop + ((realtop - bot) * tmult);
	wallVerts[1].t = ttop + ((endrealtop - endbot) * tmult);

	wallVerts[3].y = top;
	wallVerts[2].y = endtop;
	wallVerts[0].y = bot;
	wallVerts[1].y = endbot;

	HWR_Lighting(&Surf, lightlevel, colormap, P_SectorUsesDirectionalLighting(sector));

	Surf.PolyColor.s.alpha = alpha;

	{ DBG3DS_TINT(&Surf, NULL, 0, 0, 255); HWR_ProcessPolygon(&Surf, wallVerts, 4, blend|PF_Modulated, shader, false); }

	if (use_linkdraw_hack)
		HWR_LinkDrawHackAdd(wallVerts, spr);
}

static void HWR_DrawBoundingBox(gl_vissprite_t *vis)
{
	FOutVector v[24];
	FSurfaceInfo Surf = {0};

	//
	// create a cube (side view)
	//
	//  5--4  3
	//        |
	//        |
	//  0--1  2
	//
	// repeat this 4 times (overhead)
	//
	//
	// 17    20  21    11
	//    16 15  14 10
	// 27 22  *--*  07 12
	//        |  |
	// 26 23  *--*  06 13
	//    24 00  01 02
	// 25    05  04    03
	//

	v[000].x = v[005].x = v[015].x = v[016].x = v[017].x = v[020].x =
		v[022].x = v[023].x = v[024].x = v[025].x = v[026].x = v[027].x = vis->x1; // west

	v[001].x = v[002].x = v[003].x = v[004].x = v[006].x = v[007].x =
		v[010].x = v[011].x = v[012].x = v[013].x = v[014].x = v[021].x = vis->x2; // east

	v[000].z = v[001].z = v[002].z = v[003].z = v[004].z = v[005].z =
		v[006].z = v[013].z = v[023].z = v[024].z = v[025].z = v[026].z = vis->z1; // south

	v[007].z = v[010].z = v[011].z = v[012].z = v[014].z = v[015].z =
		v[016].z = v[017].z = v[020].z = v[021].z = v[022].z = v[027].z = vis->z2; // north

	v[000].y = v[001].y = v[002].y = v[006].y = v[007].y = v[010].y =
		v[014].y = v[015].y = v[016].y = v[022].y = v[023].y = v[024].y = vis->gz; // bottom

	v[003].y = v[004].y = v[005].y = v[011].y = v[012].y = v[013].y =
		v[017].y = v[020].y = v[021].y = v[025].y = v[026].y = v[027].y = vis->gzt; // top

	Surf.PolyColor = V_GetColor(R_GetBoundingBoxColor(vis->mobj));

	HWR_ProcessPolygon(&Surf, v, 24, PF_Modulated|PF_NoTexture|PF_WireFrame, SHADER_NONE, false);
}

// -----------------+
// HWR_DrawSprite   : Draw flat sprites
//                  : (monsters, bonuses, weapons, lights, ...)
// Returns          :
// -----------------+
#ifdef __3DS__
// cap on NEW KART sprites (build+upload ~4 ms each) per view. If exceeded,
// the kart (which is not the camera's) is drawn with its previous texture for one more frame.
// rr_config: kart_uploads=N (per view; 0 = no cap). Default 2.
extern "C++" boolean HWR_MappedPatchReady(patch_t *patch, const UINT8 *colormap);
static int rr3ds_kbLeft = 0;
static struct { mobj_t *m; patch_t *p; const UINT8 *cm; } rr3ds_kb[16];
static int rr3ds_kbCfg = -1;
UINT32 rr3ds_kbReused = 0, rr3ds_kbUploads = 0;
static void RR3DS_KartBudgetReset(void)
{
	if (rr3ds_kbCfg < 0) rr3ds_kbCfg = RRCfg_Int("kart_uploads", 2);
	rr3ds_kbLeft = rr3ds_kbCfg;
}
static patch_t *RR3DS_KartBudget(gl_vissprite_t *spr, patch_t *gpatch)
{
	mobj_t *mo = spr->mobj;
	if (rr3ds_kbCfg <= 0 || !mo || mo->type != MT_PLAYER)
		return gpatch;
	int slot = -1, freeSlot = -1;
	for (int i = 0; i < 16; i++)
	{
		if (rr3ds_kb[i].m == mo) { slot = i; break; }
		if (!rr3ds_kb[i].m && freeSlot < 0) freeSlot = i;
	}
	const bool camera = (mo == players[displayplayers[0]].mo);
	if (HWR_MappedPatchReady(gpatch, spr->colormap) || camera || rr3ds_kbLeft > 0)
	{
		if (!camera && !HWR_MappedPatchReady(gpatch, spr->colormap)) { rr3ds_kbLeft--; rr3ds_kbUploads++; }
		if (slot < 0) slot = (freeSlot >= 0) ? freeSlot : (int)(((uintptr_t)mo >> 4) & 15);
		rr3ds_kb[slot].m = mo; rr3ds_kb[slot].p = gpatch; rr3ds_kb[slot].cm = spr->colormap;
		return gpatch;
	}
	if (slot >= 0 && rr3ds_kb[slot].cm == spr->colormap && HWR_MappedPatchReady(rr3ds_kb[slot].p, spr->colormap))
	{
		rr3ds_kbReused++;
		return rr3ds_kb[slot].p; // no upload: previous animation frame
	}
	return gpatch; // no previous one ready: upload anyway
}
#endif
// (PC): floor sprites (boost panels, etc.) in Legacy GL.
// Software draws them without a z-buffer and they are ALWAYS visible; in GL the floor covered them when the
// sprite ended up slightly below. Real case (RR_RobotnikCoaster): panel on floor 394 with
// the slope of the NEIGHBORING sector (414) -> part of the panel below the floor.
// Moves the point up to the real floor (or down to the ceiling if upside down). Only differences < 128 units.
// Returns true if it moved it.
static boolean HWR_SplatClampY(FOutVector *v, mobj_t *mo, float zoffset)
{
	const float maxfix = 128.0f * FIXED_TO_FLOAT(mapobjectscale);
	const fixed_t vx = FLOAT_TO_FIXED(v->x), vy = FLOAT_TO_FIXED(v->z);
	const sector_t *vsec = R_PointInSubsector(vx, vy)->sector;
	if (!(mo->eflags & MFE_VERTICALFLIP))
	{
		const float fz = FIXED_TO_FLOAT(P_GetSectorFloorZAt(vsec, vx, vy)) + zoffset;
		if (v->y < fz && fz - v->y < maxfix)
		{
			v->y = fz;
			return true;
		}
	}
	else
	{
		const float cz = FIXED_TO_FLOAT(P_GetSectorCeilingZAt(vsec, vx, vy)) + zoffset;
		if (v->y > cz && v->y - cz < maxfix)
		{
			v->y = cz;
			return true;
		}
	}
	return false;
}

// (PC): if the floor under the sprite BENDS (two sectors with different slopes), a single
// flat quad cuts through it -> flicker (z-fighting). It is drawn as an NxN grid and each
// point follows the real floor. Returns false if not needed (the normal quad is drawn).
// Not on 3DS (cost: (N+1)^2 subsector lookups per floor sprite).
static boolean HWR_SplatFollowFloor(FSurfaceInfo *surf, const FOutVector *q, mobj_t *mo, float zoffset, FBITFIELD blend, INT32 shader)
{
#ifdef __3DS__
	(void)surf; (void)q; (void)mo; (void)zoffset; (void)blend; (void)shader;
	return false;
#else
	enum { N = 8 };
	FOutVector g[N+1][N+1];
	boolean moved = false;
	INT32 a, b;

	// q: 3--2 / 0--1 ; u goes from 0 to 1, w goes from 0 to 3
	for (a = 0; a <= N; a++)
	{
		const float u = (float)a / N;
		for (b = 0; b <= N; b++)
		{
			const float w = (float)b / N;
			const float k0 = (1.0f - u) * (1.0f - w), k1 = u * (1.0f - w), k2 = u * w, k3 = (1.0f - u) * w;
			FOutVector *p = &g[a][b];
			memset(p, 0, sizeof(*p));
			p->x = k0 * q[0].x + k1 * q[1].x + k2 * q[2].x + k3 * q[3].x;
			p->y = k0 * q[0].y + k1 * q[1].y + k2 * q[2].y + k3 * q[3].y;
			p->z = k0 * q[0].z + k1 * q[1].z + k2 * q[2].z + k3 * q[3].z;
			p->s = k0 * q[0].s + k1 * q[1].s + k2 * q[2].s + k3 * q[3].s;
			p->t = k0 * q[0].t + k1 * q[1].t + k2 * q[2].t + k3 * q[3].t;
			if (HWR_SplatClampY(p, mo, zoffset))
				moved = true;
		}
	}

	if (!moved)
		return false;

	for (a = 0; a < N; a++)
	{
		for (b = 0; b < N; b++)
		{
			FOutVector cell[4];
			cell[0] = g[a][b];
			cell[1] = g[a+1][b];
			cell[2] = g[a+1][b+1];
			cell[3] = g[a][b+1];
			HWR_ProcessPolygon(surf, cell, 4, blend, shader, false);
		}
	}
	return true;
#endif
}

static void HWR_DrawSprite(gl_vissprite_t *spr)
{
	FOutVector wallVerts[4];
	patch_t *gpatch;
	FSurfaceInfo Surf;
	const boolean splat = R_ThingIsFloorSprite(spr->mobj);
	// (PC): floor sprite with its OWN slope (not the floor's), defined in the
	// object (floorspriteslope): it is tilted on purpose. Do not snap it to the real floor point by point.
	boolean splatlibre = false;

	if (!spr->mobj)
		return;

	if (!spr->mobj->subsector)
		return;

	if (spr->mobj->subsector->sector->numlights
		&& (spr->mobj->renderflags & RF_ABSOLUTELIGHTLEVEL) == 0
		&& !splat)
	{
		HWR_SplitSprite(spr);
		return;
	}

	// cache sprite graphics
	//12/12/99: Hurdler:
	//          OK, I don't change anything for MD2 support because I want to be
	//          sure to do it the right way. So actually, we keep normal sprite
	//          in memory and we add the md2 model if it exists for that sprite

	gpatch = spr->gpatch;

#ifdef ALAM_LIGHTING
	if (!(spr->mobj->flags2 & MF2_DEBRIS) && (spr->mobj->sprite != SPR_PLAY))
		HWR_DL_AddLight(spr, gpatch);
#endif

	// create the sprite billboard
	//
	//  3--2
	//  | /|
	//  |/ |
	//  0--1

	if (splat)
	{
		F2DCoord verts[4];
		F2DCoord rotated[4];

		angle_t angle;
		float ca, sa;
		float w, h;
		float xscale, yscale;
		float xoffset, yoffset;
		float leftoffset, topoffset;
		float scale = spr->scale;
		float zoffset = (P_MobjFlip(spr->mobj) * 0.05f);
		pslope_t *splatslope = NULL;
		INT32 i;

		renderflags_t renderflags = spr->renderflags;
		if (renderflags & RF_SHADOWEFFECTS)
			scale *= spr->shadowscale;

		if (spr->rotateflags & SRF_3D || renderflags & RF_NOSPLATBILLBOARD)
			angle = spr->angle;
		else
			angle = viewangle;

		if (!spr->rotated)
			angle += spr->mobj->rollangle;

		angle = -angle;
		angle += ANGLE_90;

		topoffset = spr->spriteyoffset;
		leftoffset = spr->spritexoffset;
		if (spr->flip)
			leftoffset = ((float)gpatch->width - leftoffset);

		xscale = spr->scale * spr->spritexscale;
		yscale = spr->scale * spr->spriteyscale;

		xoffset = leftoffset * xscale;
		yoffset = topoffset * yscale;

		w = (float)gpatch->width * xscale;
		h = (float)gpatch->height * yscale;

		// Set positions

		// 3--2
		// |  |
		// 0--1

		verts[3].x = -xoffset;
		verts[3].y = yoffset;

		verts[2].x = w - xoffset;
		verts[2].y = yoffset;

		verts[1].x = w - xoffset;
		verts[1].y = -h + yoffset;

		verts[0].x = -xoffset;
		verts[0].y = -h + yoffset;

		ca = FIXED_TO_FLOAT(FINECOSINE((-angle)>>ANGLETOFINESHIFT));
		sa = FIXED_TO_FLOAT(FINESINE((-angle)>>ANGLETOFINESHIFT));

		// Rotate
		for (i = 0; i < 4; i++)
		{
			rotated[i].x = (verts[i].x * ca) - (verts[i].y * sa);
			rotated[i].y = (verts[i].x * sa) + (verts[i].y * ca);
		}

		// Translate
		for (i = 0; i < 4; i++)
		{
			wallVerts[i].x = rotated[i].x + spr->x1;
			wallVerts[i].z = rotated[i].y + spr->z1;
		}

		if (renderflags & (RF_SLOPESPLAT | RF_OBJECTSLOPESPLAT))
		{
			pslope_t *standingslope = spr->mobj->standingslope; // The slope that the object is standing on.

			// The slope that was defined for the sprite.
			if (renderflags & RF_SLOPESPLAT)
				splatslope = spr->mobj->floorspriteslope;

			if (standingslope && (renderflags & RF_OBJECTSLOPESPLAT))
				splatslope = standingslope;
		}

		// (PC): R_SplatSlope (whip charge, MT_INSTAWHIP_RECHARGE) is NOT used in GL:
		// tilted in GL it came out as "white pillars" (Software clips it differently). Plane = original GL.
		splatlibre = (splatslope != NULL && splatslope != spr->mobj->standingslope);

		// Set vertical position
		if (splatslope)
		{
			for (i = 0; i < 4; i++)
			{
				fixed_t slopez = P_GetSlopeZAt(splatslope, FLOAT_TO_FIXED(wallVerts[i].x), FLOAT_TO_FIXED(wallVerts[i].z));
				wallVerts[i].y = FIXED_TO_FLOAT(slopez) + zoffset;
			}
		}
		else
		{
			for (i = 0; i < 4; i++)
				wallVerts[i].y = FIXED_TO_FLOAT(spr->gz) + zoffset;
		}

		// (PC): the floor sprite cannot end up BELOW the real floor (see HWR_SplatFollowFloor).
		if (!splatlibre)
			for (i = 0; i < 4; i++)
				HWR_SplatClampY(&wallVerts[i], spr->mobj, zoffset);
	}
	else
	{
		// these were already scaled in HWR_ProjectSprite
		wallVerts[0].x = wallVerts[3].x = spr->x1;
		wallVerts[2].x = wallVerts[1].x = spr->x2;
		wallVerts[2].y = wallVerts[3].y = spr->gzt;
		wallVerts[0].y = wallVerts[1].y = spr->gz;

		// make a wall polygon (with 2 triangles), using the floor/ceiling heights,
		// and the 2d map coords of start/end vertices
		wallVerts[0].z = wallVerts[3].z = spr->z1;
		wallVerts[1].z = wallVerts[2].z = spr->z2;
#ifdef RR3DS
		if (spr->rr_roll != 0.0f)
		{
			// L axis = from x2 to x1 (left on screen), vertical axis = y
			float lx = spr->x1 - spr->x2, lz = spr->z1 - spr->z2;
			const float len = sqrtf(lx * lx + lz * lz);
			if (len > 0.0001f)
			{
				lx /= len; lz /= len;
				const float c = cosf(spr->rr_roll), sn = sinf(spr->rr_roll);
				for (INT32 i = 0; i < 4; i++)
				{
					const float u = (wallVerts[i].x - spr->rr_px) * lx + (wallVerts[i].z - spr->rr_pz) * lz;
					const float w = wallVerts[i].y - spr->rr_py;
					const float u2 = u * c + w * sn;
					const float w2 = -u * sn + w * c;
					wallVerts[i].x = spr->rr_px + u2 * lx;
					wallVerts[i].z = spr->rr_pz + u2 * lz;
					wallVerts[i].y = spr->rr_py + w2;
				}
			}
		}
#endif
	}

	// cache the patch in the graphics card memory
	//12/12/99: Hurdler: same comment as above (for md2)
	//Hurdler: 25/04/2000: now support colormap in hardware mode
#ifdef __3DS__
	gpatch = RR3DS_KartBudget(spr, gpatch); // A4
#endif
	rr3ds_sprmobj = spr->mobj; HWR_GetMappedPatch(gpatch, spr->colormap); rr3ds_sprmobj = NULL;

	if (spr->flip)
	{
		wallVerts[0].s = wallVerts[3].s = ((GLPatch_t *)gpatch->hardware)->max_s;
		wallVerts[2].s = wallVerts[1].s = 0;
	}else{
		wallVerts[0].s = wallVerts[3].s = 0;
		wallVerts[2].s = wallVerts[1].s = ((GLPatch_t *)gpatch->hardware)->max_s;
	}

	// flip the texture coords (look familiar?)
	if (spr->vflip)
	{
		wallVerts[3].t = wallVerts[2].t = ((GLPatch_t *)gpatch->hardware)->max_t;
		wallVerts[0].t = wallVerts[1].t = 0;
	}else{
		wallVerts[3].t = wallVerts[2].t = 0;
		wallVerts[0].t = wallVerts[1].t = ((GLPatch_t *)gpatch->hardware)->max_t;
	}

	if (!splat)
	{
		// if it has a dispoffset, push it a little towards the camera
		if (spr->dispoffset) {
			float co = -gl_viewcos*(0.05f*spr->dispoffset);
			float si = -gl_viewsin*(0.05f*spr->dispoffset);
			wallVerts[0].z = wallVerts[3].z = wallVerts[0].z+si;
			wallVerts[1].z = wallVerts[2].z = wallVerts[1].z+si;
			wallVerts[0].x = wallVerts[3].x = wallVerts[0].x+co;
			wallVerts[1].x = wallVerts[2].x = wallVerts[1].x+co;
		}

		// Let dispoffset work first since this adjust each vertex
		HWR_RotateSpritePolyToAim(spr, wallVerts, false);
	}

	// This needs to be AFTER the shadows so that the regular sprites aren't drawn completely black.
	// sprite lighting by modulating the RGB components
	/// \todo coloured

	// colormap test
	{
		sector_t *sector = spr->mobj->subsector->sector;
		INT32 lightlevel = 0;
		boolean lightset = HWR_OverrideObjectLightLevel(spr->mobj, &lightlevel);
		extracolormap_t *colormap = NULL;

		if (!R_ThingIsFullBright(spr->mobj) && !(spr->mobj->renderflags & RF_NOCOLORMAPS))
			colormap = sector->extra_colormap;

		if (splat && sector->numlights)
		{
			INT32 light = R_GetPlaneLight(sector, spr->mobj->z, false);

			if (!lightset)
				lightlevel = *sector->lightlist[light].lightlevel > 255 ? 255 : *sector->lightlist[light].lightlevel;

			if (!R_ThingIsFullBright(spr->mobj) && *sector->lightlist[light].extra_colormap && !(spr->mobj->renderflags & RF_NOCOLORMAPS))
				colormap = *sector->lightlist[light].extra_colormap;
		}
		else if (!lightset)
			lightlevel = sector->lightlevel > 255 ? 255 : sector->lightlevel;

		if (!lightset)
			HWR_ObjectLightLevelPost(spr, sector, &lightlevel, false);

		HWR_Lighting(&Surf, lightlevel, colormap, P_SectorUsesDirectionalLighting(sector) && !R_ThingIsFullBright(spr->mobj));
#ifdef RR3DS
		// DIAG [DARK2]: why Sonic comes out black on Sealed Stars
		if (spr->mobj->player)
		{
			static tic_t s_lastOsc = 0;
			if (gametic - s_lastOsc >= TICRATE)
			{
				s_lastOsc = gametic;
				RR_DIAGPRINT("[DARK2] sector_light=%d final_light=%d lightset=%d dir=%d/%d cmap=%p tint=%08lx fade=%08lx poly=%08lx fstart=%d fend=%d fb=%d\n",
					sector->lightlevel, lightlevel, (int)lightset, (int)maplighting.directional,
					(int)P_SectorUsesDirectionalLighting(sector), (void *)colormap,
					(unsigned long)Surf.TintColor.rgba, (unsigned long)Surf.FadeColor.rgba, (unsigned long)Surf.PolyColor.rgba,
					(int)Surf.LightInfo.fade_start, (int)Surf.LightInfo.fade_end, (int)R_ThingIsFullBright(spr->mobj));
			}
		}
#endif
	}

	{
		INT32 shader = SHADER_DEFAULT;
		FBITFIELD blend = 0;
		FBITFIELD occlusion;
		boolean use_linkdraw_hack = false;

		// if sprite has linkdraw, then dont write to z-buffer (by not using PF_Occlude)
		// this will result in sprites drawn afterwards to be drawn on top like intended when using linkdraw.
		if ((spr->mobj->flags2 & MF2_LINKDRAW) && spr->mobj->tracer)
			occlusion = 0;
		else
			occlusion = PF_Occlude;

		// Determine the blendmode and translucency value
		{
			UINT32 blendmode, trans;
			if (spr->mobj->renderflags & RF_BLENDMASK)
				blendmode = (spr->mobj->renderflags & RF_BLENDMASK) >> RF_BLENDSHIFT;
			else
				blendmode = (spr->mobj->frame & FF_BLENDMASK) >> FF_BLENDSHIFT;
			if (blendmode)
				blendmode++; // realign to constants

			if (spr->mobj->renderflags & RF_TRANSMASK)
				trans = (spr->mobj->renderflags & RF_TRANSMASK) >> RF_TRANSSHIFT;
			else
				trans = (spr->mobj->frame & FF_TRANSMASK) >> FF_TRANSSHIFT;
			if (trans >= NUMTRANSMAPS)
				return; // cap

			blend = HWR_SurfaceBlend(blendmode, trans, &Surf);

			// if sprite has PF_ALWAYSONTOP, draw on top of everything.
			if (cv_debugrender_spriteclip.value || spr->mobj->renderflags & RF_ALWAYSONTOP)
				blend |= PF_NoDepthTest;

			if (!trans && !blendmode)
			{
				// BP: i agree that is little better in environement but it don't
				//     work properly under glide nor with fogcolor to ffffff :(
				// Hurdler: PF_Environement would be cool, but we need to fix
				//          the issue with the fog before
				blend |= occlusion;
				if (!occlusion) use_linkdraw_hack = true;
			}
		}

		if (spr->renderflags & RF_SHADOWEFFECTS)
		{
			INT32 alpha = Surf.PolyColor.s.alpha;
			alpha -= ((INT32)(spr->shadowheight / 4.0f)) + 75;
			if (alpha < 1)
				return;

			Surf.PolyColor.s.alpha = (UINT8)(alpha);
			blend = PF_Translucent|occlusion;
			if (!occlusion) use_linkdraw_hack = true;
		}

		// (PC): floor sprites (boost panels, etc.) with depth offset.
		// They were only 0.05 units above the floor: from far away the z-buffer lacks precision and the floor
		// covered them (in Software they are always visible). PF_Decal = glPolygonOffset(-1,-1).
		if (R_ThingIsFloorSprite(spr->mobj))
			blend |= PF_Decal;

		if (HWR_UseShader())
		{
			shader = (R_ThingIsPaperSprite(spr->mobj)
				|| R_ThingIsFloorSprite(spr->mobj)
				|| (spr->mobj->terrain && spr->mobj->terrain->floorClip)
				) ? SHADER_SPRITE : SHADER_SPRITECLIPHACK;
			blend |= PF_ColorMapped;
		}

		{ DBG3DS_TINT(&Surf, NULL, 0, 0, 255);
			if (!(splat && !splatlibre && HWR_SplatFollowFloor(&Surf, wallVerts, spr->mobj, P_MobjFlip(spr->mobj) * 0.05f, blend|PF_Modulated, shader)))
				HWR_ProcessPolygon(&Surf, wallVerts, 4, blend|PF_Modulated, shader, false); }

		if (use_linkdraw_hack)
			HWR_LinkDrawHackAdd(wallVerts, spr);
	}
}

#ifdef HWPRECIP
// Sprite drawer for precipitation
static inline void HWR_DrawPrecipitationSprite(gl_vissprite_t *spr)
{
	INT32 shader = SHADER_DEFAULT;
	FBITFIELD blend = 0;
	FOutVector wallVerts[4];
	patch_t *gpatch;
	FSurfaceInfo Surf;

	if (!spr->mobj)
		return;

	if (!spr->mobj->subsector)
		return;

	// cache sprite graphics
	gpatch = spr->gpatch;

	// create the sprite billboard
	//
	//  3--2
	//  | /|
	//  |/ |
	//  0--1
	wallVerts[0].x = wallVerts[3].x = spr->x1;
	wallVerts[2].x = wallVerts[1].x = spr->x2;
	wallVerts[2].y = wallVerts[3].y = spr->gzt;
	wallVerts[0].y = wallVerts[1].y = spr->gz;

	// make a wall polygon (with 2 triangles), using the floor/ceiling heights,
	// and the 2d map coords of start/end vertices
	wallVerts[0].z = wallVerts[3].z = spr->z1;
	wallVerts[1].z = wallVerts[2].z = spr->z2;

	// Let dispoffset work first since this adjust each vertex
	HWR_RotateSpritePolyToAim(spr, wallVerts, true);

	wallVerts[0].s = wallVerts[3].s = 0;
	wallVerts[2].s = wallVerts[1].s = ((GLPatch_t *)gpatch->hardware)->max_s;

	wallVerts[3].t = wallVerts[2].t = 0;
	wallVerts[0].t = wallVerts[1].t = ((GLPatch_t *)gpatch->hardware)->max_t;

	// cache the patch in the graphics card memory
	//12/12/99: Hurdler: same comment as above (for md2)
	//Hurdler: 25/04/2000: now support colormap in hardware mode
	rr3ds_sprmobj = spr->mobj; HWR_GetMappedPatch(gpatch, spr->colormap); rr3ds_sprmobj = NULL;

	// colormap test
	{
		sector_t *sector = spr->mobj->subsector->sector;
		UINT8 lightlevel = 255;
		extracolormap_t *colormap = sector->extra_colormap;

		if (sector->numlights)
		{
			// Always use the light at the top instead of whatever I was doing before
			INT32 light = R_GetPlaneLight(sector, spr->mobj->z + spr->mobj->height, false);

			if (!R_ThingIsFullBright(spr->mobj))
				lightlevel = *sector->lightlist[light].lightlevel > 255 ? 255 : *sector->lightlist[light].lightlevel;

			if (*sector->lightlist[light].extra_colormap)
				colormap = *sector->lightlist[light].extra_colormap;
		}
		else
		{
			if (!R_ThingIsFullBright(spr->mobj))
				lightlevel = sector->lightlevel > 255 ? 255 : sector->lightlevel;

			if (sector->extra_colormap)
				colormap = sector->extra_colormap;
		}

		//lightlevel = 128 + (lightlevel>>1);

		HWR_Lighting(&Surf, lightlevel, colormap, P_SectorUsesDirectionalLighting(sector));
	}

	// Determine the blendmode and translucency value
	{
		UINT32 blendmode, trans;
		if (spr->mobj->renderflags & RF_BLENDMASK)
			blendmode = (spr->mobj->renderflags & RF_BLENDMASK) >> RF_BLENDSHIFT;
		else
			blendmode = (spr->mobj->frame & FF_BLENDMASK) >> FF_BLENDSHIFT;
		if (blendmode)
			blendmode++; // realign to constants

		if (spr->mobj->renderflags & RF_TRANSMASK)
			trans = (spr->mobj->renderflags & RF_TRANSMASK) >> RF_TRANSSHIFT;
		else
			trans = (spr->mobj->frame & FF_TRANSMASK) >> FF_TRANSSHIFT;
		if (trans >= NUMTRANSMAPS)
			return; // cap

		blend = HWR_SurfaceBlend(blendmode, trans, &Surf);
		if (!trans && !blendmode)
		{
			// BP: i agree that is little better in environement but it don't
			//     work properly under glide nor with fogcolor to ffffff :(
			// Hurdler: PF_Environement would be cool, but we need to fix
			//          the issue with the fog before
			blend |= PF_Occlude;
		}
	}

	if (HWR_UseShader())
	{
		shader = SHADER_SPRITE;
		blend |= PF_ColorMapped;
	}

	{ DBG3DS_TINT(&Surf, NULL, 0, 0, 255); HWR_ProcessPolygon(&Surf, wallVerts, 4, blend|PF_Modulated, shader, false); }
}
#endif

// --------------------------------------------------------------------------
// Sort vissprites by distance
// --------------------------------------------------------------------------
gl_vissprite_t* gl_vsprorder[MAXVISSPRITES];

// Note: For more correct transparency the transparent sprites would need to be
// sorted and drawn together with transparent surfaces.
static int CompareVisSprites(const void *p1, const void *p2)
{
	gl_vissprite_t* spr1 = *(gl_vissprite_t*const*)p1;
	gl_vissprite_t* spr2 = *(gl_vissprite_t*const*)p2;
	int idiff;
	float fdiff;
	float tz1, tz2;

	// Make transparent sprites last. Comment from the previous sort implementation:
	// Sryder:	Oh boy, while it's nice having ALL the sprites sorted properly, it fails when we bring MD2's into the
	//			mix and they want to be translucent. So let's place all the translucent sprites and MD2's AFTER
	//			everything else, but still ordered of course, the depth buffer can handle the opaque ones plenty fine.
	//			We just need to move all translucent ones to the end in order
	// TODO:	Fully sort all sprites and MD2s with walls and floors, this part will be unnecessary after that
	int transparency1;
	int transparency2;

	int renderflags1;
	int renderflags2;

	int frame1;
	int frame2;

	int linkdraw1;
	int linkdraw2;

	// bbox doesn't need to be sorted
	if (spr1->bbox || spr2->bbox)
		return 0;

	// check for precip first, because then sprX->mobj is actually a precipmobj_t and does not have flags2 or tracer
	linkdraw1 = !spr1->precip && (spr1->mobj->flags2 & MF2_LINKDRAW) && spr1->mobj->tracer;
	linkdraw2 = !spr2->precip && (spr2->mobj->flags2 & MF2_LINKDRAW) && spr2->mobj->tracer;

	// ^ is the XOR operation
	// if comparing a linkdraw and non-linkdraw sprite or 2 linkdraw sprites with different tracers, then use
	// the tracer's properties instead of the main sprite's.
	if ((linkdraw1 && linkdraw2 && spr1->mobj->tracer != spr2->mobj->tracer) || (linkdraw1 ^ linkdraw2))
	{
		if (linkdraw1)
		{
			tz1 = spr1->tracertz;
			renderflags1 = spr1->mobj->tracer->renderflags;
			frame1 = spr1->mobj->tracer->frame;
		}
		else
		{
			tz1 = spr1->tz;
			renderflags1 = spr1->mobj->renderflags;
			frame1 = spr1->mobj->frame;
		}
		if (linkdraw2)
		{
			tz2 = spr2->tracertz;
			renderflags2 = spr2->mobj->tracer->renderflags;
			frame2 = spr2->mobj->tracer->frame;
		}
		else
		{
			tz2 = spr2->tz;
			renderflags2 = spr2->mobj->renderflags;
			frame2 = spr2->mobj->frame;
		}
	}
	else
	{
		tz1 = spr1->tz;
		renderflags1 = (spr1->precip ? 0 : spr1->mobj->renderflags);
		frame1 = spr1->mobj->frame;
		tz2 = spr2->tz;
		renderflags2 = (spr2->precip ? 0 : spr2->mobj->renderflags);
		frame2 = spr2->mobj->frame;
	}

	// first compare transparency flags, then compare tz, then compare dispoffset

	transparency1 = (renderflags1 & RF_TRANSMASK) ?
		((renderflags1 & RF_TRANSMASK)>>RF_TRANSSHIFT) :
		((frame1 & FF_TRANSMASK)>>FF_TRANSSHIFT);

	transparency2 = (renderflags2 & RF_TRANSMASK) ?
		((renderflags2 & RF_TRANSMASK)>>RF_TRANSSHIFT) :
		((frame2 & FF_TRANSMASK)>>FF_TRANSSHIFT);

	idiff = transparency1 - transparency2;
	if (idiff != 0) return idiff;

	fdiff = tz2 - tz1; // this order seems correct when checking with apitrace. Back to front.
	if (fabsf(fdiff) < 1.0E-36f)
		return spr1->dispoffset - spr2->dispoffset; // smallest dispoffset first if sprites are at (almost) same location.
	else if (fdiff > 0)
		return 1;
	else
		return -1;
}

static void HWR_SortVisSprites(void)
{
	UINT32 i;
	for (i = 0; i < gl_visspritecount; i++)
	{
		gl_vsprorder[i] = HWR_GetVisSprite(i);
	}
	qsort(gl_vsprorder, gl_visspritecount, sizeof(gl_vissprite_t*), CompareVisSprites);
}

// A drawnode is something that points to a 3D floor, 3D side, or masked
// middle texture. This is used for sorting with sprites.
typedef struct
{
	FOutVector    wallVerts[4];
	FSurfaceInfo  Surf;
	INT32         texnum, basetexnum;
	FBITFIELD     blend;
	INT32         drawcount;
	boolean fogwall;
	INT32 lightlevel;
	extracolormap_t *wallcolormap; // Doing the lighting in HWR_RenderWall now for correct fog after sorting
} wallinfo_t;

static wallinfo_t *wallinfo = NULL;
static size_t numwalls = 0; // a list of transparent walls to be drawn

void HWR_RenderWall(FOutVector *wallVerts, FSurfaceInfo *pSurf, FBITFIELD blend, boolean fogwall, INT32 lightlevel, extracolormap_t *wallcolormap);

#define MAX_TRANSPARENTWALL 256

typedef struct
{
	extrasubsector_t *xsub;
	boolean isceiling;
	fixed_t fixedheight;
	INT32 lightlevel;
	levelflat_t *levelflat;
	INT32 alpha;
	sector_t *FOFSector;
	FBITFIELD blend;
	boolean fogplane;
	extracolormap_t *planecolormap;
	INT32 drawcount;
} planeinfo_t;

static size_t numplanes = 0; // a list of transparent floors to be drawn
static planeinfo_t *planeinfo = NULL;

typedef struct
{
	polyobj_t *polysector;
	boolean isceiling;
	fixed_t fixedheight;
	INT32 lightlevel;
	levelflat_t *levelflat;
	INT32 alpha;
	sector_t *FOFSector;
	FBITFIELD blend;
	extracolormap_t *planecolormap;
	INT32 drawcount;
} polyplaneinfo_t;

static size_t numpolyplanes = 0; // a list of transparent poyobject floors to be drawn
static polyplaneinfo_t *polyplaneinfo = NULL;

//Hurdler: 3D water sutffs
typedef struct gl_drawnode_s
{
	planeinfo_t *plane;
	polyplaneinfo_t *polyplane;
	wallinfo_t *wall;
	gl_vissprite_t *sprite;

//	struct gl_drawnode_s *next;
//	struct gl_drawnode_s *prev;
} gl_drawnode_t;

static INT32 drawcount = 0;

#define MAX_TRANSPARENTFLOOR 512

// This will likely turn into a copy of HWR_Add3DWater and replace it.
void HWR_AddTransparentFloor(levelflat_t *levelflat, extrasubsector_t *xsub, boolean isceiling, fixed_t fixedheight, INT32 lightlevel, INT32 alpha, sector_t *FOFSector, FBITFIELD blend, boolean fogplane, extracolormap_t *planecolormap)
{
	static size_t allocedplanes = 0;

	// Force realloc if buffer has been freed
	if (!planeinfo)
		allocedplanes = 0;

	if (allocedplanes < numplanes + 1)
	{
		allocedplanes += MAX_TRANSPARENTFLOOR;
		Z_Realloc(planeinfo, allocedplanes * sizeof (*planeinfo), PU_LEVEL, &planeinfo);
	}

	planeinfo[numplanes].isceiling = isceiling;
	planeinfo[numplanes].fixedheight = fixedheight;
	planeinfo[numplanes].lightlevel = (planecolormap && (planecolormap->flags & CMF_FOG)) ? lightlevel : 255;
	planeinfo[numplanes].levelflat = levelflat;
	planeinfo[numplanes].xsub = xsub;
	planeinfo[numplanes].alpha = alpha;
	planeinfo[numplanes].FOFSector = FOFSector;
	planeinfo[numplanes].blend = blend;
	planeinfo[numplanes].fogplane = fogplane;
	planeinfo[numplanes].planecolormap = planecolormap;
	planeinfo[numplanes].drawcount = drawcount++;

	numplanes++;
}

// Adding this for now until I can create extrasubsector info for polyobjects
// When that happens it'll just be done through HWR_AddTransparentFloor and HWR_RenderPlane
void HWR_AddTransparentPolyobjectFloor(levelflat_t *levelflat, polyobj_t *polysector, boolean isceiling, fixed_t fixedheight, INT32 lightlevel, INT32 alpha, sector_t *FOFSector, FBITFIELD blend, extracolormap_t *planecolormap)
{
	static size_t allocedpolyplanes = 0;

	// Force realloc if buffer has been freed
	if (!polyplaneinfo)
		allocedpolyplanes = 0;

	if (allocedpolyplanes < numpolyplanes + 1)
	{
		allocedpolyplanes += MAX_TRANSPARENTFLOOR;
		Z_Realloc(polyplaneinfo, allocedpolyplanes * sizeof (*polyplaneinfo), PU_LEVEL, &polyplaneinfo);
	}

	polyplaneinfo[numpolyplanes].isceiling = isceiling;
	polyplaneinfo[numpolyplanes].fixedheight = fixedheight;
	polyplaneinfo[numpolyplanes].lightlevel = (planecolormap && (planecolormap->flags & CMF_FOG)) ? lightlevel : 255;
	polyplaneinfo[numpolyplanes].levelflat = levelflat;
	polyplaneinfo[numpolyplanes].polysector = polysector;
	polyplaneinfo[numpolyplanes].alpha = alpha;
	polyplaneinfo[numpolyplanes].FOFSector = FOFSector;
	polyplaneinfo[numpolyplanes].blend = blend;
	polyplaneinfo[numpolyplanes].planecolormap = planecolormap;
	polyplaneinfo[numpolyplanes].drawcount = drawcount++;
	numpolyplanes++;
}

// putting sortindex and sortnode here so the comparator function can see them
gl_drawnode_t *sortnode;
size_t *sortindex;

static int CompareDrawNodes(const void *p1, const void *p2)
{
	size_t n1 = *(const size_t*)p1;
	size_t n2 = *(const size_t*)p2;
	INT32 v1 = 0;
	INT32 v2 = 0;
	INT32 diff;
	if (sortnode[n1].plane)
		v1 = sortnode[n1].plane->drawcount;
	else if (sortnode[n1].polyplane)
		v1 = sortnode[n1].polyplane->drawcount;
	else if (sortnode[n1].wall)
		v1 = sortnode[n1].wall->drawcount;
	else I_Error("CompareDrawNodes: n1 unknown");

	if (sortnode[n2].plane)
		v2 = sortnode[n2].plane->drawcount;
	else if (sortnode[n2].polyplane)
		v2 = sortnode[n2].polyplane->drawcount;
	else if (sortnode[n2].wall)
		v2 = sortnode[n2].wall->drawcount;
	else I_Error("CompareDrawNodes: n2 unknown");

	diff = v2 - v1;
	if (diff == 0) I_Error("CompareDrawNodes: diff is zero");
	return diff;
}

static int CompareDrawNodePlanes(const void *p1, const void *p2)
{
	size_t n1 = *(const size_t*)p1;
	size_t n2 = *(const size_t*)p2;
	if (!sortnode[n1].plane) I_Error("CompareDrawNodePlanes: Uh.. This isn't a plane! (n1)");
	if (!sortnode[n2].plane) I_Error("CompareDrawNodePlanes: Uh.. This isn't a plane! (n2)");
	return ABS(sortnode[n2].plane->fixedheight - viewz) - ABS(sortnode[n1].plane->fixedheight - viewz);
}

//
// HWR_CreateDrawNodes
// Creates and sorts a list of drawnodes for the scene being rendered.
static void HWR_CreateDrawNodes(void)
{
	UINT32 i = 0, p = 0;
	size_t run_start = 0;

	// Dump EVERYTHING into a huge drawnode list. Then we'll sort it!
	// Could this be optimized into _AddTransparentWall/_AddTransparentPlane?
	// Hell yes! But sort algorithm must be modified to use a linked list.
	// MEMORY: it used to be Z_Calloc + Z_Free EVERY frame (variable size, ~90 KB in
	// Playground): a large request per frame = fragmentation, and that is where Playground died
	// in Azahar 64 MB. Now a buffer that is reused and only grows (with headroom).
	{
		static gl_drawnode_t *s_nodes = NULL;
		static size_t *s_index = NULL;
		static size_t s_cap = 0;
		const size_t need = (size_t)numplanes + numpolyplanes + numwalls;
		if (need > s_cap || !s_nodes)
		{
			const size_t cap = need + need / 4 + 256;
			if (s_nodes) Z_Free(s_nodes);
			if (s_index) Z_Free(s_index);
			s_nodes = (gl_drawnode_t *)Z_Malloc(sizeof(gl_drawnode_t) * cap, PU_STATIC, NULL);
			s_index = (size_t *)Z_Malloc(sizeof(size_t) * cap, PU_STATIC, NULL);
			s_cap = cap;
		}
		memset(s_nodes, 0, sizeof(gl_drawnode_t) * need);
		sortnode = s_nodes;
		sortindex = s_index;
	}
	// todo:
	// However, in reality we shouldn't be re-copying and shifting all this information
	// that is already lying around. This should all be in some sort of linked list or lists.

	ps_hw_nodesorttime = I_GetPreciseTime();

	for (i = 0; i < numplanes; i++, p++)
	{
		sortnode[p].plane = &planeinfo[i];
		sortindex[p] = p;
	}

	for (i = 0; i < numpolyplanes; i++, p++)
	{
		sortnode[p].polyplane = &polyplaneinfo[i];
		sortindex[p] = p;
	}

	for (i = 0; i < numwalls; i++, p++)
	{
		sortnode[p].wall = &wallinfo[i];
		sortindex[p] = p;
	}

	ps_numdrawnodes = p;

	// p is the number of stuff to sort

	// sort the list based on the value of the 'drawcount' member of the drawnodes.
	qsort(sortindex, p, sizeof(size_t), CompareDrawNodes);

	// an additional pass is needed to correct the order of consecutive planes in the list.
	// for each consecutive run of planes in the list, sort that run based on plane height and view height.
	while (run_start < p-1)// p-1 because a 1 plane run at the end of the list does not count
	{
		// locate run start
		if (sortnode[sortindex[run_start]].plane)
		{
			// found it, now look for run end
			size_t run_end;// (inclusive)
			for (i = run_start+1; i < p; i++)// size_t and UINT32 being used mixed here... shouldnt break anything though..
			{
				if (!sortnode[sortindex[i]].plane) break;
			}
			run_end = i-1;
			if (run_end > run_start)// if there are multiple consecutive planes, not just one
			{
				// consecutive run of planes found, now sort it
				qsort(sortindex + run_start, run_end - run_start + 1, sizeof(size_t), CompareDrawNodePlanes);
			}
			run_start = run_end + 1;// continue looking for runs coming right after this one
		}
		else
		{
			// this wasnt the run start, try next one
			run_start++;
		}
	}

	ps_hw_nodesorttime = I_GetPreciseTime() - ps_hw_nodesorttime;

	ps_hw_nodedrawtime = I_GetPreciseTime();

	// Okay! Let's draw it all! Woo!
	HWD.pfnSetTransform(&atransform);
	HWD.pfnSetShader(SHADER_DEFAULT);

	for (i = 0; i < p; i++)
	{
		if (sortnode[sortindex[i]].plane)
		{
			// We aren't traversing the BSP tree, so make gl_frontsector null to avoid crashes.
			gl_frontsector = NULL;

			if (!(sortnode[sortindex[i]].plane->blend & PF_NoTexture))
				HWR_GetLevelFlat(sortnode[sortindex[i]].plane->levelflat, R_NoEncore(sortnode[sortindex[i]].plane->FOFSector, sortnode[sortindex[i]].plane->levelflat, sortnode[sortindex[i]].plane->isceiling));
			HWR_RenderPlane(NULL, sortnode[sortindex[i]].plane->xsub, sortnode[sortindex[i]].plane->isceiling, sortnode[sortindex[i]].plane->fixedheight, sortnode[sortindex[i]].plane->blend, sortnode[sortindex[i]].plane->lightlevel,
				sortnode[sortindex[i]].plane->levelflat, sortnode[sortindex[i]].plane->FOFSector, sortnode[sortindex[i]].plane->alpha, sortnode[sortindex[i]].plane->planecolormap);
		}
		else if (sortnode[sortindex[i]].polyplane)
		{
			// We aren't traversing the BSP tree, so make gl_frontsector null to avoid crashes.
			gl_frontsector = NULL;

			if (!(sortnode[sortindex[i]].polyplane->blend & PF_NoTexture))
				HWR_GetLevelFlat(sortnode[sortindex[i]].polyplane->levelflat, R_NoEncore(sortnode[sortindex[i]].polyplane->FOFSector, sortnode[sortindex[i]].polyplane->levelflat, sortnode[sortindex[i]].polyplane->isceiling));
			HWR_RenderPolyObjectPlane(sortnode[sortindex[i]].polyplane->polysector, sortnode[sortindex[i]].polyplane->isceiling, sortnode[sortindex[i]].polyplane->fixedheight, sortnode[sortindex[i]].polyplane->blend, sortnode[sortindex[i]].polyplane->lightlevel,
				sortnode[sortindex[i]].polyplane->levelflat, sortnode[sortindex[i]].polyplane->FOFSector, sortnode[sortindex[i]].polyplane->alpha, sortnode[sortindex[i]].polyplane->planecolormap);
		}
		else if (sortnode[sortindex[i]].wall)
		{
			if (!(sortnode[sortindex[i]].wall->blend & PF_NoTexture))
				HWR_GetTexture(sortnode[sortindex[i]].wall->texnum, sortnode[sortindex[i]].wall->basetexnum);
			HWR_RenderWall(sortnode[sortindex[i]].wall->wallVerts, &sortnode[sortindex[i]].wall->Surf, sortnode[sortindex[i]].wall->blend, sortnode[sortindex[i]].wall->fogwall,
				sortnode[sortindex[i]].wall->lightlevel, sortnode[sortindex[i]].wall->wallcolormap);
		}
	}

	ps_hw_nodedrawtime = I_GetPreciseTime() - ps_hw_nodedrawtime;

	numwalls = 0;
	numplanes = 0;
	numpolyplanes = 0;

	// the buffer is reused next frame (no Z_Free per frame)
	sortnode = NULL;
	sortindex = NULL;
}

// --------------------------------------------------------------------------
//  Draw all vissprites
// --------------------------------------------------------------------------

// added the stransform so they can be switched as drawing happenes so MD2s and sprites are sorted correctly with each other
#ifdef RR3DS
// sdraw time per mobj type and per shadow
static UINT64 rr3ds_sdTypeT[NUMMOBJTYPES];
static UINT32 rr3ds_sdTypeN[NUMMOBJTYPES];
static UINT64 rr3ds_sdShadowT = 0;
#endif
#ifdef __3DS__
extern UINT64 rr3ds_sprMakeT, rr3ds_sprUpT;
extern UINT32 rr3ds_sprMakeN, rr3ds_sprUpN, rr3ds_sprMakeCm;
extern "C" int I_3DS_UpBreakdown(char* buf, int n, int frames); // r_citro3d.cpp 
#endif
static void HWR_DrawSprites(void)
{
	UINT32 i;
	boolean skipshadow = false; // skip shadow if it was drawn already for a linkdraw sprite encountered earlier in the list

#ifdef BAD_MODEL_OPTIONS
	HWD.pfnSetSpecialState(HWD_SET_MODEL_LIGHTING, cv_glmodellighting.value);
#else
	HWD.pfnSetSpecialState(HWD_SET_MODEL_LIGHTING, 1);
#endif
#ifdef __3DS__
	RR3DS_KartBudgetReset(); // A4
#endif

	for (i = 0; i < gl_visspritecount; i++)
	{
		gl_vissprite_t *spr = gl_vsprorder[i];
		if (spr->bbox)
			HWR_DrawBoundingBox(spr);
		else
#ifdef HWPRECIP
		if (spr->precip)
			HWR_DrawPrecipitationSprite(spr);
		else
#endif
#ifdef __3DS__
		// pieces of an exploded kart (MT_KART_PARTICLE, ~30 per frame, ~2 ms each for uploading
		// recolored texture): half are drawn (fixed per object, no flicker).
		if (spr->mobj && spr->mobj->type == MT_KART_PARTICLE && (((uintptr_t)spr->mobj >> 4) & 1))
			continue;
		else
#endif
		{
#ifdef RR3DS
			const UINT64 rrT0 = I_GetPreciseTime(); // A5
#endif
			if (spr->mobj && spr->mobj->shadowscale && cv_shadow.value && !skipshadow)
			{
				HWR_DrawDropShadow(spr->mobj, spr->mobj->shadowscale);
			}
#ifdef RR3DS
			const UINT64 rrT1 = I_GetPreciseTime();
			rr3ds_sdShadowT += rrT1 - rrT0;
#endif

			if ((spr->mobj->flags2 & MF2_LINKDRAW) && spr->mobj->tracer)
			{
				// If this linkdraw sprite is behind a sprite that has a shadow,
				// then that shadow has to be drawn first, otherwise the shadow ends up on top of
				// the linkdraw sprite because the linkdraw sprite does not modify the z-buffer.
				// The !skipshadow check is there in case there are multiple linkdraw sprites connected
				// to the same tracer, so the tracer's shadow only gets drawn once.
				if (cv_shadow.value && !skipshadow && spr->dispoffset < 0 && spr->mobj->tracer->shadowscale)
				{
					HWR_DrawDropShadow(spr->mobj->tracer, spr->mobj->tracer->shadowscale);
					skipshadow = true;
					// The next sprite in this loop should be either another linkdraw sprite or the tracer.
					// When the tracer is inevitably encountered, skipshadow will cause it's shadow
					// to get skipped and skipshadow will get set to false by the 'else' clause below.
				}
			}
			else
			{
				skipshadow = false;
			}

#ifdef __3DS__ // no 3D models on 3DS
			HWR_DrawSprite(spr);
#ifdef RR3DS
			if (spr->mobj && spr->mobj->type < NUMMOBJTYPES)
			{
				rr3ds_sdTypeT[spr->mobj->type] += I_GetPreciseTime() - rrT1;
				rr3ds_sdTypeN[spr->mobj->type]++;
			}
#endif
#else
			if (spr->mobj && spr->mobj->skin && spr->mobj->sprite == SPR_PLAY)
			{
				if (!cv_glmodels.value || md2_playermodels[((skin_t*)(spr->mobj->skin))->skinnum].notfound || md2_playermodels[((skin_t*)(spr->mobj->skin))->skinnum].scale < 0.0f)
					HWR_DrawSprite(spr);
				else
				{
					if (!HWR_DrawModel(spr))
						HWR_DrawSprite(spr);
				}
			}
			else
			{
				if (!cv_glmodels.value || md2_models[spr->mobj->sprite].notfound || md2_models[spr->mobj->sprite].scale < 0.0f)
					HWR_DrawSprite(spr);
				else
				{
					if (!HWR_DrawModel(spr))
						HWR_DrawSprite(spr);
				}
			}
#endif
		}
	}
	HWD.pfnSetSpecialState(HWD_SET_MODEL_LIGHTING, 0);

	// At the end of sprite drawing, draw shapes of linkdraw sprites to z-buffer, so they
	// don't get drawn over by transparent surfaces.
	HWR_LinkDrawHackFinish();
	// Work around a r_opengl.c bug with PF_Invisible by making this SetBlend call
	// where PF_Invisible is off and PF_Masked is on.
	// (Other states probably don't matter. Here I left them same as in LinkDrawHackFinish)
	// Without this workaround the rest of the draw calls in this frame (including UI, screen texture)
	// can get drawn using an incorrect glBlendFunc, resulting in a occasional black screen.
	HWD.pfnSetBlend(PF_Translucent|PF_Occlude|PF_Masked);
}

// --------------------------------------------------------------------------
// HWR_AddSprites
// During BSP traversal, this adds sprites by sector.
// --------------------------------------------------------------------------
static UINT8 sectorlight;
#ifdef RR3DS
static UINT32 rr3ds_sprTypeCnt[NUMMOBJTYPES]; // diag: sprites projected per type
#endif
#ifdef __3DS__
// ---- PER-TRACK BUDGET (adaptive quality level 0..3) ----
// RR3DS_GovernorFrame (called once per displayed frame) averages the real frame time while racing and
// raises the level when the frame is slower than the target (rr_config budget_target_fps, default 14) and
// lowers it when there is room. The level only trims things measured to cost frame time:
//   rings drawn per BSP pass {20,14,8,4}. (Sprite distance and rain were tried as levels: measured 0 ms, removed.)
// rr_config: budget=0 turns it off; budget_fixed_level=N forces a level (for measuring).
static int rr3ds_q = 0;
extern "C" int RR3DS_Quality(void) { return rr3ds_q; }
extern "C" void RR3DS_GovernorFrame(void)
{
	static int on = -1, fixedq = -1, meta = 14;
	static precise_t last = 0;
	static double accMs = 0; static int accN = 0, up = 0, down = 0;
	if (on < 0)
	{
		on = RRCfg_Int("budget", 1);
		fixedq = RRCfg_Int("budget_fixed_level", -1);
		meta = RRCfg_Int("budget_target_fps", 14); if (meta < 4) meta = 4;
	}
	if (!on) { rr3ds_q = 0; return; }
	if (fixedq >= 0) { rr3ds_q = fixedq > 3 ? 3 : fixedq; return; }
	if (gamestate != GS_LEVEL) { last = 0; accMs = 0; accN = 0; if (rr3ds_q) rr3ds_q = 0; return; }
	const precise_t now = I_GetPreciseTime();
	if (last)
	{
		const double ms = (double)(now - last) * 1000.0 / (double)I_GetPrecisePrecision();
		if (ms < 1000.0) { accMs += ms; accN++; } // longer = loading / pause, not a frame
	}
	last = now;
	if (accN < 12) return;
	const double avg = accMs / accN, target = 1000.0 / meta;
	accMs = 0; accN = 0;
	if (avg > target * 1.15) { down = 0; if (++up >= 2 && rr3ds_q < 3) { rr3ds_q++; up = 0; CONS_Printf("[BUDGET] level %d (frame %.0f ms, target %.0f)\n", rr3ds_q, avg, target); } }
	else if (avg < target * 0.75) { up = 0; if (++down >= 4 && rr3ds_q > 0) { rr3ds_q--; down = 0; CONS_Printf("[BUDGET] level %d (frame %.0f ms, target %.0f)\n", rr3ds_q, avg, target); } }
	else { up = down = 0; }
}
// rings projected per BSP pass (validcount changes on every pass); rr_config rings_max (default by level, 0 = no limit)
static boolean rr3ds_ringBudget(INT32 vc)
{
	static INT32 lastvc = -1;
	static INT32 n = 0, maxn = -1;
	static const INT32 byLevel[4] = { 20, 14, 8, 4 };
	if (maxn < 0) maxn = RRCfg_Int("rings_max", -1);
	const INT32 lim = (maxn < 0) ? byLevel[rr3ds_q] : maxn;
	if (!lim) return true;
	if (vc != lastvc) { lastvc = vc; n = 0; }
	return ++n <= lim;
}
#endif
#if defined(RR_PCGEN) && !defined(__3DS__)
// Ring Racers E, Low-End only (render only, nothing of the simulation): same ring trimming as the 3DS.
// Rings farther than 2800 units are not projected and at most 20 rings per BSP pass are
// (the BSP walks near to far, so the dropped ones are the farthest). rr_config: rings_max=N (0 = no limit).
static boolean RRE_ringBudget(INT32 vc)
{
	static INT32 lastvc = -1;
	static INT32 n = 0, maxn = -1;
	if (maxn < 0) maxn = RRCfg_Int("rings_max", 20);
	if (!maxn) return true;
	if (vc != lastvc) { lastvc = vc; n = 0; }
	return ++n <= maxn;
}
#endif
#ifdef __3DS__
static boolean rr3ds_otherBudget(INT32 vc, const mobj_t *thing)
{
	static INT32 lastvc = -1;
	static INT32 n = 0, maxn = -1;
	if (maxn < 0) maxn = RRCfg_Int("sprites_max", 0);
	if (!maxn) return true;
	if (thing->type == MT_PLAYER || thing->player || (thing->flags & MF_DRAWFROMFARAWAY)) return true;
	if (vc != lastvc) { lastvc = vc; n = 0; }
	return ++n <= maxn;
}
#endif
static void HWR_AddSprites(sector_t *sec)
{
	mobj_t *thing;
	fixed_t limit_dist;

	// BSP is traversed by subsector.
	// A sector might have been split into several
	//  subsectors during BSP building.
	// Thus we check whether its already added.
	if (sec->validcount == validcount)
		return;

	// Well, now it will be done.
	sec->validcount = validcount;

	// sprite lighting
	sectorlight = sec->lightlevel & 0xff;

	// Handle all things in sector.
	// If a limit exists, handle things a tiny bit different.
	limit_dist = (fixed_t)(cv_drawdist.value) * mapobjectscale;
	for (thing = sec->thinglist; thing; thing = thing->snext)
	{
		if (R_ThingWithinDist(thing, limit_dist))
		{
#ifdef __3DS__
			// distant rings are not drawn (each sprite costs ~0.15 ms; ~80 rings per frame).
			// Only in races: in Battle/prison (small maps, 1/4 scale) they were seen vanishing
			// trees and objects, so nothing is culled there (decor either).
			if ((gametyperules & GTR_CIRCUIT) && R_ThingVisible(thing)
				&& thing->type == MT_RING && !R_ThingWithinDist(thing, 2800*mapobjectscale)) // 1100 -> 2800 (they popped in right at the face)
			{
				// out of range: do not project
			}
			else if ((gametyperules & GTR_CIRCUIT) && thing->type == MT_RING && R_ThingVisible(thing)
				&& rr3ds_ringBudget(validcount) == false)
			{
				// more than rings_max rings already projected in this BSP pass (the BSP walks near to far,
				// so the ones dropped are the farthest): 50-80 ring sprites per frame cost ~5 ms
			}
			else if ((gametyperules & GTR_CIRCUIT) && thing->type != MT_RING && R_ThingVisible(thing)
				&& rr3ds_otherBudget(validcount, thing) == false)
			{
				// same idea for the other sprites (decor, springs, capsule parts...): sprites_max per BSP pass,
				// never players, MF_DRAWFROMFARAWAY things or anything tied to a player
			}
			else
#endif
#if defined(RR_PCGEN) && !defined(__3DS__)
			if (RR_LOWEND && (gametyperules & GTR_CIRCUIT) && thing->type == MT_RING && R_ThingVisible(thing)
				&& (!R_ThingWithinDist(thing, 2800*mapobjectscale) || RRE_ringBudget(validcount) == false))
			{
				// Low-End: far ring, not projected
			}
			else
#endif
			if (R_ThingVisible(thing))
			{
#ifdef RR3DS
				rr3ds_sprTypeCnt[thing->type]++;
#endif
				HWR_ProjectSprite(thing);
			}

			HWR_ProjectBoundingBox(thing);
		}
	}
}

#ifdef HWPRECIP
// --------------------------------------------------------------------------
// HWR_AddPrecipitationSprites
// This renders through the blockmap instead of BSP to avoid
// iterating a huge amount of precipitation sprites in sectors
// that are beyond drawdist.
// --------------------------------------------------------------------------
static void HWR_AddPrecipitationSprites(void)
{
	const fixed_t drawdist = cv_drawdist_precip.value * mapobjectscale;

	INT32 xl, xh, yl, yh, bx, by;
	precipmobj_t *th;

	// no, no infinite draw distance for precipitation. this option at zero is supposed to turn it off
	if (drawdist == 0)
	{
		return;
	}

	R_GetRenderBlockMapDimensions(drawdist, &xl, &xh, &yl, &yh);

	for (bx = xl; bx <= xh; bx++)
	{
		for (by = yl; by <= yh; by++)
		{
			for (th = precipblocklinks[(by * bmapwidth) + bx]; th; th = th->bnext)
			{
				if (R_PrecipThingVisible(th))
				{
					HWR_ProjectPrecipitationSprite(th);
				}
			}
		}
	}
}
#endif

// --------------------------------------------------------------------------
// HWR_ProjectSprite
//  Generates a vissprite for a thing if it might be visible.
// --------------------------------------------------------------------------
// BP why not use xtoviexangle/viewangletox like in bsp ?....
static void HWR_ProjectSprite(mobj_t *thing)
{
	gl_vissprite_t *vis;
	float tr_x, tr_y;
	float tz;
	float tracertz = 0.0f;
	float x1, x2;
	float rightsin, rightcos;
	float this_scale, this_xscale, this_yscale;
	float spritexscale, spriteyscale;
	float shadowheight = 1.0f, shadowscale = 1.0f;
	float gz, gzt;
	spritedef_t *sprdef;
	spriteframe_t *sprframe;
#ifdef ROTSPRITE
	spriteinfo_t *sprinfo;
#endif
	md2_t *md2;
	size_t lumpoff;
	unsigned rot;
	UINT16 flip;
	boolean vflip = (!(thing->eflags & MFE_VERTICALFLIP) != !R_ThingVerticallyFlipped(thing));
	boolean mirrored = thing->mirrored;
	boolean hflip = (!R_ThingHorizontallyFlipped(thing) != !mirrored);
	INT32 dispoffset;

	angle_t ang;
	INT32 heightsec, phs;
	const boolean splat = R_ThingIsFloorSprite(thing);
	const boolean papersprite = (R_ThingIsPaperSprite(thing) && !splat);
	float z1, z2;

	fixed_t spr_width, spr_height;
	fixed_t spr_offset, spr_topoffset;
#ifdef ROTSPRITE
	patch_t *rotsprite = NULL;
	INT32 rollangle = 0;
	angle_t spriterotangle = 0;
	vector2_t visoffs;
#endif
#ifdef RR3DS
	float rr_roll = 0.0f, rr_px = 0.0f, rr_pz = 0.0f, rr_pyoff = 0.0f;
#endif

	// uncapped/interpolation
	interpmobjstate_t interp = {0};

	if (!thing)
		return;

	if (R_UsingFrameInterpolation() && !paused)
	{
		R_InterpolateMobjState(thing, rendertimefrac, &interp);
	}
	else
	{
		R_InterpolateMobjState(thing, FRACUNIT, &interp);
	}

	dispoffset = thing->dispoffset;

	// hitlag vibrating
	if (thing->hitlag > 0 && (thing->eflags & MFE_DAMAGEHITLAG))
	{
		fixed_t jitters = HITLAGJITTERS;
		if (R_UsingFrameInterpolation() && !paused)
			jitters += (rendertimefrac / HITLAGDIV);

		fixed_t mul = thing->hitlag * jitters;

		if (leveltime & 1)
		{
			mul = -mul;
		}

		interp.x += FixedMul(thing->momx, mul);
		interp.y += FixedMul(thing->momy, mul);
		interp.z += FixedMul(thing->momz, mul);
	}

	// sprite offset
	interp.x += thing->sprxoff;
	interp.y += thing->spryoff;
	interp.z += thing->sprzoff;

	if (interp.spritexscale < 1 || interp.spriteyscale < 1)
		return;

	this_scale = FIXED_TO_FLOAT(interp.scale);
	spritexscale = FIXED_TO_FLOAT(interp.spritexscale);
	spriteyscale = FIXED_TO_FLOAT(interp.spriteyscale);

	// transform the origin point
	tr_x = FIXED_TO_FLOAT(interp.x) - gl_viewx;
	tr_y = FIXED_TO_FLOAT(interp.y) - gl_viewy;

	// rotation around vertical axis
	tz = (tr_x * gl_viewcos) + (tr_y * gl_viewsin);

	// thing is behind view plane?
	if (tz < ZCLIP_PLANE && !(papersprite || splat))
	{
#ifdef __3DS__ // no 3D models on 3DS
		(void)md2;
		return;
#else
		if (cv_glmodels.value) //Yellow: Only MD2's dont disappear
		{
			if (thing->skin && thing->sprite == SPR_PLAY)
				md2 = &md2_playermodels[((skin_t *)thing->skin)->skinnum];
			else
				md2 = &md2_models[thing->sprite];

			if (md2->notfound || md2->scale < 0.0f)
				return;
		}
		else
			return;
#endif
	}

	// The above can stay as it works for cutting sprites that are too close
	tr_x = FIXED_TO_FLOAT(interp.x);
	tr_y = FIXED_TO_FLOAT(interp.y);

	// decide which patch to use for sprite relative to player
	if ((unsigned)thing->sprite >= numsprites)
	{
		CONS_Debug(DBG_RENDER, "HWR_ProjectSprite: invalid sprite number %i\n", thing->sprite);
		return;
	}

#ifdef __3DS__
	if (thing->thinker.rr3ds_sleep)
		RR3DS_RingSyncFrame(thing); // sleeping ring: frame according to the clock
#endif
	rot = thing->frame&FF_FRAMEMASK;

	//Fab : 02-08-98: 'skin' override spritedef currently used for skin
	if (thing->skin && thing->sprite == SPR_PLAY)
	{
#ifdef __3DS__
		R_3DS_EnsureSkin(((skin_t *)thing->skin)->skinnum); // lazy character
#endif
		sprdef = &((skin_t *)thing->skin)->sprites[thing->sprite2];
#ifdef ROTSPRITE
		sprinfo = R_SkinSprInfo((skin_t *)thing->skin, thing->sprite2);
#endif
	}
	else
	{
		sprdef = &sprites[thing->sprite];
#ifdef ROTSPRITE
		sprinfo = &spriteinfo[thing->sprite];
#endif
	}

	if (rot >= sprdef->numframes)
	{
		CONS_Alert(CONS_ERROR, M_GetText("HWR_ProjectSprite: invalid sprite frame %s/%s for %s\n"),
			sizeu1(rot), sizeu2(sprdef->numframes), sprnames[thing->sprite]);
		thing->sprite = states[S_UNKNOWN].sprite;
		thing->frame = states[S_UNKNOWN].frame;
		sprdef = &sprites[thing->sprite];
#ifdef ROTSPRITE
		sprinfo = &spriteinfo[thing->sprite];
#endif
		rot = thing->frame&FF_FRAMEMASK;
		thing->state->sprite = thing->sprite;
		thing->state->frame = thing->frame;
	}

	sprframe = &sprdef->spriteframes[rot];

#ifdef PARANOIA
	if (!sprframe)
		I_Error("sprframes NULL for sprite %d\n", thing->sprite);
#endif

	if (splat)
	{
		ang = R_PointToAngle2(0, viewz, 0, interp.z);
	}
	else
	{
		ang = R_PointToAngle (interp.x, interp.y) - interp.angle;
		if (mirrored)
			ang = InvAngle(ang);
	}

	if (sprframe->rotate == SRF_SINGLE)
	{
		// use single rotation for all views
		rot = 0;                        //Fab: for vis->patch below
		lumpoff = sprframe->lumpid[0];     //Fab: see note above
		flip = sprframe->flip; // Will only be 0x00 or 0xFF

		if (papersprite && ang < ANGLE_180)
			flip ^= 0xFFFF;
	}
	else
	{
		// choose a different rotation based on player view
		if ((sprframe->rotate & SRF_RIGHT) && (ang < ANGLE_180)) // See from right
			rot = 6; // F7 slot
		else if ((sprframe->rotate & SRF_LEFT) && (ang >= ANGLE_180)) // See from left
			rot = 2; // F3 slot
		else if (sprframe->rotate & SRF_3DGE) // 16-angle mode
		{
			rot = (ang+ANGLE_180+ANGLE_11hh)>>28;
			rot = ((rot & 1)<<3)|(rot>>1);
		}
		else // Normal behaviour
			rot = (ang+ANGLE_202h)>>29;

		//Fab: lumpid is the index for spritewidth,spriteoffset... tables
		lumpoff = sprframe->lumpid[rot];
		flip = sprframe->flip & (1<<rot);

		if (papersprite && ang < ANGLE_180)
			flip ^= (1<<rot);
	}

	if (thing->skin && ((skin_t *)thing->skin)->highresscale != FRACUNIT)
		this_scale *= FIXED_TO_FLOAT(((skin_t *)thing->skin)->highresscale);

	spr_width = spritecachedinfo[lumpoff].width;
	spr_height = spritecachedinfo[lumpoff].height;
	spr_offset = spritecachedinfo[lumpoff].offset;
	spr_topoffset = spritecachedinfo[lumpoff].topoffset;

#ifdef ROTSPRITE
	spriterotangle = R_SpriteRotationAngle(thing, NULL);

	if (spriterotangle != 0
	&& !(splat && !(thing->renderflags & RF_NOSPLATROLLANGLE))
#ifdef RR3DS
	// on 3DS the roll is done by rotating the quad on the GPU (see rr_roll).
	// Rotsprite created a new texture per angle (item boxes: ~1000
	// uploads at race start). It is kept only for sprites
	// split by lights (HWR_SplitSprite) and planes (splat).
	&& (splat || thing->subsector->sector->numlights)
#endif
	)
	{
		rollangle = R_GetRollAngle(papersprite == vflip
				? spriterotangle : InvAngle(spriterotangle));
		rotsprite = Patch_GetRotatedSprite(sprframe, (thing->frame & FF_FRAMEMASK), rot, flip, false, sprinfo, rollangle);

		if (rotsprite != NULL)
		{
			spr_width = rotsprite->width << FRACBITS;
			spr_height = rotsprite->height << FRACBITS;
			spr_offset = rotsprite->leftoffset << FRACBITS;
			spr_topoffset = rotsprite->topoffset << FRACBITS;
			spr_topoffset += FEETADJUST;

			// flip -> rotate, not rotate -> flip
			flip = 0;
		}
	}

	// initialize and rotate pitch/roll vector
	visoffs.x = 0;
	visoffs.y = 0;

	const fixed_t visoffymul = (vflip ? -FRACUNIT : FRACUNIT);

	if (R_ThingIsUsingBakedOffsets(thing))
	{
		R_RotateSpriteOffsetsByPitchRoll(thing,
										 vflip,
										 hflip,
										 &visoffs);
	}
#endif

	if (thing->renderflags & RF_ABSOLUTEOFFSETS)
	{
		spr_offset = interp.spritexoffset;
#ifdef ROTSPRITE
		spr_topoffset = (interp.spriteyoffset + FixedDiv((visoffs.y * visoffymul),
																mapobjectscale));
#else
		spr_topoffset = interp.spriteyoffset;
#endif
	}
	else
	{
		SINT8 flipoffset = 1;

		if ((thing->renderflags & RF_FLIPOFFSETS) && flip)
			flipoffset = -1;

		spr_offset += interp.spritexoffset * flipoffset;
#ifdef ROTSPRITE
		spr_topoffset += (interp.spriteyoffset + FixedDiv((visoffs.y * visoffymul),
															mapobjectscale))
																* flipoffset;
#else
		spr_topoffset += interp.spriteyoffset * flipoffset;
#endif
	}

	if (papersprite)
	{
		rightsin = FIXED_TO_FLOAT(FINESINE(interp.angle >> ANGLETOFINESHIFT));
		rightcos = FIXED_TO_FLOAT(FINECOSINE(interp.angle >> ANGLETOFINESHIFT));
	}
	else
	{
		rightsin = FIXED_TO_FLOAT(FINESINE((viewangle + ANGLE_90)>>ANGLETOFINESHIFT));
		rightcos = FIXED_TO_FLOAT(FINECOSINE((viewangle + ANGLE_90)>>ANGLETOFINESHIFT));
	}

	flip = !flip != !hflip;

	if (thing->renderflags & RF_SHADOWEFFECTS)
	{
		mobj_t *caster = thing->target;

		if (caster && !P_MobjWasRemoved(caster))
		{
			interpmobjstate_t casterinterp = {0};
			fixed_t groundz;
			fixed_t floordiff;

			if (R_UsingFrameInterpolation() && !paused)
			{
				R_InterpolateMobjState(caster, rendertimefrac, &casterinterp);
			}
			else
			{
				R_InterpolateMobjState(caster, FRACUNIT, &casterinterp);
			}

			groundz = R_GetShadowZ(thing, NULL);
			floordiff = abs(((thing->eflags & MFE_VERTICALFLIP) ? caster->height : 0) + casterinterp.z - groundz);

			shadowheight = FIXED_TO_FLOAT(floordiff);
			shadowscale = FIXED_TO_FLOAT(FixedMul(FRACUNIT - floordiff/640, casterinterp.scale));

			if (splat)
				spritexscale *= shadowscale;
			spriteyscale *= shadowscale;
		}
	}

	this_xscale = spritexscale * this_scale;
	this_yscale = spriteyscale * this_scale;

	if (splat)
	{
		z1 = z2 = tr_y;
		x1 = x2 = tr_x;
		gz = gzt = interp.z;
	}
	else
	{
#ifdef ROTSPRITE
		if (visoffs.x)
		{
			visoffs.x = (FixedDiv((visoffs.x * FRACUNIT), mapobjectscale));
		}
#endif
		if (flip)
		{
#ifdef ROTSPRITE
			spr_offset -= visoffs.x;
#endif
			x1 = (FIXED_TO_FLOAT((spr_width - spr_offset)) * this_xscale);
			x2 = (FIXED_TO_FLOAT(spr_offset) * this_xscale);
		}
		else
		{
#ifdef ROTSPRITE
			spr_offset += visoffs.x;
#endif
			x1 = (FIXED_TO_FLOAT(spr_offset) * this_xscale);
			x2 = (FIXED_TO_FLOAT(spr_width - spr_offset) * this_xscale);
		}
		// test if too close
	/*
		if (papersprite)
		{
			z1 = tz - x1 * angle_scalez;
			z2 = tz + x2 * angle_scalez;

			if (max(z1, z2) < ZCLIP_PLANE)
				return;
		}
	*/

#ifdef RR3DS
		rr_roll = 0.0f;
		if (spriterotangle != 0 && !rotsprite && !thing->subsector->sector->numlights)
		{
			INT32 xpiv, ypiv;
			const size_t fr = (thing->frame & FF_FRAMEMASK);
			patch_t *pp = (patch_t *)W_CachePatchNum(sprframe->lumppat[rot], PU_SPRITE);
			if (in_bit_array(sprinfo->available, fr))
				xpiv = sprinfo->pivot[fr].x, ypiv = sprinfo->pivot[fr].y;
			else if (in_bit_array(sprinfo->available, SPRINFO_DEFAULT_PIVOT))
				xpiv = sprinfo->pivot[SPRINFO_DEFAULT_PIVOT].x, ypiv = sprinfo->pivot[SPRINFO_DEFAULT_PIVOT].y;
			else
				xpiv = pp->leftoffset, ypiv = pp->height / 2;
			// u: distance along (rightcos,rightsin) from the origin
			float pu = flip ? (x1 - (float)(pp->width - xpiv) * this_xscale)
			                : (x1 - (float)xpiv * this_xscale);
			rr_px = tr_x + pu * rightcos;
			rr_pz = tr_y + pu * rightsin;
			rr_pyoff = (float)ypiv * this_yscale;
			// on-screen roll, counterclockwise (same as rotsprite)
			float deg = FIXED_TO_FLOAT(AngleFixed(spriterotangle));
			rr_roll = deg * (float)(M_PIl / 180.0);
			if (papersprite)
				rr_roll = -rr_roll;
		}
#endif
		z1 = tr_y + x1 * rightsin;
		z2 = tr_y - x2 * rightsin;
		x1 = tr_x + x1 * rightcos;
		x2 = tr_x - x2 * rightcos;

		if (thing->terrain && thing->terrain->floorClip)
			spr_topoffset -=  thing->terrain->floorClip;

		if (vflip)
		{
			gz = FIXED_TO_FLOAT(interp.z + thing->height) - (FIXED_TO_FLOAT(spr_topoffset) * this_yscale);
			gzt = gz + (FIXED_TO_FLOAT(spr_height) * this_yscale);
		}
		else
		{
			gzt = FIXED_TO_FLOAT(interp.z) + (FIXED_TO_FLOAT(spr_topoffset) * this_yscale);
			gz = gzt - (FIXED_TO_FLOAT(spr_height) * this_yscale);
		}
	}

	if (thing->subsector->sector->cullheight)
	{
		if (HWR_DoCulling(thing->subsector->sector->cullheight, viewsector->cullheight, gl_viewz, gz, gzt))
			return;
	}

	heightsec = thing->subsector->sector->heightsec;
	if (viewplayer && viewplayer->mo && viewplayer->mo->subsector)
		phs = viewplayer->mo->subsector->sector->heightsec;
	else
		phs = -1;

	if (heightsec != -1 && phs != -1) // only clip things which are in special sectors
	{
		if (gl_viewz < FIXED_TO_FLOAT(sectors[phs].floorheight) ?
		FIXED_TO_FLOAT(interp.z) >= FIXED_TO_FLOAT(sectors[heightsec].floorheight) :
		gzt < FIXED_TO_FLOAT(sectors[heightsec].floorheight))
			return;
		if (gl_viewz > FIXED_TO_FLOAT(sectors[phs].ceilingheight) ?
		gzt < FIXED_TO_FLOAT(sectors[heightsec].ceilingheight) && gl_viewz >= FIXED_TO_FLOAT(sectors[heightsec].ceilingheight) :
		FIXED_TO_FLOAT(interp.z) >= FIXED_TO_FLOAT(sectors[heightsec].ceilingheight))
			return;
	}

	if ((thing->flags2 & MF2_LINKDRAW) && thing->tracer)
	{
		interpmobjstate_t tracer_interp = {0};

		if (! R_ThingVisible(thing->tracer))
			return;

		if (R_UsingFrameInterpolation() && !paused)
		{
			R_InterpolateMobjState(thing->tracer, rendertimefrac, &tracer_interp);
		}
		else
		{
			R_InterpolateMobjState(thing->tracer, FRACUNIT, &tracer_interp);
		}

		// calculate tz for tracer, same way it is calculated for this sprite
		// transform the origin point
		tr_x = FIXED_TO_FLOAT(tracer_interp.x) - gl_viewx;
		tr_y = FIXED_TO_FLOAT(tracer_interp.y) - gl_viewy;

		// rotation around vertical axis
		tracertz = (tr_x * gl_viewcos) + (tr_y * gl_viewsin);

		// Software does not render the linkdraw sprite if the tracer is behind the view plane,
		// so do the same check here.
		// NOTE: This check has the same flaw as the view plane check at the beginning of HWR_ProjectSprite:
		// the view aiming angle is not taken into account, leading to sprites disappearing too early when they
		// can still be seen when looking down/up at steep angles.
		if (tracertz < ZCLIP_PLANE)
			return;

		// if the sprite is behind the tracer, invert dispoffset, putting the sprite behind the tracer
		if (tz > tracertz)
			dispoffset *= -1;
	}

	// store information in a vissprite
	vis = HWR_NewVisSprite();
	vis->x1 = x1;
	vis->x2 = x2;
	vis->z1 = z1;
	vis->z2 = z2;
#ifdef RR3DS
	vis->rr_roll = splat ? 0.0f : rr_roll;
	vis->rr_px = rr_px; vis->rr_pz = rr_pz;
	vis->rr_py = vflip ? (gz + rr_pyoff) : (gzt - rr_pyoff);
#endif

	vis->tz = tz; // Keep tz for the simple sprite sorting that happens
	vis->tracertz = tracertz;

	vis->renderflags = thing->renderflags;
	vis->rotateflags = sprframe->rotate;

	vis->shadowheight = shadowheight;
	vis->shadowscale = shadowscale;
	vis->dispoffset = dispoffset; // Monster Iestyn: 23/11/15: HARDWARE SUPPORT AT LAST
	vis->flip = flip;

	vis->scale = this_scale;
	vis->spritexscale = spritexscale;
	vis->spriteyscale = spriteyscale;
	vis->spritexoffset = FIXED_TO_FLOAT(spr_offset);
	vis->spriteyoffset = FIXED_TO_FLOAT(spr_topoffset);

	vis->rotated = false;

#ifdef ROTSPRITE
	if (rotsprite)
	{
		vis->gpatch = (patch_t *)rotsprite;
		vis->rotated = true;
	}
	else
#endif
		vis->gpatch = (patch_t *)W_CachePatchNum(sprframe->lumppat[rot], PU_SPRITE);

	vis->mobj = thing;

	INT32 skinnum = TC_DEFAULT;

	if (vis->mobj->skin && vis->mobj->sprite == SPR_PLAY) // This thing is a player!
	{
		skinnum = ((skin_t*)vis->mobj->skin)->skinnum;
	}

	// Hide not-yet-unlocked characters in replays from other people
	if (skinnum >= 0 && !R_CanShowSkinInDemo(skinnum))
	{
		vis->colormap = R_GetTranslationColormap(TC_BLINK, (skincolornum_t)thing->color, GTC_CACHE);
	}
	//Hurdler: 25/04/2000: now support colormap in hardware mode
	else if (R_ThingIsFlashing(vis->mobj))
	{
		vis->colormap = R_GetTranslationColormap(TC_HITLAG, (skincolornum_t)0, GTC_CACHE);
	}
	else if (thing->color)
	{
		vis->colormap = R_GetTranslationColormap(thing->colorized ? TC_RAINBOW : skinnum, (skincolornum_t)thing->color, GTC_CACHE);
	}
	else
	{
		vis->colormap = colormaps;

		if (encoremap && !(thing->flags & MF_DONTENCOREMAP))
			vis->colormap += COLORMAP_REMAPOFFSET;
	}

	// set top/bottom coords
	vis->gzt = gzt;
	vis->gz = gz;

	//CONS_Debug(DBG_RENDER, "------------------\nH: sprite  : %d\nH: frame   : %x\nH: type    : %d\nH: sname   : %s\n\n",
	//            thing->sprite, thing->frame, thing->type, sprnames[thing->sprite]);

	vis->vflip = vflip;

	vis->precip = false;
	vis->bbox = false;

	vis->angle = interp.angle;
}

#ifdef HWPRECIP
// Precipitation projector for hardware mode
static void HWR_ProjectPrecipitationSprite(precipmobj_t *thing)
{
	gl_vissprite_t *vis;
	float tr_x, tr_y;
	float tz;
	float x1, x2;
	float z1, z2;
	float rightsin, rightcos;
	float this_scale;
	spritedef_t *sprdef;
	spriteframe_t *sprframe;
	size_t lumpoff;
	unsigned rot = 0;
	UINT8 flip;

	if (!thing)
		return;

	// uncapped/interpolation
	interpmobjstate_t interp = {0};

	// okay... this is a hack, but weather isn't networked, so it should be ok
	if (!P_PrecipThinker(thing))
	{
		return;
	}

	// do interpolation
	if (R_UsingFrameInterpolation() && !paused)
	{
		R_InterpolatePrecipMobjState(thing, rendertimefrac, &interp);
	}
	else
	{
		R_InterpolatePrecipMobjState(thing, FRACUNIT, &interp);
	}

	this_scale = FIXED_TO_FLOAT(interp.scale);

	// transform the origin point
	tr_x = FIXED_TO_FLOAT(interp.x) - gl_viewx;
	tr_y = FIXED_TO_FLOAT(interp.y) - gl_viewy;

	// rotation around vertical axis
	tz = (tr_x * gl_viewcos) + (tr_y * gl_viewsin);

	// thing is behind view plane?
	if (tz < ZCLIP_PLANE)
		return;

	tr_x = FIXED_TO_FLOAT(interp.x);
	tr_y = FIXED_TO_FLOAT(interp.y);

	// decide which patch to use for sprite relative to player
	if ((unsigned)thing->sprite >= numsprites)
	{
		CONS_Debug(DBG_RENDER, "HWR_ProjectPrecipitationSprite: invalid sprite number %i\n",
		        thing->sprite);
		return;
	}

	sprdef = &sprites[thing->sprite];

	if ((size_t)(thing->frame&FF_FRAMEMASK) >= sprdef->numframes)
	{
		CONS_Debug(DBG_RENDER, "HWR_ProjectPrecipitationSprite: invalid sprite frame %i : %i for %s\n",
		        thing->sprite, thing->frame, sprnames[thing->sprite]);
		return;
	}

	sprframe = &sprdef->spriteframes[ thing->frame & FF_FRAMEMASK];

	// use single rotation for all views
	lumpoff = sprframe->lumpid[0];
	flip = sprframe->flip; // Will only be 0x00 or 0xFF

	rightsin = FIXED_TO_FLOAT(FINESINE((viewangle + ANGLE_90)>>ANGLETOFINESHIFT));
	rightcos = FIXED_TO_FLOAT(FINECOSINE((viewangle + ANGLE_90)>>ANGLETOFINESHIFT));
	if (flip)
	{
		x1 = FIXED_TO_FLOAT(spritecachedinfo[lumpoff].width - spritecachedinfo[lumpoff].offset);
		x2 = FIXED_TO_FLOAT(spritecachedinfo[lumpoff].offset);
	}
	else
	{
		x1 = FIXED_TO_FLOAT(spritecachedinfo[lumpoff].offset);
		x2 = FIXED_TO_FLOAT(spritecachedinfo[lumpoff].width - spritecachedinfo[lumpoff].offset);
	}

	x1 *= this_scale;
	x2 *= this_scale;

	z1 = tr_y + x1 * rightsin;
	z2 = tr_y - x2 * rightsin;
	x1 = tr_x + x1 * rightcos;
	x2 = tr_x - x2 * rightcos;

	//
	// store information in a vissprite
	//
	vis = HWR_NewVisSprite();
	vis->x1 = x1;
	vis->x2 = x2;
	vis->z1 = z1;
	vis->z2 = z2;
	vis->tz = tz;
	vis->dispoffset = 0; // Monster Iestyn: 23/11/15: HARDWARE SUPPORT AT LAST
	vis->gpatch = (patch_t *)W_CachePatchNum(sprframe->lumppat[rot], PU_SPRITE);
	vis->flip = flip;
	vis->mobj = (mobj_t *)thing;

	vis->colormap = NULL;

	if (encoremap && !(thing->flags & MF_DONTENCOREMAP))
		vis->colormap += COLORMAP_REMAPOFFSET;

	// set top/bottom coords
	vis->gzt = FIXED_TO_FLOAT(interp.z) + (FIXED_TO_FLOAT(spritecachedinfo[lumpoff].topoffset) * this_scale);
	vis->gz = vis->gzt - (FIXED_TO_FLOAT(spritecachedinfo[lumpoff].height) * this_scale);

	vis->precip = true;
	vis->bbox = false;
}
#endif

static void HWR_ProjectBoundingBox(mobj_t *thing)
{
	gl_vissprite_t *vis;
	float tr_x, tr_y;
	float tz;
	float rad;

	// uncapped/interpolation
	interpmobjstate_t interp = {0};

	if (!thing)
		return;

	if (!R_ThingBoundingBoxVisible(thing))
		return;

	if (R_UsingFrameInterpolation() && !paused)
	{
		R_InterpolateMobjState(thing, rendertimefrac, &interp);
	}
	else
	{
		R_InterpolateMobjState(thing, FRACUNIT, &interp);
	}

	// transform the origin point
	tr_x = FIXED_TO_FLOAT(interp.x) - gl_viewx;
	tr_y = FIXED_TO_FLOAT(interp.y) - gl_viewy;

	// rotation around vertical axis
	tz = (tr_x * gl_viewcos) + (tr_y * gl_viewsin);

	// thing is behind view plane?
	if (tz < ZCLIP_PLANE)
		return;

	tr_x += gl_viewx;
	tr_y += gl_viewy;

	rad = FIXED_TO_FLOAT(thing->radius);

	vis = HWR_NewVisSprite();
	vis->x1 = tr_x - rad;
	vis->x2 = tr_x + rad;
	vis->z1 = tr_y - rad;
	vis->z2 = tr_y + rad;
	vis->gz = FIXED_TO_FLOAT(interp.z);
	vis->gzt = vis->gz + FIXED_TO_FLOAT(thing->height);
	vis->mobj = thing;

	vis->precip = false;
	vis->bbox = true;
}

// ==========================================================================
// Sky dome rendering, ported from PrBoom+
// ==========================================================================

static gl_sky_t gl_sky;

// live API switch: the sky VBO died with the old context -> rebuild it when drawing
void HWR_SkyContextLost(void)
{
	gl_sky.vbo = 0;
	gl_sky.rebuild = true;
}

static void HWR_SkyDomeVertex(gl_sky_t *sky, gl_skyvertex_t *vbo, int r, int c, signed char yflip, float delta, boolean foglayer)
{
	const float radians = (float)(M_PIl / 180.0f);
	const float scale = 10000.0f;
	const float maxSideAngle = 60.0f;

	float topAngle = (c / (float)sky->columns * 360.0f);
	float sideAngle = (maxSideAngle * (sky->rows - r) / sky->rows);
	float height = (float)(sin(sideAngle * radians));
	float realRadius = (float)(scale * cos(sideAngle * radians));
	float x = (float)(realRadius * cos(topAngle * radians));
	float y = (!yflip) ? scale * height : -scale * height;
	float z = (float)(realRadius * sin(topAngle * radians));
	float timesRepeat = (4 * (256.0f / sky->width));
	if (fpclassify(timesRepeat) == FP_ZERO)
		timesRepeat = 1.0f;

	if (!foglayer)
	{
		vbo->r = 255;
		vbo->g = 255;
		vbo->b = 255;
		vbo->a = (r == 0 ? 0 : 255);

		// And the texture coordinates.
		vbo->u = (-timesRepeat * c / (float)sky->columns);
		if (!yflip)	// Flipped Y is for the lower hemisphere.
			vbo->v = (r / (float)sky->rows) + 0.5f;
		else
			vbo->v = 1.0f + ((sky->rows - r) / (float)sky->rows) + 0.5f;
	}

	if (r != 4)
		y += 300.0f;

	// And finally the vertex.
	vbo->x = x;
	vbo->y = y + delta;
	vbo->z = z;
}

// Clears the sky dome.
void HWR_ClearSkyDome(void)
{
	gl_sky_t *sky = &gl_sky;

	if (sky->loops)
		free(sky->loops);
	if (sky->data)
		free(sky->data);

	sky->loops = NULL;
	sky->data = NULL;

	sky->rows = sky->columns = 0;
	sky->loopcount = 0;

	sky->detail = 0;
	sky->texture = -1;
	sky->width = sky->height = 0;

	sky->rebuild = true;
}

void HWR_BuildSkyDome(void)
{
	int c, r;
	signed char yflip;
	int row_count = 4;
	int col_count = 4;
	float delta;

	gl_sky_t *sky = &gl_sky;
	gl_skyvertex_t *vertex_p;
	texture_t *texture = textures[texturetranslation[skytexture]];

	sky->detail = 16;
	col_count *= sky->detail;

	if ((sky->columns != col_count) || (sky->rows != row_count))
		HWR_ClearSkyDome();

	sky->columns = col_count;
	sky->rows = row_count;
	sky->vertex_count = 2 * sky->rows * (sky->columns * 2 + 2) + sky->columns * 2;

	if (!sky->loops)
		sky->loops = (gl_skyloopdef_t *)malloc((sky->rows * 2 + 2) * sizeof(sky->loops[0]));

	// create vertex array
	if (!sky->data)
		sky->data = (gl_skyvertex_t *)malloc(sky->vertex_count * sizeof(sky->data[0]));

	sky->texture = texturetranslation[skytexture];
	sky->width = texture->width;
	sky->height = texture->height;

	vertex_p = &sky->data[0];
	sky->loopcount = 0;

	for (yflip = 0; yflip < 2; yflip++)
	{
		sky->loops[sky->loopcount].mode = HWD_SKYLOOP_FAN;
		sky->loops[sky->loopcount].vertexindex = vertex_p - &sky->data[0];
		sky->loops[sky->loopcount].vertexcount = col_count;
		sky->loops[sky->loopcount].use_texture = false;
		sky->loopcount++;

		delta = 0.0f;

		for (c = 0; c < col_count; c++)
		{
			HWR_SkyDomeVertex(sky, vertex_p, 1, c, yflip, 0.0f, true);
			vertex_p->r = 255;
			vertex_p->g = 255;
			vertex_p->b = 255;
			vertex_p->a = 255;
			vertex_p++;
		}

		delta = (yflip ? 5.0f : -5.0f) / 128.0f;

		for (r = 0; r < row_count; r++)
		{
			sky->loops[sky->loopcount].mode = HWD_SKYLOOP_STRIP;
			sky->loops[sky->loopcount].vertexindex = vertex_p - &sky->data[0];
			sky->loops[sky->loopcount].vertexcount = 2 * col_count + 2;
			sky->loops[sky->loopcount].use_texture = true;
			sky->loopcount++;

			for (c = 0; c <= col_count; c++)
			{
				HWR_SkyDomeVertex(sky, vertex_p++, r + (yflip ? 1 : 0), (c ? c : 0), yflip, delta, false);
				HWR_SkyDomeVertex(sky, vertex_p++, r + (yflip ? 0 : 1), (c ? c : 0), yflip, delta, false);
			}
		}
	}
}

static void HWR_DrawSkyBackground(player_t *player)
{
	HWD.pfnSetBlend(PF_Translucent|PF_NoDepthTest|PF_Modulated);

#if defined(RR3DS) && !defined(__3DS__)
	// 3DS: the dome cost 5-34 ms/frame; flat sky (1 quad). On E only with Low-End.
	// with ANGLE (DirectX 11 / Vulkan) the dome left ALL of the 3D black (since that change, also in the
	// 2.4E release): always flat sky there, as before.
	if (!RRE_GLLOWEND && cv_glskydome.value && !RRGfx_IsAngle(RRGfx_Active()))
#elif defined(RR3DS)
	if (!RRE_GLLOWEND && cv_glskydome.value)
#else
	if (cv_glskydome.value)
#endif
	{
		FTransform dometransform;
		const float fpov = FIXED_TO_FLOAT(R_FOV(viewssnum)+player->fovadd);
		postimg_t *type = &postimgtype[R_GetViewNumber()];

		memset(&dometransform, 0x00, sizeof(FTransform));

		//04/01/2000: Hurdler: added for T&L
		//                     It should replace all other gl_viewxxx when finished
		HWR_SetTransformAiming(&dometransform, player, false);
		dometransform.angley = (float)((viewangle-ANGLE_270)>>ANGLETOFINESHIFT)*(360.0f/(float)FINEANGLES);

		if (*type == postimg_flip)
			dometransform.flip = true;
		else
			dometransform.flip = false;

		if (*type == postimg_mirror)
			dometransform.mirror = true;
		else
			dometransform.mirror = false;

		dometransform.scalex = 1;
		dometransform.scaley = (float)vid.width/vid.height;
		dometransform.scalez = 1;
		dometransform.fovxangle = fpov; // Tails
		dometransform.fovyangle = fpov; // Tails
		HWR_RollTransform(&dometransform, viewroll);
		dometransform.splitscreen = r_splitscreen;

		HWR_GetTexture(texturetranslation[skytexture], skytexture);

		if (gl_sky.texture != texturetranslation[skytexture])
		{
			HWR_ClearSkyDome();
			HWR_BuildSkyDome();
		}

		HWD.pfnSetShader(SHADER_SKY); // sky shader
		HWD.pfnSetTransform(&dometransform);
		HWD.pfnRenderSkyDome(&gl_sky);
	}
	else
	{
		FOutVector v[4];
		angle_t angle;
		float dimensionmultiply;
		float aspectratio;
		float angleturn;

		HWR_GetTexture(texturetranslation[skytexture], skytexture);
		aspectratio = (float)vid.width/(float)vid.height;

		//Hurdler: the sky is the only texture who need 4.0f instead of 1.0
		//         because it's called just after clearing the screen
		//         and thus, the near clipping plane is set to 3.99
		// Sryder: Just use the near clipping plane value then

		//  3--2
		//  | /|
		//  |/ |
		//  0--1
		v[0].x = v[3].x = -ZCLIP_PLANE-1;
		v[1].x = v[2].x =  ZCLIP_PLANE+1;
		v[0].y = v[1].y = -ZCLIP_PLANE-1;
		v[2].y = v[3].y =  ZCLIP_PLANE+1;

		v[0].z = v[1].z = v[2].z = v[3].z = ZCLIP_PLANE+1;

		// X

		// NOTE: This doesn't work right with texture widths greater than 1024
		// software doesn't draw any further than 1024 for skies anyway, but this doesn't overlap properly
		// The only time this will probably be an issue is when a sky wider than 1024 is used as a sky AND a regular wall texture

		angle = (dup_viewangle + gl_xtoviewangle[0]);

		dimensionmultiply = ((float)textures[texturetranslation[skytexture]]->width/256.0f);

		v[0].s = v[3].s = (-1.0f * angle) / ((ANGLE_90-1.0f)*dimensionmultiply); // left
		v[2].s = v[1].s = v[0].s + (1.0f/dimensionmultiply); // right (or left + 1.0f)
		// use +angle and -1.0f above instead if you wanted old backwards behavior

		// Y
		angle = aimingangle;
		dimensionmultiply = ((float)textures[texturetranslation[skytexture]]->height/(128.0f*aspectratio));

		if (r_splitscreen == 1)
		{
			dimensionmultiply *= 2;
			angle *= 2;
		}

		// Middle of the sky should always be at angle 0
		// need to keep correct aspect ratio with X
		if (atransform.flip)
		{
			// During vertical flip the sky should be flipped and it's y movement should also be flipped obviously
			v[3].t = v[2].t = -(0.5f-(0.5f/dimensionmultiply)); // top
			v[0].t = v[1].t = v[3].t - (1.0f/dimensionmultiply); // bottom (or top - 1.0f)
		}
		else
		{
			v[0].t = v[1].t = -(0.5f-(0.5f/dimensionmultiply)); // bottom
			v[3].t = v[2].t = v[0].t - (1.0f/dimensionmultiply); // top (or bottom - 1.0f)
		}

		angleturn = (((float)ANGLE_45-1.0f)*aspectratio)*dimensionmultiply;

		if (angle > ANGLE_180) // Do this because we don't want the sky to suddenly teleport when crossing over 0 to 360 and vice versa
		{
			angle = InvAngle(angle);
			v[3].t = v[2].t += ((float) angle / angleturn);
			v[0].t = v[1].t += ((float) angle / angleturn);
		}
		else
		{
			v[3].t = v[2].t -= ((float) angle / angleturn);
			v[0].t = v[1].t -= ((float) angle / angleturn);
		}

#ifdef RR3DS
		// drawing into the sky texture (larger than the screen):
		// stretch the quad (and its coordinates, which are linear) to cover the margins.
		if (I_3DS_SkyTexDrawing())
		{
			float rw = 240.0f * vid.width / (float)vid.height; if (rw > 400.0f) rw = 400.0f; // 384 (320x200) o 400 (400x240)
			const float fx = 512.0f / rw, fy = 256.0f / 240.0f;
			const float cs = (v[0].s + v[1].s) * 0.5f, ct = (v[0].t + v[3].t) * 0.5f;
			for (int k = 0; k < 4; k++)
			{
				v[k].x *= fx; v[k].s = cs + (v[k].s - cs) * fx;
				v[k].y *= fy; v[k].t = ct + (v[k].t - ct) * fy;
			}
		}
#endif
		HWD.pfnUnSetShader();
		HWD.pfnDrawPolygon(NULL, v, 4, 0);
	}

	HWD.pfnSetShader(SHADER_DEFAULT);
}


// -----------------+
// HWR_ClearView : clear the viewwindow, with maximum z value
// -----------------+
static inline void HWR_ClearView(void)
{
	//  3--2
	//  | /|
	//  |/ |
	//  0--1

	/// \bug faB - enable depth mask, disable color mask

	HWD.pfnGClipRect((INT32)gl_viewwindowx,
	                 (INT32)gl_viewwindowy,
	                 (INT32)(gl_viewwindowx + gl_viewwidth),
	                 (INT32)(gl_viewwindowy + gl_viewheight),
	                 ZCLIP_PLANE);
	HWD.pfnClearBuffer(false, true, 0);

	//disable clip window - set to full size
	// rem by Hurdler
	// HWD.pfnGClipRect(0, 0, vid.width, vid.height);
}


// -----------------+
// HWR_SetViewSize  : set projection and scaling values
// -----------------+
void HWR_SetViewSize(void)
{
	// setup view size
	gl_viewwidth = (float)vid.width;
	gl_viewheight = (float)vid.height;

	if (r_splitscreen > 0)
	{
		gl_viewheight /= 2;

		if (r_splitscreen > 1)
		{
			gl_viewwidth /= 2;
		}
	}

	gl_basecenterx = gl_viewwidth / 2;
	gl_basecentery = gl_viewheight / 2;

	gl_baseviewwindowx = 0;
	gl_basewindowcenterx = gl_viewwidth / 2;

	gl_baseviewwindowy = 0;
	gl_basewindowcentery = gl_viewheight / 2;

	gl_pspritexscale = gl_viewwidth / BASEVIDWIDTH;
	gl_pspriteyscale = ((vid.height*gl_pspritexscale*BASEVIDWIDTH)/BASEVIDHEIGHT)/vid.width;

	HWD.pfnFlushScreenTextures();
}

// -------------------+
// HWR_ShiftViewPort  : offset viewport according to current split
// -------------------+
static void HWR_ShiftViewPort(void)
{
	gl_centerx = gl_basecenterx;
	gl_viewwindowx = gl_baseviewwindowx;
	gl_windowcenterx = gl_basewindowcenterx;

	gl_centery = gl_basecentery;
	gl_viewwindowy = gl_baseviewwindowy;
	gl_windowcentery = gl_basewindowcentery;

	if (viewssnum > ( r_splitscreen > 1 ))
	{
		gl_viewwindowy += gl_viewheight;
		gl_windowcentery += gl_viewheight;
	}

	if (r_splitscreen > 1 && viewssnum & 1)
	{
		gl_viewwindowx += gl_viewwidth;
		gl_windowcenterx += gl_viewwidth;
	}
}

// Set view aiming, for the sky dome, the skybox,
// and the normal view, all with a single function.
static void HWR_SetTransformAiming(FTransform *trans, player_t *player, boolean skybox)
{
	// 1 = always on
	// 2 = chasecam only
	if (cv_glshearing.value == 1 || (cv_glshearing.value == 2 && R_IsViewpointThirdPerson(player, skybox)))
	{
		fixed_t fixedaiming = AIMINGTODY(aimingangle);
		trans->viewaiming = FIXED_TO_FLOAT(fixedaiming);
		trans->shearing = true;
		gl_aimingangle = 0;
	}
	else
	{
		trans->shearing = false;
		gl_aimingangle = aimingangle;
	}

	trans->anglex = (float)(gl_aimingangle>>ANGLETOFINESHIFT)*(360.0f/(float)FINEANGLES);
}

//
// Sets the shader state.
//
static void HWR_SetShaderState(void)
{
	hwdshaderoption_t state = (hwdshaderoption_t)cv_glshaders.value;

	if (!cv_glallowshaders.value)
		state = (hwdshaderoption_t)(cv_glshaders.value == HWD_SHADEROPTION_ON ? HWD_SHADEROPTION_NOCUSTOM : cv_glshaders.value);

	HWD.pfnSetSpecialState(HWD_SET_SHADERS, (INT32)state);
	HWD.pfnSetShader(SHADER_DEFAULT);
}

#ifdef RR3DS
// 3DS: [DIB] drawing breakdown by part. v=0 main view, 1 skybox.
namespace rr3ds_dib {
	enum { D_PREP, D_BSP, D_BATCH, D_STATIC, D_SSORT, D_SDRAW, D_NODES, D_FIN, D_N };
	static UINT64 acc[2][D_N];
	static UINT64 accPost = 0, accTotal = 0, accSkyTot = 0;
	static UINT64 nSpr[2], nTr[2], nBsp[2];
	static int frames = 0;
	static UINT64 last;
	static int cur = 0;
	static inline void Start(int v) { cur = v; last = I_GetPreciseTime(); }
	static inline void Mark(int seg) { UINT64 n = I_GetPreciseTime(); acc[cur][seg] += n - last; last = n; }
	static void Report(void)
	{
		const double us = 1000000.0 / (double)I_GetPrecisePrecision() / frames;
		static const char *nm[D_N] = {"prep","bsp","batch","static","ssort","sdraw","nodes","end"};
		RR_DIAGPRINT("[DIB] us/frame total=%.0f skybox=%.0f post=%.0f\n", accTotal*us, accSkyTot*us, accPost*us);
		{ // top 10 sprite types per frame (both views)
			char tb[256]; int to = 0;
			for (int k = 0; k < 10; k++)
			{
				int bi = -1; UINT32 bv = 0;
				for (int t = 0; t < NUMMOBJTYPES; t++) if (rr3ds_sprTypeCnt[t] > bv) { bv = rr3ds_sprTypeCnt[t]; bi = t; }
				if (bi < 0) break;
				to += snprintf(tb+to, sizeof(tb)-to, " %d=%.1f", bi, bv/(double)frames);
				rr3ds_sprTypeCnt[bi] = 0;
			}
			RR_DIAGPRINT("[SPRTYPE] mobjtype=sprites/frame:%s\n", tb);
			memset(rr3ds_sprTypeCnt, 0, sizeof(rr3ds_sprTypeCnt));
		}
		{ // sdraw per type (us/frame, draws/frame) + shadows + building/uploading sprites
			char tb[320]; int to = 0;
			for (int k = 0; k < 8; k++)
			{
				int bi = -1; UINT64 bv = 0;
				for (int t = 0; t < NUMMOBJTYPES; t++) if (rr3ds_sdTypeT[t] > bv) { bv = rr3ds_sdTypeT[t]; bi = t; }
				if (bi < 0) break;
				to += snprintf(tb+to, sizeof(tb)-to, " %d=%.0fus/%.1f", bi, bv*us, rr3ds_sdTypeN[bi]/(double)frames);
				rr3ds_sdTypeT[bi] = 0;
			}
#ifdef __3DS__
			RR_DIAGPRINT("[SDRAW] shadows=%.0f build=%.0fus n=%.1f (cmap %.1f) upload=%.0fus n=%.1f | type=us/draws:%s\n",
				rr3ds_sdShadowT*us, rr3ds_sprMakeT*us, rr3ds_sprMakeN/(double)frames, rr3ds_sprMakeCm/(double)frames,
				rr3ds_sprUpT*us, rr3ds_sprUpN/(double)frames, tb);
			rr3ds_sprMakeT = rr3ds_sprUpT = 0; rr3ds_sprMakeN = rr3ds_sprUpN = rr3ds_sprMakeCm = 0;
			{ char ub[360]; I_3DS_UpBreakdown(ub, sizeof(ub), frames); RR_DIAGPRINT("[UPDETAIL] all uploads: %s\n", ub); }
			RR_DIAGPRINT("[KARTCAP] kart_uploads=%.1f reused=%.1f per frame\n", rr3ds_kbUploads/(double)frames, rr3ds_kbReused/(double)frames);
			rr3ds_kbUploads = rr3ds_kbReused = 0;
#endif
			memset(rr3ds_sdTypeT, 0, sizeof(rr3ds_sdTypeT)); memset(rr3ds_sdTypeN, 0, sizeof(rr3ds_sdTypeN));
			rr3ds_sdShadowT = 0;
		}
		for (int v = 0; v < 2; v++)
		{
			char buf[256]; int o = 0;
			for (int i = 0; i < D_N; i++)
				o += snprintf(buf+o, sizeof(buf)-o, " %s=%.0f", nm[i], acc[v][i]*us);
			RR_DIAGPRINT("[DIB] %s:%s | spr=%.0f transl=%.0f bspnodos=%.0f\n", v ? "SKY " : "MAIN", buf,
				nSpr[v]/(double)frames, nTr[v]/(double)frames, nBsp[v]/(double)frames);
		}
		for (int v = 0; v < 2; v++)
		{
			using namespace rr3ds_sub;
			if (!framesV[v]) continue;
			const double u2 = 1000000.0 / (double)I_GetPrecisePrecision() / framesV[v];
			UINT64 sum = 0; for (int i = 0; i < S_N; i++) sum += accV[v][i];
			RR_DIAGPRINT("[SUB] %s bsp=%.0f fakeflat=%.0f planes=%.0f polyobj=%.0f addsprites=%.0f lines=%.0f walk=%.0f | subsect=%.0f segs=%.0f\n",
				v ? "SKY " : "MAIN", bspTotV[v]*u2, accV[v][S_FAKE]*u2, accV[v][S_PLANES]*u2, accV[v][S_POLY]*u2, accV[v][S_SPR]*u2, accV[v][S_LINES]*u2,
				((double)bspTotV[v] - (double)sum)*u2, nSubV[v]/(double)framesV[v], nLinesV[v]/(double)framesV[v]);
			RR_DIAGPRINT("[SUB] %s walls: front=%.0f processed=%.0f no_cache=%.0f angle_reused=%.0f processing_time=%.0f us\n",
				v ? "SKY " : "MAIN", nFront[v]/(double)framesV[v], nProc[v]/(double)framesV[v], nMiss[v]/(double)framesV[v],
				nAngReuse[v]/(double)framesV[v], segT[v]*u2);
			RR_DIAGPRINT("[SUB] %s wall breakdown: signature=%.0f use_cache=%.0f compute_misses=%.0f rest=%.0f us | memo A1 hit=%d miss=%d (both views)\n", // /
				v ? "SKY " : "MAIN", sigT[v]*u2, useT[v]*u2, missT[v]*u2, ((double)segT[v] - (double)sigT[v] - (double)useT[v] - (double)missT[v])*u2,
				rr3ds_static::stMemoHit, rr3ds_static::stMemoMiss);
		}
		memset(rr3ds_sub::sigT, 0, sizeof(rr3ds_sub::sigT)); memset(rr3ds_sub::useT, 0, sizeof(rr3ds_sub::useT)); memset(rr3ds_sub::missT, 0, sizeof(rr3ds_sub::missT)); rr3ds_static::stMemoHit = rr3ds_static::stMemoMiss = 0; // /
		{
			const double u3 = 1000000.0 / (double)I_GetPrecisePrecision() / frames;
			RR_DIAGPRINT("[DIB] sky(background) MAIN=%.0f SKY=%.0f us/frame\n", rr3ds_sub::skyBgV[0]*u3, rr3ds_sub::skyBgV[1]*u3);
		}
		memset(rr3ds_sub::accV, 0, sizeof(rr3ds_sub::accV)); memset(rr3ds_sub::nSubV, 0, sizeof(rr3ds_sub::nSubV));
		memset(rr3ds_sub::nLinesV, 0, sizeof(rr3ds_sub::nLinesV)); memset(rr3ds_sub::bspTotV, 0, sizeof(rr3ds_sub::bspTotV));
		memset(rr3ds_sub::skyBgV, 0, sizeof(rr3ds_sub::skyBgV)); memset(rr3ds_sub::framesV, 0, sizeof(rr3ds_sub::framesV)); memset(rr3ds_sub::segT, 0, sizeof(rr3ds_sub::segT)); memset(rr3ds_sub::nFront, 0, sizeof(rr3ds_sub::nFront));
		memset(rr3ds_sub::nProc, 0, sizeof(rr3ds_sub::nProc)); memset(rr3ds_sub::nMiss, 0, sizeof(rr3ds_sub::nMiss)); memset(rr3ds_sub::nAngReuse, 0, sizeof(rr3ds_sub::nAngReuse));
		RR_DIAGPRINT("[DIB] skybox: cv=%d viewpoint=%d drawsky=%d\n", (int)cv_skybox.value,
			(int)(players[displayplayers[0]].skybox.viewpoint != NULL), (int)drawsky);
		memset(acc, 0, sizeof(acc)); memset(nSpr,0,sizeof(nSpr)); memset(nTr,0,sizeof(nTr)); memset(nBsp,0,sizeof(nBsp));
		accPost = accTotal = accSkyTot = 0; frames = 0;
	}
}
#define DIB_START(v) rr3ds_dib::Start(v)
#define DIB_MARK(s) rr3ds_dib::Mark(rr3ds_dib::s)
#else
#define DIB_START(v)
#define DIB_MARK(s)
#endif

#ifdef RR3DS
// ===========================================================================
// MAP PACKAGE GENERATOR ("precompute everything on the PC").
// With sdmc:/3ds/rr_genmap = "1" (set by rrbench "generate"), the game walks
// ALL zones of the map with the same code that draws (slopes, FOFs, offsets
// same) and stores its triangles in sdmc:/3ds/RingRacers3ds/cache/maps/<MAP>.rr3.
// THROTTLE: few zones per frame, file written and memory freed on each
// batch; if memory drops below the margin, it resumes where it left off (.prog).
// For now it ONLY generates and measures (the game does not yet use it for drawing).
// FORMAT v2: "RR3G" u32 ver=2, u32 numsubsectors, u32 numsegs, u32 fingerprint; then items:
// u32 item, u16 npolys, per poly: u8 kind, char name[8] (base texture or flat; 0 if none),
//   i32 a, i32 b, u32 flags, f32 sx, f32 sy, u8 n, n x {f32 x,y,z,s,t; u32 color}
// fingerprint = FNV-1a of the map's vertices/lines/sectors (to know whether the .rr3 is valid).
// (old v1:) "RR3G" u32 ver=1, u32 numsubsectors, u32 numsegs; then items:
// u32 item (0..2*nsub-1 = floor/ceiling of zone item/2; >= 2*nsub = seg), u16 npolys,
// per poly: u8 kind, i32 a, i32 b, i32 flat (levelflat index or -1), u32 flags,
//   f32 sx, f32 sy, u8 n, n x {f32 x,y,z,s,t; u32 color}
//   final: u32 0xFFFFFFFF, "RR3F", u32 items, u32 polys, u32 verts
// Translucent walls/floors are NOT included yet (marked "never").
// ===========================================================================
extern "C" const void *I_3DS_StaticVerts(void);
#ifdef __3DS__
#include <sys/stat.h>
#include <malloc.h>
#endif
#ifndef __3DS__
#include "../command.h" // COM_BufAddText (quit when finished)
#endif
namespace rr3ds_gen {
	static int st = -1;            // -1 unread, 0 off, 1 active
	static INT32 curMap = -1;
	static size_t next = 0;
	static FILE *out = NULL;
	static char path[160], prog[170];
	static UINT32 nItems = 0, nPolys = 0, nVerts = 0, nBatch = 0;
	static UINT64 t0 = 0;
	static void W32(UINT32 v) { fwrite(&v, 4, 1, out); }
	// map fingerprint (FNV-1a). The same on PC and 3DS if the map is the same.
	static UINT32 Fingerprint(void)
	{
		UINT32 h = 2166136261u;
		auto mix = [&h](UINT32 v) { for (int k = 0; k < 4; k++) { h ^= (v >> (k * 8)) & 0xFF; h *= 16777619u; } };
		mix((UINT32)numvertexes); mix((UINT32)numlines); mix((UINT32)numsectors); mix((UINT32)numsubsectors);
		for (size_t i = 0; i < numvertexes; i++) { mix((UINT32)vertexes[i].x); mix((UINT32)vertexes[i].y); }
		for (size_t i = 0; i < numsectors; i++) { mix((UINT32)sectors[i].floorheight); mix((UINT32)sectors[i].ceilingheight); }
		return h;
	}
	// GEOMETRY-ONLY fingerprint (counts + vertices, NO sector heights). Maps with moving sectors
	// (RobotnikCoaster: the coaster) change heights every tic, so Fingerprint() above differs between the PC preparer and
	// the 3DS and the .rrv was never accepted. .rrv version 3 uses this one; version 2 keeps the old one.
	static UINT32 GeoFingerprint(void)
	{
		UINT32 h = 2166136261u ^ 0x47454F33u;
		auto mix = [&h](UINT32 v) { for (int k = 0; k < 4; k++) { h ^= (v >> (k * 8)) & 0xFF; h *= 16777619u; } };
		mix((UINT32)numvertexes); mix((UINT32)numlines); mix((UINT32)numsectors); mix((UINT32)numsubsectors);
		for (size_t i = 0; i < numvertexes; i++) { mix((UINT32)vertexes[i].x); mix((UINT32)vertexes[i].y); }
		return h;
	}
#ifndef __3DS__
	// map queue (written by buildpc.sh): removes the current one, loads the next with "map"
	// in the SAME game session. Empty queue = quit. If the game crashes, the first line is the culprit.
	static void Next(void)
	{
		const char *cur = G_BuildMapName(gamemap);
		std::vector<std::string> v;
		if (FILE *f = fopen(RR3DS_DIR "/rr_queue.txt", "rb"))
		{
			char ln[128];
			while (fgets(ln, sizeof ln, f))
			{
				std::string m(ln);
				while (!m.empty() && (m.back() == '\n' || m.back() == '\r' || m.back() == ' ')) m.pop_back();
				if (!m.empty() && strcasecmp(m.c_str(), cur)) v.push_back(m);
			}
			fclose(f);
		}
		if (FILE *f = fopen(RR3DS_DIR "/rr_queue.txt", "wb")) { for (auto &m : v) fprintf(f, "%s\n", m.c_str()); fclose(f); }
		if (v.empty()) { RR_DIAGPRINT("[GEN] queue empty: exiting\n"); COM_BufAddText("quit\n"); return; }
		RR_DIAGPRINT("[GEN] NEXT %s (%u left)\n", v[0].c_str(), (unsigned)v.size());
		// "map -force" requests cheats (locked maps) -> direct switch, like -warp
		const INT32 num = G_FindMapByNameOrCode(v[0].c_str(), NULL);
		if (num <= 0 || !mapheaderinfo[num-1])
		{
			RR_DIAGPRINT("[GEN] cannot find map %s: exiting (the preparer logs it and moves on)\n", v[0].c_str());
			COM_BufAddText("quit\n"); return;
		}
		INT32 gt = G_GuessGametypeByTOL(mapheaderinfo[num-1]->typeoflevel);
		if (gt < 0) gt = GT_RACE;
		D_MapChange((UINT16)num, gt, false, true, 0, false, false);
	}
#endif
	// zones that crash the game while generating: recorded in skip_<MAP>.txt and skipped
	// on resume (the preparer relaunches the game). That way the map still finishes and the culprit is recorded.
	static std::vector<UINT8> skipSet;
	static volatile long curSub = -1;
	static char skipPath[180];
	static void ReadSkip(void)
	{
		skipSet.assign(numsubsectors, 0);
		snprintf(skipPath, sizeof skipPath, RR3DS_DIR "/skip_%s.txt", G_BuildMapName(gamemap));
		if (FILE *f = fopen(skipPath, "rb"))
		{
			long n; unsigned c = 0;
			while (fscanf(f, "%ld", &n) == 1) if (n >= 0 && (size_t)n < skipSet.size()) { skipSet[n] = 1; c++; }
			fclose(f);
			if (c) RR_DIAGPRINT("[GEN] skipping %u zone(s) that crashed the game (%s)\n", c, skipPath);
		}
	}
	static void OnFall(int sig)
	{
		if (curSub >= 0) { if (FILE *f = fopen(skipPath, "ab")) { fprintf(f, "%ld\n", (long)curSub); fclose(f); } }
		fprintf(stderr, "[GEN] CRASHED in zone %ld (signal %d): marked to be skipped\n", (long)curSub, sig);
#ifdef _WIN32
		{ // stack (addresses relative to the .exe) to find the culprit function
			void *fr[40]; const unsigned short n = RtlCaptureStackBackTrace(0, 40, fr, NULL);
			const uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
			fprintf(stderr, "[GEN] STACK base=%p:", (void *)base);
			for (unsigned short i = 0; i < n; i++) fprintf(stderr, " +%llx", (unsigned long long)((uintptr_t)fr[i] - base));
			fprintf(stderr, "\n");
		}
#endif
		fflush(stderr);
		_exit(3);
	}
	static void SaveProg(void)
	{
		FILE *f = fopen(prog, "wb");
		if (!f) return;
		UINT32 d[5] = { (UINT32)next, (UINT32)ftell(out), nItems, nPolys, nVerts };
		fwrite(d, 4, 5, f); fclose(f);
	}
}
#ifndef __3DS__
// ===========================================================================
// PRECOMPUTED VISIBILITY -> <MAP>.rrv (PC PREPARER only)
// For each subsector S: which subsectors T can be seen from S. Rays from
// points of S (3 camera heights) to points of T (the whole floor-to-ceiling column),
// narrowing the vertical window at every 2-sided line (like P_CheckSight).
// CONSERVATIVE: sectors with a tag (they move / FOF / triggers) and polyobjects = open;
// FOFs do not occlude; sky ceiling on both sides = no limit. Only affects DRAWING.
// Format: "RRV1" u32 ver=1, u32 fingerprint (= .rr3), u32 nsub, u32 distMax;
// per subsector: u16 nRuns, nRuns x (varint skip, varint count)
//   final: "RRVF" u32 totalVisibles
// ===========================================================================
#include <thread>
#include <atomic>
namespace rr3ds_vis {
	struct Pt { double x, y; };
	static std::vector<std::vector<Pt>> pts;   // sample points per subsector
	static std::vector<double> bb;             // box per subsector (l,r,b,t)
	static std::vector<UINT8> openSec;         // 1 = moving/special sector -> never occludes
	static const double kDist = 12288.0;       // = RR3DS_WORLDDIST (anything farther is not drawn)
	static const double kInf = 1e12;

	static double FZ(const sector_t *s, double x, double y) { return FIXED_TO_FLOAT(P_GetSectorFloorZAt(s, FLOAT_TO_FIXED(x), FLOAT_TO_FIXED(y))); }
	static double CZ(const sector_t *s, double x, double y) { return FIXED_TO_FLOAT(P_GetSectorCeilingZAt(s, FLOAT_TO_FIXED(x), FLOAT_TO_FIXED(y))); }

	struct Ray // per-thread state
	{
		std::vector<UINT32> stamp; UINT32 cur = 0;
		std::vector<std::pair<double, const line_t *>> hits;
		// true = some point of the column [qlo,qhi] at (qx,qy) is visible from (px,py,pz)
		bool See(double px, double py, double pz, double qx, double qy, double qlo, double qhi)
		{
			const double dx = qx - px, dy = qy - py;
			const double len = sqrt(dx * dx + dy * dy);
			if (len < 1.0) return true;
			if (++cur == 0) { std::fill(stamp.begin(), stamp.end(), 0); cur = 1; }
			hits.clear();
			// walk the blockmap blocks (DDA)
			const double ox = FIXED_TO_FLOAT(bmaporgx), oy = FIXED_TO_FLOAT(bmaporgy), B = 128.0;
			double fx = (px - ox) / B, fy = (py - oy) / B, tx = (qx - ox) / B, ty = (qy - oy) / B;
			int bx = (int)floor(fx), by = (int)floor(fy); const int ex = (int)floor(tx), ey = (int)floor(ty);
			const int sx = (tx > fx) ? 1 : -1, sy = (ty > fy) ? 1 : -1;
			const double ddx = (tx != fx) ? fabs(1.0 / (tx - fx)) : kInf, ddy = (ty != fy) ? fabs(1.0 / (ty - fy)) : kInf;
			double mx = (tx != fx) ? ((sx > 0 ? (bx + 1 - fx) : (fx - bx)) * ddx) : kInf;
			double my = (ty != fy) ? ((sy > 0 ? (by + 1 - fy) : (fy - by)) * ddy) : kInf;
			for (int guard = 0; guard < 4096; guard++)
			{
				if (bx >= 0 && by >= 0 && bx < bmapwidth && by < bmapheight)
				{
					const INT32 off = blockmap[by * bmapwidth + bx];
					for (const INT32 *l = blockmaplump + off + 1; *l != -1; l++)
					{
						const line_t *ld = &lines[*l];
						if (stamp[*l] == cur) continue;
						stamp[*l] = cur;
						const double ax = FIXED_TO_FLOAT(ld->v1->x), ay = FIXED_TO_FLOAT(ld->v1->y);
						const double lx = FIXED_TO_FLOAT(ld->v2->x) - ax, ly = FIXED_TO_FLOAT(ld->v2->y) - ay;
						const double den = dx * ly - dy * lx;
						if (fabs(den) < 1e-9) continue;
						const double t = ((ax - px) * ly - (ay - py) * lx) / den; // along the ray
						const double u = ((ax - px) * dy - (ay - py) * dx) / den; // along the line
						if (t <= 1e-6 || t >= 1.0 - 1e-6 || u < 0.0 || u > 1.0) continue;
						// one-sided wall on the ray = ALWAYS occludes (order does not matter): stop right away
						// (before, the whole ray was walked and sorted before discovering it).
						if (!ld->polyobj && (!ld->backsector || !ld->frontsector)) return false;
						hits.push_back({ t, ld });
					}
				}
				if (bx == ex && by == ey) break;
				if (mx < my) { mx += ddx; bx += sx; } else { my += ddy; by += sy; }
				if (mx > 1.0 + ddx && my > 1.0 + ddy) break;
			}
			std::sort(hits.begin(), hits.end(), [](const std::pair<double, const line_t *> &a, const std::pair<double, const line_t *> &b) { return a.first < b.first; });
			double bot = (qlo - pz) / len, top = (qhi >= kInf ? kInf : (qhi - pz) / len);
			for (auto &h : hits)
			{
				const line_t *ld = h.second;
				if (ld->polyobj) continue;
				if (!ld->backsector || !ld->frontsector) return false;
				const sector_t *f = ld->frontsector, *b = ld->backsector;
				if (openSec[f - sectors] || openSec[b - sectors]) continue;
				const double d = h.first * len;
				const double cx = px + dx * h.first, cy = py + dy * h.first;
				const double ob = std::max(FZ(f, cx, cy), FZ(b, cx, cy));
				const bool sky = (f->ceilingpic == skyflatnum && b->ceilingpic == skyflatnum);
				if (!sky)
				{
					const double ot = std::min(CZ(f, cx, cy), CZ(b, cx, cy));
					if (ot <= ob) return false;
					top = std::min(top, (ot - pz) / d);
				}
				bot = std::max(bot, (ob - pz) / d);
				if (bot >= top) return false;
			}
			return true;
		}
	};

	static bool Near(size_t a, size_t b)
	{
		const double *A = &bb[a * 4], *Bx = &bb[b * 4];
		const double dx = std::max(0.0, std::max(A[0] - Bx[1], Bx[0] - A[1]));
		const double dy = std::max(0.0, std::max(A[2] - Bx[3], Bx[2] - A[3]));
		return dx * dx + dy * dy <= kDist * kDist;
	}

	static void Gen(UINT32 fingerprint)
	{
		char path[200];
		snprintf(path, sizeof path, RR3DS_DIR "/cache/maps/%s.rrv", G_BuildMapName(gamemap));
		if (FILE *f = fopen(path, "rb")) // version 2 (corrected open sectors); v1 is redone
		{
			UINT32 h[4] = {0}; char tail[4] = {0};
			const bool ok = fread(h, 4, 4, f) == 4 && !memcmp(h, "RRV1", 4) && h[1] == 3 && h[2] == fingerprint
				&& !fseek(f, -8, SEEK_END) && fread(tail, 1, 4, f) == 4 && !memcmp(tail, "RRVF", 4);
			fclose(f);
			if (ok) { RR_DIAGPRINT("[VIS] %s already exists\n", path); return; }
		}
		remove(path); // no stale .rrv = buildpc.sh knows it is being computed (does not time out at 90 s)
		const UINT64 t0 = I_GetPreciseTime();
		const size_t N = numsubsectors;
		pts.assign(N, {}); bb.assign(N * 4, 0.0);
		openSec.assign(numsectors, 0);
		size_t nOpen = 0;
		// "open" = a sector that CAN move: its tag (nonzero) is named by some line with
		// special (args[0..1] or line tag), or already has a mover. (Before: any tag -> 911/911 open.)
		{
			std::vector<INT32> ref;
			for (size_t i = 0; i < numlines; i++)
			{
				if (!lines[i].special) continue;
				for (int k = 0; k < 2; k++) if (lines[i].args[k]) ref.push_back(lines[i].args[k]);
				for (UINT16 k = 0; k < lines[i].tags.count; k++) if (lines[i].tags.tags[k]) ref.push_back(lines[i].tags.tags[k]);
			}
			std::sort(ref.begin(), ref.end()); ref.erase(std::unique(ref.begin(), ref.end()), ref.end());
			for (size_t i = 0; i < numsectors; i++)
			{
				bool o = sectors[i].floordata || sectors[i].ceilingdata;
				for (UINT16 k = 0; k < sectors[i].tags.count && !o; k++)
				{
					const INT32 t = sectors[i].tags.tags[k];
					o = t && std::binary_search(ref.begin(), ref.end(), t);
				}
				if (o) { openSec[i] = 1; nOpen++; }
			}
		}
		for (size_t s = 0; s < N; s++)
		{
			const subsector_t *sub = &subsectors[s];
			double cx = 0, cy = 0; int n = 0;
			double *b = &bb[s * 4]; b[0] = b[2] = kInf; b[1] = b[3] = -kInf;
			std::vector<Pt> vs;
			for (INT32 k = 0; k < sub->numlines; k++)
			{
				const vertex_t *v = segs[sub->firstline + k].v1;
				if (!v) continue;
				const Pt p{ FIXED_TO_FLOAT(v->x), FIXED_TO_FLOAT(v->y) };
				vs.push_back(p); cx += p.x; cy += p.y; n++;
				b[0] = std::min(b[0], p.x); b[1] = std::max(b[1], p.x); b[2] = std::min(b[2], p.y); b[3] = std::max(b[3], p.y);
			}
			if (!n) continue;
			cx /= n; cy /= n;
			pts[s].push_back({ cx, cy });
			const int step = std::max(1, n / 4);
			for (int k = 0; k < n && pts[s].size() < 5; k += step)
				pts[s].push_back({ cx + 0.8 * (vs[k].x - cx), cy + 0.8 * (vs[k].y - cy) });
		}
		std::vector<std::vector<UINT8>> vis(N, std::vector<UINT8>((N + 7) / 8, 0));
		std::atomic<size_t> nextS{0}, done{0};
		unsigned nth = std::max(1u, std::thread::hardware_concurrency());
		auto work = [&]()
		{
			Ray r; r.stamp.assign(numlines, 0);
			for (;;)
			{
				const size_t s = nextS++;
				if (s >= N) break;
				if (s % 500 == 0) RR_DIAGPRINT("[VIS] progress %u/%u (%.0f s)\n", (unsigned)s, (unsigned)N, (double)(I_GetPreciseTime() - t0) / (double)I_GetPrecisePrecision());
				std::vector<UINT8> &row = vis[s];
				row[s >> 3] |= (UINT8)(1 << (s & 7));
				if (pts[s].empty()) { done++; continue; }
				const sector_t *ss = subsectors[s].sector;
				for (size_t t = 0; t < N; t++)
				{
					if (t == s || pts[t].empty() || !Near(s, t)) continue;
					const sector_t *ts = subsectors[t].sector;
					bool seen = false;
					for (size_t a = 0; a < pts[s].size() && !seen; a++)
					{
						const Pt &P = pts[s][a];
						const double fz = FZ(ss, P.x, P.y);
						const double cz = (ss->ceilingpic == skyflatnum) ? kInf : CZ(ss, P.x, P.y);
						static const double hz[3] = { 32.0, 160.0, 400.0 }; // camera: kart, behind, high
						for (int zi = 0; zi < 3 && !seen; zi++)
						{
							double pz = fz + hz[zi];
							if (pz > cz - 8.0) { if (zi) break; pz = (fz + cz) * 0.5; }
							for (size_t c = 0; c < pts[t].size() && !seen; c++)
							{
								const Pt &Q = pts[t][c];
								const double qlo = FZ(ts, Q.x, Q.y);
								const double qhi = (ts->ceilingpic == skyflatnum) ? kInf : CZ(ts, Q.x, Q.y);
								seen = r.See(P.x, P.y, pz, Q.x, Q.y, qlo, qhi);
							}
						}
					}
					if (seen) row[t >> 3] |= (UINT8)(1 << (t & 7));
				}
				done++;
			}
		};
		RR_DIAGPRINT("[VIS] %s: %u zones, %u open sectors (tagged) of %u, %u threads\n", G_BuildMapName(gamemap),
			(unsigned)N, (unsigned)nOpen, (unsigned)numsectors, nth);
		std::vector<std::thread> th;
		for (unsigned i = 0; i < nth; i++) th.emplace_back(work);
		for (auto &x : th) x.join();
		// direct neighbors of each zone too (margin against gaps at the edges)
		// (a subsector touches another through a seg with a partner: we use the lines' sectors)
		FILE *f = fopen(path, "wb");
		if (!f) { RR_DIAGPRINT("[VIS] cannot create %s\n", path); return; }
		const UINT32 hdr[5] = { 0, 3, fingerprint, (UINT32)N, (UINT32)kDist };
		fwrite("RRV1", 1, 4, f); fwrite(&hdr[1], 4, 4, f);
		auto varint = [&](UINT32 v) { do { UINT8 b = v & 0x7F; v >>= 7; if (v) b |= 0x80; fputc(b, f); } while (v); };
		UINT64 total = 0; size_t maxv = 0;
		for (size_t s = 0; s < N; s++)
		{
			std::vector<std::pair<UINT32, UINT32>> runs;
			size_t i = 0, cnt = 0;
			while (i < N)
			{
				size_t j = i; while (j < N && !(vis[s][j >> 3] & (1 << (j & 7)))) j++;
				if (j >= N) break;
				size_t k = j; while (k < N && (vis[s][k >> 3] & (1 << (k & 7)))) k++;
				runs.push_back({ (UINT32)(j - i), (UINT32)(k - j) }); cnt += k - j; i = k;
			}
			total += cnt; maxv = std::max(maxv, cnt);
			const UINT16 nr = (UINT16)std::min<size_t>(runs.size(), 65535);
			fwrite(&nr, 2, 1, f);
			for (size_t r = 0; r < nr; r++) { varint(runs[r].first); varint(runs[r].second); }
		}
		const UINT32 tot32 = (UINT32)total;
		fwrite("RRVF", 1, 4, f); fwrite(&tot32, 4, 1, f);
		const long sz = ftell(f); fclose(f);
		const double secs = (double)(I_GetPreciseTime() - t0) / (double)I_GetPrecisePrecision();
		RR_DIAGPRINT("[VIS] DONE %s: visible mean=%.1f%% max=%.1f%% | %ld KB | %.0f s\n", path,
			N ? 100.0 * (double)total / ((double)N * N) : 0.0, N ? 100.0 * (double)maxv / N : 0.0, sz >> 10, secs);
		decltype(pts)().swap(pts);
	}
}
#endif

static void RR3DS_GenStep(void)
{
	using namespace rr3ds_static;
	namespace G = rr3ds_gen;
	if (G::st < 0)
	{
		G::st = 0;
		G::st = RRCfg_On("genmap"); // rr_config.txt
		if (G::st) RR_DIAGPRINT("[GEN] map package GENERATE mode: ON\n");
	}
	if (G::st != 1 || !enabled || !numsubsectors)
		return;
	if (G::curMap != gamemap)
	{
		if (G::out) { fclose(G::out); G::out = NULL; }
		G::curMap = gamemap; G::next = 0; G::nItems = G::nPolys = G::nVerts = G::nBatch = 0;
		RR3DS_MKDIR(RR3DS_DIR "/cache"); RR3DS_MKDIR(RR3DS_DIR "/cache/maps");
		snprintf(G::path, sizeof G::path, RR3DS_DIR "/cache/maps/%s.rr3", G_BuildMapName(gamemap));
		snprintf(G::prog, sizeof G::prog, "%s.prog", G::path);
#ifndef __3DS__
		rr3ds_vis::Gen(G::GeoFingerprint()); // .rrv v3 (geometry-only fingerprint) before the .rr3 (fast, multithreaded)
#endif
		// already complete?
		FILE *f = fopen(G::path, "rb");
		boolean done = false;
		if (f)
		{
			char tag[4] = {0};
			UINT32 ver = 0; // an old v1 .rr3 does NOT count as complete (it is redone as v2)
			if (fseek(f, 4, SEEK_SET) || fread(&ver, 4, 1, f) != 1) ver = 0;
			if (ver == 2 && !fseek(f, -16, SEEK_END) && fread(tag, 1, 4, f) == 4 && !memcmp(tag, "RR3F", 4)) done = true;
			fclose(f);
		}
		if (done)
		{
			RR_DIAGPRINT("[GEN] %s is already complete\n", G::path); G::next = numsubsectors;
#ifndef __3DS__
			G::Next(); // PC preparer: move on to the next map (without reopening the game)
#endif
			return;
		}
		// resume?
		FILE *pf = fopen(G::prog, "rb");
		UINT32 d[5];
		if (pf && fread(d, 4, 5, pf) == 5 && (G::out = fopen(G::path, "r+b")) && !fseek(G::out, (long)d[1], SEEK_SET))
		{
			G::next = d[0]; G::nItems = d[2]; G::nPolys = d[3]; G::nVerts = d[4];
			RR_DIAGPRINT("[GEN] %s: resuming from zone %u of %u\n", G::path, (unsigned)G::next, (unsigned)numsubsectors);
		}
		else
		{
			if (G::out) fclose(G::out);
			G::out = fopen(G::path, "wb");
			if (!G::out) { RR_DIAGPRINT("[GEN] cannot create %s\n", G::path); G::next = numsubsectors; if (pf) fclose(pf); return; }
			fwrite("RR3G", 1, 4, G::out); G::W32(2); G::W32((UINT32)numsubsectors); G::W32((UINT32)numsegs); G::W32(G::Fingerprint());
			RR_DIAGPRINT("[GEN] %s: starting (%u zones, %u walls)\n", G::path, (unsigned)numsubsectors, (unsigned)numsegs);
		}
		if (pf) fclose(pf);
		G::t0 = I_GetPreciseTime();
		G::ReadSkip();
	}
	if (!G::out || G::next >= numsubsectors)
		return;
#ifdef __3DS__
	// MEMORY THROTTLE: with less than 384 KB free on the heap, wait (and warn).
	// used to be 3 MB -> it never started (the measured free heap is around 2 MB and
	// generation barely requests memory: it uses the linear pool that already exists).
	{
		struct mallinfo mi = mallinfo();
		static int warnings = 0;
		if (mi.fordblks < 384u * 1024 && warnings < 20)
		{
			if ((warnings++ % 5) == 0) RR_DIAGPRINT("[GEN] low memory (%u KB free): waiting\n", (unsigned)(mi.fordblks >> 10));
			return;
		}
	}
#endif
	if (!I_3DS_StaticReset())
	{
		static unsigned failures = 0; // warn (it used to stop without saying anything)
		if ((failures++ % 300) == 0) RR_DIAGPRINT("[GEN] could not empty the pool (already drawn this frame): %u times\n", failures);
		return;
	}
	gen++; polys.clear(); groups.clear(); groupMap.clear();

	const fixed_t svx = viewx, svy = viewy, svz = viewz, sdx = dup_viewx, sdy = dup_viewy, sdz = dup_viewz;
	const size_t K = 12; // THROTTLE: zones per frame
	const size_t end = std::min(G::next + K, (size_t)numsubsectors);
	G::busy = true;
	void (*prevSegv)(int) = signal(SIGSEGV, G::OnFall);
	for (size_t num = G::next; num < end; num++)
	{
		const subsector_t *sub = &subsectors[num];
		if (!sub->numlines) continue;
		if (num < G::skipSet.size() && G::skipSet[num]) continue;
		G::curSub = (long)num;
		float cx = 0.0f, cy = 0.0f;
		for (INT32 k = 0; k < sub->numlines; k++)
		{
			// v1 (map vertex), not pv1: pv1 is NULL in some segs (CAUSE of the crashes
			// en BalloonPark/CoastalTemple/DeathEgg/EspressoLane/LostColony, PC y 3DS)
			const vertex_t *vv = segs[sub->firstline + k].v1;
			if (!vv) continue;
			cx += FIXED_TO_FLOAT(vv->x); cy += FIXED_TO_FLOAT(vv->y);
		}
		cx /= sub->numlines; cy /= sub->numlines;
		viewx = dup_viewx = FLOAT_TO_FIXED(cx);
		viewy = dup_viewy = FLOAT_TO_FIXED(cy);
		for (int pass = 0; pass < 2; pass++)
		{
			// pass 0: camera very high (floors, FOF ceilings); pass 1: very low (ceilings, FOF bases)
			viewz = dup_viewz = pass ? -30000 * FRACUNIT : 30000 * FRACUNIT;
			gld_clipper_Clear();
			validcount++;
			rr3ds_seccache::stamp++;
			stamp++;
			HWR_Subsector(num);
			EndCapture();
		}
	}
	G::busy = false;
	G::curSub = -1; signal(SIGSEGV, prevSegv);
	viewx = svx; viewy = svy; viewz = svz; dup_viewx = sdx; dup_viewy = sdy; dup_viewz = sdz;

	// dump what was captured in this batch
	const UINT8 *vb = (const UINT8 *)I_3DS_StaticVerts();
	for (size_t i = 0; i < items.size() && vb; i++)
	{
		const SItem &it = items[i];
		if (it.state != ST_VALID || it.gen != gen || !it.numPolys) continue;
		G::W32((UINT32)i);
		const UINT16 np = it.numPolys; fwrite(&np, 2, 1, G::out);
		for (UINT32 p = it.firstPoly; p < it.firstPoly + it.numPolys; p++)
		{
			const SPoly &sp = polys[p];
			const SGroup &g = groups[sp.group];
			const UINT8 kind = (UINT8)g.req.kind; fwrite(&kind, 1, 1, G::out);
			// v2: NAME (8 chars) instead of a number: numbers differ between PC and 3DS
			char nm[8] = {0};
			const INT32 fl = (g.req.kind == 2 && g.req.p) ? (INT32)((levelflat_t *)g.req.p - levelflats) : -1;
			if (g.req.kind == 1)
			{
				INT32 tx = (g.req.b >= 0 && g.req.b < numtextures) ? g.req.b : g.req.a; // base (the animation is resolved when drawing)
				if (tx > 0 && tx < numtextures && textures[tx]) memcpy(nm, textures[tx]->name, 8);
			}
			else if (fl >= 0 && (size_t)fl < numlevelflats) memcpy(nm, levelflats[fl].name, 8);
			fwrite(nm, 1, 8, G::out);
			G::W32((UINT32)g.req.a); G::W32((UINT32)g.req.b); G::W32((UINT32)g.flags);
			fwrite(&sp.sx, 4, 1, G::out); fwrite(&sp.sy, 4, 1, G::out);
			fwrite(&sp.n, 1, 1, G::out);
			fwrite(vb + (size_t)sp.base * 24, 24, sp.n, G::out);
			G::nPolys++; G::nVerts += sp.n;
		}
		G::nItems++;
	}
	for (SGroup &g : groups) g.idx.clear();
	// leave the pool clean for this frame's normal drawing
	I_3DS_StaticReset();
	gen++; polys.clear(); groups.clear(); groupMap.clear();

	G::next = end;
	// write to the SD every 8 batches (before: fflush + open/close .prog on EVERY batch: very slow)
	{ fflush(G::out); G::SaveProg(); } // every batch (if it crashes, it resumes without trailing garbage)
	if (++G::nBatch % 25 == 0)
		RR_DIAGPRINT("[GEN] %u/%u zones | %u polys, %u verts, %u KB\n", (unsigned)G::next, (unsigned)numsubsectors,
			(unsigned)G::nPolys, (unsigned)G::nVerts, (unsigned)(ftell(G::out) >> 10));
	if (G::next >= numsubsectors)
	{
		G::W32(0xFFFFFFFFu); fwrite("RR3F", 1, 4, G::out); G::W32(G::nItems); G::W32(G::nPolys); G::W32(G::nVerts);
		const long sz = ftell(G::out);
		fclose(G::out); G::out = NULL;
		remove(G::prog);
		const double secs = (double)(I_GetPreciseTime() - G::t0) / (double)I_GetPrecisePrecision();
		RR_DIAGPRINT("[GEN] DONE %s: %u items, %u polys, %u verts, %ld KB (on GPU ~%u KB), %.0f s\n", G::path,
			(unsigned)G::nItems, (unsigned)G::nPolys, (unsigned)G::nVerts, sz >> 10, (unsigned)(G::nVerts * 24 >> 10), secs);
#ifndef __3DS__
		G::Next(); // next map in the queue (or exit)
#endif
	}
}
#endif

#ifdef __3DS__
// PER-MAP RAM TABLE. Every ~1 s it measures heap (malloc) and linear memory and keeps
// the WORST value for the map. On map change it appends a row to
// sdmc:/3ds/ringracers/ram_table.csv; every ~10 s it rewrites ram_actual.txt (if
// it crashes, the last measurement remains).
extern "C" void RR3DS_LinFallbackStats(unsigned *n, unsigned *bytes, unsigned *peak);
extern "C" UINT32 linearSpaceFree(void);
extern "C" void RR3DS_PurgeStats(unsigned *n, unsigned *kb);
extern "C" unsigned int g_3ds_reserved_kb; // i_main.c (block that simulates 64 MB)
// new columns.
// heap_used_max = WITHOUT counting the 64 MB reservation (before it was added: 65586 = 32768 + actual use)
// purgable/level/patch = zone at the moment of the peak
// needed_KB = peak - purgeable (what the map really needs; the cache can be released)
// available_KB = free heap + free linear + purgeable  ("all the holes together")
// purges = times the cache was released due to lack of memory
static const char *const kRamHdr = "map,zones,walls,heap_used_max_KB,needed_KB,purgeable_KB,level_KB,patch_KB,"
	"heap_free_min_KB,linear_free_min_KB,available_min_KB,safety_net_KB,purges,purged_KB,seconds\n";
namespace rr3ds_ram {
	static INT32 map = -1; static int frames = 0, secs = 0;
	static unsigned usedMax = 0, freeMin = ~0u, linMin = ~0u, redMax0 = 0, dispMin = ~0u;
	static unsigned purgAtMax = 0, levelAtMax = 0, patchAtMax = 0, purges0 = 0, purgKB0 = 0;
	static void Row(FILE *f)
	{
		unsigned n, b, pk; RR3DS_LinFallbackStats(&n, &b, &pk);
		unsigned pn, pkb; RR3DS_PurgeStats(&pn, &pkb);
		const unsigned u = usedMax >> 10;
		fprintf(f, "%s,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%d\n", G_BuildMapName(map), (unsigned)numsubsectors, (unsigned)numsegs,
			u, u > (purgAtMax >> 10) ? u - (purgAtMax >> 10) : 0, purgAtMax >> 10, levelAtMax >> 10, patchAtMax >> 10,
			freeMin >> 10, linMin >> 10, dispMin == ~0u ? 0 : dispMin >> 10, (pk - redMax0) >> 10,
			pn - purges0, pkb - purgKB0, secs);
	}
}
static void RR3DS_RamTable(void)
{
	namespace R = rr3ds_ram;
	if (R::map != gamemap)
	{
		if (R::map >= 0 && R::secs > 0)
		{
			RR3DS_MKDIR(RR3DS_DIR "/cache");
			FILE *t = fopen(RR3DS_DIR "/cache/ram_table.csv", "rb");
			const boolean isNewFile = (t == NULL); if (t) fclose(t);
			FILE *f = fopen(RR3DS_DIR "/cache/ram_table.csv", "ab");
			if (f) { if (isNewFile) fputs(kRamHdr, f); R::Row(f); fclose(f); }
		}
		unsigned n, b, pk; RR3DS_LinFallbackStats(&n, &b, &pk);
		R::map = gamemap; R::frames = 0; R::secs = 0; R::usedMax = 0; R::freeMin = ~0u; R::linMin = ~0u; R::redMax0 = pk; // peak (before: current bytes)
		R::dispMin = ~0u; R::purgAtMax = R::levelAtMax = R::patchAtMax = 0; RR3DS_PurgeStats(&R::purges0, &R::purgKB0);
	}
	if (++R::frames % 30) return;
	struct mallinfo mi = mallinfo();
	const unsigned lin = (unsigned)linearSpaceFree();
	const unsigned purg = (unsigned)Z_TagsUsage(PU_PURGELEVEL, INT32_MAX);
	const unsigned used = (unsigned)mi.uordblks - (g_3ds_reserved_kb << 10); // without the 64 MB reservation
	if (used > R::usedMax)
	{
		R::usedMax = used; R::purgAtMax = purg;
		R::levelAtMax = (unsigned)Z_TagsUsage(PU_LEVEL, PU_PURGELEVEL - 1);
		R::patchAtMax = (unsigned)Z_TagsUsage(PU_PATCH, PU_PATCH);
	}
	{ const unsigned disp = (unsigned)mi.fordblks + lin + purg; if (disp < R::dispMin) R::dispMin = disp; }
	if (mi.fordblks < R::freeMin) R::freeMin = mi.fordblks;
	if (lin < R::linMin) R::linMin = lin;
	if (++R::secs % 10 == 0)
	{
		FILE *f = fopen(RR3DS_DIR "/cache/ram_actual.txt", "wb");
		if (f) { fputs(kRamHdr, f); R::Row(f); fclose(f); }
	}
}
#endif

static void HWR_RenderViewpoint(player_t *player, boolean drawSkyTexture, boolean timing)
{
	const float fpov = FIXED_TO_FLOAT(R_FOV(viewssnum)+player->fovadd);
	postimg_t *type = &postimgtype[viewssnum];
	DIB_START(timing ? 0 : 1);

	{
		// do we really need to save player (is it not the same)?
		player_t *saved_player = stplyr;
		stplyr = player;
		ST_doPaletteStuff();
		stplyr = saved_player;
#ifdef ALAM_LIGHTING
		HWR_SetLights(viewssnum);
#endif
	}

	// copy view cam position for local use
	dup_viewx = viewx;
	dup_viewy = viewy;
	dup_viewz = viewz;
	dup_viewangle = viewangle;

	// set window position
	HWR_ShiftViewPort();

	gl_viewx = FIXED_TO_FLOAT(dup_viewx);
	gl_viewy = FIXED_TO_FLOAT(dup_viewy);
	gl_viewz = FIXED_TO_FLOAT(dup_viewz);
	gl_viewsin = FIXED_TO_FLOAT(viewsin);
	gl_viewcos = FIXED_TO_FLOAT(viewcos);

	//04/01/2000: Hurdler: added for T&L
	//                     It should replace all other gl_viewxxx when finished
	memset(&atransform, 0x00, sizeof(FTransform));

	HWR_SetTransformAiming(&atransform, player, false);
	atransform.angley = (float)(viewangle>>ANGLETOFINESHIFT)*(360.0f/(float)FINEANGLES);

	gl_viewludsin = FIXED_TO_FLOAT(FINECOSINE(gl_aimingangle>>ANGLETOFINESHIFT));
	gl_viewludcos = FIXED_TO_FLOAT(-FINESINE(gl_aimingangle>>ANGLETOFINESHIFT));

	if (*type == postimg_flip)
		atransform.flip = true;
	else
		atransform.flip = false;

	if (*type == postimg_mirror)
		atransform.mirror = true;
	else
		atransform.mirror = false;

	atransform.x      = gl_viewx;  // FIXED_TO_FLOAT(viewx)
	atransform.y      = gl_viewy;  // FIXED_TO_FLOAT(viewy)
	atransform.z      = gl_viewz;  // FIXED_TO_FLOAT(viewz)
	atransform.scalex = 1;
	atransform.scaley = (float)vid.width/vid.height;
	atransform.scalez = 1;

	atransform.fovxangle = fpov; // Tails
	atransform.fovyangle = fpov; // Tails
	HWR_RollTransform(&atransform, viewroll);
	atransform.splitscreen = r_splitscreen;

	gl_fovlud = (float)(1.0l/tan((double)(fpov*M_PIl/360l)));

	//------------------------------------------------------------------------
	HWR_ClearView(); // Clears the depth buffer and resets the view I believe

	if (drawSkyTexture)
	{
#ifdef RR3DS
		const UINT64 sk0 = I_GetPreciseTime();
#endif
		HWR_DrawSkyBackground(player);
#ifdef RR3DS
		rr3ds_sub::skyBgV[timing ? 0 : 1] += I_GetPreciseTime() - sk0;
#endif
	}

	//Hurdler: it doesn't work in splitscreen mode
	drawsky = r_splitscreen;

	HWR_ClearSprites();

	drawcount = 0;

	{
		angle_t a1 = gld_FrustumAngle(gl_aimingangle);
		gld_clipper_Clear();
		// in the sky texture, slightly wider culling (margins ~+8 degrees per side).
		// Without culling it cost ~3x (Sealed Star: skybox 16-19 ms instead of dropping).
		if (I_3DS_SkyTexDrawing()) { const angle_t ex = a1 + ANG1 * 12; if (ex > a1 && ex < ANGLE_180) a1 = ex; }
		gld_clipper_SafeAddClipRange(viewangle + a1, viewangle - a1);
#ifdef HAVE_SPHEREFRUSTRUM
		gld_FrustrumSetup();
#endif
	}

	//04/01/2000: Hurdler: added for T&L
	//                     Actually it only works on Walls and Planes
	HWD.pfnSetTransform(&atransform);
#ifdef RR3DS
	if (timing && rr3ds_skyQuad) I_3DS_SkyTexDraw(); // the stored sky, behind everything
#endif

	// Reset the shader state.
	HWR_SetShaderState();

	if (timing)
	{
		ps_numbspcalls = 0;
		ps_numpolyobjects = 0;
		ps_bsptime = I_GetPreciseTime();
	}

	validcount++;

	if (cv_glbatching.value)
		HWR_StartBatching();
	DIB_MARK(D_PREP);

#ifdef RR3DS
	// GenStep on the FIRST view of the frame (the sky/skybox goes before the main one).
	// Before it only ran on the main one: when the skybox had already drawn from the pool,
	// I_3DS_StaticReset() failed and generation silently stopped (765Stadium at 912/2518).
#ifdef __3DS__
	if (timing) RR3DS_RamTable();
#endif
	{ static int genFrame = -1; if (genFrame != rr3ds_sub::frameNo) { genFrame = rr3ds_sub::frameNo; RR3DS_GenStep(); } }
	RR3DS_StaticFrameBegin();
	rr3ds_sub::view = timing ? 0 : 1;
	if (timing) rr3ds_sub::frameNo++;
	rr3ds_sub::on = rr3ds_static::Diag() && ((rr3ds_sub::frameNo & 3) == 0);
	if (rr3ds_sub::on && timing) rr3ds_fast::nSamp++;
	const UINT64 subT0 = I_GetPreciseTime();
#endif
#ifdef RR3DS
	rr3ds_seccache::stamp++;
	rr3ds_fast::stamp++; if (timing) rr3ds_fast::Frame();
	if (timing && rr3ds_static::Diag()) rr3ds_chg::Tick();
	if (timing) rr3ds_pvs::Begin(); // (A0)
#endif
	HWR_RenderBSPNode((INT32)numnodes-1);
#ifdef RR3DS
	if (timing) rr3ds_pvs::End();
	if (rr3ds_sub::on)
	{
		rr3ds_sub::bspTotV[rr3ds_sub::view] += I_GetPreciseTime() - subT0;
		rr3ds_sub::framesV[rr3ds_sub::view]++;
		rr3ds_sub::on = 0;
	}
#endif

#ifdef HWPRECIP
	HWR_AddPrecipitationSprites();
#endif

	if (timing)
	{
		ps_bsptime = I_GetPreciseTime() - ps_bsptime;
	}

#ifdef RR3DS
	DIB_MARK(D_BSP);
	if (timing) rr3ds_dib::nBsp[0] += ps_numbspcalls;
#endif
	if (cv_glbatching.value)
		HWR_RenderBatches();
	DIB_MARK(D_BATCH);
#ifdef RR3DS
	RR3DS_StaticFrameDraw();
	DIB_MARK(D_STATIC);
#endif

#ifdef ALAM_LIGHTING
	//14/11/99: Hurdler: moved here because it doesn't work with
	// subsector, see other comments;
	HWR_ResetLights();
#endif

	// Draw MD2 and sprites

	if (timing)
	{
		ps_numsprites = gl_visspritecount;
		ps_hw_spritesorttime = I_GetPreciseTime();
	}

	HWR_SortVisSprites();
	DIB_MARK(D_SSORT);

	if (timing)
	{
		ps_hw_spritesorttime = I_GetPreciseTime() - ps_hw_spritesorttime;
		ps_hw_spritedrawtime = I_GetPreciseTime();
	}

#ifdef RR3DS
	rr3ds_dib::nSpr[rr3ds_dib::cur] += gl_visspritecount;
	rr3ds_dib::nTr[rr3ds_dib::cur] += numplanes + numpolyplanes + numwalls;
#endif
	HWR_DrawSprites();
	DIB_MARK(D_SDRAW);

	if (timing)
	{
		ps_hw_spritedrawtime = I_GetPreciseTime() - ps_hw_spritedrawtime;
	}

#ifdef NEWCORONAS
	//Hurdler: they must be drawn before translucent planes, what about gl fog?
	HWR_DrawCoronas();
#endif

	if (timing)
	{
		ps_numdrawnodes = 0;
		ps_hw_nodesorttime = 0;
		ps_hw_nodedrawtime = 0;
	}

	if (numplanes || numpolyplanes || numwalls) //Hurdler: render 3D water and transparent walls after everything
	{
		HWR_CreateDrawNodes();
	}
	DIB_MARK(D_NODES);

	HWD.pfnSetTransform(NULL);
	HWD.pfnUnSetShader();

	// added by Hurdler for correct splitscreen
	// moved here by hurdler so it works with the new near clipping plane
	HWD.pfnGClipRect(0, 0, vid.width, vid.height, NZCLIP_PLANE);
	DIB_MARK(D_FIN);
}

// ==========================================================================
// Same as rendering the player view, but from the skybox object
// ==========================================================================
void HWR_RenderSkyboxView(player_t *player)
{
	// note: sets viewangle, viewx, viewy, viewz
	R_SkyboxFrame(viewssnum);

#ifdef RR3DS
	rr3ds_skycull::Begin(player);
#endif
	HWR_RenderViewpoint(player, drawsky, false);
#ifdef RR3DS
	rr3ds_skycull::active = false;
#endif
}

// ==========================================================================
//
// ==========================================================================

static void HWR_RollTransform(FTransform *tr, angle_t roll)
{
	if (roll != 0)
	{
		tr->rollangle = roll / (float)ANG1;
		tr->roll = true;
		tr->rollx = 1.0f;
		tr->rollz = 0.0f;
	}
}

void HWR_RenderPlayerView(void)
{
#ifdef RR3DS
	const UINT64 dibT0 = I_GetPreciseTime();
#endif
	HWD.pfnResetRenderState();

	player_t * player = &players[displayplayers[viewssnum]];

	const boolean skybox = (player->skybox.viewpoint && cv_skybox.value); // True if there's a skybox object and skyboxes are on

	FRGBAFloat ClearColor;

	ClearColor.red = 0.0f;
	ClearColor.green = 0.0f;
	ClearColor.blue = 0.0f;
	ClearColor.alpha = 1.0f;

	if (cv_glshaders.value)
	{
		HWD.pfnSetShaderInfo(HWD_SHADERINFO_LEVELTIME, (INT32)leveltime); // The water surface shader needs the leveltime.

		const angle_t light_angle = maplighting.angle - viewangle + ANGLE_90; // I fucking hate OGL's coordinate system
		HWD.pfnSetShaderInfo(HWD_SHADERINFO_LIGHT_X, FINECOSINE(light_angle >> ANGLETOFINESHIFT));
		HWD.pfnSetShaderInfo(HWD_SHADERINFO_LIGHT_Y, 0);
		HWD.pfnSetShaderInfo(HWD_SHADERINFO_LIGHT_Z,  -FINESINE(light_angle >> ANGLETOFINESHIFT));

		HWD.pfnSetShaderInfo(HWD_SHADERINFO_LIGHT_CONTRAST, maplighting.contrast);
		HWD.pfnSetShaderInfo(HWD_SHADERINFO_LIGHT_BACKLIGHT, maplighting.backlight);
	}

	if (viewssnum == 0) // Only do it if it's the first screen being rendered
		HWD.pfnClearBuffer(true, false, &ClearColor); // Clear the Color Buffer, stops HOMs. Also seems to fix the skybox issue on Intel GPUs.

	ps_hw_skyboxtime = I_GetPreciseTime();
#ifdef RR3DS
	// the skybox is drawn into a texture at most 1 of every 3 frames
	// (or earlier if the camera turns a lot) and in the others it is reused, shifted.
	rr3ds_skyQuad = false;
	if (skybox && drawsky)
	{
		static int skyEvery = RRCfg_Int("sky_every", 4); // 1 of every N+1 frames is redrawn (it used to be a fixed 2)
		static int skyAge = 99;
		static INT32 skyMap = -1;
		if (gamemap != skyMap) { skyMap = gamemap; I_3DS_SkyTexInvalidate(); skyAge = 99; }
		if (I_3DS_SkyTexOn() && r_splitscreen == 0)
		{
			if (skyAge >= skyEvery || I_3DS_SkyTexNeedRedraw())
			{
				if (I_3DS_SkyTexBegin())
				{
					HWR_RenderSkyboxView(player);
					I_3DS_SkyTexEnd();
					skyAge = 0; rr3ds_skyDrawn++;
					rr3ds_skyQuad = true;
				}
				else
					HWR_RenderSkyboxView(player);
			}
			else { skyAge++; rr3ds_skyReused++; rr3ds_skyQuad = true; }
		}
		else
			HWR_RenderSkyboxView(player);
		if (rr3ds_skyDrawn + rr3ds_skyReused >= 90)
		{
			int w0, w1, w2; float ox, oy; I_3DS_SkyTexWhy(&w0, &w1, &w2, &ox, &oy);
			RR_DIAGPRINT("[SKYTEX] 90 frames: drawn=%d reused=%d | because: no_data=%d behind=%d turn=%d | last offset=%.3f,%.3f\n",
				rr3ds_skyDrawn, rr3ds_skyReused, w0, w1, w2, ox, oy);
			rr3ds_skyDrawn = rr3ds_skyReused = 0;
		}
	}
#else
	if (skybox && drawsky) // If there's a skybox and we should be drawing the sky, draw the skybox
		HWR_RenderSkyboxView(player); // This is drawn before everything else so it is placed behind
#endif
	ps_hw_skyboxtime = I_GetPreciseTime() - ps_hw_skyboxtime;
#ifdef RR3DS
	rr3ds_dib::accSkyTot += ps_hw_skyboxtime;
#endif

	// note: sets viewangle, viewx, viewy, viewz
	R_SetupFrame(viewssnum);
	framecount++; // timedemo

	HWR_RenderViewpoint(player,
			!skybox && drawsky, // Don't draw the regular sky if there's a skybox
			true); // Main view is profiled

#ifdef RR3DS
	const UINT64 dibP0 = I_GetPreciseTime();
#endif
	HWR_DoPostProcessor(player);
#ifdef RR3DS
	{
		const UINT64 n = I_GetPreciseTime();
		rr3ds_dib::accPost += n - dibP0;
		rr3ds_dib::accTotal += n - dibT0;
		if (viewssnum == 0 && ++rr3ds_dib::frames >= 30)
			rr3ds_dib::Report();
	}
#endif
}

void HWR_LoadLevel(void)
{
#ifdef ALAM_LIGHTING
	// BP: reset light between levels (we draw preview frame lights on current frame)
	HWR_ResetLights();
#endif

	HWR_CreatePlanePolygons((INT32)numnodes - 1);
#ifdef RR3DS
	RR3DS_StaticLevelInit();
	rr3ds_pvs::Load(rr3ds_gen::Fingerprint(), rr3ds_gen::GeoFingerprint()); // (A0)
	rr3ds_skycull::LevelReset();
	rr3ds_seccache::LevelReset();
#endif

	// Build the sky dome
	HWR_ClearSkyDome();
	HWR_BuildSkyDome();

#ifndef __3DS__
	// (PC): preload the map's textures in GL (Software already does it with R_PrecacheLevel,
	// GL does not). Without this every wall/floor was converted and uploaded the 1st time it was seen: fps drop
	// in the first seconds (RR_SealedStarBalconies: 20-25 ms/frame at the start). Not on 3DS (VRAM).
	{
		const precise_t t0 = I_GetPreciseTime();
		size_t i;
		if (skytexture > 0 && skytexture < numtextures)
			HWR_GetTexture(texturetranslation[skytexture], skytexture);
		for (i = 0; i < numsides; i++)
		{
			const INT32 base[3] = { sides[i].toptexture, sides[i].midtexture, sides[i].bottomtexture };
			for (INT32 k = 0; k < 3; k++)
				if (base[k] > 0 && base[k] < numtextures)
					HWR_GetTexture(R_GetTextureNum(base[k]), base[k]);
		}
		for (i = 0; i < numsectors; i++)
		{
			sector_t *sec = &sectors[i];
			if (sec->floorpic >= 0 && (size_t)sec->floorpic < numlevelflats)
				HWR_GetLevelFlat(&levelflats[sec->floorpic], R_NoEncore(sec, &levelflats[sec->floorpic], false));
			if (sec->ceilingpic >= 0 && (size_t)sec->ceilingpic < numlevelflats)
				HWR_GetLevelFlat(&levelflats[sec->ceilingpic], R_NoEncore(sec, &levelflats[sec->ceilingpic], true));
		}
		I_OutputMsg("[PRELOAD] GL textures of the map: %.0f ms\n",
			(double)(I_GetPreciseTime() - t0) * 1000.0 / (double)I_GetPrecisePrecision());
	}
#endif

	gl_maploaded = true;
}

// ==========================================================================
//                                                         3D ENGINE COMMANDS
// ==========================================================================

extern "C" CV_PossibleValue_t glshaders_cons_t[];
CV_PossibleValue_t glshaders_cons_t[] = {{HWD_SHADEROPTION_OFF, "Off"}, {HWD_SHADEROPTION_ON, "On"}, {HWD_SHADEROPTION_NOCUSTOM, "Ignore custom shaders"}, {0, NULL}};

extern "C" CV_PossibleValue_t glfiltermode_cons_t[];
CV_PossibleValue_t glfiltermode_cons_t[]= {{HWD_SET_TEXTUREFILTER_POINTSAMPLED, "Nearest"},
	{HWD_SET_TEXTUREFILTER_BILINEAR, "Bilinear"}, {HWD_SET_TEXTUREFILTER_TRILINEAR, "Trilinear"},
	{HWD_SET_TEXTUREFILTER_MIXED1, "Linear_Nearest"},
	{HWD_SET_TEXTUREFILTER_MIXED2, "Nearest_Linear"},
	{HWD_SET_TEXTUREFILTER_MIXED3, "Nearest_Mipmap"},
	{0, NULL}};

extern "C" CV_PossibleValue_t glanisotropicmode_cons_t[];
CV_PossibleValue_t glanisotropicmode_cons_t[] = {{1, "MIN"}, {16, "MAX"}, {0, NULL}};

extern "C" void CV_glfiltermode_OnChange(void);
void CV_glfiltermode_OnChange(void)
{
	if (rendermode == render_opengl)
		HWD.pfnSetSpecialState(HWD_SET_TEXTUREFILTERMODE, cv_glfiltermode.value);
}

extern "C" void CV_glanisotropic_OnChange(void);
void CV_glanisotropic_OnChange(void)
{
	if (rendermode == render_opengl)
		HWD.pfnSetSpecialState(HWD_SET_TEXTUREANISOTROPICMODE, cv_glanisotropicmode.value);
}

extern "C" struct CVarList *cvlist_opengl;

//added by Hurdler: console varibale that are saved
void HWR_AddCommands(void)
{
	{
		CV_RegisterList(cvlist_opengl);
	}
}

void HWR_AddSessionCommands(void)
{
	if (gl_sessioncommandsadded)
		return;
	// Kept in case we ever need this again.
	gl_sessioncommandsadded = true;
}

// --------------------------------------------------------------------------
// Setup the hardware renderer
// --------------------------------------------------------------------------
void HWR_Startup(void)
{
	if (!gl_init)
	{
		CONS_Printf("HWR_Startup()...\n");

		HWR_InitPolyPool();
		HWR_AddSessionCommands();
		HWR_InitMapTextures();
#ifndef __3DS__ // no 3D models on 3DS
		HWR_InitModels();
#endif
#ifdef ALAM_LIGHTING
		HWR_InitLight();
#endif

		HWR_LoadAllCustomShaders();
		if (!HWR_CompileShaders())
			gl_shadersavailable = false;
	}

	if (rendermode == render_opengl)
		textureformat = patchformat = GL_TEXFMT_RGBA;

	gl_init = true;
}

// --------------------------------------------------------------------------
// Called after switching to the hardware renderer
// --------------------------------------------------------------------------
void HWR_Switch(void)
{
	// Add session commands
	if (!gl_sessioncommandsadded)
		HWR_AddSessionCommands();

	// Set special states from CVARs
	HWD.pfnSetSpecialState(HWD_SET_TEXTUREFILTERMODE, cv_glfiltermode.value);
	HWD.pfnSetSpecialState(HWD_SET_TEXTUREANISOTROPICMODE, cv_glanisotropicmode.value);

	// Load textures
	if (!gl_maptexturesloaded)
		HWR_LoadMapTextures(numtextures);

	// Create plane polygons
	if (!gl_maploaded && (gamestate == GS_LEVEL || (gamestate == GS_TITLESCREEN && titlemapinaction)))
	{
		HWR_ClearAllTextures();
		HWR_LoadLevel();
	}
}

// --------------------------------------------------------------------------
// Free resources allocated by the hardware renderer
// --------------------------------------------------------------------------
void HWR_Shutdown(void)
{
	CONS_Printf("HWR_Shutdown()\n");
	HWR_FreeExtraSubsectors();
	HWR_FreePolyPool();
	HWR_FreeMapTextures();
	HWD.pfnFlushScreenTextures();
}

void transform(float *cx, float *cy, float *cz)
{
	float tr_x,tr_y;
	// translation
	tr_x = *cx - gl_viewx;
	tr_y = *cz - gl_viewy;
//	*cy = *cy;

	// rotation around vertical y axis
	*cx = (tr_x * gl_viewsin) - (tr_y * gl_viewcos);
	tr_x = (tr_x * gl_viewcos) + (tr_y * gl_viewsin);

	//look up/down ----TOTAL SUCKS!!!--- do the 2 in one!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
	tr_y = *cy - gl_viewz;

	*cy = (tr_x * gl_viewludcos) + (tr_y * gl_viewludsin);
	*cz = (tr_x * gl_viewludsin) - (tr_y * gl_viewludcos);

	//scale y before frustum so that frustum can be scaled to screen height
	*cy *= ORIGINAL_ASPECT * gl_fovlud;
	*cx *= gl_fovlud;
}

void HWR_AddTransparentWall(FOutVector *wallVerts, FSurfaceInfo *pSurf, INT32 texnum, INT32 basetexnum, FBITFIELD blend, boolean fogwall, INT32 lightlevel, extracolormap_t *wallcolormap)
{
#ifdef RR3DS
	rr3ds_static::Never(); // side effect: it cannot be cached
#endif
	static size_t allocedwalls = 0;

	// Force realloc if buffer has been freed
	if (!wallinfo)
		allocedwalls = 0;

	if (allocedwalls < numwalls + 1)
	{
		allocedwalls += MAX_TRANSPARENTWALL;
		Z_Realloc(wallinfo, allocedwalls * sizeof (*wallinfo), PU_LEVEL, &wallinfo);
	}

	M_Memcpy(wallinfo[numwalls].wallVerts, wallVerts, sizeof (wallinfo[numwalls].wallVerts));
	M_Memcpy(&wallinfo[numwalls].Surf, pSurf, sizeof (FSurfaceInfo));
	wallinfo[numwalls].texnum = texnum;
	wallinfo[numwalls].basetexnum = basetexnum;
	wallinfo[numwalls].blend = blend;
	wallinfo[numwalls].drawcount = drawcount++;
	wallinfo[numwalls].fogwall = fogwall;
	wallinfo[numwalls].lightlevel = lightlevel;
	wallinfo[numwalls].wallcolormap = wallcolormap;
	numwalls++;
}

void HWR_RenderWall(FOutVector *wallVerts, FSurfaceInfo *pSurf, FBITFIELD blend, boolean fogwall, INT32 lightlevel, extracolormap_t *wallcolormap)
{
	FBITFIELD blendmode = blend;
	UINT8 alpha = pSurf->PolyColor.s.alpha; // retain the alpha

	INT32 shader = SHADER_DEFAULT;

	// Lighting is done here instead so that fog isn't drawn incorrectly on transparent walls after sorting
	HWR_Lighting(pSurf, lightlevel, wallcolormap, P_SectorUsesDirectionalLighting(gl_frontsector));

	pSurf->PolyColor.s.alpha = alpha; // put the alpha back after lighting
#ifndef __3DS__
	if (fogwall && !HWR_UseShader())
		HWR_FogNoShaderColor(pSurf, lightlevel, wallcolormap);
#endif

	if (blend & PF_Environment)
		blendmode |= PF_Occlude;	// PF_Occlude must be used for solid objects

	if (HWR_UseShader())
	{
		if (fogwall)
			shader = SHADER_FOG;
		else
			shader = SHADER_WALL;

		blendmode |= PF_ColorMapped;
	}

	if (fogwall)
		blendmode |= PF_Fog;

	blendmode |= PF_Modulated;	// No PF_Occlude means overlapping (incorrect) transparency
	DBG3DS_TINT(pSurf, &blendmode, 0, 160, 0);
	HWR_ProcessPolygon(pSurf, wallVerts, 4, blendmode, shader, false);
}

INT32 HWR_GetTextureUsed(void)
{
	return HWD.pfnGetTextureUsed();
}

void HWR_DoPostProcessor(player_t *player)
{
	postimg_t *type = &postimgtype[R_GetViewNumber()];

	HWD.pfnUnSetShader();

	// Armageddon Blast Flash!
	// Could this even be considered postprocessor?
	if (player->flashcount)
	{
		FOutVector      v[4];
		FSurfaceInfo Surf;

		v[0].x = v[2].y = v[3].x = v[3].y = -4.0f;
		v[0].y = v[1].x = v[1].y = v[2].x = 4.0f;
		v[0].z = v[1].z = v[2].z = v[3].z = 4.0f; // 4.0 because of the same reason as with the sky, just after the screen is cleared so near clipping plane is 3.99

		// This won't change if the flash palettes are changed unfortunately, but it works for its purpose
		if (player->flashpal == PAL_NUKE)
		{
			Surf.PolyColor.s.red = 0xff;
			Surf.PolyColor.s.green = Surf.PolyColor.s.blue = 0x7F; // The nuke palette is kind of pink-ish
		}
		else
			Surf.PolyColor.s.red = Surf.PolyColor.s.green = Surf.PolyColor.s.blue = 0xff;

		Surf.PolyColor.s.alpha = 0xc0; // match software mode

		HWD.pfnDrawPolygon(&Surf, v, 4, PF_Modulated|PF_Additive|PF_NoTexture|PF_NoDepthTest);
	}

	// Capture the screen for intermission and screen waving
#ifdef RR_PCGEN
	// only with a water/heat effect in this view: it is the only thing that uses the texture (PostImgRedraw, same
	// frame); HWR_DrawIntermissionBG is not called in RR. Copying the screen every frame cost 1-17 ms
	// with ANGLE (the CPU waits for the GPU) and something on all APIs.
	if (gamestate != GS_INTERMISSION && !r_splitscreen && (*type == postimg_water || *type == postimg_heat))
#else
	if(gamestate != GS_INTERMISSION)
#endif
		HWD.pfnMakeScreenTexture();

	if (r_splitscreen) // Not supported in splitscreen - someone want to add support?
		return;

	// Drunken vision! WooOOooo~
	if (*type == postimg_water || *type == postimg_heat)
	{
		// 10 by 10 grid. 2 coordinates (xy)
		float v[SCREENVERTS][SCREENVERTS][2];
		static double disStart = 0;

		UINT8 x, y;
		INT32 WAVELENGTH;
		INT32 AMPLITUDE;
		INT32 FREQUENCY;

		// Modifies the wave.
		if (*type == postimg_water)
		{
			WAVELENGTH = 5;
			AMPLITUDE = 20;
			FREQUENCY = 8;
		}
		else
		{
			WAVELENGTH = 10;
			AMPLITUDE = 60;
			FREQUENCY = 4;
		}

		for (x = 0; x < SCREENVERTS; x++)
		{
			for (y = 0; y < SCREENVERTS; y++)
			{
				// Change X position based on its Y position.
				v[x][y][0] = (x/((float)(SCREENVERTS-1.0f)/9.0f))-4.5f + (float)sin((disStart+(y*WAVELENGTH))/FREQUENCY)/AMPLITUDE;
				v[x][y][1] = (y/((float)(SCREENVERTS-1.0f)/9.0f))-4.5f;
			}
		}
		HWD.pfnPostImgRedraw(v);
		if (!(paused || P_AutoPause()))
			disStart += FIXED_TO_FLOAT(renderdeltatics);

		// Capture the screen again for screen waving on the intermission
		if(gamestate != GS_INTERMISSION)
			HWD.pfnMakeScreenTexture();
	}
	// Flipping of the screen isn't done here anymore
}

void HWR_StartScreenWipe(void)
{
	//CONS_Debug(DBG_RENDER, "In HWR_StartScreenWipe()\n");
	HWD.pfnStartScreenWipe();
}

void HWR_EndScreenWipe(void)
{
	//CONS_Debug(DBG_RENDER, "In HWR_EndScreenWipe()\n");
	HWD.pfnEndScreenWipe();
}

void HWR_DrawIntermissionBG(void)
{
	HWD.pfnDrawIntermissionBG();
}

//
// hwr mode wipes
//
static lumpnum_t wipelumpnum;

// puts wipe lumpname in wipename[9]
static boolean HWR_WipeCheck(UINT8 wipenum, UINT8 scrnnum)
{
	static char lumpname[9] = "FADEmmss";
	size_t lsize;

	// not a valid wipe number
	if (wipenum > 99 || scrnnum > 99)
		return false; // shouldn't end up here really, the loop should've stopped running beforehand

	// puts the numbers into the wipename
	lumpname[4] = '0'+(wipenum/10);
	lumpname[5] = '0'+(wipenum%10);
	lumpname[6] = '0'+(scrnnum/10);
	lumpname[7] = '0'+(scrnnum%10);
	wipelumpnum = W_CheckNumForName(lumpname);

	// again, shouldn't be here really
	if (wipelumpnum == LUMPERROR)
		return false;

	lsize = W_LumpLength(wipelumpnum);
	if (!(lsize == 256000 || lsize == 64000 || lsize == 16000 || lsize == 4000))
	{
		CONS_Alert(CONS_WARNING, "Fade mask lump %s of incorrect size, ignored\n", lumpname);
		return false; // again, shouldn't get here if it is a bad size
	}

	return true;
}

void HWR_DoWipe(UINT8 wipenum, UINT8 scrnnum)
{
	if (!HWR_WipeCheck(wipenum, scrnnum))
		return;

	HWR_GetFadeMask(wipelumpnum);
	HWD.pfnDoScreenWipe();
}

void HWR_DoTintedWipe(UINT8 wipenum, UINT8 scrnnum)
{
	// It does the same thing
	HWR_DoWipe(wipenum, scrnnum);
}

void HWR_MakeScreenFinalTexture(void)
{
    HWD.pfnMakeScreenFinalTexture();
}

void HWR_DrawScreenFinalTexture(int width, int height)
{
    HWD.pfnDrawScreenFinalTexture(width, height);
}

static inline UINT16 HWR_FindShaderDefs(UINT16 wadnum)
{
	UINT16 i;
	lumpinfo_t *lump_p;

	lump_p = wadfiles[wadnum]->lumpinfo;
	for (i = 0; i < wadfiles[wadnum]->numlumps; i++, lump_p++)
		if (memcmp(lump_p->name, "SHADERS", 7) == 0)
			return i;

	return INT16_MAX;
}

boolean HWR_CompileShaders(void)
{
	return HWD.pfnCompileShaders();
}

customshaderxlat_t shaderxlat[] =
{
	{"Flat", SHADER_FLOOR},
	{"WallTexture", SHADER_WALL},
	{"Sprite", SHADER_SPRITE},
	{"Model", SHADER_MODEL},
	{"ModelLighting", SHADER_MODEL_LIGHTING},
	{"WaterRipple", SHADER_WATER},
	{"Fog", SHADER_FOG},
	{"Sky", SHADER_SKY},
	{NULL, 0},
};

void HWR_LoadAllCustomShaders(void)
{
	INT32 i;

	// read every custom shader
	for (i = 0; i < numwadfiles; i++)
		HWR_LoadCustomShadersFromFile(i, (wadfiles[i]->type == RET_PK3));
}

void HWR_LoadCustomShadersFromFile(UINT16 wadnum, boolean PK3)
{
	UINT16 lump;
	char *shaderdef, *line;
	char *stoken;
	char *value;
	size_t size;
	int linenum = 1;
	int shadertype = 0;
	int i;

	lump = HWR_FindShaderDefs(wadnum);
	if (lump == INT16_MAX)
		return;

	shaderdef = (char *)W_CacheLumpNumPwad(wadnum, lump, PU_CACHE);
	size = W_LumpLengthPwad(wadnum, lump);

	line = (char *)Z_Malloc(size+1, PU_STATIC, NULL);
	M_Memcpy(line, shaderdef, size);
	line[size] = '\0';

	stoken = strtok(line, "\r\n ");
	while (stoken)
	{
		if ((stoken[0] == '/' && stoken[1] == '/')
			|| (stoken[0] == '#'))// skip comments
		{
			stoken = strtok(NULL, "\r\n");
			goto skip_field;
		}

		if (!stricmp(stoken, "GLSL"))
		{
			value = strtok(NULL, "\r\n ");
			if (!value)
			{
				CONS_Alert(CONS_WARNING, "HWR_LoadCustomShadersFromFile: Missing shader type (file %s, line %d)\n", wadfiles[wadnum]->filename, linenum);
				stoken = strtok(NULL, "\r\n"); // skip end of line
				goto skip_lump;
			}

			if (!stricmp(value, "VERTEX"))
				shadertype = 1;
			else if (!stricmp(value, "FRAGMENT"))
				shadertype = 2;

skip_lump:
			stoken = strtok(NULL, "\r\n ");
			linenum++;
		}
		else
		{
			value = strtok(NULL, "\r\n= ");
			if (!value)
			{
				CONS_Alert(CONS_WARNING, "HWR_LoadCustomShadersFromFile: Missing shader target (file %s, line %d)\n", wadfiles[wadnum]->filename, linenum);
				stoken = strtok(NULL, "\r\n"); // skip end of line
				goto skip_field;
			}

			if (!shadertype)
			{
				CONS_Alert(CONS_ERROR, "HWR_LoadCustomShadersFromFile: Missing shader type (file %s, line %d)\n", wadfiles[wadnum]->filename, linenum);
				Z_Free(line);
				return;
			}

			for (i = 0; shaderxlat[i].type; i++)
			{
				if (!stricmp(shaderxlat[i].type, stoken))
				{
					size_t shader_size;
					char *shader_source;
					char *shader_lumpname;
					UINT16 shader_lumpnum;

					if (PK3)
					{
						shader_lumpname = (char *)Z_Malloc(strlen(value) + 12, PU_STATIC, NULL);
						strcpy(shader_lumpname, "Shaders/sh_");
						strcat(shader_lumpname, value);
						shader_lumpnum = W_CheckNumForFullNamePK3(shader_lumpname, wadnum, 0);
					}
					else
					{
						shader_lumpname = (char *)Z_Malloc(strlen(value) + 4, PU_STATIC, NULL);
						strcpy(shader_lumpname, "SH_");
						strcat(shader_lumpname, value);
						shader_lumpnum = W_CheckNumForNamePwad(shader_lumpname, wadnum, 0);
					}

					if (shader_lumpnum == INT16_MAX)
					{
						CONS_Alert(CONS_ERROR, "HWR_LoadCustomShadersFromFile: Missing shader source %s (file %s, line %d)\n", shader_lumpname, wadfiles[wadnum]->filename, linenum);
						Z_Free(shader_lumpname);
						continue;
					}

					shader_size = W_LumpLengthPwad(wadnum, shader_lumpnum);
					shader_source = (char *)Z_Malloc(shader_size, PU_STATIC, NULL);
					W_ReadLumpPwad(wadnum, shader_lumpnum, shader_source);

					HWD.pfnLoadCustomShader(shaderxlat[i].id, shader_source, shader_size, (shadertype == 2));

					Z_Free(shader_source);
					Z_Free(shader_lumpname);
				}
			}

skip_field:
			stoken = strtok(NULL, "\r\n= ");
			linenum++;
		}
	}

	Z_Free(line);
	return;
}

const char *HWR_GetShaderName(INT32 shader)
{
	INT32 i;

	if (shader)
	{
		for (i = 0; shaderxlat[i].type; i++)
		{
			if (shaderxlat[i].id == shader)
				return shaderxlat[i].type;
		}

		return "Unknown";
	}

	return "Default";
}

#endif // HWRENDER
