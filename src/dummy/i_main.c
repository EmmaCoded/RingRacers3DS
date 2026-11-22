// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2025 by Kart Krew.
// Copyright (C) 2020 by Sonic Team Junior.
// Copyright (C) 2000 by DooM Legacy Team.
// Copyright (C) 1996 by id Software, Inc.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------

#include "i_3ds_log.h"
#include "../doomdef.h"
#include "../rr_version.h"
#include "../rr_config.h"  // single-file configuration
#include "../d_main.h"
#include "../m_argv.h"
#include <stdio.h>
#include <unistd.h>

#ifdef __3DS__
#include <3ds.h>
unsigned int __stacksize__ = 2 * 1024 * 1024;
// LINEAR memory (the kind the GPU/DSP reads). NOTE: current libctru ignores the
// old name "__linear_heap_size__"; without this variable it splits the memory
// free HALF and HALF (Old 3DS: ~23 MB malloc + ~23 MB linear). Azahar's log
// showed 25 MB of linear memory FREE while malloc ran out of
// memory (std::bad_alloc in RR_RobotnikCoaster and in the special stage).
// Measured real usage: ~7 MB (GPU buffers, framebuffers, textures that do not fit
// in VRAM, audio). 12 MB linear -> the rest (~35 MB on Old 3DS) goes to malloc.
u32 __ctru_linear_heap_size = 12 * 1024 * 1024; // split experiment (12 vs 16 vs 20)
#endif

#ifdef __3DS__
PrintConsole *g_3ds_bootcon = NULL; // boot console (silenced afterwards)
#include <stdlib.h>
#include <string.h>
// Old 3DS mode (64 MB). Azahar gives 96 MB even when set to Old 3DS, so if
// sdmc:/3ds/rr_64mb exists the game "eats" the leftover memory at startup and never uses it.
// That way the free memory matches a real Old 3DS, on any emulator.
unsigned int g_3ds_reserved_kb = 0, g_3ds_appmem_kb = 0;
static void *g_3ds_reserve = NULL;
static void I_3DS_Reserve64(void)
{
	// ALWAYS measure the app memory size (in dev it came out as 0 KB)
	const unsigned int total = (unsigned int)osGetMemRegionSize(MEMREGION_APPLICATION);
	g_3ds_appmem_kb = total >> 10;
	if (!RRCfg_On("64mb")) return; // rr_config.txt
	const unsigned int target = 64u * 1024u * 1024u;
	if (total <= target) return;
	unsigned int sz = total - target;
	while (sz >= 1024u * 1024u && !(g_3ds_reserve = malloc(sz))) sz -= 1024u * 1024u;
	if (g_3ds_reserve) { memset(g_3ds_reserve, 0, sz); g_3ds_reserved_kb = sz >> 10; }
}
#include "i_3ds.h"
// CONSOLE SIMULATOR. Azahar barely fragments the heap; with `fragment_heap=1` in
// rr_config.txt, after reserving the arena the normal heap is filled with
// 48 KB blocks and every other one is released: the heap ends up with 48 KB holes (like real
// 3DS hardware: largest block ~50-100 KB). Used to verify nothing large depends on the heap.
extern void *__real_malloc(size_t);
extern void __real_free(void *);
static void I_3DS_FragmentHeap(void)
{
	static void *s_frag[4096];
	unsigned n = 0, k, loose = 0;
	if (!RRCfg_On("fragment_heap")) return;
	while (n < 4096 && (s_frag[n] = __real_malloc(48u * 1024u)) != NULL) n++;
	for (k = 0; k < n; k += 2) { __real_free(s_frag[k]); s_frag[k] = NULL; loose++; }
	I_3DS_Both("[3DS] FRAGMENT: heap with %u holes of 48 KB (%u KB held)\n", loose, (n - loose) * 48u);
}
#endif

#ifdef __3DS__
void I_Quit(void);
// The HOME button used to crash the 'menu' process. Now HOME = quit (same as Exit Game).
static aptHookCookie s_homeCookie;
static void s_homeHook(APT_HookType hook, void *param)
{
	(void)param;
	{
		extern void I_3DS_AudioHold(int on);
		CONS_Printf("[HOME] hook %s\n", hook == APTHOOK_ONSUSPEND ? "ONSUSPEND" : hook == APTHOOK_ONRESTORE ? "ONRESTORE" : hook == APTHOOK_ONSLEEP ? "ONSLEEP" : hook == APTHOOK_ONWAKEUP ? "ONWAKEUP" : "ONEXIT");
		if (hook == APTHOOK_ONRESTORE || hook == APTHOOK_ONWAKEUP) I_3DS_AudioHold(0);
	}
	// now HOME really works (i_system.c I_OsPolling). home_quit=1 in
	// rr_config.txt goes back to the old behavior (HOME = quit the game).
	if (hook == APTHOOK_ONSUSPEND && RRCfg_Int("home_quit", 0)) I_Quit();
}
#endif

int main(int argc, char **argv)
{
#ifdef __3DS__
	aptInit();
	aptHook(&s_homeCookie, s_homeHook, NULL);
	aptSetSleepAllowed(false); // as in derrekr/srb2_3ds 3b1269f (sleeping while the GPU is busy also crashes)
	romfsInit(); // assets inside the .3dsx (RomFS) if built with "romfs"
	gfxInitDefault();
	// Debug console on the BOTTOM screen: the top one is for the game.
	g_3ds_bootcon = consoleInit(GFX_BOTTOM, NULL);

	// Only printf - no swap, no wait, no flush.
	// The engine will do the first real gfxSwapBuffers inside aptMainLoop.
	I_3DS_Both("[3DS] VERSION %s (%s %s)\n", RR3DS_VERSION, __DATE__, __TIME__);
	{
		// VFP "RunFast" like MKW 3DS: flush-to-zero (bit 24) + default NaN (bit 25).
		// Does not touch rounding. Game logic is fixed-point: no effect on the game.
		unsigned fp0, fp1;
		__asm__ volatile("vmrs %0, fpscr" : "=r"(fp0));
		fp1 = fp0 | (3u << 24);
		__asm__ volatile("vmsr fpscr, %0" :: "r"(fp1));
		I_3DS_Both("[FPSCR] before=%08x now=%08x\n", fp0, fp1);
	}
	I_3DS_Both("[3DS] 1 top ok\n");
	I_3DS_Both("[3DS] 2 pre-args\n");
#endif

	myargc = argc;
	myargv = argv;

#ifdef __3DS__
	I_3DS_Both("[3DS] 3 post-args\n");
	// A single flush+swap here so earlier prints are visible
	// before entering the engine.
	gfxFlushBuffers();
	gfxSwapBuffers();
	I_3DS_Both("[3DS] 4 pre D_SRB2Main\n");
	gfxFlushBuffers();
	gfxSwapBuffers();
#endif

#ifdef __3DS__
	I_3DS_Reserve64();
	if (g_3ds_reserved_kb) I_3DS_Both("[3DS] 64MB mode (Old 3DS): reserved %u KB de %u KB\n", g_3ds_reserved_kb, g_3ds_appmem_kb);
	else I_3DS_Both("[3DS] 96MB mode (dev): no memory limit\n"); // used to always say 64MB
	// Single arena at startup, while the heap is still clean (z_zone.cpp)
	{ extern void Z_3DS_ArenaInit(void); Z_3DS_ArenaInit(); }
	I_3DS_FragmentHeap();
#endif
	CONS_Printf("Setting up SRB2...\n");
	D_SRB2Main();

#ifdef __3DS__
	I_3DS_Both("[3DS] 5 post D_SRB2Main\n");
	gfxFlushBuffers();
	gfxSwapBuffers();
	// End of startup: the bottom screen becomes a fixed panel + touch button
	I_3DS_Panel_Start();
#endif

	CONS_Printf("Entering main game loop...\n");
	D_SRB2Loop();

#ifndef __GNUC__
	return 0;
#endif
}
