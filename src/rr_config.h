// rr_config.h - A SINGLE configuration file for tests/bench.
// There used to be ~12 loose files (rr_bench, rr_64mb, rr_gpu3d, rr_diag...).
// Now everything lives in <game folder>/rr_config.txt, one line per option:
//     key = value      # comment
// If a key is NOT in rr_config.txt, the old rr_<key> file is checked
// (compatibility: nothing breaks if old files are still around).
// Header only (static inline): no need to touch CMake or the PC build.
#ifndef RR_CONFIG_H
#define RR_CONFIG_H

#include "dummy/i_3ds_log.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#ifdef __3DS__
#define RRCFG_DIR "sdmc:/3ds/RingRacers3ds"
#else
#define RRCFG_DIR "prepared"
#endif

// Copies the value of 'key' into out. Returns 1 if it exists (in rr_config.txt or the old file).
static inline int RRCfg_Str(const char *key, char *out, size_t n)
{
	static char buf[4096];
	static int loaded = 0;
	if (!loaded)
	{
		loaded = 1;
		FILE *f = fopen(RRCFG_DIR "/rr_config.txt", "rb");
		if (f) { size_t r = fread(buf, 1, sizeof(buf) - 1, f); buf[r] = 0; fclose(f); I_3DS_Both("[CFG] rr_config.txt: %u bytes\n", (unsigned)r); }
		else { buf[0] = 0; } // no rr_config.txt = default values (normal on the console), no warning
	}
	if (n) out[0] = 0;
	const size_t lc = strlen(key);
	for (const char *p = buf; *p; )
	{
		const char *fin = p; while (*fin && *fin != '\n') fin++;
		const char *s = p; while (s < fin && (*s == ' ' || *s == '\t')) s++;
		if ((size_t)(fin - s) > lc && !strncasecmp(s, key, lc))
		{
			const char *q = s + lc; while (q < fin && (*q == ' ' || *q == '\t')) q++;
			if (q < fin && (*q == '=' || *q == ':'))
			{
				q++; while (q < fin && (*q == ' ' || *q == '\t')) q++;
				const char *e = q; while (e < fin && *e != '#' && *e != '\r') e++;
				while (e > q && (e[-1] == ' ' || e[-1] == '\t')) e--;
				size_t len = (size_t)(e - q); if (n && len >= n) len = n - 1;
				if (n) { memcpy(out, q, len); out[len] = 0; }
				return 1;
			}
		}
		p = *fin ? fin + 1 : fin;
	}
#ifdef __3DS__
	return 0; // on the console the old rr_<key> files are no longer searched (every failed fopen on the SD wastes time and file handles)
#endif
	// compatibility: old file rr_<key>
	char filePath[128];
	snprintf(filePath, sizeof filePath, RRCFG_DIR "/rr_%s", key);
	FILE *f = fopen(filePath, "rb");
	if (!f) return 0;
	size_t r = n ? fread(out, 1, n - 1, f) : 0;
	fclose(f);
	if (n) { out[r] = 0; while (r && isspace((unsigned char)out[r - 1])) out[--r] = 0; }
	return 1;
}

// Switch: on if it exists and is not 0/off/no. (Empty old file = on, as before.)
static inline int RRCfg_OnDef(const char *key, int def)
{
	char v[32];
	if (!RRCfg_Str(key, v, sizeof v)) return def;
	return !(v[0] == '0' || !strcasecmp(v, "off") || !strcasecmp(v, "no"));
}
static inline int RRCfg_On(const char *key)
{
	char v[32];
	if (!RRCfg_Str(key, v, sizeof v)) return 0;
	return !(v[0] == '0' || !strcasecmp(v, "off") || !strcasecmp(v, "no"));
}

// Number; 'def' if it does not exist or is not a number.
static inline int RRCfg_Int(const char *key, int def)
{
	char v[32]; int x;
	if (!RRCfg_Str(key, v, sizeof v) || sscanf(v, "%d", &x) != 1) return def;
	return x;
}

// (PC): [TAG] diagnostics of the 3DS port. On 3DS to the log with CONS_Printf; on PC ONLY to the log
// (I_OutputMsg): with CONS_Printf they came out as white text at the top during the race in Legacy GL.
#ifdef __3DS__
#define RR_DIAGPRINT CONS_Printf
#else
#define RR_DIAGPRINT I_OutputMsg
#endif

// MODE LOW-END = the 3DS's optimized logic (sleeping rings, bot AI every
// 2 tics, distant objects that think less, fewer particles...). ALWAYS active on 3DS.
// On PC (Ring Racers E) it is chosen in Video > Advanced (cv_rrlowend) and fixed when each map loads
// in rr_lowend: off online and when watching replays (it changes the simulation -> desyncs).
#if defined(__3DS__)
#define RR_LOWEND_CODE 1
#define RR_LOWEND 1
#elif defined(RR_PCGEN)
#define RR_LOWEND_CODE 1
#ifdef __cplusplus
extern "C" int rr_lowend;
#else
extern int rr_lowend;
#endif
#define RR_LOWEND (rr_lowend)
#endif

#endif
