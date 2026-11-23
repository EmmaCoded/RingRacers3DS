// bottom_3ds.c - Bottom screen during a race: minimap + positions + BAIL button.
// Game option (Options > HUD > "Bottom Screen", cvar bottomscreen, saved in
// ringconfig.cfg): 0 = diagnostics panel (as before), 1 = BAIL on the right,
// 2 = BAIL on the left, 3 = BAIL at the bottom (wide).
// Drawn by the CPU straight into the bottom framebuffer (RGB565, 320x240 rotated, simple
// buffer left by consoleInit). Redrawn ~5 times per second (wall clock), not per frame.
// Memory: only one fixed buffer of map indices (.bss, 50 KB) -> no malloc (fragmented heap).
#ifdef __3DS__

#include <3ds.h>
#include <string.h>
#include <stdio.h>

#include "../doomdef.h"
#include "../doomstat.h"
#include "../command.h"
#include "../d_player.h"
#include "../g_game.h"
#include "../k_menu.h"
#include "../p_setup.h" // levelloading
#include "../st_stuff.h"
#include "../r_skins.h"
#include "../p_local.h"
#include "../p_mobj.h"
#include "../r_defs.h"
#include "../r_draw.h"
#include "../v_video.h"
#include "../rr_version.h"
#include "i_3ds.h"

extern consvar_t cv_3ds_bottom;

#define BW 320
#define BH 240
#define MAPMAX 224

typedef struct { int x0, y0, x1, y1; } rect_t;
typedef struct { rect_t map, list, btn; } layout_t;

// 1 = BAIL right, 2 = BAIL left, 3 = BAIL down
static const layout_t s_layouts[4] = {
	{{0,0,0,0},       {0,0,0,0},       {0,0,0,0}},
	{{0,8,224,232},   {228,4,316,150}, {228,156,316,236}},
	{{96,8,320,232},  {4,4,92,150},    {4,156,92,236}},
	{{0,0,200,168},   {204,4,316,168}, {8,176,312,236}},
};

static u8  s_idx[MAPMAX * MAPMAX];       // palette index of the already-scaled map
static u8  s_opaque[MAPMAX * MAPMAX / 8]; // bit = pixel with map
static u16 s_lut[256];
static int s_mw, s_mh, s_mx, s_my;        // size and origin of the scaled map on screen
static fixed_t s_scale;                   // screen pixels per image pixel
static int s_picw, s_pich;
static int s_haveMap;

static int s_lastLayout = -1, s_lastMap = -1, s_lastState = -1, s_lastEncore = -1;
static void *s_lastPic = NULL;
static int s_btnPressed = -1;
static u64 s_nextDraw = 0;

static inline u16 rgb(int r, int g, int b) { return (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); }
static inline u16 palc(UINT8 i) { RGBA_t c = pLocalPalette ? pLocalPalette[i] : (RGBA_t){0}; return rgb(c.s.red, c.s.green, c.s.blue); }

static u16 *fb(void) { return (u16 *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL); }

// Everything is composed in an off-screen copy and sent to the screen in one memcpy:
// drawing straight into the visible framebuffer (clear + redraw every 200 ms) made the
// bottom screen flicker. Falls back to direct drawing if the 150 KB buffer cannot be had.
static u16 *s_back = NULL;
static int s_backTried = 0;
static void present(u16 *f)
{
	u16 *dst = fb();
	if (f != dst) memcpy(dst, f, BW * BH * 2);
	GSPGPU_FlushDataCache(dst, BW * BH * 2);
}
static inline void px(u16 *f, int x, int y, u16 c)
{
	if ((unsigned)x < BW && (unsigned)y < BH)
		f[x * BH + (BH - 1 - y)] = c;
}
static void fill(u16 *f, int x0, int y0, int x1, int y1, u16 c)
{
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > BW) x1 = BW;
	if (y1 > BH) y1 = BH;
	for (int x = x0; x < x1; x++)
	{
		u16 *col = f + x * BH;
		for (int y = y0; y < y1; y++)
			col[BH - 1 - y] = c;
	}
}

// Text using libctru's 8x8 font (full ASCII)
static void text(u16 *f, int x, int y, const char *t, u16 c, int maxch)
{
	const PrintConsole *pc = consoleGetDefault();
	if (!pc || !pc->font.gfx) return;
	for (int n = 0; *t && n < maxch; t++, n++, x += 8)
	{
		int ch = (unsigned char)*t;
		if (ch >= 'a' && ch <= 'z') ch -= 32;
		ch -= pc->font.asciiOffset;
		if (ch < 0 || ch >= pc->font.numChars) continue;
		const u8 *g = pc->font.gfx + ch * 8;
		for (int ry = 0; ry < 8; ry++)
			for (int rx = 0; rx < 8; rx++)
				if (g[ry] & (0x80 >> rx))
					px(f, x + rx, y + ry, c);
	}
}

static const u16 C_BG = 0x1085;   // dark blue (16,16,40)
static const u16 C_LINE = 0xAD59; // light grey

// Time Attack, Prison Break and Special Stages have no Bail: the button is a RESET (retry) there
extern boolean blockreset;
static int reset_mode(void)
{
	return modeattacking != ATTACKING_NONE || gametype == GT_SPECIAL || (gametyperules & GTR_PRISONS);
}

static void draw_button(u16 *f, const rect_t *b, int pressed)
{
	const u16 border = pressed ? rgb(255, 255, 255) : rgb(240, 200, 40);
	const u16 face = pressed ? rgb(200, 40, 40) : rgb(150, 20, 20);
	fill(f, b->x0, b->y0, b->x1, b->y1, C_BG);
	const int o = pressed ? 2 : 0;
	if (!pressed) fill(f, b->x0 + 4, b->y0 + 4, b->x1, b->y1, rgb(60, 0, 0));
	fill(f, b->x0 + o, b->y0 + o, b->x1 - 4 + o, b->y1 - 4 + o, border);
	fill(f, b->x0 + 4 + o, b->y0 + 4 + o, b->x1 - 8 + o, b->y1 - 8 + o, face);
	const int w = b->x1 - b->x0 - 4, h = b->y1 - b->y0 - 4;
	const char *label = (gamestate != GS_LEVEL) ? "TOUCH" : reset_mode() ? "RESET" : "BAIL";
	const int lw = (int)strlen(label) * 8;
	text(f, b->x0 + o + (w - lw) / 2, b->y0 + o + (h - 8) / 2, label, rgb(255, 255, 255), 5);
}

// Scales the minimap image (column-based patch) to the map rectangle
static void build_map(const rect_t *r)
{
	patch_t *p = (patch_t *)minimapinfo.minimap_pic;
	s_haveMap = 0;
	memset(s_opaque, 0, sizeof s_opaque);
	if (!p || p->width <= 0 || p->height <= 0 || !p->columns || !p->columnofs)
		return;
	const int aw = r->x1 - r->x0, ah = r->y1 - r->y0;
	s_picw = p->width; s_pich = p->height;
	fixed_t sx = FixedDiv(aw << FRACBITS, s_picw << FRACBITS);
	fixed_t sy = FixedDiv(ah << FRACBITS, s_pich << FRACBITS);
	s_scale = sx < sy ? sx : sy;
	s_mw = FixedMul(s_picw << FRACBITS, s_scale) >> FRACBITS;
	s_mh = FixedMul(s_pich << FRACBITS, s_scale) >> FRACBITS;
	if (s_mw > MAPMAX) s_mw = MAPMAX;
	if (s_mh > MAPMAX) s_mh = MAPMAX;
	if (s_mw <= 0 || s_mh <= 0) return;
	s_mx = r->x0 + (aw - s_mw) / 2;
	s_my = r->y0 + (ah - s_mh) / 2;

	// Column by column over the image; each destination pixel takes the nearest source pixel
	static u8 colbuf[1024];
	static u8 colop[1024];
	const int H = s_pich < 1024 ? s_pich : 1024;
	for (int dx = 0; dx < s_mw; dx++)
	{
		int sxp = FixedDiv(dx << FRACBITS, s_scale) >> FRACBITS;
		if (sxp >= s_picw) sxp = s_picw - 1;
		if ((UINT32)p->columnofs[sxp] > (1u << 22)) continue;
		memset(colop, 0, H);
		const column_t *col = (const column_t *)(p->columns + p->columnofs[sxp]);
		int prev = 0;
		for (int guard = 0; col->topdelta != 0xFF && guard < 256; guard++)
		{
			int top = col->topdelta;
			if (top <= prev) top += prev;
			prev = top;
			const UINT8 *src = (const UINT8 *)col + 3;
			for (int i = 0; i < col->length && top + i < H; i++)
			{
				colbuf[top + i] = src[i];
				colop[top + i] = 1;
			}
			col = (const column_t *)((const UINT8 *)col + col->length + 4);
		}
		for (int dy = 0; dy < s_mh; dy++)
		{
			int syp = FixedDiv(dy << FRACBITS, s_scale) >> FRACBITS;
			if (syp >= H) continue;
			if (!colop[syp]) continue;
			const int o = dy * MAPMAX + dx;
			s_idx[o] = colbuf[syp];
			s_opaque[o >> 3] |= (u8)(1 << (o & 7));
		}
	}
	// Color: light gray (like the top-screen map before the exit)
	UINT8 *cm = R_GetTranslationColormap(TC_DEFAULT, SKINCOLOR_WHITE, GTC_CACHE);
	for (int i = 0; i < 256; i++)
		s_lut[i] = palc(cm ? cm[i] : (UINT8)i);
	s_haveMap = 1;
}

static void draw_map(u16 *f, const rect_t *r)
{
	fill(f, r->x0, r->y0, r->x1, r->y1, C_BG);
	if (!s_haveMap) return;
	for (int dy = 0; dy < s_mh; dy++)
		for (int dx = 0; dx < s_mw; dx++)
		{
			const int o = dy * MAPMAX + dx;
			if (s_opaque[o >> 3] & (1 << (o & 7)))
			{
				const int x = encoremode ? (s_mx + s_mw - 1 - dx) : (s_mx + dx);
				px(f, x, s_my + dy, s_lut[s_idx[o]]);
			}
		}
}

// World position -> bottom screen (same math as K_GetKartObjectPosToMinimapPos)
static int world_to_screen(fixed_t wx, fixed_t wy, int *sx, int *sy)
{
	if (!s_haveMap) return 0;
	fixed_t ax = FixedMul(wx, minimapinfo.zoom) - minimapinfo.offs_x;
	fixed_t ay = -(FixedMul(wy, minimapinfo.zoom) - minimapinfo.offs_y);
	fixed_t ix = ax + (s_picw << (FRACBITS - 1));
	fixed_t iy = ay + (s_pich << (FRACBITS - 1));
	int x = FixedMul(ix, s_scale) >> FRACBITS;
	int y = FixedMul(iy, s_scale) >> FRACBITS;
	if (encoremode) x = s_mw - 1 - x;
	*sx = s_mx + x;
	*sy = s_my + y;
	return 1;
}

static void dot(u16 *f, int x, int y, int rad, u16 c, u16 border)
{
	fill(f, x - rad - 1, y - rad - 1, x + rad + 2, y + rad + 2, border);
	fill(f, x - rad, y - rad, x + rad + 1, y + rad + 1, c);
}

extern patch_t *kp_unknownminimap;
extern patch_t *K_3DS_MinimapObjIcon(mobj_t *mobj, UINT8 **cmap);

// Draws a column-based patch centered on (cx, cy), optionally through a translation colormap
static void draw_icon(u16 *f, const patch_t *p, int cx, int cy, const UINT8 *cm)
{
	if (!p || p->width <= 0 || p->height <= 0 || p->width > 64 || p->height > 64 || !p->columns || !p->columnofs)
		return;
	const int ox = cx - p->width / 2, oy = cy - p->height / 2;
	for (int x = 0; x < p->width; x++)
	{
		if ((UINT32)p->columnofs[x] > (1u << 22)) continue;
		const column_t *col = (const column_t *)(p->columns + p->columnofs[x]);
		int prev = 0;
		for (int guard = 0; col->topdelta != 0xFF && guard < 64; guard++)
		{
			int top = col->topdelta;
			if (top <= prev) top += prev;
			prev = top;
			const UINT8 *src = (const UINT8 *)col + 3;
			for (int i = 0; i < col->length; i++)
			{
				const UINT8 c = cm ? cm[src[i]] : src[i];
				px(f, ox + x, oy + top + i, palc(c));
			}
			col = (const column_t *)((const UINT8 *)col + col->length + 4);
		}
	}
}

// White arrow (black outline) around the racer icon: where the character is facing.
static void facing_arrow(u16 *f, int cx, int cy, angle_t ang)
{
	float dx = FIXED_TO_FLOAT(FCOS(ang)), dy = -FIXED_TO_FLOAT(FSIN(ang));
	if (encoremode) dx = -dx;
	const float nx = -dy, ny = dx;
	const float base = 9.0f, tip = 18.0f, half = 5.0f;
	for (int pass = 0; pass < 2; pass++)
	{
		const float grow = pass == 0 ? 1.3f : 0.0f;
		const u16 col = pass == 0 ? rgb(0, 0, 0) : rgb(255, 255, 255);
		for (float r = base - grow; r <= tip + grow; r += 0.5f)
		{
			float hw = (tip - r) * half / (tip - base);
			if (hw < 0.0f) hw = 0.0f;
			hw += grow;
			for (float o = -hw; o <= hw; o += 0.5f)
				px(f, (int)(cx + dx * r + nx * o + 0.5f), (int)(cy + dy * r + ny * o + 0.5f), col);
		}
	}
}

static void player_icon(u16 *f, int i)
{
	mobj_t *mo = players[i].mo;
	if (!mo || P_MobjWasRemoved(mo) || !mo->skin) return;
	int x, y;
	if (!world_to_screen(mo->x, mo->y, &x, &y)) return;
	const INT32 skin = ((skin_t *)mo->skin)->skinnum;
	patch_t *pic = R_CanShowSkinInDemo(skin) ? faceprefix[skin][FACE_MINIMAP] : kp_unknownminimap;
	const UINT8 *cm = NULL;
	if (mo->color)
		cm = mo->colorized ? R_GetTranslationColormap(TC_RAINBOW, (skincolornum_t)mo->color, GTC_CACHE)
		                   : R_GetTranslationColormap(skin, (skincolornum_t)mo->color, GTC_CACHE);
	if (i == displayplayers[0])
		facing_arrow(f, x, y, mo->angle);
	if (pic && pic->width > 0)
		draw_icon(f, pic, x, y, cm);
	else
		dot(f, x, y, 2, palc(skincolors[players[i].skincolor].ramp[7]), rgb(0, 0, 0));
}

static void draw_players(u16 *f)
{
	const int me = displayplayers[0];

	// Minimap objects (SPB, prison capsules, UFO...), same ones as the top-screen map
	// Crash 41 (special stage after Meadow): the list held a stale pointer (0xffdfebc0) while the
	// level was loading. Skip it while loading and stop at any pointer outside user memory.
	for (mobj_t *mo = levelloading ? NULL : trackercap; mo; mo = mo->itnext)
	{
		if (((uintptr_t)mo & 3) || (uintptr_t)mo < 0x00100000u || (uintptr_t)mo >= 0x40000000u)
			break;
		UINT8 *cm = NULL;
		patch_t *pic = K_3DS_MinimapObjIcon(mo, &cm);
		int x, y;
		if (!pic || !world_to_screen(mo->x, mo->y, &x, &y)) continue;
		draw_icon(f, pic, x, y, cm);
	}

	// Others first, the player on top
	for (int pass = 0; pass < 2; pass++)
		for (int i = 0; i < MAXPLAYERS; i++)
		{
			if (!playeringame[i] || players[i].spectator || players[i].exiting) continue;
			if ((gametyperules & GTR_BUMPERS) && (players[i].pflags & PF_ELIMINATED)) continue;
			if ((i == me) != (pass == 1)) continue;
			player_icon(f, i);
		}
}

static void draw_list(u16 *f, const rect_t *r)
{
	fill(f, r->x0, r->y0, r->x1, r->y1, C_BG);
	const int me = displayplayers[0];
	char line[32];
	int y = r->y0 + 2;
	if (playeringame[me] && numlaps > 0)
	{
		int lap = players[me].laps;
		if (lap < 1) lap = 1;
		if (lap > numlaps) lap = numlaps;
		snprintf(line, sizeof line, "LAP %d/%d", lap, (int)numlaps);
		text(f, r->x0 + 2, y, line, rgb(240, 200, 40), (r->x1 - r->x0) / 8);
		y += 12;
	}
	fill(f, r->x0, y - 2, r->x1, y - 1, C_LINE);
	// One row per racer, sorted by position: place number, game icon, name as far as it fits.
	// A GP rival bot shows as RIVAL.
	const int rowh = 18;
	for (int pos = 1; pos <= MAXPLAYERS && y + rowh <= r->y1; pos++)
		for (int i = 0; i < MAXPLAYERS && y + rowh <= r->y1; i++)
		{
			if (!playeringame[i] || players[i].spectator || players[i].position != pos) continue;
			const u16 tc = i == me ? rgb(255, 255, 255) : rgb(170, 170, 200);
			snprintf(line, sizeof line, "%d", pos);
			text(f, r->x0 + 2, y + 5, line, rgb(240, 200, 40), 1);
			const INT32 skin = players[i].skin;
			patch_t *pic = (skin >= 0 && skin < numskins && R_CanShowSkinInDemo(skin)) ? faceprefix[skin][FACE_MINIMAP] : kp_unknownminimap;
			const UINT8 *cm = players[i].skincolor ? R_GetTranslationColormap(skin, (skincolornum_t)players[i].skincolor, GTC_CACHE) : NULL;
			if (pic && pic->width > 0)
				draw_icon(f, pic, r->x0 + 18, y + 8, cm);
			else
				fill(f, r->x0 + 12, y + 2, r->x0 + 24, y + 14, palc(skincolors[players[i].skincolor].ramp[7]));
			const char *name = (players[i].bot && players[i].botvars.rival) ? "Rival" : player_names[i];
			text(f, r->x0 + 30, y + 5, name, tc, (r->x1 - r->x0 - 30) / 8);
			y += rowh;
		}
}

static void draw_idle(u16 *f, const rect_t *btn, int pressed, int loading)
{
	fill(f, 0, 0, BW, BH, C_BG);
	text(f, (BW - 15 * 8) / 2, 100, "RING RACERS 3DS", rgb(240, 200, 40), 40);
	if (loading)
	{
		text(f, (BW - 10 * 8) / 2, 116, "LOADING...", rgb(255, 255, 255), 40);
		return;
	}
	text(f, (BW - (int)(strlen(RR3DS_VERSION) + 1) * 8) / 2, 116, "V" RR3DS_VERSION, C_LINE, 40);
	draw_button(f, btn, pressed); // menus use the TOUCH SCREEN button too
}

static int inside(const rect_t *r, int x, int y) { return x >= r->x0 && x < r->x1 && y >= r->y0 && y < r->y1; }

// Called every time input is read. Returns 1 if the touch counts as the BAIL button.
// layout 0: returns -1 (handled by the diagnostics panel, touch anywhere).
int I_3DS_Bottom_Tick(int touching, int tx, int ty)
{
	const int lay = cv_3ds_bottom.value;
	if (lay <= 0 || lay > 3)
	{
		s_lastLayout = lay;
		return -1;
	}
	const layout_t *L = &s_layouts[lay];
	// While a map loads the old minimap/objects are still there: show the blue screen, no map
	const int loading = (gamestate == GS_LEVEL && levelloading);
	const int inLevel = (gamestate == GS_LEVEL && !levelloading);
	// Still booting (loading, SEGA / Kart Krew logos): leave the bottom screen to the boot console
	if (gamestate == GS_NULL || gamestate == GS_INTRO)
	{
		s_lastLayout = -1; s_lastState = -1;
		return -1;
	}
	const int pressed = touching && inside(&L->btn, tx, ty);
	int full = 0;
	if (!s_backTried)
	{
		s_backTried = 1;
		s_back = (u16 *)malloc(BW * BH * 2);
		full = 1;
	}
	u16 *f = s_back ? s_back : fb();

	if (lay != s_lastLayout || (inLevel * 2 + loading) != s_lastState)
		full = 1;
	if (inLevel && (gamemap != s_lastMap || minimapinfo.minimap_pic != s_lastPic || encoremode != s_lastEncore))
		full = 1;

	if (full)
	{
		s_lastLayout = lay; s_lastState = inLevel * 2 + loading; s_lastMap = gamemap;
		s_lastPic = minimapinfo.minimap_pic; s_lastEncore = encoremode;
		s_btnPressed = -1;
		s_nextDraw = 0;
		if (!inLevel)
		{
			draw_idle(f, &L->btn, pressed, loading);
			present(f);
			s_btnPressed = pressed;
			return loading ? 0 : pressed;
		}
		fill(f, 0, 0, BW, BH, C_BG);
		build_map(&L->map);
	}
	if (!inLevel)
	{
		// Menus: the button works as TOUCH SCREEN (touch only counts inside it)
		if (pressed != s_btnPressed)
		{
			s_btnPressed = pressed;
			draw_idle(f, &L->btn, pressed, loading);
			present(f);
		}
		return loading ? 0 : pressed;
	}

	int dirty = 0;
	int result = pressed;
	if (pressed != s_btnPressed)
	{
		// RESET (Time Attack / Prison / Special): retry on press instead of sending Bail
		if (pressed && reset_mode() && !netgame && !demo.playback && !blockreset
			&& (modeattacking != ATTACKING_NONE || G_GametypeAllowsRetrying() || gametype == GT_TUTORIAL))
			M_TryAgain(0);
		s_btnPressed = pressed;
		draw_button(f, &L->btn, pressed);
		dirty = 1;
	}
	const u64 now = svcGetSystemTick();
	if (now >= s_nextDraw)
	{
		s_nextDraw = now + SYSCLOCK_ARM11 / 5; // ~200 ms
		// Repaint everything (background + button) so stale framebuffer contents can never show through
		fill(f, 0, 0, BW, BH, C_BG);
		draw_button(f, &L->btn, s_btnPressed > 0);
		draw_map(f, &L->map);
		draw_players(f);
		draw_list(f, &L->list);
		dirty = 1;
	}
	if (dirty)
		present(f);
	return reset_mode() ? 0 : result;
}

// For K_drawKartMinimap: 1 = the map is already shown on the bottom screen, do not draw it on top
int I_3DS_Bottom_HasMap(void)
{
	return cv_3ds_bottom.value >= 1 && cv_3ds_bottom.value <= 3;
}

#endif // __3DS__
