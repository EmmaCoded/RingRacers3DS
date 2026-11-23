// DR. ROBOTNIK'S RING RACERS — 3DS port
//-----------------------------------------------------------------------------
// Video presentation + input for 3DS, without citro3d.
//
// - The game draws in 8 bits (screens[0]) with the software renderer.
// - I_3DS_FinishFrame() copies screens[0] into a composition buffer, paints onto it
// the 2D layer on top (twodee_soft_3ds.cpp), converts with the palette to BGR8 and
// writes it straight into the top screen framebuffer.
// - The debug console (printf) lives on the BOTTOM screen.
//-----------------------------------------------------------------------------

#ifdef __3DS__

#include "i_3ds_log.h"
#include <3ds.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "../doomdef.h"
#include "../doomtype.h"
#include "../d_event.h"
#include "../d_main.h"
#include "../keys.h"
#include "../screen.h"
#include "../v_video.h"
#include "../i_system.h"
#include "../command.h"
#include "../i_joy.h"
#include "../g_input.h"
#include "i_3ds.h"


#define TOP_W 400
#define TOP_H 240

static UINT8 *s_compose = NULL;
static size_t s_compose_size = 0;
static UINT8 s_lut[256][3]; // BGR

// ---------------------------------------------------------------------------
// Video
// ---------------------------------------------------------------------------

int I_3DS_VidW(void) { return vid.width > 0 ? vid.width : I3DS_VIDWIDTH; }
int I_3DS_VidH(void) { return vid.height > 0 ? vid.height : I3DS_VIDHEIGHT; }

void I_3DS_SetupVideoBuffer(void)
{
	vid.width = I3DS_VIDWIDTH;
	vid.height = I3DS_VIDHEIGHT;
	vid.realwidth = vid.width;
	vid.realheight = vid.height;
	vid.bpp = 1;
	vid.rowbytes = vid.width * vid.bpp;
	vid.direct = NULL;
	vid.recalc = 1;
	vid.modenum = 0;

	if (vid.buffer)
		free(vid.buffer);
	vid.buffer = calloc(vid.rowbytes * vid.height, NUMSCREENS);
	if (!vid.buffer)
		I_Error("Not enough memory for video buffer\n");

	I_3DS_Both("[3DS] video %dx%d ready\n", vid.width, vid.height);
	gfxSetDoubleBuffering(GFX_TOP, true);
	// Clear both top-screen buffers (the console is no longer there)
	for (int i = 0; i < 2; i++)
	{
		u16 fw, fh;
		u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fw, &fh);
		memset(fb, 0, (size_t)fw * fh * 3);
		gfxSwapBuffers();
	}
}


// ---------------------------------------------------------------------------
// Simple profiler: accumulates ms per section and every 60 frames
// writes an average to sdmc:/ringracers_perf.txt
// ---------------------------------------------------------------------------
enum { I3DS_PROF_FRAME, I3DS_PROF_TICS, I3DS_PROF_DISPLAY, I3DS_PROF_2D, I3DS_PROF_PRESENT, I3DS_PROF_VBLANK, I3DS_PROF_MISC, I3DS_PROF_STATE, I3DS_PROF_WORLD, I3DS_PROF_MENU, I3DS_PROF_START, I3DS_PROF_COUNT };
static u64 s_prof_start[I3DS_PROF_COUNT];
static u64 s_prof_acc[I3DS_PROF_COUNT];
static int s_prof_frames = 0;
static int s_prof_calls[I3DS_PROF_COUNT];
int g_3ds_prof_cap = 0; // current fps cap (set by d_main)
static const char *s_prof_names[I3DS_PROF_COUNT] = {
	"frame", "tics", "display", "2d", "present", "wait", "sound", "state", "world3d", "menu", "start"
};

void I_3DS_ProfBegin(int id) { s_prof_start[id] = svcGetSystemTick(); s_prof_calls[id]++; }
void I_3DS_ProfEnd(int id) { s_prof_acc[id] += svcGetSystemTick() - s_prof_start[id]; }

unsigned long long g_gpu_t[10]; // FrameBegin, tex uploads, P3D wait, FrameEnd, evicted, bytes uploaded
unsigned long long g_2d_prof_t[8];
int g_2d_prof_n[8];
int g_2d_prof_px[8];
unsigned long long I_3DS_Ticks(void) { return svcGetSystemTick(); }

void I_3DS_ProfFrameDone(void)
{
	if (++s_prof_frames < 60)
		return;
	FILE *f = fopen("sdmc:/3ds/RingRacers3ds/ringracers_perf.txt", "a");
	if (f)
	{
		const double total = (double)s_prof_acc[I3DS_PROF_FRAME] / s_prof_frames / (SYSCLOCK_ARM11 / 1000.0);
		fprintf(f, "fps~%.1f cap=%d drawn=%d/%d waits=%d |", total > 0 ? 1000.0 / total : 0.0,
			g_3ds_prof_cap, s_prof_calls[I3DS_PROF_DISPLAY], s_prof_frames, s_prof_calls[I3DS_PROF_VBLANK]);
		for (int i = 0; i < I3DS_PROF_COUNT; i++)
			fprintf(f, " %s=%.2fms", s_prof_names[i],
				(double)s_prof_acc[i] / s_prof_frames / (SYSCLOCK_ARM11 / 1000.0));
		fprintf(f, "\n");
		{
			static const char *n2d[7] = {"quadOpaque", "quadBlend", "fill", "triFlat", "triColor", "lines", "decode"};
			fprintf(f, "   2D:");
			for (int i = 0; i < 7; i++)
				fprintf(f, " %s=%.2fms/%d/%dkpx", n2d[i],
					(double)g_2d_prof_t[i] / s_prof_frames / (SYSCLOCK_ARM11 / 1000.0),
					g_2d_prof_n[i] / s_prof_frames, g_2d_prof_px[i] / s_prof_frames / 1000);
			fprintf(f, "\n");
			memset(g_2d_prof_t, 0, sizeof(g_2d_prof_t));
			memset(g_2d_prof_n, 0, sizeof(g_2d_prof_n));
			memset(g_2d_prof_px, 0, sizeof(g_2d_prof_px));
		}
		{
			extern int g_3ds_gpu3d;
			extern int g_gpu_stat[8];
			if (g_3ds_gpu3d)
			{
				fprintf(f, "   GPU: polys=%d verts=%d uploads=%d failures=%d notex=%d clears=%d transforms=%d discarded=%d\n",
					g_gpu_stat[0], g_gpu_stat[1], g_gpu_stat[2], g_gpu_stat[3],
					g_gpu_stat[4], g_gpu_stat[5], g_gpu_stat[6], g_gpu_stat[7]);
				memset(g_gpu_stat, 0, sizeof(g_gpu_stat));
				fprintf(f, "   GPUT: framebegin=%.2fms uploads=%.2fms waitP3D=%.2fms frameend=%.2fms evicted=%llu uploadedKB=%llu byCap=%llu initFail=%llu residentKB=%llu ntex=%llu\n",
					(double)g_gpu_t[0] / s_prof_frames / (SYSCLOCK_ARM11 / 1000.0),
					(double)g_gpu_t[1] / s_prof_frames / (SYSCLOCK_ARM11 / 1000.0),
					(double)g_gpu_t[2] / s_prof_frames / (SYSCLOCK_ARM11 / 1000.0),
					(double)g_gpu_t[3] / s_prof_frames / (SYSCLOCK_ARM11 / 1000.0),
					g_gpu_t[4], g_gpu_t[5] / 1024, g_gpu_t[6], g_gpu_t[7], g_gpu_t[8] / 1024, g_gpu_t[9]);
				memset(g_gpu_t, 0, 8 * sizeof(g_gpu_t[0]));
				{ extern void rr3ds_texstats(FILE*); rr3ds_texstats(f); }
				{ extern void rr3ds_drvstats(FILE*, int); rr3ds_drvstats(f, s_prof_frames); } // 
			}
		}
		fclose(f);
	}
	memset(s_prof_acc, 0, sizeof(s_prof_acc));
	memset(s_prof_calls, 0, sizeof(s_prof_calls));
	s_prof_frames = 0;
}

static void build_lut(void)
{
	for (int i = 0; i < 256; i++)
	{
		const RGBA_t c = pLocalPalette ? pLocalPalette[i] : (RGBA_t){0};
		s_lut[i][0] = c.s.blue;
		s_lut[i][1] = c.s.green;
		s_lut[i][2] = c.s.red;
	}
}

int I_3DS_GpuPresent(const unsigned char *src, int w, int h, const unsigned char (*bgr)[3]);

static void present_indexed(const UINT8 *src, int w, int h)
{
	build_lut();
	if (I_3DS_GpuPresent(src, w, h, (const unsigned char (*)[3])s_lut))
		return;

	u16 fw, fh; // fw = 240 (logical height), fh = 400 (logical width): rotated screen
	u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fw, &fh);

	const int ox = (TOP_W - w) / 2;
	const int oy = (TOP_H - h) / 2;

	build_lut();

	// The framebuffer is column-major: each screen "column" x is
	// 240 pixels starting from the BOTTOM.
	for (int x = 0; x < w; x++)
	{
		const int sx = x + ox;
		if (sx < 0 || sx >= TOP_W)
			continue;
		u8 *col = fb + (size_t)sx * TOP_H * 3;
		const UINT8 *s = src + x;
		for (int y = 0; y < h; y++, s += w)
		{
			const int sy = y + oy;
			if (sy < 0 || sy >= TOP_H)
				continue;
			u8 *d = col + (size_t)(TOP_H - 1 - sy) * 3;
			const UINT8 *c = s_lut[*s];
			d[0] = c[0];
			d[1] = c[1];
			d[2] = c[2];
		}
	}

	gfxFlushBuffers();
	gfxSwapBuffers();
	// Without gspWaitForVBlank: in earlier testing, waiting on GSP events
	// can block forever in this setup. Pacing is set by the engine.

}

void I_3DS_FinishFrame(void)
{
	{ extern void I_3DS_PresentJoin(void); I_3DS_PresentJoin(); } // the core 1 thread still uses s_compose and the GPU
	if (!screens[0] || vid.width <= 0 || vid.height <= 0)
	{
		I_3DS_RasterizeTwodee(NULL, 0, 0);
		return;
	}

	const size_t sz = (size_t)vid.width * vid.height;
	if (s_compose_size != sz)
	{
		free(s_compose);
		s_compose = malloc(sz);
		s_compose_size = s_compose ? sz : 0;
	}
	if (!s_compose)
	{
		I_3DS_RasterizeTwodee(screens[0], vid.width, vid.height);
		present_indexed(screens[0], vid.width, vid.height);
		return;
	}

	// screens[0] stays untouched (3D only), as in the PC hwr2 pipeline.
	extern int g_3ds_gpu3d;
	if (g_3ds_gpu3d)
		memset(s_compose, 255, sz); // 255 = transparent: the GPU's 3D goes underneath
	else
		memcpy(s_compose, screens[0], sz);
	I_3DS_ProfBegin(I3DS_PROF_2D);
	{
		// menu blends on the GPU -> 2 layers (A = s_compose, B = s_compose2)
		extern int I_3DS_GpuBlendOn(void);
		extern int I_3DS_RasterizeTwodee2(UINT8 *dst, UINT8 *dstB, INT32 w, INT32 h);
		extern void I_3DS_GpuSetLayerB(const unsigned char *b);
		static UINT8 *s_compose2 = NULL;
		static size_t s_compose2_size = 0;
		UINT8 *dstB = NULL;
		if (g_3ds_gpu3d && I_3DS_GpuBlendOn())
		{
			if (s_compose2_size != sz)
			{
				free(s_compose2);
				s_compose2 = malloc(sz);
				s_compose2_size = s_compose2 ? sz : 0;
			}
			if (s_compose2)
			{
				memset(s_compose2, 255, sz);
				dstB = s_compose2;
			}
		}
		if (I_3DS_RasterizeTwodee2(s_compose, dstB, vid.width, vid.height) && dstB)
			I_3DS_GpuSetLayerB(dstB);
	}
	I_3DS_ProfEnd(I3DS_PROF_2D);
	I_3DS_ProfBegin(I3DS_PROF_PRESENT);
	present_indexed(s_compose, vid.width, vid.height);
	I_3DS_ProfEnd(I3DS_PROF_PRESENT);
}


// ---------------------------------------------------------------------------
// Bottom screen: touch button "BAIL" (Old 3DS / 2DS have no ZL).
// Drawn straight into the bottom framebuffer (RGB565, 320x240, rotated,
// simple buffer: the same one consoleInit set up).
// ---------------------------------------------------------------------------

#define BOT_W 320
#define BOT_H 240

static int s_ui_active = 0;
static int s_ui_pressed = -1;

static u16 rgb565(int r, int g, int b)
{
	return (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static void bot_fill(u16 *fb, int x0, int y0, int x1, int y1, u16 c)
{
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > BOT_W) x1 = BOT_W;
	if (y1 > BOT_H) y1 = BOT_H;
	for (int x = x0; x < x1; x++)
	{
		u16 *col = fb + x * BOT_H;
		for (int y = y0; y < y1; y++)
			col[BOT_H - 1 - y] = c;
	}
}

// Minimal 5x7 font (only the characters we use)
typedef struct { char ch; u8 rows[7]; } glyph_t;
static const glyph_t s_font[] = {
	{'A', {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
	{'B', {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
	{'C', {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}},
	{'D', {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}},
	{'E', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
	{'F', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
	{'G', {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}},
	{'H', {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
	{'V', {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
	{'>', {0x08,0x04,0x02,0x01,0x02,0x04,0x08}},
	{'I', {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}},
	{'L', {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
	{'M', {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}},
	{'N', {0x11,0x19,0x15,0x13,0x11,0x11,0x11}},
	{'O', {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
	{'P', {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
	{'R', {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
	{'S', {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},
	{'T', {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
	{'U', {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
	{'W', {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}},
	{'3', {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}},
	{'X', {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},
	{'Z', {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},
	{'+', {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}},
	{'=', {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}},
	{'/', {0x01,0x01,0x02,0x04,0x08,0x10,0x10}},
	{':', {0x00,0x04,0x04,0x00,0x04,0x04,0x00}},
	{' ', {0,0,0,0,0,0,0}},
};

static const u8 *glyph_rows(char c)
{
	for (size_t i = 0; i < sizeof(s_font) / sizeof(s_font[0]); i++)
		if (s_font[i].ch == c)
			return s_font[i].rows;
	return s_font[sizeof(s_font) / sizeof(s_font[0]) - 1].rows;
}

static int text_width(const char *t, int scale)
{
	return (int)strlen(t) * 6 * scale - scale;
}

static void bot_text(u16 *fb, int x, int y, const char *t, int scale, u16 c)
{
	for (; *t; t++, x += 6 * scale)
	{
		const u8 *rows = glyph_rows(*t);
		for (int ry = 0; ry < 7; ry++)
			for (int rx = 0; rx < 5; rx++)
				if (rows[ry] & (0x10 >> rx))
					bot_fill(fb, x + rx * scale, y + ry * scale,
					         x + (rx + 1) * scale, y + (ry + 1) * scale, c);
	}
}

static void bot_text_centered(u16 *fb, int y, const char *t, int scale, u16 c)
{
	bot_text(fb, (BOT_W - text_width(t, scale)) / 2, y, t, scale, c);
}

// the button takes only the bottom half; the info panel goes above.
#define BOT_UI_Y 124
static void bottom_ui_draw(int pressed)
{
	u16 *fb = (u16 *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
	const u16 bg     = rgb565(16, 16, 40);
	const u16 border = pressed ? rgb565(255, 255, 255) : rgb565(240, 200, 40);
	const u16 face   = pressed ? rgb565(200, 40, 40) : rgb565(150, 20, 20);
	const u16 shadow = rgb565(60, 0, 0);
	const u16 white  = rgb565(255, 255, 255);
	const u16 hint   = rgb565(170, 170, 200);

	bot_fill(fb, 0, BOT_UI_Y, BOT_W, BOT_H, bg);
	bot_fill(fb, 0, BOT_UI_Y, BOT_W, BOT_UI_Y + 2, hint);

	// Button (sinks 3px when pressed)
	const int off = pressed ? 3 : 0;
	if (!pressed)
		bot_fill(fb, 32, 136, 292, 202, shadow);
	bot_fill(fb, 26 + off, 130 + off, 286 + off, 196 + off, border);
	bot_fill(fb, 31 + off, 135 + off, 281 + off, 191 + off, face);
	bot_text(fb, (BOT_W - text_width("TOUCH: BAIL", 3)) / 2 + off, 152 + off, "TOUCH: BAIL", 3, white);

	bot_text_centered(fb, 210, "CHANGE IT IN PROFILE > CONTROLS", 1, hint);
	bot_text_centered(fb, 224, "RESPAWN: A + B + TOUCH", 1, hint);

	gfxFlushBuffers();
}

// ---------------------------------------------------------------------------
// Info panel (top half of the bottom screen).
// Keeps the last line of each "[XXX]" tag that goes through CONS_Printf
// (and up to 4 lines if they arrive in a burst, e.g. [MT]) and redraws it every
// ~1 s. It is not a scrolling log: it always shows the most recent data.
// The boot console is silenced (it draws into a buffer that is not visible).
// ---------------------------------------------------------------------------
extern PrintConsole *g_3ds_bootcon;

#define PANEL_COLS 40
#define PANEL_ROWS 30
#define PANEL_TAGS 12
#define PANEL_MAXLINES 5          // maximum rows of a block ([MT])
#define PANEL_LINELEN 80          // each line can take 2 rows of 40

// Each tag has a FIXED block of rows on screen (assigned the
// first time it appears). New text is written over the old text in
// same place: nothing scrolls upward.
typedef struct
{
	char tag[12];
	char lines[PANEL_MAXLINES][PANEL_LINELEN + 1];
	int n;          // lines stored from the current burst
	int maxlines;   // lines that fit in its block
	int row;        // first row of the block
	int rows;       // rows of the block
	unsigned last;  // tic of the last line
} panel_tag_t;

static panel_tag_t s_ptags[PANEL_TAGS];
static int s_pntags = 0;
static int s_pnextrow = 0;
static char s_pmsg[2][PANEL_LINELEN + 1]; // last untagged messages (at the very bottom)
static unsigned s_ptic = 0;
static int s_pactive = 0;
static int s_pdirty = 0;
static PrintConsole s_panelcon;
static u16 *s_mutefb = NULL;

static void panel_store(const char *line)
{
	char clean[PANEL_LINELEN + 1];
	int n = 0;
	for (const char *c = line; *c && n < PANEL_LINELEN; c++)
	{
		unsigned char u = (unsigned char)*c;
		if (u >= 0x20 && u < 0x7F)
			clean[n++] = (char)u;
	}
	clean[n] = 0;
	if (n == 0)
		return;

	if (clean[0] == '[')
	{
		const char *e = strchr(clean, ']');
		if (e && e - clean < (int)sizeof(s_ptags[0].tag))
		{
			char tag[12];
			memcpy(tag, clean, e - clean + 1);
			tag[e - clean + 1] = 0;
			panel_tag_t *t = NULL;
			for (int i = 0; i < s_pntags; i++)
				if (!strcmp(s_ptags[i].tag, tag)) { t = &s_ptags[i]; break; }
			if (!t)
			{
				// [MT] carries a header + list: 5 lines of 1 row. The rest: 1 line of 2 rows.
				const int ismt = !strcmp(tag, "[MT]");
				const int rows = ismt ? 5 : 2;
				if (s_pntags >= PANEL_TAGS || s_pnextrow + rows > PANEL_ROWS - 3)
					return;
				t = &s_ptags[s_pntags++];
				memset(t, 0, sizeof(*t));
				strcpy(t->tag, tag);
				t->maxlines = ismt ? 5 : 1;
				t->row = s_pnextrow;
				t->rows = rows;
				s_pnextrow += rows;
			}
			// New burst (more than 5 tics passed): replaces the previous one
			if (s_ptic - t->last > 5)
				t->n = 0;
			t->last = s_ptic;
			if (t->n < t->maxlines)
				strcpy(t->lines[t->n++], clean);
			else if (t->maxlines == 1)
				strcpy(t->lines[0], clean);
			s_pdirty = 1;
			return;
		}
	}
	strcpy(s_pmsg[0], s_pmsg[1]);
	strcpy(s_pmsg[1], clean);
	s_pdirty = 1;
}

void I_3DS_PanelFeed(const char *fmt, va_list ap)
{
	static char buf[512];
	vsnprintf(buf, sizeof(buf), fmt, ap);
	char *p = buf;
	while (*p)
	{
		char *nl = strchr(p, '\n');
		if (nl) *nl = 0;
		panel_store(p);
		if (!nl) break;
		p = nl + 1;
	}
}

// Writes a full row (padded with spaces to cover the previous contents)
static void panel_row(int row, const char *txt, int color)
{
	char out[PANEL_COLS + 1];
	int i = 0;
	for (; i < PANEL_COLS && txt && txt[i]; i++)
		out[i] = txt[i];
	for (; i < PANEL_COLS; i++)
		out[i] = ' ';
	out[PANEL_COLS] = 0;
	// no trailing newline: exactly 40 columns must not scroll
	printf("\x1b[%d;0H\x1b[%dm%.39s", row, color, out);
}

static void panel_draw(void)
{
	consoleSelect(&s_panelcon);
	for (int i = 0; i < s_pntags; i++)
	{
		const panel_tag_t *t = &s_ptags[i];
		if (t->maxlines == 1)
		{
			const char *l = t->lines[0];
			const int len = (int)strlen(l);
			panel_row(t->row, l, 37);
			panel_row(t->row + 1, len > 39 ? l + 39 : "", 37);
		}
		else
		{
			for (int j = 0; j < t->rows; j++)
				panel_row(t->row + j, j < t->n ? t->lines[j] : "", j == 0 ? 33 : 37);
		}
	}
	panel_row(PANEL_ROWS - 2, s_pmsg[0], 36);
	panel_row(PANEL_ROWS - 1, s_pmsg[1], 36);
	printf("\x1b[0m");
	if (g_3ds_bootcon)
		consoleSelect(g_3ds_bootcon);
}

static void panel_tick(void)
{
	s_ptic++;
	if (!s_pactive)
		return;
	if (s_pdirty && (s_ptic % 18) == 0)
	{
		s_pdirty = 0;
		panel_draw();
	}
}

void I_3DS_Panel_Start(void)
{
	// Silence the boot console: it keeps "printing" into a hidden buffer
	if (g_3ds_bootcon && !s_mutefb)
	{
		s_mutefb = (u16 *)calloc(BOT_W * BOT_H, sizeof(u16));
		if (s_mutefb)
			g_3ds_bootcon->frameBuffer = s_mutefb;
	}
	consoleInit(GFX_BOTTOM, &s_panelcon); // clears the bottom screen
	if (g_3ds_bootcon)
		consoleSelect(g_3ds_bootcon);
	s_pactive = 1;
	s_pdirty = 1;
	// BAIL touch button: disabled for now (the screen is for the panel)
}

void I_3DS_BottomUI_Activate(void)
{
	s_ui_active = 1;
	s_ui_pressed = -1; // force the first draw
}

static void bottom_ui_update(int pressed)
{
	if (!s_ui_active || pressed == s_ui_pressed)
		return;
	s_ui_pressed = pressed;
	bottom_ui_draw(pressed);
}

// ---------------------------------------------------------------------------
// Input: the 3DS is presented to the game as a GAMEPAD (device 1), with the
// same layout as an SDL/Xbox controller. That way the default controls work
// from g_input.c (gamepad column) and text fields do NOT receive letters.
//
//   A/B/X/Y      -> A/B/X/Y            START  -> Start      SELECT -> Back
//   D-Pad        -> D-Pad              Circle Pad -> left stick
// L / R        -> LT / RT triggers   (default: item / drift)
// ZL / ZR      -> LB / RB            (New 3DS only)
// Touch screen -> own button (nc_touch); Bail by default
// ---------------------------------------------------------------------------

#define PAD_DEVICE 1

// SDL_GameControllerButton-style button indices (see g_input.h KEY_JOY1/KEY_HAT1)
enum { PB_A = 0, PB_B, PB_X, PB_Y, PB_BACK, PB_GUIDE, PB_START, PB_LSTICK, PB_RSTICK,
       PB_LB, PB_RB, PB_DUP, PB_DDOWN, PB_DLEFT, PB_DRIGHT,
       PB_TOUCH = 20 /* = nc_touch (KEY_JOY1+20) */ };

typedef struct
{
	u32 mask;
	INT32 button;
} padmap_t;

static const padmap_t s_padmap[] = {
	// BY POSITION (like an Xbox controller): HUD icons are by position
	// (down/right/left/up), so the icon matches the 3DS button.
	{KEY_B,      PB_A},  // down
	{KEY_A,      PB_B},  // right
	{KEY_Y,      PB_X},  // left
	{KEY_X,      PB_Y},  // up
	{KEY_SELECT, PB_BACK},
	{KEY_START,  PB_START},
	{KEY_ZL,     PB_LB},
	{KEY_TOUCH,  PB_TOUCH}, // its own "TOUCH SCREEN" button, remappable in Controls
	{KEY_ZR,     PB_RB},
	{KEY_DUP,    PB_DUP},
	{KEY_DDOWN,  PB_DDOWN},
	{KEY_DLEFT,  PB_DLEFT},
	{KEY_DRIGHT, PB_DRIGHT},
};

static void post_pad(evtype_t type, INT32 d1, INT32 d2, INT32 d3)
{
	event_t ev;
	memset(&ev, 0, sizeof ev);
	ev.type = type;
	ev.data1 = d1;
	ev.data2 = d2;
	ev.data3 = d3;
	ev.device = PAD_DEVICE;
	D_PostEvent(&ev);
}

static INT32 circle_to_axis(int v)
{
	// Circle Pad: aprox -156..156. Dead zone + scaling to -JOYAXISRANGE..JOYAXISRANGE
	if (v > -20 && v < 20)
		return 0;
	INT32 r = (INT32)v * JOYAXISRANGE / 150;
	if (r > JOYAXISRANGE) r = JOYAXISRANGE;
	if (r < -JOYAXISRANGE) r = -JOYAXISRANGE;
	return r;
}

void I_3DS_PollInput(void)
{
	static u32 prev = 0;
	static int registered = 0;
	static INT32 last_x = 0, last_y = 0, last_lt = 0, last_rt = 0;

	if (registered == 0)
	{
		// Announce the "gamepad" to the engine (like an SDL_CONTROLLERDEVICEADDED)
		post_pad(ev_gamepad_device_added, 0, 0, 0);
		registered = 1;
	}
	else if (registered == 1)
	{
		// Add already processed: assign it to player 1
		G_SetDeviceForPlayer(0, PAD_DEVICE);
		registered = 2;
	}

	hidScanInput();
	const u32 held0 = hidKeysHeld();

	// bottom screen (bottom_3ds.c). With a map: touch only counts inside the BAIL button.
	u32 held = held0;
	{
		touchPosition tp;
		hidTouchRead(&tp);
		const int r = I_3DS_Bottom_Tick((held0 & KEY_TOUCH) != 0, tp.px, tp.py);
		static int s_wasPanel = 1;
		if (r < 0)
		{
			if (!s_wasPanel)
			{
				// back to the panel: clear what the map drew
				consoleSelect(&s_panelcon);
				consoleClear();
				if (g_3ds_bootcon) consoleSelect(g_3ds_bootcon);
				s_pdirty = 1;
				s_wasPanel = 1;
			}
			bottom_ui_update((held0 & KEY_TOUCH) != 0);
			panel_tick();
		}
		else
		{
			s_wasPanel = 0;
			s_ptic++;
			if (!r) held &= ~KEY_TOUCH;
		}
	}

	// Buttons
	for (size_t i = 0; i < sizeof(s_padmap) / sizeof(s_padmap[0]); i++)
	{
		const int now = (held & s_padmap[i].mask) != 0;
		const int before = (prev & s_padmap[i].mask) != 0;
		if (now != before)
			post_pad(now ? ev_keydown : ev_keyup, KEY_JOY1 + s_padmap[i].button, 0, 0);
	}
	prev = held;

	// Left stick (axis set 0): data2 = X, data3 = Y (positive Y = down)
	circlePosition cp;
	hidCircleRead(&cp);
	const INT32 ax = circle_to_axis(cp.dx);
	const INT32 ay = -circle_to_axis(cp.dy);
	if (ax != last_x || ay != last_y)
	{
		post_pad(ev_gamepad_axis, 0, ax, ay);
		last_x = ax;
		last_y = ay;
	}

	// Triggers (axis set 2): data2 = LT (L), data3 = RT (R)
	const INT32 lt = (held & KEY_L) ? JOYAXISRANGE : 0;
	const INT32 rt = (held & KEY_R) ? JOYAXISRANGE : 0;
	if (lt != last_lt || rt != last_rt)
	{
		post_pad(ev_gamepad_axis, 2, lt, rt);
		last_lt = lt;
		last_rt = rt;
	}
}

#endif // __3DS__