// fopen_count_3ds.c - counts open files (fopen/fclose) and warns when fopen fails.
// On real 3DS hardware the file system (FS) has a limit on open files; Azahar does not.
// Enabled with -Wl,--wrap=fopen,--wrap=fclose (src/dummy/CMakeLists.txt).
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include "i_3ds_log.h"

extern void I_3DS_Both(const char *fmt, ...);
extern unsigned I_3DS_SdError(void);

FILE *__real_fopen(const char *path, const char *mode);
int __real_fclose(FILE *f);

static int s_openFiles = 0, s_max = 0, s_failures = 0, s_busy = 0;

// Table of currently open files (to log WHO holds the handles when the FS runs out)
#define OPENTAB 40
static struct { FILE *f; char path[72]; } s_tab[OPENTAB];

// Handle releasers (the files below reopen themselves on demand)
extern void W_3DS_ReleaseHandles(void);
extern void I_3DS_EtcReleaseHandles(void);

static void dump_open(const char *why)
{
	int i;
	I_3DS_Both("[FOPEN] %s: %d open\n", why, s_openFiles);
	for (i = 0; i < OPENTAB; i++)
		if (s_tab[i].f) I_3DS_Both("[FOPEN]   open: %s\n", s_tab[i].path);
}

int I_3DS_OpenFiles(void) { return s_openFiles; }

FILE *__wrap_fopen(const char *path, const char *mode)
{
	errno = 0;
	FILE *f = __real_fopen(path, mode);
	if (!f && !s_busy && path && !strncmp(path, "sdmc:", 5) && errno != ENOENT && s_openFiles >= 8)
	{
		// FS out of handles: release the on-demand ones (pk3 LRU, ETC parts) and retry once
		static int s_dumps = 0;
		s_busy = 1;
		if (s_dumps < 3) { s_dumps++; dump_open(path); }
		W_3DS_ReleaseHandles();
		I_3DS_EtcReleaseHandles();
		s_busy = 0;
		errno = 0;
		f = __real_fopen(path, mode);
	}
	if (f)
	{
		int k;
		for (k = 0; k < OPENTAB; k++) if (!s_tab[k].f) { s_tab[k].f = f; strncpy(s_tab[k].path, path ? path : "?", sizeof s_tab[k].path - 1); s_tab[k].path[sizeof s_tab[k].path - 1] = 0; break; }
		s_openFiles++;
		if (s_openFiles > s_max)
		{
			s_max = s_openFiles;
			// warn for every 4 new files opened at once (to the log, which is already open)
			if (!s_busy && (s_max % 4) == 0 && strncmp(path, "romfs:", 6))
			{
				s_busy = 1;
				I_3DS_Both("[FOPEN] open=%d (last '%s')\n", s_openFiles, path);
				s_busy = 0;
			}
		}
	}
	else if (!s_busy && s_failures < 40 && path && strncmp(path, "romfs:", 6)) // romfs: failures are normal
	{
		const int e = errno;
		s_busy = 1; s_failures++;
		I_3DS_Both("[FOPEN] failed '%s' open=%d max=%d errno=%d fatfs=%u\n", path, s_openFiles, s_max, e, I_3DS_SdError());
		FILE *d = __real_fopen(I_3DS_DiagPath(), "a");
		if (d) { fprintf(d, "[FOPEN] failed '%s' open=%d max=%d errno=%d\n", path, s_openFiles, s_max, e); __real_fclose(d); }
		s_busy = 0;
	}
	return f;
}

int __wrap_fclose(FILE *f)
{
	if (f && s_openFiles > 0) s_openFiles--;
	if (f) { int k; for (k = 0; k < OPENTAB; k++) if (s_tab[k].f == f) { s_tab[k].f = NULL; break; } }
	return __real_fclose(f);
}

// SD READ METER ([SD]). Counts every fread (times, bytes, time) and who
// requested it. I_3DS_SdReport() (once per frame) writes a summary every 5 s if there were reads.
// Used to catch anything still reading the SD card mid-race (on real hardware every read stalls).
#include <3ds.h>
size_t __real_fread(void *ptr, size_t size, size_t n, FILE *f);
#define SD_TOP 12
static struct { void *ra; unsigned n, bytes; } s_sdTop[SD_TOP];
static unsigned s_sdN = 0, s_sdBytes = 0;
static u64 s_sdTicks = 0, s_sdLast = 0;

size_t __wrap_fread(void *ptr, size_t size, size_t n, FILE *f)
{
	const u64 t0 = svcGetSystemTick();
	const size_t r = __real_fread(ptr, size, n, f);
	void *ra = __builtin_return_address(0);
	int i;
	s_sdTicks += svcGetSystemTick() - t0;
	s_sdN++; s_sdBytes += (unsigned)(r * size);
	for (i = 0; i < SD_TOP; i++)
		if (s_sdTop[i].ra == ra || !s_sdTop[i].ra)
		{
			s_sdTop[i].ra = ra; s_sdTop[i].n++; s_sdTop[i].bytes += (unsigned)(r * size);
			break;
		}
	return r;
}

void I_3DS_SdReport(void)
{
	const u64 nowMs = osGetTime();
	char buf[400]; int o, i;
	if (!s_sdLast) s_sdLast = nowMs;
	if (nowMs - s_sdLast < 5000) return;
	s_sdLast = nowMs;
	if (!s_sdN) return;
	o = snprintf(buf, sizeof buf, "[SD] 5s: reads=%u KB=%u ms=%u |", s_sdN, s_sdBytes >> 10,
		(unsigned)(s_sdTicks * 1000ULL / SYSCLOCK_ARM11));
	for (i = 0; i < SD_TOP && s_sdTop[i].ra && o < (int)sizeof buf - 40; i++)
		o += snprintf(buf + o, sizeof buf - o, " %p=%u/%uKB", s_sdTop[i].ra, s_sdTop[i].n, s_sdTop[i].bytes >> 10);
	s_busy = 1;
	I_3DS_Both("%s\n", buf);
	s_busy = 0;
	s_sdN = s_sdBytes = 0; s_sdTicks = 0;
	memset(s_sdTop, 0, sizeof s_sdTop);
}
