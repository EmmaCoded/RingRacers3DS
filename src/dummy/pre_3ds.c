// pre_3ds.c - PER-MAP PRELOAD LIST.
// On real hardware every SD read in the middle of a race is a stall. Here the game
// "learns": while running it records which patches (sprites) and sounds it had to read from the SD,
// and when finished it saves them to sdmc:/3ds/RingRacers3ds/cache/pre/<MAP>.txt. The next time
// that map loads, ALL of them are read at once during loading so the SD stays idle during the race.
// The lists can be generated in Azahar (bench) and shipped with the PATCH.
// Sprites of lazily-loaded characters (data/chars/<name>.pk3) go in a list PER CHARACTER
// (cache/pre/skin_<name>.pk3.txt): opponents change every race, the map does not.
// rr_config.txt: preload=0 turns everything off; preload_min_kb = free RAM that must remain (default 3072).
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <3ds.h>
#include "../doomdef.h"
#include "../doomstat.h"
#include "../g_game.h"
#include "../g_state.h"
#include "../p_tick.h"
#include "../w_wad.h"
#include "../z_zone.h"
#include "../s_sound.h"
#include "../sounds.h"
#include "../i_sound.h"
#include "../rr_config.h"

extern size_t RR3DS_ArenaFreeBytes(void); // z_zone.cpp

#define PRE_MAX 2000
typedef struct
{
	char kind;        // 'P' patch, 'S' sound, 'L' other lump (informational only)
	UINT16 wad, lump;
	INT16 tag;
	char nombre[10];  // sound or lump name
	char isNew;       // 1 = learned in this race (still needs saving)
} pre_t;

static pre_t s_pre[PRE_MAX];
static int s_n = 0;
static int s_reading = 0;             // 1 = reading a list from disk (entries are not new)
static int s_loading = 0, s_on = -1;
static char s_mapName[32] = "";

static int pre_on(void)
{
	if (s_on < 0) s_on = RRCfg_Int("preload", 1) ? 1 : 0;
	return s_on;
}

static const char *baseOf(const char *filePath)
{
	const char *b = filePath, *p;
	for (p = filePath; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
	return b;
}

static int recording(void)
{
	return pre_on() && !s_loading && s_mapName[0] && gamestate == GS_LEVEL && leveltime > 0;
}

static int alreadyHas(char kind, UINT16 wad, UINT16 lump, const char *nombre)
{
	int i;
	for (i = 0; i < s_n; i++)
	{
		if (s_pre[i].kind != kind && !(kind == 'L' && s_pre[i].kind == 'P')) continue;
		if (kind == 'S') { if (!strncmp(s_pre[i].nombre, nombre, sizeof s_pre[i].nombre - 1)) return 1; }
		else if (s_pre[i].wad == wad && s_pre[i].lump == lump) return 1;
	}
	return 0;
}

static void add(char kind, UINT16 wad, UINT16 lump, INT32 tag, const char *nombre)
{
	pre_t *e;
	if (s_n >= PRE_MAX || alreadyHas(kind, wad, lump, nombre ? nombre : "")) return;
	e = &s_pre[s_n++];
	e->kind = kind; e->wad = wad; e->lump = lump; e->tag = (INT16)tag;
	e->isNew = (char)!s_reading;
	memset(e->nombre, 0, sizeof e->nombre);
	if (nombre) strncpy(e->nombre, nombre, sizeof e->nombre - 1);
}

// lazily loaded character: its .pk3 is in the data/chars folder
static int isSkin(UINT16 wad)
{
	const char *f;
	if (wad >= numwadfiles) return 0;
	f = wadfiles[wad]->filename;
	return strstr(f, "/chars/") || strstr(f, "\\chars\\");
}

// --- record (called from w_wad.cpp and i_sound.c when the SD must be read) ---
void RR3DS_PreNote(int kind, UINT16 wad, UINT16 lump, INT32 tag)
{
	if (!recording() || wad >= numwadfiles) return;
	add((char)kind, wad, lump, tag, wadfiles[wad]->lumpinfo[lump].name);
}

void RR3DS_PreNoteSfx(const char *nombre)
{
	if (!recording() || !nombre) return;
	add('S', 0, 0, 0, nombre);
}

static void pathFor(char *out, size_t n)
{
	snprintf(out, n, "sdmc:/3ds/RingRacers3ds/cache/pre/%s.txt", s_mapName);
}

static void skinPath(char *out, size_t n, UINT16 wad)
{
	snprintf(out, n, "sdmc:/3ds/RingRacers3ds/cache/pre/skin_%s.txt", baseOf(wadfiles[wad]->filename));
}

static void writeEntry(FILE *f, const pre_t *e)
{
	if (e->kind == 'S') fprintf(f, "S %s\n", e->nombre);
	else fprintf(f, "%c %s %u %d %s\n", e->kind, baseOf(wadfiles[e->wad]->filename), (unsigned)e->lump, (int)e->tag, e->nombre);
}

// Each SD write call costs several ms on real hardware (a 1300-line list took ~9 s):
// write through a big static buffer so the file goes out in a few large writes.
static char s_wbuf[32 * 1024];

// --- save (end of race / before loading another map) ---
void RR3DS_PreSave(void)
{
	char filePath[128];
	FILE *f;
	int i, j, np = 0, ns = 0, newMap = 0, newSkin = 0;
	if (!pre_on() || !s_mapName[0]) return;
	for (i = 0; i < s_n; i++)
		if (s_pre[i].isNew) { if (s_pre[i].kind != 'S' && isSkin(s_pre[i].wad)) newSkin++; else newMap++; }
	if (!newMap && !newSkin) return;
	mkdir("sdmc:/3ds/RingRacers3ds/cache", 0777);
	mkdir("sdmc:/3ds/RingRacers3ds/cache/pre", 0777);
	if (newMap) // the map list is written out in full (without the per-character entries)
	{
		pathFor(filePath, sizeof filePath);
		f = fopen(filePath, "w");
		if (!f) { CONS_Printf("[PRELIST] could not write %s\n", filePath); return; }
		setvbuf(f, s_wbuf, _IOFBF, sizeof s_wbuf);
		for (i = 0; i < s_n; i++)
		{
			pre_t *e = &s_pre[i];
			if (e->kind != 'S' && isSkin(e->wad)) continue;
			writeEntry(f, e);
			e->isNew = 0;
			if (e->kind == 'S') ns++; else np++;
		}
		fclose(f);
	}
	for (i = 0; i < s_n; i++) // each character's new entries are appended to the end of its list
	{
		const UINT16 w = s_pre[i].wad;
		if (!s_pre[i].isNew) continue;
		skinPath(filePath, sizeof filePath, w);
		f = fopen(filePath, "a");
		if (f) setvbuf(f, s_wbuf, _IOFBF, sizeof s_wbuf);
		for (j = i; j < s_n; j++)
			if (s_pre[j].isNew && s_pre[j].wad == w)
			{
				if (f) writeEntry(f, &s_pre[j]);
				s_pre[j].isNew = 0;
			}
		if (f) fclose(f);
	}
	CONS_Printf("[PRELIST] %s: saved (map: %d patches, %d sounds, %d new; characters: %d new)\n", s_mapName, np, ns,
		newMap, newSkin);
}

// reads a list from disk and adds it to s_pre (without marking it as new)
static void readList(const char *filePath)
{
	char lineStr[96];
	FILE *f = fopen(filePath, "r");
	if (!f) return;
	s_reading = 1;
	while (fgets(lineStr, sizeof lineStr, f) && s_n < PRE_MAX)
	{
		char kind = lineStr[0], wadn[40] = "", fname[16] = "";
		unsigned lump = 0; int tag = 0;
		if (kind == 'S')
		{
			if (sscanf(lineStr + 1, " %15s", fname) == 1) add('S', 0, 0, 0, fname);
		}
		else if ((kind == 'P' || kind == 'L') && sscanf(lineStr + 1, " %39s %u %d %15s", wadn, &lump, &tag, fname) >= 3)
		{
			UINT16 w;
			for (w = 0; w < numwadfiles; w++)
				if (!strcasecmp(baseOf(wadfiles[w]->filename), wadn)) break;
			if (w < numwadfiles && lump < wadfiles[w]->numlumps
				&& (!fname[0] || !strncasecmp(wadfiles[w]->lumpinfo[lump].name, fname, 8)))
				add(kind, w, (UINT16)lump, tag, wadfiles[w]->lumpinfo[lump].name);
		}
	}
	s_reading = 0;
	fclose(f);
}

// brings the entries from `fromIdx` into memory; returns how many did not fit
static int bringIn(int fromIdx, int *np, int *ns)
{
	const size_t minimum = (size_t)RRCfg_Int("preload_min_kb", 3072) * 1024u;
	int i, skippedCount = 0;
	s_loading = 1;
	for (i = fromIdx; i < s_n; i++)
	{
		const pre_t *e = &s_pre[i];
		if (RR3DS_ArenaFreeBytes() < minimum) { skippedCount = s_n - i; break; }
		if (e->kind == 'P')
		{
			W_CachePatchNumPwad(e->wad, e->lump, e->tag);
			(*np)++;
		}
		else if (e->kind == 'S')
		{
			sfxenum_t k;
			for (k = 1; k < NUMSFX; k++)
				if (S_sfx[k].name && !strcasecmp(S_sfx[k].name, e->nombre)) break;
			if (k < NUMSFX && !S_sfx[k].data)
			{
				S_sfx[k].data = I_GetSfx(&S_sfx[k]);
				(*ns)++;
			}
		}
	}
	s_loading = 0;
	return skippedCount;
}

// --- load (at the end of map loading) ---
void RR3DS_Preload(void)
{
	char filePath[128];
	int np = 0, ns = 0, skippedCount;
	UINT16 w;
	u64 t0;
	if (!pre_on()) return;
	RR3DS_PreSave(); // what was learned on the previous map
	s_n = 0;
	strncpy(s_mapName, G_BuildMapName(gamemap), sizeof s_mapName - 1);
	s_mapName[sizeof s_mapName - 1] = '\0';
	t0 = osGetTime();
	pathFor(filePath, sizeof filePath);
	readList(filePath);
	if (!s_n) CONS_Printf("[PRELIST] %s: no list yet (learned during this race)\n", s_mapName);
	for (w = 0; w < numwadfiles; w++) // characters that were already mounted
		if (isSkin(w)) { skinPath(filePath, sizeof filePath, w); readList(filePath); }
	if (!s_n) return;
	skippedCount = bringIn(0, &np, &ns);
	CONS_Printf("[PRELIST] %s: preloaded %d patches and %d sounds in %u ms (no room for %d)\n", s_mapName, np, ns,
		(unsigned)(osGetTime() - t0), skippedCount);
}

// --- a lazily loaded character was released (r_skins.c): forget its entries (wad numbers get reused) ---
void RR3DS_PreForget(UINT16 wad)
{
	int i, j = 0;
	for (i = 0; i < s_n; i++)
		if (s_pre[i].kind == 'S' || s_pre[i].wad != wad) s_pre[j++] = s_pre[i];
	s_n = j;
}

// --- a lazily loaded character was just mounted (r_skins.c): bring in ITS already-known sprites ---
void RR3DS_PreSkin(UINT16 wad)
{
	char filePath[128];
	int np = 0, ns = 0, skippedCount, fromIdx = s_n;
	u64 t0;
	if (!pre_on() || !isSkin(wad)) return;
	t0 = osGetTime();
	skinPath(filePath, sizeof filePath, wad);
	readList(filePath);
	if (s_n == fromIdx) return;
	skippedCount = bringIn(fromIdx, &np, &ns);
	CONS_Printf("[PRELIST] %s: preloaded %d patches in %u ms (no room for %d)\n", baseOf(wadfiles[wad]->filename), np,
		(unsigned)(osGetTime() - t0), skippedCount);
}

// [LINMARK] linear RAM free at load steps (who holds ~10 MB at race start? )
void RR3DS_LinMark(const char *paso)
{
	CONS_Printf("[LINMARK] %s: linear free=%uKB vram free=%uKB\n", paso,
		(unsigned)(linearSpaceFree() >> 10), (unsigned)(vramSpaceFree() >> 10));
}