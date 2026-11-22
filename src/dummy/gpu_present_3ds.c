// DR. ROBOTNIK'S RING RACERS — 3DS port
//-----------------------------------------------------------------------------
// GPU presentation (citro3d). Phase 1 of the hardware renderer.
//
// The game still renders in 8 bits on the CPU (320x200). Here:
// 1. CPU: palette indices -> RGB565 into a linear buffer (sequential write,
// much cheaper than the previous column-by-column conversion + rotation).
// 2. GPU: linear DisplayTransfer -> "tiled" 512x256 texture.
// 3. GPU: draws a quad scaled to 384x240 (takes the full height of the
// top screen) with linear filtering, and citro3d sends it to the framebuffer.
//
// Also validates that citro3d + PICA shaders work in this
// build: it is the base of the GPU 3D renderer (phase 2).
//
// If sdmc:/3ds/rr_cpu_present exists, the old CPU path is used.
//-----------------------------------------------------------------------------

#ifdef __3DS__

#include "i_3ds_log.h"
#include <3ds.h>
#include "../rr_config.h"  // single-file configuration
#include <citro3d.h>
#include <string.h>
#include <stdio.h>
#include <math.h>


#define TEX_W 512
#define TEX_H 256

#define DISPLAY_TRANSFER_FLAGS \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	 GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	 GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

#define TEXTURE_TRANSFER_FLAGS(fmt) \
	(GX_TRANSFER_FLIP_VERT(1) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) | \
	 GX_TRANSFER_IN_FORMAT(fmt) | GX_TRANSFER_OUT_FORMAT(fmt) | \
	 GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

static const u8 s_present_shbin[] __attribute__((aligned(4))) = {
	0x44,0x56,0x4c,0x42,0x01,0x00,0x00,0x00,0x8c,0x00,0x00,0x00,0x44,0x56,0x4c,0x50,
	0x00,0x00,0x00,0x00,0x28,0x00,0x00,0x00,0x08,0x00,0x00,0x00,0x48,0x00,0x00,0x00,
	0x07,0x00,0x00,0x00,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x4e,0x01,0xf0,0x07,0x4e,0x02,0x08,0x02,0x08,
	0x03,0x18,0x02,0x08,0x04,0x28,0x02,0x08,0x05,0x38,0x02,0x08,0x06,0x10,0x20,0x4c,
	0x00,0x00,0x00,0x88,0x6e,0x03,0x00,0x00,0x00,0x00,0x00,0x00,0xa1,0x0a,0x00,0x00,
	0x00,0x00,0x00,0x00,0x68,0xc3,0x06,0x00,0x00,0x00,0x00,0x00,0x64,0xc3,0x06,0x00,
	0x00,0x00,0x00,0x00,0x62,0xc3,0x06,0x00,0x00,0x00,0x00,0x00,0x61,0xc3,0x06,0x00,
	0x00,0x00,0x00,0x00,0x6f,0x03,0x00,0x00,0x00,0x00,0x00,0x00,0x44,0x56,0x4c,0x45,
	0x02,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x08,0x00,0x00,0x00,0x00,0x00,0x03,0x00,
	0x00,0x00,0x00,0x00,0x40,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x54,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x54,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x64,0x00,0x00,0x00,
	0x01,0x00,0x00,0x00,0x6c,0x00,0x00,0x00,0x0b,0x00,0x00,0x00,0x02,0x00,0x5f,0x00,
	0x00,0x00,0x00,0x00,0x00,0x00,0x3f,0x00,0x00,0x00,0xbf,0x00,0x99,0x99,0x3b,0x00,
	0x00,0x00,0x00,0x00,0x0f,0x00,0x00,0x00,0x03,0x00,0x01,0x00,0x0f,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x10,0x00,0x13,0x00,0x70,0x72,0x6f,0x6a,0x65,0x63,0x74,0x69,
	0x6f,0x6e,0x00,0x00,
};

static int s_state = 0; // 0 = not started, 1 = ok, -1 = disabled/failed
static DVLB_s *s_dvlb;
static shaderProgram_s s_prog;
static int s_uloc_proj;
static C3D_Mtx s_proj;
static C3D_RenderTarget *s_target;
static C3D_Tex s_tex;
static u16 *s_linear; // TEX_W*TEX_H 16 bpp (CPU mode) or 32 bpp RGBA8 (GPU 3D mode), linear memory
static u16 s_lut16[256];
static u32 s_lut32[256]; // RGBA8 (0xRRGGBBAA) for the 2D layer with real alpha
// Alpha plane of the 2D layer (twodee_soft_3ds.cpp): 255 = opaque
extern unsigned char *g_3ds_2d_alpha;
extern int g_3ds_2d_alpha_n;
extern int g_3ds_2d_alpha_used;
static int s_inframe = 0;
static int s_frameid = 0;

// 1 = the 3D world is drawn by the GPU (hardware/r_citro3d). Set by i_video.c
// if sdmc:/3ds/rr_gpu3d exists. The 2D layer is drawn on top with transparency.
int g_3ds_gpu3d = 0;

// GPU renderer statistics (accumulated by r_citro3d, printed by the profiler):
// 0 polygons, 1 vertices, 2 textures uploaded, 3 upload failures,
// 4 untextured draws, 5 ClearBuffer, 6 SetTransform, 7 discarded draws
int g_gpu_stat[8];

// 1 = fixed debug mode (no 2D layer + triangle + flat world). Flag
// sdmc:/3ds/rr_dbgflat (SELECT opens menus on the title screen, no use).
int g_3ds_dbgflat = 0;

static int gpu_init(void)
{
	{
		g_3ds_dbgflat = RRCfg_Int("dbgflat", 0); /* rr_config.txt; 0 = off */
		if (g_3ds_dbgflat < 0 || g_3ds_dbgflat > 9) g_3ds_dbgflat = 0;
		if (g_3ds_dbgflat) I_3DS_Both("[3DS] DEBUG dbgflat mode %d\n", g_3ds_dbgflat);
	}
	if (RRCfg_On("cpu_present")) // rr_config.txt
	{
		I_3DS_Both("[3DS] GPU present disabled (cpu_present)\n");
		g_3ds_gpu3d = 0;
		return -1;
	}

	if (!C3D_Init(0x200000) /* 2MB: maps filled the default 256KB */)
	{
		I_3DS_Both("[3DS] C3D_Init failed\n");
		return -1;
	}

	// A depth buffer is needed with GPU 3D.
	s_target = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8,
		g_3ds_gpu3d ? GPU_RB_DEPTH24_STENCIL8 : -1);
	if (!s_target)
	{
		I_3DS_Both("[3DS] C3D_RenderTargetCreate failed\n");
		C3D_Fini();
		return -1;
	}
	// Without C3D_RenderTargetSetOutput: citro3d would do the copy to the framebuffer in the
	// VBlank and C3D_FrameBegin would wait on it -> fps rounded to 60/n (20 fps).
	// We copy by hand right when drawing finishes (see the end of I_3DS_GpuPresent).

	if (!C3D_TexInit(&s_tex, TEX_W, TEX_H, g_3ds_gpu3d ? GPU_RGBA8 : GPU_RGB565))
	{
		I_3DS_Both("[3DS] C3D_TexInit failed\n");
		C3D_Fini();
		return -1;
	}
	C3D_TexSetFilter(&s_tex, GPU_LINEAR, GPU_LINEAR);
	C3D_TexSetWrap(&s_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

	const size_t bpp = g_3ds_gpu3d ? 4 : 2;
	s_linear = (u16 *)linearAlloc(TEX_W * TEX_H * bpp);
	if (!s_linear)
	{
		I_3DS_Both("[3DS] linearAlloc failed\n");
		C3D_Fini();
		return -1;
	}
	memset(s_linear, 0, TEX_W * TEX_H * bpp);

	s_dvlb = DVLB_ParseFile((u32 *)s_present_shbin, sizeof(s_present_shbin));
	shaderProgramInit(&s_prog);
	shaderProgramSetVsh(&s_prog, &s_dvlb->DVLE[0]);
	s_uloc_proj = shaderInstanceGetUniformLocation(s_prog.vertexShader, "projection");

	// Rotated top screen: OrthoTilt straightens it. (0,0) top-left.
	Mtx_OrthoTilt(&s_proj, 0.0f, 400.0f, 240.0f, 0.0f, 0.0f, 1.0f, true);

	I_3DS_Both("[3DS] GPU present ready (citro3d%s)\n", g_3ds_gpu3d ? ", 3D world on the GPU" : "");
	return 1;
}

// ---------------------------------------------------------------------------
// GPU-based fade (wipe). Previously NOWIPE (no hwr2). The already-rendered image is copied
// drawn (render target 240x400 RGBA8 "tiled") into 2 textures of 256x512:
// [0] = the screen before, [1] = the screen after. During the fade
// [0] is drawn and [1] on top with opacity t (0..1). Shape: uniform fade (the
// PC FADEmmss masks are only used for the duration).
// Memory: 2 x 512 KB linear, only while the fade lasts.
// rr_config.txt: wipe = 0 turns it off; wipe_flip = 1/2/3 if it looks mirrored.
// ---------------------------------------------------------------------------
static C3D_Tex s_wtex[2];
static int s_walloc[2], s_wcap[2];
static int s_noshow = -1;   // 0/1: present WITHOUT showing and capture into s_wtex[k]
static int s_haveframe = 0; // at least one frame has been shown already (colorBuf is valid)
static int s_wipe_on = 0;
static float s_wipe_t = 0.f;
static int s_wflip = -1;

extern void CONS_Printf(const char *fmt, ...); // to the log (printf only goes to the bottom console)

static void gm_free_all(void);
extern void I_3DS_FreeWorldTextures(void);
extern void I_3DS_EtcCacheFree(void);
static int wtex_alloc(int k)
{
	if (s_walloc[k]) return 1;
	gm_free_all(); // the menu's blend textures were occupying linear memory
	int ok = C3D_TexInit(&s_wtex[k], 256, 512, GPU_RGBA8);
	if (!ok)
	{
		// still no room: drop the world texture caches (they refill on demand) and retry once
		I_3DS_EtcCacheFree();
		I_3DS_FreeWorldTextures();
		ok = C3D_TexInit(&s_wtex[k], 256, 512, GPU_RGBA8);
	}
	if (!ok)
	{
		CONS_Printf("[FADE] out of memory for texture %d\n", k);
		return 0;
	}
	C3D_TexSetFilter(&s_wtex[k], GPU_NEAREST, GPU_NEAREST);
	C3D_TexSetWrap(&s_wtex[k], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	s_walloc[k] = 1;
	return 1;
}

// render target (tiled, 30 tiles x 50 tile rows) -> texture (32 x 64 tiles)
static void wipe_capture(int k)
{
	s_wcap[k] = 0;
	if (!wtex_alloc(k)) return;
	const u32 rowIdx = 240 * 8 * 4; // 7680 bytes per tile row
	C3D_SyncTextureCopy((u32 *)s_target->frameBuf.colorBuf, GX_BUFFER_DIM(rowIdx >> 4, 0),
	                    (u32 *)s_wtex[k].data, GX_BUFFER_DIM(rowIdx >> 4, (8192 - rowIdx) >> 4),
	                    rowIdx * 50, GX_TRANSFER_RAW_COPY(1));
	s_wcap[k] = 1;
	CONS_Printf("[FADE] capture %d ok\n", k);
}

static int s_postwipe = 0; // frames to record after a fade (black flicker)
extern void I_3DS_PresentJoin(void);
void I_3DS_WipeFree(void)
{
	I_3DS_PresentJoin();
	if (s_wcap[0] || s_wcap[1]) s_postwipe = 4;
	for (int k = 0; k < 2; k++)
	{
		if (s_walloc[k]) C3D_TexDelete(&s_wtex[k]);
		s_walloc[k] = s_wcap[k] = 0;
	}
	s_wipe_on = 0;
}

int I_3DS_WipeEnabled(void)
{
	static int on = -1;
	if (on < 0) on = RRCfg_OnDef("wipe", 1);
	return on && g_3ds_gpu3d;
}

// k = 0 screen before, 1 = screen after. pending = 2D is queued.
void I_3DS_WipeCapture(int k, int pending)
{
	extern void I_FinishUpdate(void);
	I_3DS_PresentJoin();
	if (s_state != 1 || !I_3DS_WipeEnabled()) return;
	s_wcap[k] = 0;
	if (pending || s_inframe || k == 1)
	{
		s_noshow = k;
		I_FinishUpdate(); // draws the frame and captures it (not shown)
		s_noshow = -1;
	}
	else if (s_haveframe)
		wipe_capture(k); // the last thing shown
	else
		CONS_Printf("[FADE] capture %d: no frame yet\n", k);
}

int I_3DS_WipeReady(void)
{
	if (!(s_state == 1 && s_wcap[0] && s_wcap[1]))
		CONS_Printf("[FADE] not ready: gpu=%d before=%d after=%d\n", s_state, s_wcap[0], s_wcap[1]);
	return s_state == 1 && s_wcap[0] && s_wcap[1];
}
void I_3DS_WipeSet(int on, float t) { I_3DS_PresentJoin(); s_wipe_on = on; s_wipe_t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t); }

// ---------------------------------------------------------------------------
// SAME effect as the PC shader (shaders.pk3: postprocesswipe):
// fade = 1 - mask*255/31 (FADEttff mask per frame, inverted 32-x if reverse)
//   mode 0 blend start->end, 1 invert, 2 "Mega Drive" to black, 3 to white.
// Computed on the CPU (only during the fade) and placed BELOW the 2D layer.
// ---------------------------------------------------------------------------
static int s_wmode = 0, s_wrev = 0;
static const unsigned char *s_wmask = NULL;
static int s_wmw = 0, s_wmh = 0;

void I_3DS_WipeFrame(int mode, int reverse, const unsigned char *mask, int mw, int mh)
{
	s_wipe_on = 1;
	s_wmode = mode; s_wrev = reverse; s_wmask = mask; s_wmw = mw; s_wmh = mh;
}

// capture offset (tiled, 32 tiles per row) split into X and Y:
// offset(X,Y) = s_offX[X] + s_offY[Y] (morton and tile bits do not overlap)
static int s_offX[400], s_offY[240], s_offok = 0;
static void wipe_win(float X, float Y, int *wx, int *wy)
{
	const C3D_FVec c = Mtx_MultiplyFVec4(&s_proj, FVec4_New(X, Y, 0.5f, 1.0f));
	*wx = (int)((c.x / c.w + 1.f) * 0.5f * 240.f);
	*wy = (int)((c.y / c.w + 1.f) * 0.5f * 400.f);
	if (*wx < 0) *wx = 0; if (*wx > 239) *wx = 239;
	if (*wy < 0) *wy = 0; if (*wy > 399) *wy = 399;
}
static inline int off_wx(int wx) { return (wx >> 3) * 64 + ((wx & 1) | ((wx & 2) << 1) | ((wx & 4) << 2)); }
static inline int off_my(int my) { return (my >> 3) * 2048 + (((my & 1) << 1) | ((my & 2) << 2) | ((my & 4) << 3)); }
static void wipe_affine(void)
{
	// Rotated screen: X only moves wy and Y only moves wx (or the reverse). It checks which.
	int ax, ay, bx, by;
	wipe_win(0.5f, 0.5f, &ax, &ay);
	wipe_win(200.5f, 0.5f, &bx, &by);
	const int x_moves_wx = (bx != ax);
	for (int X = 0; X < 400; X++)
	{
		int wx, wy; wipe_win(X + 0.5f, 0.5f, &wx, &wy);
		s_offX[X] = x_moves_wx ? off_wx(wx) : off_my(399 - wy);
	}
	for (int Y = 0; Y < 240; Y++)
	{
		int wx, wy; wipe_win(0.5f, Y + 0.5f, &wx, &wy);
		s_offY[Y] = x_moves_wx ? off_my(399 - wy) : off_wx(wx);
	}
	s_offok = 1;
}

// Puts the fade below the 2D layer (lin32: 0xRRGGBBAA premultiplied)
static void wipe_composite(u32 *lin32, int w, int h)
{
	if (!s_offok) wipe_affine();
	static int mflip = -1;
	if (mflip < 0) mflip = RRCfg_Int("wipe_mask_flip", 0) & 1;
	if (w > 400) w = 400;
	if (h > 240) h = 240;
	// shader: start = reverse ? end : start
	const u32 *ts = (const u32 *)s_wtex[s_wrev ? 1 : 0].data;
	const u32 *te = (const u32 *)s_wtex[s_wrev ? 0 : 1].data;
	// mask -> fade 0..256 (table) and mask column per X (table)
	int lutM[256];
	for (int raw = 0; raw < 256; raw++)
	{
		int r = s_wrev ? 32 - raw : raw;
		int M = 256 - (r * 256) / 31;
		lutM[raw] = M < 0 ? 0 : (M > 256 ? 256 : M);
	}
	static short mx[400];
	for (int X = 0; X < w; X++) mx[X] = (short)(s_wmw ? (X * s_wmw) / w : 0);
	for (int Y = 0; Y < h; Y++)
	{
		u32 *d = lin32 + Y * TEX_W;
		int my = s_wmh ? (Y * s_wmh) / h : 0;
		if (mflip) my = s_wmh - 1 - my;
		const unsigned char *mrow = s_wmask ? s_wmask + my * s_wmw : NULL;
		const int oy = s_offY[Y];
		for (int X = 0; X < w; X++)
		{
			const u32 top = d[X];
			const u32 ta = top & 0xFF;
			if (ta == 255) continue; // opaque 2D on top
			const int M = mrow ? lutM[mrow[mx[X]]] : lutM[0];
			const u32 sp = ts[s_offX[X] + oy];
			int r = sp >> 24, g = (sp >> 16) & 0xFF, b = (sp >> 8) & 0xFF;
			switch (s_wmode)
			{
			case 1: // invert
				r += ((255 - 2 * r) * M) >> 8; g += ((255 - 2 * g) * M) >> 8; b += ((255 - 2 * b) * M) >> 8;
				break;
			case 2: // Mega Drive to black: red goes first, then green, then blue
			{
				const int t = (3 * 255 * M) >> 8;
				int nr = r - t; if (nr < 0) nr = 0;
				int ng = g + r - t; if (ng < 0) ng = 0; if (ng > g) ng = g;
				int nb = b + g + r - t; if (nb < 0) nb = 0; if (nb > b) nb = b;
				r = nr; g = ng; b = nb;
				break;
			}
			case 3: // Mega Drive to white
			{
				const int ra = (3 * 255 * M) >> 8, ga = ra > 255 ? ra - 255 : 0, ba = ga > 255 ? ga - 255 : 0;
				r += ra; g += ga; b += ba;
				if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
				break;
			}
			default: // blend start -> end
			{
				if (M)
				{
					const u32 ep = te[s_offX[X] + oy];
					r += (((int)(ep >> 24) - r) * M) >> 8;
					g += (((int)((ep >> 16) & 0xFF) - g) * M) >> 8;
					b += (((int)((ep >> 8) & 0xFF) - b) * M) >> 8;
				}
				break;
			}
			}
			if (ta)
			{
				const int k = 255 - ta;
				r = (top >> 24) + ((r * k) >> 8); g = ((top >> 16) & 0xFF) + ((g * k) >> 8); b = ((top >> 8) & 0xFF) + ((b * k) >> 8);
				if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
			}
			d[X] = ((u32)r << 24) | ((u32)g << 16) | ((u32)b << 8) | 0xFF;
		}
	}
}

__attribute__((unused)) static void wipe_quad(void)
{
	if (s_wflip < 0) s_wflip = RRCfg_Int("wipe_flip", 0) & 3;
	static const float P[4][2] = {{0.f, 0.f}, {0.f, 240.f}, {400.f, 0.f}, {400.f, 240.f}};
	const float um = 240.f / 256.f, vm = 400.f / 512.f;
	C3D_ImmDrawBegin(GPU_TRIANGLE_STRIP);
	for (int i = 0; i < 4; i++)
	{
		// UV = position in the render target's window (same projection) -> no guessing about rotations
		const C3D_FVec c = Mtx_MultiplyFVec4(&s_proj, FVec4_New(P[i][0], P[i][1], 0.5f, 1.0f));
		float u = (c.x / c.w + 1.f) * 0.5f * um;
		float v = (c.y / c.w + 1.f) * 0.5f * vm;
		// The render target rows end up flipped relative to the texture
		// (512 tall): without this the image came out shifted ~112 px to the left.
		// wipe_flip: 1 = vertical mirror, 2 = without the 112 px correction
		// 1-v came out mirrored -> the image sits in rows 112..511 (shifted, not flipped)
		if (!(s_wflip & 2)) v += (512.f - 400.f) / 512.f;
		if (s_wflip & 1) u = 1.f - u;
		C3D_ImmSendAttrib(P[i][0], P[i][1], 0.5f, 0.0f);
		C3D_ImmSendAttrib(u, v, 0.0f, 0.0f);
	}
	C3D_ImmDrawEnd();
}

// Draws the fade below the 2D layer (program and attributes already set)
__attribute__((unused)) static void wipe_draw(void)
{
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	C3D_CullFace(GPU_CULL_NONE);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_TexEnvInit(C3D_GetTexEnv(1));
	C3D_TexEnvInit(C3D_GetTexEnv(2));
	C3D_TexEnv *env = C3D_GetTexEnv(0);

	// [0] opaque
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	C3D_TexBind(0, &s_wtex[0]);
	wipe_quad();

	// [1] with opacity t
	const u32 a = (u32)(s_wipe_t * 255.f + 0.5f);
	if (a == 0) return;
	// opacity via the constant BLEND color (with TexEnv alpha it was not visible)
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	C3D_BlendingColor((a << 24) | 0x00FFFFFF);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_CONSTANT_ALPHA, GPU_ONE_MINUS_CONSTANT_ALPHA, GPU_ONE, GPU_ZERO);
	C3D_TexBind(0, &s_wtex[1]);
	wipe_quad();
}

// ---------------------------------------------------------------------------
// GPU MENU BLENDS (moving letters + MENUBG2).
// The 2D pass is split into 2 layers: A = background (up to the blends), B = the rest (with
// transparency). Between the two the GPU draws the patches with add/subtract blending.
// Built by twodee_soft_3ds.cpp (I_3DS_RasterizeTwodee2). rr_config: menu_gpu = 0 turns it off.
// ---------------------------------------------------------------------------
static const unsigned char *s_layerB = NULL;
static u32 *s_linear2 = NULL;
static C3D_Tex s_tex2; static int s_tex2ok = 0, s_tex2idle = 0;
void I_3DS_GpuSetLayerB(const unsigned char *b) { s_layerB = b; }

typedef struct { const void *key; u32 sig; C3D_Tex tex; int W, H, idle; } GmTex;
#define GM_TEX 6
static GmTex s_gmtex[GM_TEX];
typedef struct { int t; float x0, y0, x1, y1; int flip, vflip, blend; float a; } GmQuad;
#define GM_Q 16
static GmQuad s_gmq[GM_Q];
static int s_ngmq = 0;

// Linear memory gets tight after races; the menu blend layers are small but need a contiguous block.
// Drop the world texture caches (they refill on demand) so the allocation can succeed.
extern void I_3DS_FreeWorldTextures(void);
extern void I_3DS_EtcCacheFree(void);
static void free_linear_caches(void)
{
	CONS_Printf("[GPUBLEND] dropping world texture caches (linear memory short)\n");
	I_3DS_EtcCacheFree();
	I_3DS_FreeWorldTextures();
}

static int pow2(int v) { int p = 8; while (p < v) p <<= 1; return p; }

// Returns the texture index (creating it if px != NULL) or -1.
int I_3DS_GpuBlendTex(const void *key, unsigned sig, int W, int H,
                       const unsigned char *px, const unsigned char *opaque, const unsigned *pal)
{
	for (int i = 0; i < GM_TEX; i++)
		if (s_gmtex[i].key == key && s_gmtex[i].sig == sig && s_gmtex[i].W == W && s_gmtex[i].H == H)
		{ s_gmtex[i].idle = 0; return i; }
	if (!px) return -1;
	const int TW = pow2(W), TH = pow2(H);
	if (TW > 1024 || TH > 1024) return -1;
	int slot = -1;
	for (int i = 0; i < GM_TEX; i++) if (!s_gmtex[i].key) { slot = i; break; }
	if (slot < 0) return -1;
	GmTex *g = &s_gmtex[slot];
	if (!C3D_TexInit(&g->tex, TW, TH, GPU_RGBA8))
	{
		// Linear memory is tight after races: drop the world texture caches and retry once
		free_linear_caches();
		if (!C3D_TexInit(&g->tex, TW, TH, GPU_RGBA8)) { CONS_Printf("[GPUBLEND] out of memory %dx%d\n", TW, TH); return -1; }
	}
	C3D_TexSetFilter(&g->tex, GPU_NEAREST, GPU_NEAREST);
	C3D_TexSetWrap(&g->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	u32 *t = (u32 *)g->tex.data;
	memset(t, 0, (size_t)TW * TH * 4);
	// hand-tiled: image row y (top = v 0) -> memory row TH-1-y
	for (int y = 0; y < H; y++)
	{
		const int m = TH - 1 - y;
		const int oy = (m >> 3) * (TW >> 3) * 64 + (((m & 1) << 1) | ((m & 2) << 2) | ((m & 4) << 3));
		for (int x = 0; x < W; x++)
		{
			if (!opaque[y * W + x]) continue;
			const unsigned c = pal[px[y * W + x]]; // 0xRRGGBB00
			t[oy + (x >> 3) * 64 + ((x & 1) | ((x & 2) << 1) | ((x & 4) << 2))] = c | 0xFF;
		}
	}
	GSPGPU_FlushDataCache(t, (size_t)TW * TH * 4);
	g->key = key; g->sig = sig; g->W = W; g->H = H; g->idle = 0;
	CONS_Printf("[GPUBLEND] texture %dx%d (%dx%d)\n", W, H, TW, TH);
	return slot;
}

void I_3DS_GpuBlendQuad(int t, float x0, float y0, float x1, float y1, int flip, int vflip, int blend, float a)
{
	if (t < 0 || s_ngmq >= GM_Q) return;
	GmQuad *q = &s_gmq[s_ngmq++];
	q->t = t; q->x0 = x0; q->y0 = y0; q->x1 = x1; q->y1 = y1; q->flip = flip; q->vflip = vflip; q->blend = blend; q->a = a;
}

// blend: 1 add, 2 subtract (destination - source), 3 subtract (source - destination)
static void gm_draw(float sx0, float scale)
{
	C3D_TexEnv *env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	C3D_AlphaTest(true, GPU_GREATER, 0);
	for (int i = 0; i < s_ngmq; i++)
	{
		const GmQuad *q = &s_gmq[i];
		const GmTex *g = &s_gmtex[q->t];
		if (!g->key) { static int n; if (n++ < 20) CONS_Printf("[GPUBLENDFAIL] quad %d of %d has no texture (freed)\n", i, s_ngmq); continue; }
		const GPU_BLENDEQUATION eq = q->blend == 1 ? GPU_BLEND_ADD : (q->blend == 2 ? GPU_BLEND_REVERSE_SUBTRACT : GPU_BLEND_SUBTRACT);
		const u32 a = (u32)(q->a * 255.f + 0.5f);
		if (a >= 255)
			C3D_AlphaBlend(eq, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ZERO, GPU_ONE);
		else
		{
			C3D_BlendingColor((a << 24) | 0x00FFFFFF);
			C3D_AlphaBlend(eq, GPU_BLEND_ADD, GPU_CONSTANT_ALPHA, GPU_ONE, GPU_ZERO, GPU_ONE);
		}
		C3D_TexBind(0, (C3D_Tex *)&g->tex);
		float u0 = 0.f, u1 = (float)g->W / g->tex.width, v0 = 0.f, v1 = (float)g->H / g->tex.height;
		if (q->flip) { const float tt = u0; u0 = u1; u1 = tt; }
		if (q->vflip) { const float tt = v0; v0 = v1; v1 = tt; }
		const float X0 = sx0 + q->x0 * scale, X1 = sx0 + q->x1 * scale, Y0 = q->y0 * scale, Y1 = q->y1 * scale;
		C3D_ImmDrawBegin(GPU_TRIANGLE_STRIP);
		C3D_ImmSendAttrib(X0, Y0, 0.5f, 0.0f); C3D_ImmSendAttrib(u0, v0, 0.0f, 0.0f);
		C3D_ImmSendAttrib(X0, Y1, 0.5f, 0.0f); C3D_ImmSendAttrib(u0, v1, 0.0f, 0.0f);
		C3D_ImmSendAttrib(X1, Y0, 0.5f, 0.0f); C3D_ImmSendAttrib(u1, v0, 0.0f, 0.0f);
		C3D_ImmSendAttrib(X1, Y1, 0.5f, 0.0f); C3D_ImmSendAttrib(u1, v1, 0.0f, 0.0f);
		C3D_ImmDrawEnd();
	}
}

static void gm_free_all(void)
{
	for (int i = 0; i < GM_TEX; i++)
		if (s_gmtex[i].key) { C3D_TexDelete(&s_gmtex[i].tex); s_gmtex[i].key = NULL; }
	if (s_tex2ok) { C3D_TexDelete(&s_tex2); s_tex2ok = 0; }
	if (s_linear2) { linearFree(s_linear2); s_linear2 = NULL; }
}

// Frees unused textures (when leaving the menu)
static void gm_idle(void)
{
	for (int i = 0; i < GM_TEX; i++)
		if (s_gmtex[i].key && ++s_gmtex[i].idle > 120)
		{
			C3D_TexDelete(&s_gmtex[i].tex);
			s_gmtex[i].key = NULL;
		}
	if (s_tex2ok && ++s_tex2idle > 120)
	{
		C3D_TexDelete(&s_tex2); s_tex2ok = 0;
		linearFree(s_linear2); s_linear2 = NULL;
	}
}

int I_3DS_GpuBlendOn(void)
{
	static int on = -1;
	if (on < 0) on = RRCfg_OnDef("menu_gpu", 1);
	return on && g_3ds_gpu3d && s_state == 1 && !s_wipe_on && s_noshow < 0;
}

// r_citro3d returns to the screen after drawing the sky into its texture
C3D_RenderTarget *I_3DS_GpuMainTarget(void) { return s_target; }

static void begin_frame(void)
{
	if (s_inframe)
		return;
	// Without SYNCDRAW: do not wait for vblank (see the note in gpu_init).
	extern unsigned long long g_gpu_t[10];
	const u64 t0_ = svcGetSystemTick();
	C3D_FrameBegin(0);
	g_gpu_t[0] += svcGetSystemTick() - t0_;
	C3D_RenderTargetClear(s_target, C3D_CLEAR_ALL, 0x000000FF, 0);
	C3D_FrameDrawOn(s_target);
	s_inframe = 1;
	s_frameid++;
}

// Called by hardware/r_citro3d before drawing. Returns the frame id
// (to know when to reconfigure state) or -1 if there is no GPU.
int I_3DS_GpuBegin3D(void)
{
	I_3DS_PresentJoin();
	if (s_state == 0)
		s_state = gpu_init();
	if (s_state != 1)
		return -1;
	begin_frame();
	return s_frameid;
}

// bgr: palette table [256][3] in B,G,R order (built by i_video_3ds.c)
static void build_lut16(const unsigned char (*bgr)[3])
{
	for (int i = 0; i < 256; i++)
	{
		if (g_3ds_gpu3d)
			// RGBA8 without alpha (set during conversion); index 255 is "transparent"
			s_lut32[i] = (i == 255) ? 0 :
				((u32)bgr[i][2] << 24) | ((u32)bgr[i][1] << 16) | ((u32)bgr[i][0] << 8);
		else
			s_lut16[i] = (u16)(((bgr[i][2] >> 3) << 11) | ((bgr[i][1] >> 2) << 5) | (bgr[i][0] >> 3));
	}
}

// Returns 1 if presented via GPU, 0 if the CPU path must be used.
static int s_drew3d = 0;
static int gpu_present_sync(const unsigned char *src, int w, int h, const unsigned char (*bgr)[3]);
// ---------------------------------------------------------------------------
// DEFERRED PRESENT. Measured in Azahar: `present` = ~12 ms per frame, and almost all of it
// is C3D_SyncDisplayTransfer waiting for the GPU to finish drawing (the conversion
// of the 2D layer was not the bottleneck). During a race, the frame is now left with the
// screen transfer + swap PENDING and they are completed at the start of the next
// frame (I_3DS_PresentJoin): the GPU draws while core 0 runs the tics.
// No threads (an attempt with a thread on core 1 hung inside the GPU).
// Cost: the image appears ~1 logic frame later. It is forced to complete before
// touching the GPU, s_linear, the fade or deleting textures. Only in GS_LEVEL with 3D.
// rr_config: deferred_present=0 turns it off.
// ---------------------------------------------------------------------------
static int s_pend = 0;
static unsigned long long s_pn_def = 0, s_pn_sync = 0, s_pt_join = 0;

void I_3DS_PresentJoin(void)
{
	if (!s_pend) return;
	s_pend = 0;
	extern unsigned long long g_gpu_t[10];
	const u64 t0 = svcGetSystemTick();
	gspWaitForP3D();
	g_gpu_t[2] += svcGetSystemTick() - t0;
	{ extern void I_3DS_GpuUploadsDone(void); I_3DS_GpuUploadsDone(); } // VRAM upload ring is free
	u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
	C3D_SyncDisplayTransfer((u32 *)s_target->frameBuf.colorBuf, GX_BUFFER_DIM(240, 400),
	                        (u32 *)fb, GX_BUFFER_DIM(240, 400), DISPLAY_TRANSFER_FLAGS);
	gfxSwapBuffers();
	s_pt_join += svcGetSystemTick() - t0;
}

extern int gamestate; // g_state.h (GS_LEVEL = 1)
static int s_defer_now = 0;

int I_3DS_GpuPresent(const unsigned char *src, int w, int h, const unsigned char (*bgr)[3])
{
	static int cfg = -1;
	if (cfg < 0) cfg = RRCfg_OnDef("deferred_present", 1);
	I_3DS_PresentJoin();
	s_defer_now = cfg && g_3ds_gpu3d && s_inframe && gamestate == 1 && s_noshow < 0 && s_postwipe == 0
		&& !s_wipe_on && !g_3ds_dbgflat;
	if (s_defer_now) s_pn_def++; else s_pn_sync++;
	return gpu_present_sync(src, w, h, bgr);
}

// Report every ~20 s (with diag).
void I_3DS_PresentReport(void)
{
	const double tpm = (double)SYSCLOCK_ARM11 / 1000.0;
	CONS_Printf("[PRESENT] deferred=%llu synchronous=%llu | wait for the transfer (join)=%.0f ms\n",
		s_pn_def, s_pn_sync, s_pt_join / tpm);
}

static int gpu_present_sync(const unsigned char *src, int w, int h, const unsigned char (*bgr)[3])
{
	s_drew3d = s_inframe; // 1 = the 3D pass was already drawn this frame
	if (s_state == 0)
		s_state = gpu_init();
	if (s_state != 1 || w > TEX_W || h > TEX_H)
		return 0;

	// 1. CPU: indices -> 16 bpp (sequential rows)
	build_lut16(bgr);
	if (g_3ds_gpu3d)
	{
		// real alpha (before RGBA5551 + 4x4 dithering in the 2D layer)
		u32 *lin32 = (u32 *)s_linear;
		// with layer B the alpha plane belongs to B (A = opaque background)
		const int useA = !s_layerB && g_3ds_2d_alpha_used && g_3ds_2d_alpha && g_3ds_2d_alpha_n >= w * h;
		for (int y = 0; y < h; y++)
		{
			const unsigned char *s = src + y * w;
			u32 *d = lin32 + y * TEX_W;
			const int al4 = (((unsigned)(uintptr_t)s) & 3) == 0; // 4 transparent pixels at once
			if (useA)
			{
				const unsigned char *a = g_3ds_2d_alpha + y * w;
				for (int x = 0; x < w; x++)
				{
					if (al4 && !(x & 3) && x + 4 <= w && *(const u32 *)(s + x) == 0xFFFFFFFFu) { d[x] = d[x + 1] = d[x + 2] = d[x + 3] = 0; x += 3; continue; }
					if (s[x] == 255) { d[x] = 0; continue; }
					const u32 c = s_lut32[s[x]], al = a[x];
					if (al == 255) { d[x] = c | 0xFF; continue; }
					// premultiplied (color * alpha): no dark edges with linear filtering
					d[x] = ((((c >> 24) * al / 255) << 24) | ((((c >> 16) & 0xFF) * al / 255) << 16)
						| ((((c >> 8) & 0xFF) * al / 255) << 8) | al);
				}
			}
			else
				for (int x = 0; x < w; x++)
				{
					if (al4 && !(x & 3) && x + 4 <= w && *(const u32 *)(s + x) == 0xFFFFFFFFu) { d[x] = d[x + 1] = d[x + 2] = d[x + 3] = 0; x += 3; continue; }
					d[x] = s[x] == 255 ? 0 : (s_lut32[s[x]] | 0xFF);
				}
		}
		if (useA)
		{
			memset(g_3ds_2d_alpha, 255, (size_t)w * h);
			g_3ds_2d_alpha_used = 0;
		}
		if (s_wipe_on && s_wcap[0] && s_wcap[1]) // fade as on PC, below the 2D
			wipe_composite(lin32, w, h);
		GSPGPU_FlushDataCache(s_linear, TEX_W * h * 4);
		// layer B (what goes on top of the GPU blends)
		if (s_layerB && !s_tex2ok)
		{
			s_linear2 = (u32 *)linearAlloc(TEX_W * TEX_H * 4);
			if (!s_linear2 || !linearGetSize(s_linear2)) { free_linear_caches(); s_linear2 = (u32 *)linearAlloc(TEX_W * TEX_H * 4); }
			if (s_linear2 && (C3D_TexInit(&s_tex2, TEX_W, TEX_H, GPU_RGBA8) || (free_linear_caches(), C3D_TexInit(&s_tex2, TEX_W, TEX_H, GPU_RGBA8))))
			{
				C3D_TexSetFilter(&s_tex2, GPU_LINEAR, GPU_LINEAR);
				C3D_TexSetWrap(&s_tex2, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
				memset(s_linear2, 0, TEX_W * TEX_H * 4);
				s_tex2ok = 1;
			}
			else
			{
				if (s_linear2) linearFree(s_linear2);
				s_linear2 = NULL;
				CONS_Printf("[GPUBLEND] out of memory for layer B\n");
			}
		}
		if (s_layerB && !s_tex2ok) // out of memory: B over A on the CPU (no blends)
		{
			for (int y = 0; y < h; y++)
			{
				const unsigned char *sb = s_layerB + y * w;
				u32 *d = lin32 + y * TEX_W;
				for (int x = 0; x < w; x++)
					if (sb[x] != 255) d[x] = s_lut32[sb[x]] | 0xFF;
			}
			GSPGPU_FlushDataCache(s_linear, TEX_W * h * 4);
			s_ngmq = 0;
		}
		if (s_layerB && s_tex2ok)
		{
			s_tex2idle = 0;
			const int useB = g_3ds_2d_alpha_used && g_3ds_2d_alpha && g_3ds_2d_alpha_n >= w * h;
			for (int y = 0; y < h; y++)
			{
				const unsigned char *sb = s_layerB + y * w;
				u32 *d = s_linear2 + y * TEX_W;
				const unsigned char *a = useB ? g_3ds_2d_alpha + y * w : NULL;
				for (int x = 0; x < w; x++)
				{
					if (sb[x] == 255) { d[x] = 0; continue; }
					const u32 c = s_lut32[sb[x]];
					const u32 al = a ? a[x] : 255;
					if (al == 255) { d[x] = c | 0xFF; continue; }
					d[x] = ((((c >> 24) * al / 255) << 24) | ((((c >> 16) & 0xFF) * al / 255) << 16)
						| ((((c >> 8) & 0xFF) * al / 255) << 8) | al);
				}
			}
			if (useB) { memset(g_3ds_2d_alpha, 255, (size_t)w * h); g_3ds_2d_alpha_used = 0; }
			GSPGPU_FlushDataCache(s_linear2, TEX_W * h * 4);
		}
	}
	else
	{
		for (int y = 0; y < h; y++)
		{
			const unsigned char *s = src + y * w;
			u16 *d = s_linear + y * TEX_W;
			for (int x = 0; x < w; x++)
				d[x] = s_lut16[s[x]];
		}
		GSPGPU_FlushDataCache(s_linear, TEX_W * h * sizeof(u16));
	}

	// flush the 3D vertex cache BEFORE the transfer: C3D_SyncDisplayTransfer
	// mid-frame does FrameSplit and the GPU starts reading the vertices right away.
	// (Citra is coherent; the console is not -> missing polygons/garbage)
	if (g_3ds_gpu3d)
	{
		extern void I_3DS_GpuFlush3D(void); // hardware/r_citro3d
		I_3DS_GpuFlush3D();
	}
	// 2. GPU: linear -> tiled texture
	const GX_TRANSFER_FORMAT tf = g_3ds_gpu3d ? GX_TRANSFER_FMT_RGBA8 : GX_TRANSFER_FMT_RGB565;
	C3D_SyncDisplayTransfer((u32 *)s_linear, GX_BUFFER_DIM(TEX_W, TEX_H),
	                        (u32 *)s_tex.data, GX_BUFFER_DIM(TEX_W, TEX_H),
	                        TEXTURE_TRANSFER_FLAGS(tf));
	const int layerB = g_3ds_gpu3d && s_layerB && s_tex2ok;
	if (layerB)
		C3D_SyncDisplayTransfer((u32 *)s_linear2, GX_BUFFER_DIM(TEX_W, TEX_H),
		                        (u32 *)s_tex2.data, GX_BUFFER_DIM(TEX_W, TEX_H),
		                        TEXTURE_TRANSFER_FLAGS(GX_TRANSFER_FMT_RGBA8));

	// 3. GPU: quad scaled to the screen height (320x200 -> 384x240),
	// on top of the 3D if there was any this frame.
	const float scale = 240.0f / (float)h;
	const float qw = w * scale, qh = 240.0f;
	const float x0 = (400.0f - qw) * 0.5f, x1 = x0 + qw;
	const float u1 = (float)w / TEX_W;
	const float vtop = 0.0f, vbot = (float)h / TEX_H; // image row 0 at v=0

	begin_frame();

	// DEBUG: SELECT = test triangle + hide the 2D layer
	const int dbg = g_3ds_gpu3d && g_3ds_dbgflat && g_3ds_dbgflat != 6 && g_3ds_dbgflat != 7; // modes 6 and 7: colors, with HUD and menus
	if (dbg)
	{
		extern void I_3DS_GpuDebugTri(void); // hardware/r_citro3d
		I_3DS_GpuDebugTri();
	}

	C3D_BindProgram(&s_prog);
	C3D_AttrInfo *attr = C3D_GetAttrInfo();
	AttrInfo_Init(attr);
	AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3); // v0 = position
	AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2); // v1 = uv
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uloc_proj, &s_proj);


	C3D_TexEnv *env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	C3D_TexEnvInit(C3D_GetTexEnv(1)); // the 3D uses 3 stages (Software light + brightmap)
	C3D_TexEnvInit(C3D_GetTexEnv(2));
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	C3D_CullFace(GPU_CULL_NONE);
	if (g_3ds_gpu3d) // real alpha blending of the 2D layer over the 3D
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA, GPU_ZERO, GPU_ONE); // premultiplied
	else
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	C3D_AlphaTest(g_3ds_gpu3d ? true : false, GPU_GREATER, 0);
	C3D_TexBind(0, &s_tex);

	if (!dbg) {
	C3D_ImmDrawBegin(GPU_TRIANGLE_STRIP);
	C3D_ImmSendAttrib(x0, 0.0f, 0.5f, 0.0f); C3D_ImmSendAttrib(0.0f, vtop, 0.0f, 0.0f);
	C3D_ImmSendAttrib(x0, qh,   0.5f, 0.0f); C3D_ImmSendAttrib(0.0f, vbot, 0.0f, 0.0f);
	C3D_ImmSendAttrib(x1, 0.0f, 0.5f, 0.0f); C3D_ImmSendAttrib(u1,   vtop, 0.0f, 0.0f);
	C3D_ImmSendAttrib(x1, qh,   0.5f, 0.0f); C3D_ImmSendAttrib(u1,   vbot, 0.0f, 0.0f);
	C3D_ImmDrawEnd();
	if (layerB) // GPU blends and layer B on top
	{
		gm_draw(x0, scale);
		C3D_TexEnvInit(env);
		C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
		C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA, GPU_ZERO, GPU_ONE);
		C3D_AlphaTest(true, GPU_GREATER, 0);
		C3D_TexBind(0, &s_tex2);
		C3D_ImmDrawBegin(GPU_TRIANGLE_STRIP);
		C3D_ImmSendAttrib(x0, 0.0f, 0.5f, 0.0f); C3D_ImmSendAttrib(0.0f, vtop, 0.0f, 0.0f);
		C3D_ImmSendAttrib(x0, qh,   0.5f, 0.0f); C3D_ImmSendAttrib(0.0f, vbot, 0.0f, 0.0f);
		C3D_ImmSendAttrib(x1, 0.0f, 0.5f, 0.0f); C3D_ImmSendAttrib(u1,   vtop, 0.0f, 0.0f);
		C3D_ImmSendAttrib(x1, qh,   0.5f, 0.0f); C3D_ImmSendAttrib(u1,   vbot, 0.0f, 0.0f);
		C3D_ImmDrawEnd();
	}
	}
	s_ngmq = 0;
	s_layerB = NULL;
	gm_idle();

	if (g_3ds_gpu3d && dbg) // only the test triangle adds vertices afterwards
	{
		extern void I_3DS_GpuFlush3D(void); // hardware/r_citro3d
		I_3DS_GpuFlush3D();
	}
	extern unsigned long long g_gpu_t[10];
	{ const u64 t1_ = svcGetSystemTick(); C3D_FrameEnd(0); g_gpu_t[3] += svcGetSystemTick() - t1_; }
	s_inframe = 0;
	if (s_defer_now) // waiting for the GPU and the transfer complete in I_3DS_PresentJoin
	{
		s_haveframe = 1;
		s_pend = 1;
		return 1;
	}

	// Wait for the GPU to finish and copy to the top framebuffer.
	{ const u64 t1_ = svcGetSystemTick(); gspWaitForP3D(); g_gpu_t[2] += svcGetSystemTick() - t1_; }
	{ extern void I_3DS_GpuUploadsDone(void); I_3DS_GpuUploadsDone(); } // VRAM upload ring is free
	if (s_noshow >= 0) // frame for the fade: capture it and do NOT show it
	{
		wipe_capture(s_noshow);
		return 1;
	}
	if (s_postwipe > 0) // right after a fade, an EMPTY frame (no 3D or 2D) looked black: it is not shown
	{
		s_postwipe--;
		if (!s_drew3d && g_3ds_gpu3d)
		{
			const u32 *l = (const u32 *)s_linear;
			int opaco = 0;
			for (int y = 0; y < h && !opaco; y += 4)
				for (int x = 0; x < w; x += 4)
					if (l[y * TEX_W + x] & 0xFF) { opaco = 1; break; }
			if (!opaco)
				return 1;
		}
	}
	s_haveframe = 1;
	u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
	C3D_SyncDisplayTransfer((u32 *)s_target->frameBuf.colorBuf, GX_BUFFER_DIM(240, 400),
	                        (u32 *)fb, GX_BUFFER_DIM(240, 400), DISPLAY_TRANSFER_FLAGS);
	gfxSwapBuffers();
	return 1;
}

#endif // __3DS__
