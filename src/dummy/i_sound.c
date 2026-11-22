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
// 3DS audio module using the console's DSP (libctru ndsp).
//
// - Sounds and music do NOT come from the pk3 files (that required decoding OGG on
// the CPU and loading whole songs into memory). They come already converted in the
// PC by assets3ds/convert_audio.py and live in the RomFS (or in sdmc:/3ds):
// sfx.rrp          all effects (DSP-ADPCM mono) in a single package
// mus/<lump>.rrm   music: stereo DSP-ADPCM in blocks (2 DSP channels)
// DSP-ADPCM = the DSP's native format: 4 bits per sample, decoded by the
// the DSP itself (0% CPU) and takes 1/4 the space of PCM16.
// - Music: read from the file in chunks (streaming): only ~64 KB in memory.
// Refilled from the DSP callback (ndsp thread), so it keeps playing
// even while the game is loading.
// - Effects: loaded the first time they play and kept in linear memory
// up to a cap (kSfxCacheMax); beyond it, the least recently used is freed.
// - Requires the DSP firmware: sdmc:/3ds/dspfirm.cdc (dumped from the console
// itself with "DSP1"). Without it the game runs fine but silent.
//-----------------------------------------------------------------------------

#include "../i_sound.h"

#ifdef __3DS__
#include <3ds.h>
#include "i_3ds_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <stdarg.h>
#include "../z_zone.h"
#include "../s_sound.h"
#include "../w_wad.h"
#include "../i_system.h"
void CONS_Printf(const char *fmt, ...);
#endif

UINT8 sound_started = 0;

#ifdef __3DS__

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------
#define RR_MUSCH        0           // ndsp channels 0 (left) and 1 (right) for music
#define RR_SFXCH0       2           // channels 2..23 for effects
#define RR_NSFXCH       22
#define kSfxCacheMax    (1u * 1024u * 1024u) // 1 MB of ADPCM effects (~3.5 MB as PCM16)

static boolean s_dsp_ok = false;

extern unsigned I_3DS_SdError(void);
extern int I_3DS_OpenFiles(void);

// SD diagnostics: to the log (CONS_Printf) AND to diag_sd.txt (opened/closed every time so the size is preserved)
static void sddiag(const char *fmt, ...)
{
	char b[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	CONS_Printf("%s", b);
	FILE *d = fopen(I_3DS_DiagPath(), "a");
	if (d) { fputs(b, d); fclose(d); }
}

static FILE *open_asset(const char *dir, const char *name, const char *ext)
{
	char lname[64], path[128];
	size_t i;
	for (i = 0; name[i] && i < sizeof lname - 1; i++)
		lname[i] = (char)tolower((unsigned char)name[i]);
	lname[i] = '\0';
	snprintf(path, sizeof path, "romfs:/%s/%s.%s", dir, lname, ext);
	FILE *f = fopen(path, "rb");
	if (!f)
	{
		snprintf(path, sizeof path, "sdmc:/3ds/RingRacers3ds/%s/%s.%s", dir, lname, ext);
		errno = 0;
		f = fopen(path, "rb");
		if (!f)
		{
			// diagnostics (once): what the game sees on the real SD card
			static int s_diag = 0;
			int e = errno;
			sddiag("[SDDIAG] fopen '%s' failed: errno=%d fatfs=%u\n", path, e, I_3DS_SdError());
			if (!s_diag)
			{
				s_diag = 1;
				const char *dirs[2] = { "sdmc:/3ds/RingRacers3ds", "sdmc:/3ds/RingRacers3ds/mus" };
				for (int k = 0; k < 2; k++)
				{
					DIR *d = opendir(dirs[k]);
					if (!d) { sddiag("[SDDIAG] opendir '%s' failed: errno=%d\n", dirs[k], errno); continue; }
					int cnt = 0; struct dirent *de;
					while ((de = readdir(d)) != NULL)
					{
						if (cnt < 40) sddiag("[SDDIAG]   %s: %s\n", k ? "mus" : "root", de->d_name);
						cnt++;
					}
					closedir(d);
					sddiag("[SDDIAG] '%s': %d entries\n", dirs[k], cnt);
				}
			}
		}
	}
	return f;
}

static UINT32 rd32(const UINT8 *p) { return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24); }
static UINT16 rd16(const UINT8 *p) { return (UINT16)(p[0] | (p[1] << 8)); }
#define FRAME_SAMPLES 14
#define FRAME_BYTES   8

// ---------------------------------------------------------------------------
// Effects
// ---------------------------------------------------------------------------
typedef struct rr_sfx_s
{
	UINT32 hz, n, bytes;
	u8 *pcm;                // ADPCM frames in linear memory (read by the DSP)
	u16 coefs[16];
	sfxinfo_t *owner;
	UINT32 lastuse;
	struct rr_sfx_s *next;
} rr_sfx_t;

static rr_sfx_t *s_sfxlist = NULL;
static UINT32 s_sfxbytes = 0, s_sfxclock = 0;
static float s_sfxvol = 1.0f, s_mastervol = 1.0f;

typedef struct
{
	ndspWaveBuf wbs[2];       // 2 alternating: ndsp releases the queue on its next tick,
	ndspAdpcmData adpcms[2];  // rewriting the same node right away corrupted its list
	int cur;
	rr_sfx_t *sfx;
	boolean used;
} rr_chan_t;
static rr_chan_t s_ch[RR_NSFXCH];

static boolean sfx_in_use(const rr_sfx_t *s)
{
	for (int i = 0; i < RR_NSFXCH; i++)
		if (s_ch[i].used && s_ch[i].sfx == s && ndspChnIsPlaying(RR_SFXCH0 + i))
			return true;
	return false;
}

static void sfx_unlink_free(rr_sfx_t *s)
{
	rr_sfx_t **pp = &s_sfxlist;
	while (*pp && *pp != s) pp = &(*pp)->next;
	if (*pp) *pp = s->next;
	for (int i = 0; i < RR_NSFXCH; i++)
		if (s_ch[i].sfx == s) { ndspChnWaveBufClear(RR_SFXCH0 + i); s_ch[i].sfx = NULL; s_ch[i].used = false; }
	if (s->owner && s->owner->data == s) s->owner->data = NULL;
	s_sfxbytes -= s->bytes;
	linearFree(s->pcm);
	free(s);
}

// Frees the least recently used effects until 'need' bytes fit.
static void sfx_make_room(UINT32 need)
{
	while (s_sfxbytes + need > kSfxCacheMax)
	{
		rr_sfx_t *old = NULL;
		for (rr_sfx_t *s = s_sfxlist; s; s = s->next)
			if (!sfx_in_use(s) && (!old || s->lastuse < old->lastuse))
				old = s;
		if (!old) break;
		sfx_unlink_free(old);
	}
}

// sfx.rrp effects package: index in memory (~28 KB), data in the file.
typedef struct { char name[12]; UINT32 off, size; } rr_pakent_t;
static FILE *s_pak = NULL;
static rr_pakent_t *s_pakidx = NULL;
static UINT32 s_pakn = 0;

static void pak_open(void)
{
	s_pak = fopen("romfs:/sfx.rrp", "rb");
	if (!s_pak) s_pak = fopen("sdmc:/3ds/RingRacers3ds/sfx.rrp", "rb");
	if (!s_pak) { CONS_Printf("[AUDIO] no sfx.rrp: no effects\n"); return; }
	UINT8 h[8];
	if (fread(h, 1, 8, s_pak) != 8 || memcmp(h, "RRP1", 4)) { fclose(s_pak); s_pak = NULL; return; }
	s_pakn = rd32(h + 4);
	s_pakidx = (rr_pakent_t *)malloc(sizeof(rr_pakent_t) * s_pakn);
	if (!s_pakidx || fread(s_pakidx, sizeof(rr_pakent_t), s_pakn, s_pak) != s_pakn)
	{
		free(s_pakidx); s_pakidx = NULL; s_pakn = 0; fclose(s_pak); s_pak = NULL; return;
	}
	CONS_Printf("[AUDIO] sfx.rrp: %u effects\n", (unsigned)s_pakn);
}

// Looks up an effect by name (lowercase) in the package (sorted index).
static FILE *pak_find(const char *name)
{
	if (!s_pak) return NULL;
	char k[13] = {0};
	for (int i = 0; i < 12 && name[i]; i++) k[i] = (char)tolower((unsigned char)name[i]);
	int lo = 0, hi = (int)s_pakn - 1;
	while (lo <= hi)
	{
		const int mid = (lo + hi) / 2;
		const int c = strncmp(k, s_pakidx[mid].name, 12);
		if (!c) { fseek(s_pak, (long)s_pakidx[mid].off, SEEK_SET); return s_pak; }
		if (c < 0) hi = mid - 1; else lo = mid + 1;
	}
	return NULL;
}

// MOD effects (skins): lump "ds<name>" already converted to RRS2 by tools\convert_mod.py
// (the 3DS does not decode ogg/wav). Same format as in sfx.rrp.
static void *I_GetSfxLump(sfxinfo_t *sfx)
{
	const lumpnum_t ln = S_GetSfxLumpNum(sfx);
	if (ln == LUMPERROR) return NULL;
	const size_t len = W_LumpLength(ln);
	if (len < 12 + 32 + FRAME_BYTES || len > 2u * kSfxCacheMax) return NULL;
	UINT8 *buf = (UINT8 *)malloc(len);
	if (!buf) return NULL;
	W_ReadLump(ln, buf);
	if (memcmp(buf, "RRS2", 4))
	{
		CONS_Printf("[AUDIO] mod: '%s' is not RRS2 (use convert_mod.py)\n", sfx->name);
		free(buf); return NULL;
	}
	const UINT32 hz = rd32(buf + 4), n = rd32(buf + 8);
	const UINT32 bytes = ((n + FRAME_SAMPLES - 1) / FRAME_SAMPLES) * FRAME_BYTES;
	if (!n || bytes > kSfxCacheMax || 12 + 32 + (size_t)bytes > len) { free(buf); return NULL; }
	sfx_make_room(bytes);
	u8 *pcm = (u8 *)linearAlloc(bytes);
	if (!pcm) { sfx_make_room(kSfxCacheMax); pcm = (u8 *)linearAlloc(bytes); }
	if (!pcm) { free(buf); return NULL; }
	memcpy(pcm, buf + 12 + 32, bytes);
	DSP_FlushDataCache(pcm, bytes);
	rr_sfx_t *s = (rr_sfx_t *)calloc(1, sizeof *s);
	if (!s) { linearFree(pcm); free(buf); return NULL; }
	s->hz = hz; s->n = n; s->bytes = bytes; s->pcm = pcm; s->owner = sfx;
	for (int i = 0; i < 16; i++) s->coefs[i] = rd16(buf + 12 + i * 2);
	s->lastuse = ++s_sfxclock;
	s->next = s_sfxlist; s_sfxlist = s;
	s_sfxbytes += bytes;
	sfx->length = bytes;
	free(buf);
	return s;
}

void *I_GetSfx(sfxinfo_t *sfx)
{
	if (!s_dsp_ok || !sfx || !sfx->name)
		return NULL;
	char nm[16];
	snprintf(nm, sizeof nm, "ds%s", sfx->name);
	FILE *f = pak_find(nm);
	if (!f) f = pak_find(sfx->name);
	if (!f) return I_GetSfxLump(sfx);

	UINT8 h[12 + 32];
	if (fread(h, 1, sizeof h, f) != sizeof h || memcmp(h, "RRS2", 4)) { return NULL; }
	const UINT32 hz = rd32(h + 4), n = rd32(h + 8);
	const UINT32 bytes = ((n + FRAME_SAMPLES - 1) / FRAME_SAMPLES) * FRAME_BYTES;
	if (!n || bytes > kSfxCacheMax) { return NULL; }

	sfx_make_room(bytes);
	u8 *pcm = (u8 *)linearAlloc(bytes);
	if (!pcm) { sfx_make_room(kSfxCacheMax); pcm = (u8 *)linearAlloc(bytes); }
	if (!pcm) { return NULL; }
	if (fread(pcm, 1, bytes, f) != bytes) { linearFree(pcm); return NULL; }
	DSP_FlushDataCache(pcm, bytes);

	rr_sfx_t *s = (rr_sfx_t *)calloc(1, sizeof *s);
	if (!s) { linearFree(pcm); return NULL; }
	s->hz = hz; s->n = n; s->bytes = bytes; s->pcm = pcm; s->owner = sfx;
	for (int i = 0; i < 16; i++) s->coefs[i] = rd16(h + 12 + i * 2);
	s->lastuse = ++s_sfxclock;
	s->next = s_sfxlist; s_sfxlist = s;
	s_sfxbytes += bytes;
	sfx->length = bytes;
	return s;
}

void I_FreeSfx(sfxinfo_t *sfx)
{
	if (!sfx || !sfx->data) return;
	for (rr_sfx_t *s = s_sfxlist; s; s = s->next)
		if (s == sfx->data) { sfx_unlink_free(s); break; }
	sfx->data = NULL;
}

static void set_mix(int ch, UINT8 vol, UINT8 sep, float extra)
{
	float mix[12] = {0};
	const float v = (vol / 255.0f) * extra;
	float l = (255 - sep) / 127.0f, r = sep / 127.0f;
	if (l > 1.0f) l = 1.0f;
	if (r > 1.0f) r = 1.0f;
	mix[0] = v * l;
	mix[1] = v * r;
	ndspChnSetMix(ch, mix);
}

INT32 I_StartSound(sfxenum_t id, UINT8 vol, UINT8 sep, UINT8 pitch, UINT8 priority, INT32 channel)
{
	(void)pitch; (void)priority; // as in the PC version (SDL_mixer): no pitch change
	if (!s_dsp_ok || id <= 0 || id >= NUMSFX)
		return -1;
	rr_sfx_t *s = (rr_sfx_t *)S_sfx[id].data;
	if (!s) return -1;
	const int c = (channel >= 0 ? channel : 0) % RR_NSFXCH;
	const int ch = RR_SFXCH0 + c;

	ndspChnWaveBufClear(ch);
	ndspChnReset(ch);
	ndspChnSetInterp(ch, NDSP_INTERP_LINEAR);
	ndspChnSetRate(ch, (float)s->hz);
	ndspChnSetFormat(ch, NDSP_FORMAT_MONO_ADPCM);
	ndspChnSetAdpcmCoefs(ch, s->coefs);
	set_mix(ch, vol, sep, s_sfxvol * s_mastervol);

	rr_chan_t *rc = &s_ch[c];
	rc->cur ^= 1;
	ndspWaveBuf *wb = &rc->wbs[rc->cur];
	ndspAdpcmData *ad = &rc->adpcms[rc->cur];
	memset(wb, 0, sizeof *wb);
	ad->index = s->pcm[0];
	ad->history0 = ad->history1 = 0;
	wb->data_adpcm = s->pcm;
	wb->adpcm_data = ad;
	wb->nsamples = s->n;
	rc->sfx = s;
	rc->used = true;
	s->lastuse = ++s_sfxclock;
	ndspChnWaveBufAdd(ch, wb);
	return c;
}

void I_StopSound(INT32 handle)
{
	if (!s_dsp_ok || handle < 0 || handle >= RR_NSFXCH) return;
	ndspChnWaveBufClear(RR_SFXCH0 + handle);
	s_ch[handle].used = false;
}

boolean I_SoundIsPlaying(INT32 handle)
{
	if (!s_dsp_ok || handle < 0 || handle >= RR_NSFXCH || !s_ch[handle].used) return false;
	return ndspChnIsPlaying(RR_SFXCH0 + handle) && s_ch[handle].wbs[s_ch[handle].cur].status != NDSP_WBUF_DONE;
}

void I_UpdateSoundParams(INT32 handle, UINT8 vol, UINT8 sep, UINT8 pitch)
{
	(void)pitch;
	if (!s_dsp_ok || handle < 0 || handle >= RR_NSFXCH) return;
	set_mix(RR_SFXCH0 + handle, vol, sep, s_sfxvol * s_mastervol);
}

void I_SetSfxVolume(int volume)
{
	s_sfxvol = volume / 100.0f;
}

void I_SetVoiceVolume(int volume)
{
	(void)volume;
}

// ---------------------------------------------------------------------------
// Music (streaming)
// ---------------------------------------------------------------------------
#define kMusBufs    4
#define kMusBlock   1024            // frames per block (set by the converter)

static LightLock s_muslock;
static FILE *s_musf = NULL;
static UINT32 s_mushz = 32000, s_musframes = 0, s_musloop = 0, s_musnsamp = 0, s_musblk = kMusBlock;
static UINT32 s_musreadpos = 0;     // next frame to read from the file
static UINT32 s_musplayed = 0;      // frames already delivered to the DSP (position)
static boolean s_muslooping = false, s_musplaying = false, s_muspaused = false, s_musended = false;
static boolean s_musjump = false;   // the next chunk starts at the loop / after a jump
static float s_musspeed = 1.0f;
static int s_musnch = 2;            // 1 = mono ("RRMM", low/minimal quality), 2 = stereo ("RRM2")
static u16 s_muscoef[2][16];
static ndspAdpcmData s_musloophist[2];
static ndspWaveBuf s_muswb[kMusBufs][2];
static ndspAdpcmData s_musad[kMusBufs][2];
static UINT32 s_muswbpos[kMusBufs]; // start frame of each chunk
static u8 *s_musbuf = NULL;         // kMusBufs x 2 channels x kMusBlock frames
static char s_musname[64];
#define MUS_HDR 100

// volumes 0..100
static int s_vol_music = 100, s_vol_song = 100, s_vol_internal = 100;
// fade
static float s_fade_from = 100, s_fade_to = 100, s_fade_cur = 100;
static u64 s_fade_t0 = 0, s_fade_len = 0;
static void (*s_fade_cb)(void) = NULL;
static boolean s_fade_stop = false;

static void mus_apply_volume(void)
{
	float mix[12] = {0};
	const float v = (s_vol_music / 100.0f) * (s_vol_song / 100.0f) * (s_vol_internal / 100.0f)
		* (s_fade_cur / 100.0f) * s_mastervol;
	if (s_musnch == 1)
	{
		mix[0] = mix[1] = v; ndspChnSetMix(RR_MUSCH, mix); // mono: one channel to both sides
		mix[0] = mix[1] = 0; ndspChnSetMix(RR_MUSCH + 1, mix);
		return;
	}
	mix[0] = v; ndspChnSetMix(RR_MUSCH, mix);         // left
	mix[0] = 0; mix[1] = v; ndspChnSetMix(RR_MUSCH + 1, mix); // right
}

// Reads 'cnt' frames of channel 'c' starting at frame 'fr' (may cross blocks).
static boolean mus_read(int c, UINT32 fr, UINT32 cnt, u8 *dst)
{
	while (cnt)
	{
		const UINT32 b = fr / s_musblk, o = fr % s_musblk;
		UINT32 take = s_musblk - o;
		if (take > cnt) take = cnt;
		const long off = MUS_HDR + ((long)b * s_musnch + c) * (long)s_musblk * FRAME_BYTES + (long)o * FRAME_BYTES;
		if (fseek(s_musf, off, SEEK_SET) || fread(dst, FRAME_BYTES, take, s_musf) != take)
			return false;
		dst += take * FRAME_BYTES; fr += take; cnt -= take;
	}
	return true;
}

// Fills a chunk (both channels) from the file. Call with s_muslock held.
static boolean mus_fill(int i)
{
	if (!s_musf || s_musended) return false;
	if (s_musreadpos >= s_musframes)
	{
		if (!s_muslooping) { s_musended = true; return false; }
		s_musreadpos = s_musloop / FRAME_SAMPLES;
		if (s_musreadpos >= s_musframes) s_musreadpos = 0;
		s_musjump = true;
	}
	UINT32 cnt = s_musframes - s_musreadpos;
	if (cnt > kMusBlock) cnt = kMusBlock;
	UINT32 nsamp = cnt * FRAME_SAMPLES;
	const UINT32 endsamp = (s_musreadpos + cnt) * FRAME_SAMPLES;
	if (endsamp > s_musnsamp) nsamp -= endsamp - s_musnsamp;
	const boolean atloop = s_musjump && s_musreadpos * FRAME_SAMPLES == s_musloop;
	for (int c = 0; c < s_musnch; c++)
	{
		u8 *dst = s_musbuf + ((size_t)i * 2 + c) * kMusBlock * FRAME_BYTES;
		if (!mus_read(c, s_musreadpos, cnt, dst)) { s_musended = true; return false; }
		DSP_FlushDataCache(dst, cnt * FRAME_BYTES);
		ndspWaveBuf *wb = &s_muswb[i][c];
		memset(wb, 0, sizeof *wb);
		wb->data_adpcm = dst;
		wb->nsamples = nsamp;
		if (s_musjump || s_musreadpos == 0)
		{
			// starts a new segment: the DSP needs the predictor history
			s_musad[i][c] = atloop ? s_musloophist[c] : (ndspAdpcmData){ dst[0], 0, 0 };
			s_musad[i][c].index = dst[0];
			wb->adpcm_data = &s_musad[i][c];
		}
	}
	s_musjump = false;
	s_muswbpos[i] = s_musreadpos;
	s_musreadpos += cnt;
	ndspChnWaveBufAdd(RR_MUSCH, &s_muswb[i][0]);
	if (s_musnch == 2) ndspChnWaveBufAdd(RR_MUSCH + 1, &s_muswb[i][1]);
	return true;
}

// SECOND CORE. A thread on core 1 (APT_SetAppCpuTimeLimit 30%) does the
// music SD read (fseek/fread per chunk). Before, it was done by the DSP callback and
// I_UpdateSound ON THE MAIN CORE every frame. If the thread cannot be created,
// everything works as before (core 0).
static Thread s_c1 = NULL;
int g_core1_ok = 0; // the core 1 CPU limit was requested successfully (other threads use it)
static LightEvent s_c1ev;
static volatile boolean s_c1quit = false;
static volatile UINT32 s_c1fills = 0;
static volatile UINT64 s_c1ticks = 0;
static UINT64 s_c0ticks = 0; static UINT32 s_c0fills = 0;

// HOME: the DSP must not have active channels or a thread feeding it while libctru suspends it
// (the hand-over hangs / the HOME menu dies; see ankush.one 3DS homebrew notes and derrekr/srb2_3ds).
// Hold = stop feeding, clear and pause every channel. Release = restart the music where it was
// and ask for the core 1 CPU share again (the system resets it to 0 while the app is suspended).
static volatile int s_hold = 0;
static void mus_start_at(UINT32 frame);
void I_3DS_AudioHold(int on)
{
	if (!s_dsp_ok || on == s_hold) return;
	LightLock_Lock(&s_muslock);
	if (on)
	{
		s_hold = 1;
		for (int c = 0; c < 24; c++) { ndspChnWaveBufClear(c); ndspChnSetPaused(c, true); }
	}
	else
	{
		s_hold = 0;
		if (s_c1) APT_SetAppCpuTimeLimit(30);
		if (s_musplaying && !s_muspaused && s_musf) mus_start_at(s_musplayed);
	}
	LightLock_Unlock(&s_muslock);
	CONS_Printf("[HOME] audio %s\n", on ? "stopped" : "resumed");
}

static void mus_service(boolean blocking)
{
	if (blocking) LightLock_Lock(&s_muslock);
	else if (LightLock_TryLock(&s_muslock)) return;
	{
		const UINT64 t0 = svcGetSystemTick();
		UINT32 n = 0;
		if (s_musplaying && !s_muspaused && !s_hold)
		{
			for (int i = 0; i < kMusBufs; i++)
				if (s_muswb[i][0].status == NDSP_WBUF_DONE && (s_musnch == 1 || s_muswb[i][1].status == NDSP_WBUF_DONE)) // in mono, channel 1 is unused
				{
					s_musplayed = s_muswbpos[i] + s_muswb[i][0].nsamples / FRAME_SAMPLES;
					s_muswb[i][0].status = s_muswb[i][1].status = NDSP_WBUF_FREE;
					mus_fill(i); n++;
				}
		}
		LightLock_Unlock(&s_muslock);
		if (n)
		{
			const UINT64 dt = svcGetSystemTick() - t0;
			if (s_c1 && blocking) { s_c1fills += n; s_c1ticks += dt; } else { s_c0fills += n; s_c0ticks += dt; }
		}
	}
}

static void core1_main(void *arg)
{
	(void)arg;
	while (!s_c1quit)
	{
		LightEvent_WaitTimeout(&s_c1ev, 20 * 1000000LL); // DSP callback or every 20 ms
		if (s_c1quit) break;
		if (s_dsp_ok) mus_service(true);
	}
}

// DSP callback (ndsp thread): refill the finished chunks.
static void mus_callback(void *arg)
{
	(void)arg;
	if (s_c1) { LightEvent_Signal(&s_c1ev); return; } // core 1 handles it
	mus_service(false);
}

static void core1_start(void)
{
	// Old 3DS: the app's core 1 sits at 0% until requested. 30% = safe (the system
	// uses the rest); maximum allowed 80%.
	Result r = APT_SetAppCpuTimeLimit(30);
	LightEvent_Init(&s_c1ev, RESET_ONESHOT);
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	s_c1quit = false;
	s_c1 = threadCreate(core1_main, NULL, 16 * 1024, prio > 0x18 ? prio - 1 : prio, 1, false);
	g_core1_ok = (s_c1 != NULL && R_SUCCEEDED(r));
	CONS_Printf("[CORE1] %s (CPU limit 30%%: %s)\n", s_c1 ? "music thread on core 1" : "could NOT create the thread (staying on core 0)",
		R_SUCCEEDED(r) ? "ok" : "failed");
}

static void core1_report(void)
{
	static int k = 0;
	if (++k % 350) return; // ~every 10 s
	const double tpm = (double)SYSCLOCK_ARM11 / 1000.0;
	CONS_Printf("[CORE1] music 10s: core1 %u refills %.2f ms | core0 %u refills %.2f ms\n",
		(unsigned)s_c1fills, s_c1ticks / tpm, (unsigned)s_c0fills, s_c0ticks / tpm);
	s_c1fills = 0; s_c1ticks = 0; s_c0fills = 0; s_c0ticks = 0;
}

static void mus_close(void)
{
	ndspChnWaveBufClear(RR_MUSCH);
	ndspChnWaveBufClear(RR_MUSCH + 1);
	if (s_musf) { fclose(s_musf); s_musf = NULL; }
	s_musplaying = s_muspaused = false;
	s_musended = false;
	s_musname[0] = '\0';
}

void I_InitMusic(void)
{
}

void I_ShutdownMusic(void)
{
	if (!s_dsp_ok) return;
	LightLock_Lock(&s_muslock);
	mus_close();
	LightLock_Unlock(&s_muslock);
}

// Called by music_manager.cpp instead of I_LoadSong (does not load the OGG lump).
boolean I_3DS_LoadSongFile(const char *song)
{
	if (!s_dsp_ok || !song || !song[0]) return false; // "" = no music (not an error)
	char nm[64];
	snprintf(nm, sizeof nm, "o_%s", song);
	FILE *f = open_asset("mus", nm, "rrm");
	if (!f) { CONS_Printf("[AUDIO] no mus/%s.rrm (errno=%d fatfs=%u open=%d)\n", nm, errno, I_3DS_SdError(), I_3DS_OpenFiles()); return false; }
	UINT8 h[MUS_HDR];
	if (fread(h, 1, MUS_HDR, f) != MUS_HDR || (memcmp(h, "RRM2", 4) && memcmp(h, "RRMM", 4)) || rd32(h + 16) != kMusBlock) { fclose(f); return false; }
	LightLock_Lock(&s_muslock);
	mus_close();
	s_musf = f;
	s_musnch = memcmp(h, "RRMM", 4) ? 2 : 1;
	s_mushz = rd32(h + 4);
	s_musnsamp = rd32(h + 8);
	s_musloop = rd32(h + 12);
	s_musblk = rd32(h + 16);
	s_musframes = (s_musnsamp + FRAME_SAMPLES - 1) / FRAME_SAMPLES;
	for (int c = 0; c < 2; c++)
	{
		const UINT8 *p = h + 20 + c * 40;
		for (int k = 0; k < 16; k++) s_muscoef[c][k] = rd16(p + k * 2);
		s_musloophist[c].index = rd16(p + 32);
		s_musloophist[c].history0 = (s16)rd16(p + 34);
		s_musloophist[c].history1 = (s16)rd16(p + 36);
	}
	s_musreadpos = s_musplayed = 0;
	snprintf(s_musname, sizeof s_musname, "%s", song);
	LightLock_Unlock(&s_muslock);
	return true;
}

boolean I_LoadSong(char *data, size_t len)
{
	(void)data; (void)len;
	return false; // on 3DS music goes through I_3DS_LoadSongFile
}

void I_UnloadSong(void)
{
	if (!s_dsp_ok) return;
	LightLock_Lock(&s_muslock);
	mus_close();
	LightLock_Unlock(&s_muslock);
}

// 'frame' in ADPCM frames (14 samples)
static void mus_start_at(UINT32 frame)
{
	for (int c = 0; c < 2; c++)
	{
		const int ch = RR_MUSCH + c;
		ndspChnWaveBufClear(ch);
		ndspChnReset(ch);
		ndspChnSetInterp(ch, NDSP_INTERP_LINEAR);
		ndspChnSetFormat(ch, NDSP_FORMAT_MONO_ADPCM);
		ndspChnSetAdpcmCoefs(ch, s_muscoef[c]);
		ndspChnSetRate(ch, s_mushz * s_musspeed);
		ndspChnSetPaused(ch, false);
	}
	mus_apply_volume();
	if (frame >= s_musframes) frame = s_muslooping ? (s_musloop / FRAME_SAMPLES) : s_musframes;
	if (frame >= s_musframes && s_muslooping) frame = 0;
	s_musreadpos = s_musplayed = frame;
	s_musended = false;
	s_musjump = true;
	for (int i = 0; i < kMusBufs; i++)
	{
		s_muswb[i][0].status = s_muswb[i][1].status = NDSP_WBUF_FREE;
		mus_fill(i);
	}
}

boolean I_PlaySong(boolean looping)
{
	if (!s_dsp_ok || !s_musf) return false;
	LightLock_Lock(&s_muslock);
	s_muslooping = looping;
	s_musplaying = true;
	s_muspaused = false;
	mus_start_at(0);
	LightLock_Unlock(&s_muslock);
	return true;
}

void I_StopSong(void)
{
	if (!s_dsp_ok) return;
	LightLock_Lock(&s_muslock);
	ndspChnWaveBufClear(RR_MUSCH);
	ndspChnWaveBufClear(RR_MUSCH + 1);
	s_musplaying = false;
	LightLock_Unlock(&s_muslock);
}

void I_PauseSong(void)
{
	if (!s_dsp_ok) return;
	s_muspaused = true;
	ndspChnSetPaused(RR_MUSCH, true);
	ndspChnSetPaused(RR_MUSCH + 1, true);
}

void I_ResumeSong(void)
{
	if (!s_dsp_ok) return;
	s_muspaused = false;
	ndspChnSetPaused(RR_MUSCH, false);
	ndspChnSetPaused(RR_MUSCH + 1, false);
}

const char *I_SongType(void)
{
	return s_musf ? "OGG" : NULL;
}

boolean I_SongPlaying(void)
{
	return s_musf && s_musplaying && !(s_musended && !ndspChnIsPlaying(RR_MUSCH));
}

boolean I_SongPaused(void)
{
	return s_muspaused;
}

boolean I_SetSongSpeed(float speed)
{
	if (!s_dsp_ok) return false;
	if (speed < 0.1f) speed = 0.1f;
	s_musspeed = speed;
	ndspChnSetRate(RR_MUSCH, s_mushz * s_musspeed);
	ndspChnSetRate(RR_MUSCH + 1, s_mushz * s_musspeed);
	return true;
}

UINT32 I_GetSongLength(void)
{
	return s_musf ? (UINT32)((UINT64)s_musnsamp * 1000 / s_mushz) : 0;
}

boolean I_SetSongLoopPoint(UINT32 looppoint)
{
	if (!s_musf) return false;
	// it can only jump with correct history to the saved loop; any other point: history 0
	UINT32 sm = (UINT32)((UINT64)looppoint * s_mushz / 1000);
	sm -= sm % FRAME_SAMPLES;
	if (sm != s_musloop) { s_musloop = sm; s_musloophist[0].history0 = s_musloophist[0].history1 = 0;
		s_musloophist[1].history0 = s_musloophist[1].history1 = 0; }
	return true;
}

UINT32 I_GetSongLoopPoint(void)
{
	return s_musf ? (UINT32)((UINT64)s_musloop * 1000 / s_mushz) : 0;
}

boolean I_SetSongPosition(UINT32 position)
{
	if (!s_dsp_ok || !s_musf) return false;
	LightLock_Lock(&s_muslock);
	UINT32 sm = (UINT32)((UINT64)position * s_mushz / 1000);
	if (s_muslooping && sm >= s_musnsamp && s_musnsamp > s_musloop)
		sm = s_musloop + (sm - s_musloop) % (s_musnsamp - s_musloop);
	const UINT32 fr = sm / FRAME_SAMPLES;
	if (s_musplaying) mus_start_at(fr);
	else { s_musreadpos = s_musplayed = fr; s_musjump = true; }
	LightLock_Unlock(&s_muslock);
	return true;
}

UINT32 I_GetSongPosition(void)
{
	if (!s_musf) return 0;
	UINT32 sm = s_musplayed * FRAME_SAMPLES;
	// add what the currently playing chunk has consumed
	for (int i = 0; i < kMusBufs; i++)
		if (s_muswb[i][0].status == NDSP_WBUF_PLAYING)
		{
			sm = s_muswbpos[i] * FRAME_SAMPLES + (UINT32)ndspChnGetSamplePos(RR_MUSCH);
			break;
		}
	return (UINT32)((UINT64)sm * 1000 / s_mushz);
}

void I_UpdateSongLagThreshold(void){}

void I_UpdateSongLagConditions(void){}

void I_SetMusicVolume(int volume)
{
	s_vol_music = volume;
	if (s_dsp_ok) mus_apply_volume();
}

void I_SetCurrentSongVolume(int volume)
{
	s_vol_song = volume;
	if (s_dsp_ok) mus_apply_volume();
}

void I_SetMasterVolume(int volume)
{
	s_mastervol = volume / 100.0f;
	if (s_dsp_ok) mus_apply_volume();
}

boolean I_SetSongTrack(INT32 track)
{
	(void)track;
	return false;
}

// --- fades (advanced in I_UpdateSound, once per frame) ---
void I_SetInternalMusicVolume(UINT8 volume)
{
	s_vol_internal = volume;
	s_fade_len = 0;
	s_fade_cur = 100;
	if (s_dsp_ok) mus_apply_volume();
}

void I_StopFadingSong(void)
{
	s_fade_len = 0;
	s_fade_cb = NULL;
	s_fade_stop = false;
}

boolean I_FadeSongFromVolume(UINT8 target_volume, UINT8 source_volume, UINT32 ms, void (*callback)(void))
{
	if (!s_dsp_ok) return false;
	s_vol_internal = 100;
	s_fade_from = source_volume;
	s_fade_to = target_volume;
	s_fade_cb = callback;
	s_fade_stop = false;
	if (!ms)
	{
		s_fade_cur = target_volume;
		s_fade_len = 0;
		if (callback) callback();
		s_fade_cb = NULL;
	}
	else
	{
		s_fade_cur = source_volume;
		s_fade_t0 = svcGetSystemTick();
		s_fade_len = (u64)ms * (SYSCLOCK_ARM11 / 1000);
	}
	mus_apply_volume();
	return true;
}

boolean I_FadeSong(UINT8 target_volume, UINT32 ms, void (*callback)(void))
{
	return I_FadeSongFromVolume(target_volume, (UINT8)s_fade_cur, ms, callback);
}

boolean I_FadeOutStopSong(UINT32 ms)
{
	boolean r = I_FadeSongFromVolume(0, (UINT8)s_fade_cur, ms, NULL);
	s_fade_stop = true;
	if (!ms) I_StopSong();
	return r;
}

boolean I_FadeInPlaySong(UINT32 ms, boolean looping)
{
	if (!I_PlaySong(looping)) return false;
	return I_FadeSongFromVolume(100, 0, ms, NULL);
}

static void fade_tick(void)
{
	if (!s_fade_len) return;
	const u64 dt = svcGetSystemTick() - s_fade_t0;
	if (dt >= s_fade_len)
	{
		s_fade_cur = s_fade_to;
		s_fade_len = 0;
		mus_apply_volume();
		if (s_fade_stop) { s_fade_stop = false; I_StopSong(); }
		if (s_fade_cb) { void (*cb)(void) = s_fade_cb; s_fade_cb = NULL; cb(); }
		return;
	}
	s_fade_cur = s_fade_from + (s_fade_to - s_fade_from) * (float)((double)dt / (double)s_fade_len);
	mus_apply_volume();
}

// ---------------------------------------------------------------------------
// Startup
// ---------------------------------------------------------------------------
void I_StartupSound(void)
{
	if (s_dsp_ok) return;
	Result r = ndspInit();
	if (R_FAILED(r))
	{
		CONS_Printf("[AUDIO] ndspInit failed (0x%08lx): missing sdmc:/3ds/dspfirm.cdc? No sound.\n", (unsigned long)r);
		sound_started = 0;
		return;
	}
	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	LightLock_Init(&s_muslock);
	s_musbuf = (u8 *)linearAlloc(kMusBufs * 2 * kMusBlock * FRAME_BYTES); // 64 KB
	if (!s_musbuf)
	{
		CONS_Printf("[AUDIO] out of linear memory for music\n");
		ndspExit();
		return;
	}
	memset(s_ch, 0, sizeof s_ch);
	pak_open();
	ndspSetCallback(mus_callback, NULL);
	s_dsp_ok = true;
	core1_start();
	sound_started = 1;
	CONS_Printf("[AUDIO] DSP ready (effects: cap %u KB, music: streaming)\n", kSfxCacheMax >> 10);
}

void I_ShutdownSound(void)
{
	if (!s_dsp_ok) return;
	I_ShutdownMusic();
	while (s_sfxlist) sfx_unlink_free(s_sfxlist);
	ndspSetCallback(NULL, NULL);
	if (s_c1) { s_c1quit = true; LightEvent_Signal(&s_c1ev); threadJoin(s_c1, U64_MAX); threadFree(s_c1); s_c1 = NULL; }
	ndspExit();
	if (s_musbuf) { linearFree(s_musbuf); s_musbuf = NULL; }
	s_dsp_ok = false;
	sound_started = 0;
}

void I_UpdateAudioRecorder(void){}

void I_UpdateSound(void)
{
	if (!s_dsp_ok) return;
	fade_tick();
	// in case the DSP callback could not take the lock: refill from here
	if (s_c1) LightEvent_Signal(&s_c1ev); // core 1 reads the SD card
	else mus_callback(NULL);
	core1_report();
}

#else // !__3DS__ -------------------------------------------------------------

void *I_GetSfx(sfxinfo_t *sfx) { (void)sfx; return NULL; }
void I_FreeSfx(sfxinfo_t *sfx) { (void)sfx; }
void I_StartupSound(void){}
void I_ShutdownSound(void){}
void I_UpdateAudioRecorder(void){}
void I_UpdateSound(void){}
INT32 I_StartSound(sfxenum_t id, UINT8 vol, UINT8 sep, UINT8 pitch, UINT8 priority, INT32 channel)
{ (void)id; (void)vol; (void)sep; (void)pitch; (void)priority; (void)channel; return -1; }
void I_StopSound(INT32 handle) { (void)handle; }
boolean I_SoundIsPlaying(INT32 handle) { (void)handle; return false; }
void I_UpdateSoundParams(INT32 handle, UINT8 vol, UINT8 sep, UINT8 pitch) { (void)handle; (void)vol; (void)sep; (void)pitch; }
void I_SetSfxVolume(int volume) { (void)volume; }
void I_SetVoiceVolume(int volume) { (void)volume; }
void I_InitMusic(void){}
void I_ShutdownMusic(void){}
const char *I_SongType(void) { return NULL; }
boolean I_SongPlaying(void) { return false; }
boolean I_SongPaused(void) { return false; }
boolean I_SetSongSpeed(float speed) { (void)speed; return false; }
UINT32 I_GetSongLength(void) { return 0; }
boolean I_SetSongLoopPoint(UINT32 looppoint) { (void)looppoint; return false; }
UINT32 I_GetSongLoopPoint(void) { return 0; }
boolean I_SetSongPosition(UINT32 position) { (void)position; return false; }
UINT32 I_GetSongPosition(void) { return 0; }
void I_UpdateSongLagThreshold(void){}
void I_UpdateSongLagConditions(void){}
boolean I_LoadSong(char *data, size_t len) { (void)data; (void)len; return false; }
void I_UnloadSong(void){}
boolean I_PlaySong(boolean looping) { (void)looping; return false; }
void I_StopSong(void){}
void I_PauseSong(void){}
void I_ResumeSong(void){}
void I_SetMusicVolume(int volume) { (void)volume; }
void I_SetCurrentSongVolume(int volume) { (void)volume; }
void I_SetMasterVolume(int volume) { (void)volume; }
boolean I_SetSongTrack(INT32 track) { (void)track; return false; }
void I_SetInternalMusicVolume(UINT8 volume) { (void)volume; }
void I_StopFadingSong(void){}
boolean I_FadeSongFromVolume(UINT8 t, UINT8 s, UINT32 ms, void (*cb)(void)) { (void)t; (void)s; (void)ms; (void)cb; return false; }
boolean I_FadeSong(UINT8 t, UINT32 ms, void (*cb)(void)) { (void)t; (void)ms; (void)cb; return false; }
boolean I_FadeOutStopSong(UINT32 ms) { (void)ms; return false; }
boolean I_FadeInPlaySong(UINT32 ms, boolean looping) { (void)ms; (void)looping; return false; }

#endif // __3DS__

// --- voice input (not available on 3DS) ---
boolean I_SoundInputIsEnabled(void)
{
	return false;
}

boolean I_SoundInputSetEnabled(boolean enabled)
{
	(void)enabled;
	return false;
}

UINT32 I_SoundInputDequeueSamples(void *data, UINT32 len)
{
	(void)data; (void)len;
	return 0;
}

UINT32 I_SoundInputRemainingSamples(void)
{
	return 0;
}

void I_QueueVoiceFrameFromPlayer(INT32 playernum, void *data, UINT32 len, boolean terminal)
{
	(void)playernum; (void)data; (void)len; (void)terminal;
}

void I_SetPlayerVoiceProperties(INT32 playernum, float volume, float sep)
{
	(void)playernum; (void)volume; (void)sep;
}

void I_ResetVoiceQueue(INT32 playernum)
{
	(void)playernum;
}
