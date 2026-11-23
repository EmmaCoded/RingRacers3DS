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

#include "../doomdef.h"
#include "../doomtype.h"
#include "../i_system.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#ifdef __3DS__
#include <3ds.h>
#include <unistd.h>
#include "i_3ds.h"
#include "i_3ds_log.h"
#endif

FILE *logstream = NULL;

#ifdef __3DS__
// Log to a file on the SD card.
// Path relative to the .3dsx CWD (normally /3ds/ringracers/).
#include <errno.h>
#include "../rr_version.h"
#include <malloc.h>
#include <time.h>
#include <sys/stat.h>
static FILE *_3ds_log = NULL;

// last FAT error from the SD card (to learn WHY fopen fails on real hardware)
unsigned I_3DS_SdError(void)
{
	u32 e = 0;
	if (R_FAILED(FSUSER_GetSdmcFatfsError(&e))) return 0xFFFFFFFFu;
	return e;
}

// name unique per boot (rrlog_MMDD_HHMMSS.txt / rrdiag_...). If sdmc:/3ds/RingRacers3ds/fixed_log exists
// (Azahar/rrbench) the usual ringracers_log.txt and diag_sd.txt are used.
static char s_logpath[96], s_diagpath[96];
static void _3ds_paths_init(void)
{
	if (s_logpath[0]) return;
	struct stat st;
	if (stat("sdmc:/3ds/RingRacers3ds/fixed_log", &st) == 0) // stat, not fopen (fopen is wrapped and would log right here)
	{
		snprintf(s_logpath, sizeof s_logpath, "sdmc:/3ds/RingRacers3ds/ringracers_log.txt");
		snprintf(s_diagpath, sizeof s_diagpath, "sdmc:/3ds/RingRacers3ds/diag_sd.txt");
		return;
	}
	const time_t t = time(NULL); const struct tm *tm = localtime(&t);
	snprintf(s_logpath, sizeof s_logpath, "sdmc:/3ds/RingRacers3ds/rrlog_%02d%02d_%02d%02d%02d.txt",
		tm ? tm->tm_mon + 1 : 0, tm ? tm->tm_mday : 0, tm ? tm->tm_hour : 0, tm ? tm->tm_min : 0, tm ? tm->tm_sec : 0);
	snprintf(s_diagpath, sizeof s_diagpath, "sdmc:/3ds/RingRacers3ds/rrdiag_%02d%02d_%02d%02d%02d.txt",
		tm ? tm->tm_mon + 1 : 0, tm ? tm->tm_mday : 0, tm ? tm->tm_hour : 0, tm ? tm->tm_min : 0, tm ? tm->tm_sec : 0);
}
const char *I_3DS_LogPath(void) { _3ds_paths_init(); return s_logpath; }
const char *I_3DS_DiagPath(void) { _3ds_paths_init(); return s_diagpath; }

static void _3ds_log_open(void)
{
	if (!_3ds_log)
	{
		mkdir("sdmc:/3ds/RingRacers3ds", 0777); // the whole game lives in sdmc:/3ds/RingRacers3ds
		_3ds_log = fopen(I_3DS_LogPath(), "a");
		if (!_3ds_log) printf("[LOG] could NOT open the log: errno=%d fatfs=%u\n", errno, I_3DS_SdError());
		if (_3ds_log)
		{
			setvbuf(_3ds_log, NULL, _IOFBF, 16 * 1024); // no fflush per line
			{ // the log is NOT erased: each startup appends a header (console testing)
				bool n3ds = false; APT_CheckNew3DS(&n3ds);
				const time_t t = time(NULL); const struct tm *tm = localtime(&t);
				fprintf(_3ds_log, "\n==================== NEW TEST %04d-%02d-%02d %02d:%02d:%02d (%s) v%s ====================\n",
					tm ? tm->tm_year + 1900 : 0, tm ? tm->tm_mon + 1 : 0, tm ? tm->tm_mday : 0,
					tm ? tm->tm_hour : 0, tm ? tm->tm_min : 0, tm ? tm->tm_sec : 0, n3ds ? "New 3DS" : "Old 3DS", RR3DS_VERSION);
			}
			// Print RAM once when the log is opened, independent of the counter
			// osGetMemRegionFree returns 0 (newlib already took the whole heap): use mallinfo + linear
			{ extern unsigned int g_3ds_reserved_kb;
			  struct mallinfo mi = mallinfo();
			  UINT32 total = (UINT32)osGetMemRegionSize(MEMREGION_APPLICATION);
			  fprintf(_3ds_log, "[MEM_INIT] app=%u KB | heap: arena=%u KB used=%u KB (64MB reserve=%u KB) free=%u KB | linear free=%u KB\n",
				total >> 10, (unsigned)(mi.arena >> 10), (unsigned)(mi.uordblks >> 10), g_3ds_reserved_kb,
				(unsigned)(mi.fordblks >> 10), (unsigned)(linearSpaceFree() >> 10)); }
			{ extern unsigned int g_3ds_reserved_kb, g_3ds_appmem_kb; // 64 MB mode (file sdmc:/3ds/rr_64mb)
			  fprintf(_3ds_log, "[MEM64] app memory=%u KB, reserved=%u KB -> %s\n", g_3ds_appmem_kb, g_3ds_reserved_kb,
				g_3ds_reserved_kb ? "SIMULATING OLD 3DS 64 MB" : "no limit (96 MB)"); }
			fflush(_3ds_log);
		}
	}
}

// each log line starts with the milliseconds since startup
// ("[  12345] ..."), to measure how long each part of loading takes.
static u64 s_log_t0 = 0;
static int s_log_bol = 1; // at the beginning of a line
static void _3ds_logv(const char *fmt, va_list ap)
{
	_3ds_log_open();
	if (_3ds_log)
	{
		char buf[1024];
		vsnprintf(buf, sizeof buf, fmt, ap);
		if (!s_log_t0) s_log_t0 = svcGetSystemTick();
		const char *p = buf;
		while (*p)
		{
			if (s_log_bol)
			{
				const unsigned ms = (unsigned)((svcGetSystemTick() - s_log_t0) / (SYSCLOCK_ARM11 / 1000));
				fprintf(_3ds_log, "[%7u] ", ms);
				s_log_bol = 0;
			}
			const char *nlp = strchr(p, '\n');
			if (nlp) { fwrite(p, 1, (size_t)(nlp - p + 1), _3ds_log); p = nlp + 1; s_log_bol = 1; }
			else { fputs(p, _3ds_log); break; }
		}
		// Used to fflush to the SD card on EVERY message (1-20 ms each, bursts of 20-40 lines).
		// Now: roughly every ~1 s, or immediately if it is an error/crash/exit (so it is not lost).
		{
			static u64 s_lastFlush = 0;
			const u64 now = svcGetSystemTick();
			if (now - s_lastFlush > SYSCLOCK_ARM11 || strstr(buf, "TRAP") || strstr(buf, "ERROR") || strstr(buf, "Error")
				|| strstr(buf, "[FIN]") || strstr(buf, "I_Quit") || strstr(buf, "out of memory") || strstr(buf, "BENCH") || strstr(buf, "memory") || strstr(buf, "OUT OF MEMORY") // Out of memory
				|| strstr(buf, "[SDDIAG]") || strstr(buf, "[STATS]") || strstr(buf, "[AUDIO] no") || strstr(buf, "[CFG]") || strstr(buf, "[PVS]") || strstr(buf, "[FOPEN]")) // diagnostics of the real SD
			{
				fflush(_3ds_log);
				s_lastFlush = now;
				// On FAT the file SIZE is only stored on close. If the console powers off or
				// hangs without closing, FTP sees the stale size (same KB). Close and reopen every ~3 s.
				static u64 s_lastClose = 0;
				if (now - s_lastClose > 3 * SYSCLOCK_ARM11 || strstr(buf, "[SDDIAG]") || strstr(buf, "[STATS]") || strstr(buf, "[AUDIO] no")
					|| strstr(buf, "ERROR") || strstr(buf, "TRAP") || strstr(buf, "[CFG]") || strstr(buf, "[FOPEN]"))
				{
					// CLOSE first and reopen (the 3DS does not allow opening the same file twice for writing;
					// and the size is only stored on the SD card on close). Retried once.
					s_lastClose = now;
					fclose(_3ds_log);
					_3ds_log = fopen(I_3DS_LogPath(), "a");
					if (!_3ds_log) _3ds_log = fopen(I_3DS_LogPath(), "a");
					if (_3ds_log) setvbuf(_3ds_log, NULL, _IOFBF, 16 * 1024);
				}
			}
		}
	}
}

static void _3ds_log_msg(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	_3ds_logv(fmt, ap);
	va_end(ap);
}

// bottom screen (printf) AND SD log at the same time.
void I_3DS_Both(const char *fmt, ...)
{
	va_list ap, ap2;
	va_start(ap, fmt);
	va_copy(ap2, ap);
	vprintf(fmt, ap);
	_3ds_logv(fmt, ap2);
	va_end(ap2);
	va_end(ap);
}

// Shows a message on screen and waits for the user to press A before resetting.
// This way the error can be read in Azahar instead of the loop hiding it.
static void _3ds_fatal_screen(const char *label, const char *fmt, va_list ap)
{
	// Write to the top screen (where consoleInit from i_main.c is)
	printf("\n\n*** %s ***\n", label);
	vprintf(fmt, ap);
	printf("\n\nPress A to continue...\n");
	gfxFlushBuffers();
	gfxSwapBuffers();

	// Wait for button A (reset the console) or HOME (leave the game).
	// Never block on the GPU here: if the GPU is stuck, HOME would be ignored.
	while (aptMainLoop())
	{
		hidScanInput();
		if (hidKeysDown() & KEY_A)
			return;
		svcSleepThread(16000000LL);
	}
	exit(0); // HOME pressed
}
#endif

UINT8 graphics_started = 0;

UINT8 keyboard_started = 0;

UINT32 I_GetFreeMem(UINT32 *total)
{
#ifdef __3DS__
	UINT32 free = (UINT32)osGetMemRegionFree(MEMREGION_APPLICATION);
	UINT32 used = (UINT32)osGetMemRegionUsed(MEMREGION_APPLICATION);
	*total = free + used;
	return free;
#else
	*total = 0;
	return 0;
#endif
}

void I_Sleep(UINT32 ms)
{
#ifdef __3DS__
	svcSleepThread((s64)ms * 1000000LL);
#endif
}

precise_t I_GetPreciseTime(void)
{
#ifdef __3DS__
	return (precise_t)svcGetSystemTick();
#else
	return 0;
#endif
}

UINT64 I_GetPrecisePrecision(void)
{
#ifdef __3DS__
	return (UINT64)SYSCLOCK_ARM11;
#else
	return 1000000;
#endif
}

void I_GetEvent(void){}

void I_OsPolling(void)
{
#ifdef __3DS__
	// HOME (idea from derrekr/srb2_3ds 3b1269f): do NOT hand control to the
	// 3DS menu with the GPU in the middle of a frame (deferred present leaves work pending).
	// On HOME/close: finish the pending frame, wait 1 frame and only then
	// there call aptMainLoop (which releases the GPU and hands over control).
	{
		extern void I_3DS_PresentJoin(void);
		extern void I_3DS_AudioHold(int on);
		static int waitFrames = 0, audioheld = 0;
		if (aptShouldJumpToHome() || aptShouldClose())
		{
			if (!waitFrames)
			{
				extern int RR3DS_ArenaVerify(const char *); RR3DS_ArenaVerify("HOME");
				_3ds_log_msg("[HOME] requested (jump=%d close=%d)\n", (int)aptShouldJumpToHome(), (int)aptShouldClose());
			}
			if (!audioheld) { audioheld = 1; I_3DS_AudioHold(1); }
			I_3DS_PresentJoin();
			if (++waitFrames < 2)
				goto pollAgain; // one more frame before releasing
		}
		waitFrames = 0;
		if (!aptMainLoop())
			I_Quit();
		if (audioheld && !aptShouldJumpToHome() && !aptShouldClose()) { audioheld = 0; I_3DS_AudioHold(0); }
	}
pollAgain:
	I_3DS_PollInput(); // hidScanInput + buttons -> keyboard events
#endif
}

ticcmd_t *I_BaseTiccmd(void)
{
	return NULL;
}

ticcmd_t *I_BaseTiccmd2(void)
{
	return NULL;
}

void I_Quit(void)
{
#ifdef __3DS__
	_3ds_log_msg("[I_Quit] clean exit\n");
	if (_3ds_log) { fflush(_3ds_log); fclose(_3ds_log); _3ds_log = NULL; }
	APT_HardwareResetAsync();
	while (1) svcSleepThread(1000000LL);
#else
	exit(0);
#endif
}

void I_Error(const char *error, ...)
{
#ifdef __3DS__
	va_list ap;

	// 1. Save to the log
	va_start(ap, error);
	_3ds_log_msg("[I_ERROR] ");
	_3ds_logv(error, ap);
	_3ds_log_msg("\n");
	va_end(ap);
	if (_3ds_log) { fflush(_3ds_log); fclose(_3ds_log); _3ds_log = NULL; }

	// 2. Show on screen and wait for A - so the error can be read in Azahar
	va_start(ap, error);
	_3ds_fatal_screen("I_ERROR", error, ap);
	va_end(ap);

	APT_HardwareResetAsync();
	while (1) svcSleepThread(1000000LL);
#else
	(void)error;
	exit(-1);
#endif
}

void I_Tactile(FFType Type, const JoyFF_t *Effect)
{
	(void)Type;
	(void)Effect;
}

void I_Tactile2(FFType Type, const JoyFF_t *Effect)
{
	(void)Type;
	(void)Effect;
}

void I_JoyScale(void){}

void I_JoyScale2(void){}

void I_InitJoystick(void){}

void I_InitJoystick2(void){}

INT32 I_NumJoys(void)
{
	return 0;
}

const char *I_GetJoyName(INT32 joyindex)
{
	(void)joyindex;
	return NULL;
}

#ifndef NOMUMBLE
void I_UpdateMumble(const mobj_t *mobj, const listener_t listener)
{
	(void)mobj;
	(void)listener;
}
#endif

void I_OutputMsg(const char *error, ...)
{
#ifdef __3DS__
	va_list ap;
	va_start(ap, error);
	_3ds_logv(error, ap);
	va_end(ap);
	va_start(ap, error);
	I_3DS_PanelFeed(error, ap);
	va_end(ap);

	static int _msg_count = 0;
	if ((++_msg_count % 200) == 0)
	{
		UINT32 total = 0;
		UINT32 free = I_GetFreeMem(&total);
		_3ds_log_msg("[MEM] free=%u KB / total=%u KB\n", free >> 10, total >> 10);
	}
#else
	(void)error;
#endif
}

void I_StartupMouse(void){}

void I_StartupMouse2(void){}

INT32 I_GetKey(void)
{
	return 0;
}

void I_StartupTimer(void){}

void I_AddExitFunc(void (*func)())
{
	(void)func;
}

void I_RemoveExitFunc(void (*func)())
{
	(void)func;
}

INT32 I_StartupSystem(void)
{
#ifdef __3DS__
	chdir("sdmc:/3ds/RingRacers3ds");
#endif
	return -1;
}

void I_ShutdownSystem(void){}

void I_GetDiskFreeSpace(INT64* freespace)
{
	*freespace = 0;
}

char *I_GetUserName(void)
{
	return NULL;
}

INT32 I_mkdir(const char *dirname, INT32 unixright)
{
	// used to be a stub (-1): media/replay/... was never created and replays
	// (time attack, prisons, special) were not saved (fopen errno=2).
	(void)unixright;
	return mkdir(dirname, 0777);
}

const char *I_LocateWad(void)
{
#ifdef __3DS__
	// The working folder (config, log, savegames) is always the SD card: RomFS is read-only.
	chdir("sdmc:/3ds/RingRacers3ds");
	// if the .3dsx carries a RomFS with bios.pk3, assets are read from there
	// (like a regular 3DS game). Otherwise, from sdmc:/3ds as before.
	FILE *rf = fopen("romfs:/bios.pk3", "rb");
	if (rf)
	{
		fclose(rf);
		_3ds_log_msg("[ROMFS] assets from romfs:/ (inside the .3dsx)\n");
		return "romfs:";
	}
	_3ds_log_msg("[ROMFS] no RomFS: assets from sdmc:/3ds\n");
	return "sdmc:/3ds/RingRacers3ds";
#else
	return NULL;
#endif
}

void I_GetJoystickEvents(UINT8 index){ (void)index; }

void I_GetJoystick2Events(UINT8 index){ (void)index; }

void I_GetMouseEvents(void){}

void I_UpdateMouseGrab(void){}

char *I_GetEnv(const char *name)
{
	(void)name;
	return NULL;
}

INT32 I_PutEnv(char *variable)
{
	(void)variable;
	return -1;
}

INT32 I_ClipboardCopy(const char *data, size_t size)
{
	(void)data;
	(void)size;
	return -1;
}

const char *I_ClipboardPaste(void)
{
	return NULL;
}

void I_RegisterSysCommands(void) {}

void I_GetCursorPosition(INT32 *x, INT32 *y)
{
	(void)x;
	(void)y;
}

boolean I_HasOpenURL()
{
	return 0;
}

void I_OpenURL(const char *data)
{
	return;
}

#include "../sdl/dosstr.c"
