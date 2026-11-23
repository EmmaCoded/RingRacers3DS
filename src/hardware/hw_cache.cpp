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
/// \file hw_cache.c
/// \brief load and convert graphics to the hardware format

#include "../doomdef.h"

#ifdef HWRENDER
#include "hw_main.h"
#include "hw_glob.h"
#include "hw_drv.h"
#include "hw_batching.h"

#include "../doomstat.h"    //gamemode
#include "../i_video.h"     //rendermode
#include "../r_data.h"
#include "../r_textures.h"
#include "../w_wad.h"
#include "../z_zone.h"
#include "../v_video.h"
#include "../r_draw.h"
#include "../r_patch.h"
#include "../r_picformats.h"
#include "../p_setup.h"
#include "../p_mobj.h" // [CMAP]
#include "../r_skins.h" // [CMAP]
#include "../i_system.h" // [REGEN]
#include "../r_sky.h" // [REGEN]
#include "../rr_config.h" // [UPLOAD]

INT32 patchformat = GL_TEXFMT_AP_88; // use alpha for holes
INT32 textureformat = GL_TEXFMT_P_8; // use chromakey for hole

static INT32 format2bpp(GLTextureFormat_t format)
{
	if (format == GL_TEXFMT_RGBA)
		return 4;
	else if (format == GL_TEXFMT_ALPHA_INTENSITY_88 || format == GL_TEXFMT_AP_88)
		return 2;
	else
		return 1;
}

// This code was originally placed directly in HWR_DrawPatchInCache.
// It is now split from it for my sanity! (and the sanity of others)
// -- Monster Iestyn (13/02/19)
static void HWR_DrawColumnInCache(const column_t *patchcol, UINT8 *block, GLMipmap_t *mipmap,
								INT32 pblockheight, INT32 blockmodulo,
								fixed_t yfracstep, fixed_t scale_y,
								texpatch_t *originPatch, INT32 patchheight,
								INT32 bpp)
{
	fixed_t yfrac, position, count;
	UINT8 *dest;
	const UINT8 *source;
	INT32 topdelta, prevdelta = -1;
	INT32 originy = 0;

	// for writing a pixel to dest
	RGBA_t colortemp;
	UINT8 alpha;
	UINT8 texel;
	UINT16 texelu16;

	(void)patchheight; // This parameter is unused

	if (originPatch) // originPatch can be NULL here, unlike in the software version
		originy = originPatch->originy;

	while (patchcol->topdelta != 0xff)
	{
		topdelta = patchcol->topdelta;
		if (topdelta <= prevdelta)
			topdelta += prevdelta;
		prevdelta = topdelta;
		source = (const UINT8 *)patchcol + 3;
		count  = ((patchcol->length * scale_y) + (FRACUNIT/2)) >> FRACBITS;
		position = originy + topdelta;

		yfrac = 0;
		//yfracstep = (patchcol->length << FRACBITS) / count;
		if (position < 0)
		{
			yfrac = -position<<FRACBITS;
			count += (((position * scale_y) + (FRACUNIT/2)) >> FRACBITS);
			position = 0;
		}

		position = ((position * scale_y) + (FRACUNIT/2)) >> FRACBITS;

		if (position < 0)
			position = 0;

		if (position + count >= pblockheight)
			count = pblockheight - position;

		dest = block + (position*blockmodulo);
		while (count > 0)
		{
			count--;

			texel = source[yfrac>>FRACBITS];
			alpha = 0xFF;
			// Make pixel transparent if chroma keyed
			if ((mipmap->flags & TF_CHROMAKEYED) && (texel == HWR_PATCHES_CHROMAKEY_COLORINDEX))
				alpha = 0x00;

			//Hurdler: 25/04/2000: now support colormap in hardware mode
			if (mipmap->colormap)
				texel = mipmap->colormap->data[texel];

			// hope compiler will get this switch out of the loops (dreams...)
			// gcc do it ! but vcc not ! (why don't use cygwin gcc for win32 ?)
			// Alam: SRB2 uses Mingw, HUGS
			switch (bpp)
			{
				case 2 : // uhhhhhhhh..........
						 if ((originPatch != NULL) && (originPatch->style != AST_COPY))
							 texel = ASTBlendPaletteIndexes(*(dest+1), texel, originPatch->style, originPatch->alpha);
						 texelu16 = (UINT16)((alpha<<8) | texel);
						 memcpy(dest, &texelu16, sizeof(UINT16));
						 break;
				case 3 : colortemp = V_GetColor(texel);
						 if ((originPatch != NULL) && (originPatch->style != AST_COPY))
						 {
							 RGBA_t rgbatexel;
							 rgbatexel.rgba = *(UINT32 *)dest;
							 colortemp.rgba = ASTBlendTexturePixel(rgbatexel, colortemp, originPatch->style, originPatch->alpha);
						 }
						 memcpy(dest, &colortemp, sizeof(RGBA_t)-sizeof(UINT8));
						 break;
				case 4 : colortemp = V_GetColor(texel);
						 colortemp.s.alpha = alpha;
						 if ((originPatch != NULL) && (originPatch->style != AST_COPY))
						 {
							 RGBA_t rgbatexel;
							 rgbatexel.rgba = *(UINT32 *)dest;
							 colortemp.rgba = ASTBlendTexturePixel(rgbatexel, colortemp, originPatch->style, originPatch->alpha);
						 }
						 memcpy(dest, &colortemp, sizeof(RGBA_t));
						 break;
				// default is 1
				default:
						 if ((originPatch != NULL) && (originPatch->style != AST_COPY))
							 *dest = ASTBlendPaletteIndexes(*dest, texel, originPatch->style, originPatch->alpha);
						 else
							 *dest = texel;
						 break;
			}

			dest += blockmodulo;
			yfrac += yfracstep;
		}
		patchcol = (const column_t *)((const UINT8 *)patchcol + patchcol->length + 4);
	}
}

static void HWR_DrawFlippedColumnInCache(const column_t *patchcol, UINT8 *block, GLMipmap_t *mipmap,
								INT32 pblockheight, INT32 blockmodulo,
								fixed_t yfracstep, fixed_t scale_y,
								texpatch_t *originPatch, INT32 patchheight,
								INT32 bpp)
{
	fixed_t yfrac, position, count;
	UINT8 *dest;
	const UINT8 *source;
	INT32 topdelta, prevdelta = -1;
	INT32 originy = 0;

	// for writing a pixel to dest
	RGBA_t colortemp;
	UINT8 alpha;
	UINT8 texel;
	UINT16 texelu16;

	if (originPatch) // originPatch can be NULL here, unlike in the software version
		originy = originPatch->originy;

	while (patchcol->topdelta != 0xff)
	{
		topdelta = patchcol->topdelta;
		if (topdelta <= prevdelta)
			topdelta += prevdelta;
		prevdelta = topdelta;
		topdelta = patchheight-patchcol->length-topdelta;
		source = (const UINT8 *)patchcol + 3;
		count  = ((patchcol->length * scale_y) + (FRACUNIT/2)) >> FRACBITS;
		position = originy + topdelta;

		yfrac = (patchcol->length-1) << FRACBITS;

		if (position < 0)
		{
			yfrac += position<<FRACBITS;
			count += (((position * scale_y) + (FRACUNIT/2)) >> FRACBITS);
			position = 0;
		}

		position = ((position * scale_y) + (FRACUNIT/2)) >> FRACBITS;

		if (position < 0)
			position = 0;

		if (position + count >= pblockheight)
			count = pblockheight - position;

		dest = block + (position*blockmodulo);
		while (count > 0)
		{
			count--;

			texel = source[yfrac>>FRACBITS];
			alpha = 0xFF;
			// Make pixel transparent if chroma keyed
			if ((mipmap->flags & TF_CHROMAKEYED) && (texel == HWR_PATCHES_CHROMAKEY_COLORINDEX))
				alpha = 0x00;

			//Hurdler: 25/04/2000: now support colormap in hardware mode
			if (mipmap->colormap)
				texel = mipmap->colormap->data[texel];

			// hope compiler will get this switch out of the loops (dreams...)
			// gcc do it ! but vcc not ! (why don't use cygwin gcc for win32 ?)
			// Alam: SRB2 uses Mingw, HUGS
			switch (bpp)
			{
				case 2 : // uhhhhhhhh..........
						 if ((originPatch != NULL) && (originPatch->style != AST_COPY))
							 texel = ASTBlendPaletteIndexes(*(dest+1), texel, originPatch->style, originPatch->alpha);
						 texelu16 = (UINT16)((alpha<<8) | texel);
						 memcpy(dest, &texelu16, sizeof(UINT16));
						 break;
				case 3 : colortemp = V_GetColor(texel);
						 if ((originPatch != NULL) && (originPatch->style != AST_COPY))
						 {
							 RGBA_t rgbatexel;
							 rgbatexel.rgba = *(UINT32 *)dest;
							 colortemp.rgba = ASTBlendTexturePixel(rgbatexel, colortemp, originPatch->style, originPatch->alpha);
						 }
						 memcpy(dest, &colortemp, sizeof(RGBA_t)-sizeof(UINT8));
						 break;
				case 4 : colortemp = V_GetColor(texel);
						 colortemp.s.alpha = alpha;
						 if ((originPatch != NULL) && (originPatch->style != AST_COPY))
						 {
							 RGBA_t rgbatexel;
							 rgbatexel.rgba = *(UINT32 *)dest;
							 colortemp.rgba = ASTBlendTexturePixel(rgbatexel, colortemp, originPatch->style, originPatch->alpha);
						 }
						 memcpy(dest, &colortemp, sizeof(RGBA_t));
						 break;
				// default is 1
				default:
						 if ((originPatch != NULL) && (originPatch->style != AST_COPY))
							 *dest = ASTBlendPaletteIndexes(*dest, texel, originPatch->style, originPatch->alpha);
						 else
							 *dest = texel;
						 break;
			}

			dest += blockmodulo;
			yfrac -= yfracstep;
		}
		patchcol = (const column_t *)((const UINT8 *)patchcol + patchcol->length + 4);
	}
}


// Simplified patch caching function
// for use by sprites and other patches that are not part of a wall texture
// no alpha or flipping should be present since we do not want non-texture graphics to have them
// no offsets are used either
// -- Monster Iestyn (13/02/19)
static void HWR_DrawPatchInCache(GLMipmap_t *mipmap,
	INT32 pblockwidth, INT32 pblockheight,
	INT32 pwidth, INT32 pheight,
	const patch_t *realpatch)
{
	INT32 ncols;
	fixed_t xfrac, xfracstep;
	fixed_t yfracstep, scale_y;
	const column_t *patchcol;
	UINT8 *block = (UINT8 *)mipmap->data;
	INT32 bpp;
	INT32 blockmodulo;

	if (pwidth <= 0 || pheight <= 0)
		return;

	ncols = pwidth;

	// source advance
	xfrac = 0;
	xfracstep = FRACUNIT;
	yfracstep = FRACUNIT;
	scale_y   = FRACUNIT;
#ifdef __3DS__
	// Reduced patch (see HWR_MakePatch): sample with a stride > 1.
	if (pwidth < realpatch->width)
		xfracstep = (realpatch->width << FRACBITS) / pwidth;
	if (pheight < realpatch->height)
	{
		yfracstep = (realpatch->height << FRACBITS) / pheight;
		scale_y   = (pheight << FRACBITS) / realpatch->height;
	}
#endif

	bpp = format2bpp(mipmap->format);

	if (bpp < 1 || bpp > 4)
		I_Error("HWR_DrawPatchInCache: no drawer defined for this bpp (%d)\n",bpp);

	// NOTE: should this actually be pblockwidth*bpp?
	blockmodulo = pblockwidth*bpp;

	// Draw each column to the block cache
	for (; ncols--; block += bpp, xfrac += xfracstep)
	{
		patchcol = (const column_t *)((const UINT8 *)realpatch->columns + (realpatch->columnofs[xfrac>>FRACBITS]));

		HWR_DrawColumnInCache(patchcol, block, mipmap,
								pblockheight, blockmodulo,
								yfracstep, scale_y,
								NULL, pheight, // not that pheight is going to get used anyway...
								bpp);
	}
}

// This function we use for caching patches that belong to textures
static void HWR_DrawTexturePatchInCache(GLMipmap_t *mipmap,
	INT32 pblockwidth, INT32 pblockheight,
	texture_t *texture, texpatch_t *patch,
	const softwarepatch_t *realpatch)
{
	INT32 x, x1, x2;
	INT32 col, ncols;
	fixed_t xfrac, xfracstep;
	fixed_t yfracstep, scale_y;
	const column_t *patchcol;
	UINT8 *block = (UINT8 *)mipmap->data;
	INT32 bpp;
	INT32 blockmodulo;
	INT32 width, height;
	// Column drawing function pointer.
	static void (*ColumnDrawerPointer)(const column_t *patchcol, UINT8 *block, GLMipmap_t *mipmap,
								INT32 pblockheight, INT32 blockmodulo,
								fixed_t yfracstep, fixed_t scale_y,
								texpatch_t *originPatch, INT32 patchheight,
								INT32 bpp);

	if (texture->width <= 0 || texture->height <= 0)
		return;

	ColumnDrawerPointer = (patch->flip & 2) ? HWR_DrawFlippedColumnInCache : HWR_DrawColumnInCache;

	x1 = patch->originx;
	width = SHORT(realpatch->width);
	height = SHORT(realpatch->height);
	x2 = x1 + width;

	if (x1 > texture->width || x2 < 0)
		return; // patch not located within texture's x bounds, ignore

	if (patch->originy > texture->height || (patch->originy + height) < 0)
		return; // patch not located within texture's y bounds, ignore

	// patch is actually inside the texture!
	// now check if texture is partly off-screen and adjust accordingly

	// left edge
	if (x1 < 0)
		x = 0;
	else
		x = x1;

	// right edge
	if (x2 > texture->width)
		x2 = texture->width;


	col = x * pblockwidth / texture->width;
	ncols = ((x2 - x) * pblockwidth) / texture->width;

/*
	CONS_Debug(DBG_RENDER, "patch %dx%d texture %dx%d block %dx%d\n",
															width, height,
															texture->width,          texture->height,
															pblockwidth,             pblockheight);
	CONS_Debug(DBG_RENDER, "      col %d ncols %d x %d\n", col, ncols, x);
*/

	// source advance
	xfrac = 0;
	if (x1 < 0)
		xfrac = -x1<<FRACBITS;

	xfracstep = (texture->width << FRACBITS) / pblockwidth;
	yfracstep = (texture->height<< FRACBITS) / pblockheight;
	scale_y   = (pblockheight  << FRACBITS) / texture->height;

	bpp = format2bpp(mipmap->format);

	if (bpp < 1 || bpp > 4)
		I_Error("HWR_DrawTexturePatchInCache: no drawer defined for this bpp (%d)\n",bpp);

	// NOTE: should this actually be pblockwidth*bpp?
	blockmodulo = pblockwidth*bpp;

	// Draw each column to the block cache
	for (block += col*bpp; ncols--; block += bpp, xfrac += xfracstep)
	{
		if (patch->flip & 1)
			patchcol = (const column_t *)((const UINT8 *)realpatch + LONG(realpatch->columnofs[(width-1)-(xfrac>>FRACBITS)]));
		else
			patchcol = (const column_t *)((const UINT8 *)realpatch + LONG(realpatch->columnofs[xfrac>>FRACBITS]));

		ColumnDrawerPointer(patchcol, block, mipmap,
								pblockheight, blockmodulo,
								yfracstep, scale_y,
								patch, height,
								bpp);
	}
}

__attribute__((noinline)) static UINT8 *MakeBlock(GLMipmap_t *grMipmap)
{
	UINT8 *block;
	INT32 bpp, i;
	UINT16 bu16 = ((0x00 <<8) | HWR_PATCHES_CHROMAKEY_COLORINDEX);
	INT32 blocksize = (grMipmap->width * grMipmap->height);

	bpp =  format2bpp(grMipmap->format);
#ifdef __3DS__
	{
		// (MK7 style): fixed contiguous 1 MB reservation. With the fragmented
		// heap (8 MB free in small pieces) there are no 128-256 KB contiguous;
		// then the reservation is released and the block falls into that hole. Since the
		// RAM copy is freed when uploaded to the GPU (HWR_3DS_DropRam), the hole
		// comes back and the reservation is recovered on the next call.
		static void *s_reserve = NULL;
		const size_t kReserve = 1024u * 1024u;
		const size_t need = (size_t)blocksize*bpp + 64;
		// the fixed 1 MB reservation is no longer requested: with the single arena (z_zone.cpp) the
		// large blocks do not depend on the fragmented heap, and that MB was missing on huge maps.
		void *probe = malloc(need + 0x10000);
		if (probe) free(probe);
		else
		{
			Z_FreeTag(PU_HWRCACHE_UNLOCKED);
			probe = malloc(need + 0x10000);
			if (probe) free(probe);
			else if (s_reserve && need <= kReserve)
			{
				free(s_reserve);
				s_reserve = NULL;
				RR_DIAGPRINT("[3DS] MakeBlock: using the reserve for %d bytes\n", blocksize*bpp);
			}
			else
			{
				Z_FreeTags(PU_PURGELEVEL, INT32_MAX);
				RR_DIAGPRINT("[3DS] MakeBlock: full purge for %d bytes\n", blocksize*bpp);
			}
		}
	}
#endif
#ifdef __3DS__
	// DIAG [BIGTEX]: who requests blocks >=128 KB (after ETC they should no longer be map textures)
	if (blocksize*bpp >= 128*1024)
	{
		static void *s_seen[64]; static int s_nv = 0;
		void *ra = __builtin_return_address(0);
		int k = 0; while (k < s_nv && s_seen[k] != ra) k++;
		if (k == s_nv && s_nv < 64) { s_seen[s_nv++] = ra;
			RR_DIAGPRINT("[BIGTEX] MakeBlock %dx%d bpp=%d (%d KB) fmt=%d from LR=0x%08lx\n", grMipmap->width, grMipmap->height,
				bpp, blocksize*bpp >> 10, (int)grMipmap->format, (unsigned long)(uintptr_t)ra); }
	}
#endif
	block = (UINT8 *)Z_Malloc(blocksize*bpp, PU_HWRCACHE, &(grMipmap->data));

	switch (bpp)
	{
		case 1: memset(block, HWR_PATCHES_CHROMAKEY_COLORINDEX, blocksize); break;
		case 2:
				// fill background with chromakey, alpha = 0
				for (i = 0; i < blocksize; i++)
					memcpy(block+i*sizeof(UINT16), &bu16, sizeof(UINT16));

				break;
		case 4: memset(block, 0x00, blocksize*sizeof(UINT32)); break;
	}

	return block;
}

#ifdef __3DS__
// Reads the pixels of a texture from the ringracers_3ds.pk3 pack (on demand).
// Leaves the mipmap at the GPU size; UVs keep using the logical
// size (scaleX = 1/texture->width), so nothing else changes.
extern "C" int I_3DS_EtcGet(int id, int *w, int *h, int *fmt); // r_citro3d.cpp 
extern "C" void I_3DS_TexSetEtc(GLMipmap_t *mip, int id);
extern "C" int I_3DS_PaletteIsDefault(void);
extern "C" int I_3DS_EtcGetCmap(int id, const UINT8 *cmap, int *w, int *h, int *fmt);

static INT32 s_whyCmap = 0, s_whyNoEtc = 0, s_whySize = 0, s_whyEtc = 0; // DIAG
// package of SMALL versions (max 256x256, palette indices) of the
// large textures (assets3ds/convert_small.py). For custom palettes.
struct R3dsSmall { UINT16 w, h; UINT8 fmt; UINT32 off, size; };
static R3dsSmall *s_small = NULL;
static UINT32 s_smallN = 0;
static FILE *s_smallF = NULL;
static boolean s_smallTried = false;
static void HWR_3DS_SmallLoad(void)
{
	s_smallTried = true;
	s_smallF = fopen("romfs:/ringracers_3ds_small.bin", "rb");
	if (!s_smallF) s_smallF = fopen("sdmc:/3ds/RingRacers3ds/data/ringracers_3ds_small.bin", "rb");
	if (!s_smallF) { RR_DIAGPRINT("[SMALLTEX] no ringracers_3ds_small.bin\n"); return; }
	UINT8 h[12];
	if (fread(h, 1, 12, s_smallF) != 12 || memcmp(h, "T3S1", 4)) { fclose(s_smallF); s_smallF = NULL; return; }
	const UINT32 n = h[8] | (h[9] << 8) | (h[10] << 16) | ((UINT32)h[11] << 24);
	UINT8 *ix = (UINT8 *)malloc((size_t)n * 16);
	s_small = (R3dsSmall *)malloc((size_t)n * sizeof(R3dsSmall));
	if (!ix || !s_small || fread(ix, 16, n, s_smallF) != n)
	{
		free(ix); free(s_small); s_small = NULL; fclose(s_smallF); s_smallF = NULL; return;
	}
	UINT32 cnt = 0;
	for (UINT32 i = 0; i < n; i++)
	{
		const UINT8 *e = ix + i * 16;
		s_small[i].w = (UINT16)(e[0] | (e[1] << 8));
		s_small[i].h = (UINT16)(e[2] | (e[3] << 8));
		s_small[i].fmt = e[4];
		s_small[i].off = e[8] | (e[9] << 8) | (e[10] << 16) | ((UINT32)e[11] << 24);
		s_small[i].size = e[12] | (e[13] << 8) | (e[14] << 16) | ((UINT32)e[15] << 24);
		if (s_small[i].w) cnt++;
	}
	free(ix);
	s_smallN = n;
	RR_DIAGPRINT("[SMALLTEX] %u large textures with a small version\n", (unsigned)cnt);
}

static boolean HWR_3DS_FromPack(GLMipmap_t *mip, INT32 texnum, const UINT8 *colormap, boolean chroma)
{
	if (!r3ds_texinfo || texnum <= 0 || texnum >= r3ds_numtex)
		return false;
	const r3ds_texinfo_t *inf = &r3ds_texinfo[texnum];
	if (!inf->gw || !inf->gh)
		return false;

	// ETC1/ETC1A4 version already converted on the PC (native GPU format,
	// 4-8 bits per pixel). Only with the normal palette (no Encore or map
	// palette). The image is not built on the CPU: the GPU receives it as is.
	{
		int ew, eh, efmt;
		// I_3DS_EtcGet already picks the ETC set for the current palette (normal or custom)
		// DIAG [REGEN]: why ETC is NOT used (cmap = different colormap,
		// noetc = no ETC for that texture/palette, tam = different size)
		int etcok = 0;
		// with a different colormap (map remap) the ETC set of the
		// effective palette (r_citro3d.cpp I_3DS_EtcGetCmap)
		if (colormap != colormaps)
		{
			if (!I_3DS_EtcGetCmap(texnum - 1, colormap, &ew, &eh, &efmt)) s_whyCmap++;
			else if (ew != inf->gw || eh != inf->gh) s_whySize++;
			else etcok = 1;
		}
		else if (!I_3DS_EtcGet(texnum - 1, &ew, &eh, &efmt)) s_whyNoEtc++;
		else if (ew != inf->gw || eh != inf->gh) s_whySize++;
		else etcok = 1;
		if (etcok)
		{
			s_whyEtc++;
			mip->width = (UINT16)ew;
			mip->height = (UINT16)eh;
			Z_Malloc(4, PU_HWRCACHE, &mip->data); // marker (the data is in the file)
			mip->flags |= TF_3DS_ETC;
			if (efmt == 0 || !chroma)
				mip->flags &= ~TF_CHROMAKEYED;
			I_3DS_TexSetEtc(mip, texnum - 1);
			return true;
		}
	}

	// precomputed small version (if present): reads 64 KB instead of 1-9 MB
	INT32 gw = inf->gw, gh = inf->gh, gfmt = inf->fmt;
	const R3dsSmall *sm = NULL;
	if (!s_smallTried) HWR_3DS_SmallLoad();
	if (s_smallF && (UINT32)(texnum - 1) < s_smallN && s_small[texnum - 1].w)
	{
		sm = &s_small[texnum - 1];
		gw = sm->w; gh = sm->h; gfmt = sm->fmt;
	}
	const size_t npx = (size_t)gw * gh;
	size_t rawsize = npx;
	if (gfmt == 1) rawsize = npx * 2;
	else if (gfmt == 2) rawsize = npx + (npx + 7) / 8;
	if (sm && sm->size != rawsize) sm = NULL, gw = inf->gw, gh = inf->gh, gfmt = inf->fmt; // odd index: regular path
	const size_t npx2 = (size_t)gw * gh;
	if (!sm) { rawsize = npx2; if (gfmt == 1) rawsize = npx2 * 2; else if (gfmt == 2) rawsize = npx2 + (npx2 + 7) / 8; }

	UINT8 *raw = (UINT8 *)malloc(rawsize);
	if (!raw)
	{
		Z_FreeTag(PU_HWRCACHE_UNLOCKED);
		raw = (UINT8 *)malloc(rawsize);
		if (!raw)
			return false;
	}
	if (sm)
	{
		if (fseek(s_smallF, (long)sm->off, SEEK_SET) || fread(raw, 1, rawsize, s_smallF) != rawsize)
		{
			free(raw);
			return false;
		}
	}
	else if (W_ReadLumpHeaderPwad(r3ds_texwad, r3ds_datalump, raw, rawsize, inf->offset) != rawsize)
	{
		CONS_Printf("3DS tex: error reading %.8s\n", textures[texnum]->name);
		free(raw);
		return false;
	}

	// special stages (custom palette) cannot use ETC1 -> previously each
	// texture was built as RGBA (4 bytes/pixel): 256x256 = 256 KB each and
	// a 3072x3072 one = 36 MB -> out of memory when the UFO moved away. Now:
	// - palette indices (P_8, 1 byte/pixel); the GPU converts on upload
	// with the current palette (upload_tex, same color as before).
	// - max 1024x1024 (the 3DS GPU limit: it was rescaled on upload anyway).
	INT32 dw = gw, dh = gh;
	// measured with [REGEN] on Balconies: 100-135 regenerations every 2 s,
	// 50-115 ms each (animated sky SSSKY00-23 1024x1024, chains
	// SSRING15 3072x3072...). They do not fit on the GPU (only the
	// sky ~48 MB) -> they get evicted and rebuilt nonstop: 5 fps and
	// 128-256 KB blocks that fragment the heap. 400x240 screen: max 256x256
	// (MK7 style) -> 16x less work/memory and 64 KB blocks.
	while (dw > 256) dw >>= 1;
	while (dh > 256) dh >>= 1;
	mip->width = (UINT16)dw;
	mip->height = (UINT16)dh;
	mip->format = GL_TEXFMT_P_8;
	UINT8 *block = MakeBlock(mip);
	const INT32 bpp = format2bpp(mip->format);
	boolean anyhole = false;
	const size_t dpx = (size_t)dw * dh;

	for (size_t di = 0; di < dpx; di++)
	{
		const size_t i = (size_t)((di / dw) * gh / dh) * gw + (size_t)((di % dw) * gw / dw);
		UINT8 idx, opaque;
		if (gfmt == 1) { idx = raw[i * 2]; opaque = (raw[i * 2 + 1] >= 128); }
		else if (gfmt == 2) { idx = raw[i]; opaque = (raw[npx2 + (i >> 3)] >> (i & 7)) & 1; }
		else { idx = raw[i]; opaque = 1; }
		if (!chroma) opaque = 1;
		if (!opaque) { anyhole = true; continue; } // MakeBlock already left the hole
		const UINT8 c = colormap[idx];
		if (bpp == 1)
			block[di] = c;
		else if (bpp == 2)
		{
			block[di * 2] = c; block[di * 2 + 1] = 0xff;
		}
		else if (bpp == 4)
		{
			RGBA_t col = V_GetColor(c);
			block[di * 4] = col.s.red; block[di * 4 + 1] = col.s.green;
			block[di * 4 + 2] = col.s.blue; block[di * 4 + 3] = 0xff;
		}
	}
	free(raw);

	if (!anyhole)
		mip->flags &= ~TF_CHROMAKEYED;
	else if (bpp == 4)
		mip->flags |= TF_TRANSPARENT;
	return true;
}
#endif

//
// Create a composite texture from patches, adapt the texture size to a power of 2
// height and width for the hardware texture cache.
//
static void HWR_GenerateTexture(GLMapTexture_t *grtex, INT32 texnum, boolean noencoremap)
{
	UINT8 *block;
	texture_t *texture;
	texpatch_t *patch;
	softwarepatch_t *realpatch;
	UINT8 *pdata;
	INT32 blockwidth, blockheight, blocksize;

	UINT8 *colormap = colormaps;

	INT32 i;
	boolean skyspecial = false; //poor hack for Legacy large skies..

	texture = textures[texnum];

	// hack the Legacy skies..
	if (texture->name[0] == 'S' &&
	    texture->name[1] == 'K' &&
	    texture->name[2] == 'Y' &&
	    (texture->name[4] == 0 ||
	     texture->name[5] == 0)
	   )
	{
		skyspecial = true;
		grtex->mipmap.flags = TF_WRAPXY; // don't use the chromakey for sky
	}
	else
		grtex->mipmap.flags = TF_CHROMAKEYED | TF_WRAPXY;

	grtex->mipmap.width = (UINT16)texture->width;
	grtex->mipmap.height = (UINT16)texture->height;
	grtex->mipmap.format = (GLTextureFormat_t)textureformat;

	if (!noencoremap && encoremap)
		colormap += COLORMAP_REMAPOFFSET;

	// LEAK FIX: on 3DS the texture is regenerated every time it leaves the GPU (HWR_3DS_DropRam
	// frees the RAM copy) and here a NEW colormap was requested without freeing the old one:
	// +264 B per regeneration (hwrinfo grew from 1.5 to 2.9 MB while playing Playground).
	// Now it is reused; FreeMapTexture releases it on map change.
	if (!grtex->mipmap.colormap)
		grtex->mipmap.colormap = (GLColormap_t *)Z_Calloc(sizeof(*grtex->mipmap.colormap), PU_HWRPATCHCOLMIPMAP, NULL);
	grtex->mipmap.colormap->source = colormap;
	M_Memcpy(grtex->mipmap.colormap->data, colormap, 256 * sizeof(UINT8));

#ifdef __3DS__
	if (HWR_3DS_FromPack(&grtex->mipmap, texnum, colormap, !skyspecial))
	{
		grtex->scaleX = 1.0f/(texture->width*FRACUNIT);
		grtex->scaleY = 1.0f/(texture->height*FRACUNIT);
		return;
	}
#endif
	blockwidth = texture->width;
	blockheight = texture->height;
	blocksize = (blockwidth * blockheight);
	block = MakeBlock(&grtex->mipmap);

	if (skyspecial) //Hurdler: not efficient, but better than holes in the sky (and it's done only at level loading)
	{
		INT32 j;
		RGBA_t col;

		col = V_GetColor(HWR_PATCHES_CHROMAKEY_COLORINDEX);
		for (j = 0; j < blockheight; j++)
		{
			for (i = 0; i < blockwidth; i++)
			{
				block[4*(j*blockwidth+i)+0] = col.s.red;
				block[4*(j*blockwidth+i)+1] = col.s.green;
				block[4*(j*blockwidth+i)+2] = col.s.blue;
				block[4*(j*blockwidth+i)+3] = 0xff;
			}
		}
	}

	// Composite the columns together.
	for (i = 0, patch = texture->patches; i < texture->patchcount; i++, patch++)
	{
		boolean dealloc = true;
		size_t lumplength = W_LumpLengthPwad(patch->wad, patch->lump);
		pdata = (uint8_t*)W_CacheLumpNumPwad(patch->wad, patch->lump, PU_CACHE);
		realpatch = (softwarepatch_t *)pdata;

#ifndef NO_PNG_LUMPS
		if (Picture_IsLumpPNG((UINT8 *)realpatch, lumplength))
			realpatch = (softwarepatch_t *)Picture_PNGConvert(pdata, PICFMT_DOOMPATCH, NULL, NULL, NULL, NULL, lumplength, NULL, (pictureflags_t)0);
		else
#endif
#ifdef WALLFLATS
		if (texture->type == TEXTURETYPE_FLAT)
			realpatch = (softwarepatch_t *)Picture_Convert(PICFMT_FLAT, pdata, PICFMT_DOOMPATCH, 0, NULL, texture->width, texture->height, 0, 0, (pictureflags_t)0);
		else
#endif
		{
			(void)lumplength;
			dealloc = false;
		}

		HWR_DrawTexturePatchInCache(&grtex->mipmap, blockwidth, blockheight, texture, patch, realpatch);

		if (dealloc)
			Z_Unlock(realpatch);
	}
#ifdef __3DS__
	// Filler texture without patches (3DS does not load the texture pk3 files): the
	// same gray checkerboard the software renderer uses (r_textures.cpp).
	if (texture->patchcount == 0)
	{
		const INT32 bpp = format2bpp(grtex->mipmap.format);
		INT32 x, y;
		for (y = 0; y < blockheight; y++)
			for (x = 0; x < blockwidth; x++)
			{
				const UINT8 idx = colormap[(((x >> 3) ^ (y >> 3)) & 1) ? 8 : 12];
				UINT8 *dst = block + (y * blockwidth + x) * bpp;
				if (bpp == 4)
				{
					RGBA_t c = V_GetColor(idx);
					dst[0] = c.s.red; dst[1] = c.s.green; dst[2] = c.s.blue; dst[3] = 0xff;
				}
				else if (bpp == 2)
				{
					dst[0] = idx; dst[1] = 0xff;
				}
				else
					dst[0] = idx;
			}
		grtex->mipmap.flags &= ~(TF_CHROMAKEYED | TF_TRANSPARENT);
	}
#endif
	//Hurdler: not efficient at all but I don't remember exactly how HWR_DrawPatchInCache works :(
	if (format2bpp(grtex->mipmap.format)==4)
	{
		for (i = 3; i < blocksize*4; i += 4) // blocksize*4 because blocksize doesn't include the bpp
		{
			if (block[i] == 0)
			{
				grtex->mipmap.flags |= TF_TRANSPARENT;
				break;
			}
		}
	}

	grtex->scaleX = 1.0f/(texture->width*FRACUNIT);
	grtex->scaleY = 1.0f/(texture->height*FRACUNIT);
}

// patch may be NULL if grMipmap has been initialised already and makebitmap is false
void HWR_MakePatch (const patch_t *patch, GLPatch_t *grPatch, GLMipmap_t *grMipmap, boolean makebitmap)
{
	if (grMipmap->width == 0)
	{
		grMipmap->width = grMipmap->height = 1;
		while (grMipmap->width < patch->width) grMipmap->width <<= 1;
		while (grMipmap->height < patch->height) grMipmap->height <<= 1;

		// no wrap around, no chroma key
		grMipmap->flags = 0;

		// setup the texture info
		grMipmap->format = (GLTextureFormat_t)patchformat;

		grPatch->max_s = (float)patch->width / (float)grMipmap->width;
		grPatch->max_t = (float)patch->height / (float)grMipmap->height;
#ifdef __3DS__
		// huge patches (>256 KB, e.g. 512x512 RGBA = 1 MB)
		// fragment the heap and caused "Out of memory" in MakeBlock.
		// The block is halved until it fits; max_s/max_t do not change
		// (same proportion) and HWR_DrawPatchInCache scales when drawing.
		// measured [BIGTEX] HWR_MakePatch (RGBA 4 bpp sprites) = the out-of-memory crash
		// of Balconies after ETCPAL (256 contiguous KB with the heap in small pieces). Cap 64 KB
		// (128x128 RGBA): small blocks that always fit. 400x240 screen.
		while ((INT32)grMipmap->width * grMipmap->height * format2bpp(grMipmap->format) > 64*1024
			&& grMipmap->width > 8 && grMipmap->height > 8)
		{
			grMipmap->width >>= 1;
			grMipmap->height >>= 1;
		}
#endif
	}

	Z_Free(grMipmap->data);
	grMipmap->data = NULL;

	if (makebitmap)
	{
		MakeBlock(grMipmap);
#ifdef __3DS__
		HWR_DrawPatchInCache(grMipmap,
			grMipmap->width, grMipmap->height,
			(INT32)(grPatch->max_s * grMipmap->width + 0.5f), (INT32)(grPatch->max_t * grMipmap->height + 0.5f),
			patch);
#else
		HWR_DrawPatchInCache(grMipmap,
			grMipmap->width, grMipmap->height,
			patch->width, patch->height,
			patch);
#endif
	}
}

// =================================================
//             CACHING HANDLING
// =================================================

static size_t gl_numtextures = 0; // Texture count
static GLMapTexture_t *gl_textures; // For all textures
static GLMapTexture_t *gl_flats; // For all (texture) flats, as normal flats don't need to be cached
#ifdef __3DS__
// these used to be 2 tables of numtextures x 36 bytes (17600 textures = 1.27 MB always occupied).
// Now 2 tables of pointers (4 bytes) and each entry is created the first time it is used.
static GLMapTexture_t **gl_textures_p, **gl_flats_p;
static GLMapTexture_t *GL_Entry(GLMapTexture_t **table, size_t i)
{
	if (!table[i])
	{
		table[i] = (GLMapTexture_t *)calloc(1, sizeof(GLMapTexture_t));
		if (!table[i])
			I_Error("GL_Entry: out of memory for texture %u", (unsigned)i);
	}
	return table[i];
}
#define GL_TEX(i) GL_Entry(gl_textures_p, (size_t)(i))
#define GL_FLAT(i) GL_Entry(gl_flats_p, (size_t)(i))
#else
#define GL_TEX(i) (&gl_textures[i])
#define GL_FLAT(i) (&gl_flats[i])
#endif
boolean gl_maptexturesloaded = false;

void HWR_FreeTextureData(patch_t *patch)
{
	GLPatch_t *grPatch;

	if (!patch || !patch->hardware)
		return;

	grPatch = (GLPatch_t*)patch->hardware;

	if (vid.glstate == VID_GL_LIBRARY_LOADED)
		HWD.pfnDeleteTexture(grPatch->mipmap);
	if (grPatch->mipmap->data)
		Z_Free(grPatch->mipmap->data);
}

void HWR_FreeTexture(patch_t *patch)
{
	if (!patch)
		return;

	if (patch->hardware)
	{
		GLPatch_t *grPatch = (GLPatch_t*)patch->hardware;

		HWR_FreeTextureColormaps(patch);

		if (grPatch->mipmap)
		{
			HWR_FreeTextureData(patch);
			Z_Free(grPatch->mipmap);
		}

		Z_Free(patch->hardware);
	}

	patch->hardware = NULL;
}

// Called by HWR_FreePatchCache.
void HWR_FreeTextureColormaps(patch_t *patch)
{
	GLPatch_t *pat;

	// The patch must be valid, obviously
	if (!patch)
		return;

	pat = (GLPatch_t*)patch->hardware;
	if (!pat)
		return;

	// The mipmap must be valid, obviously
	while (pat->mipmap)
	{
		// Confusing at first, but pat->mipmap->nextcolormap
		// at the beginning of the loop is the first colormap
		// from the linked list of colormaps.
		GLMipmap_t *next = NULL;

		// No mipmap in this patch, break out of the loop.
		if (!pat->mipmap)
			break;

		// No colormap mipmaps either.
		if (!pat->mipmap->nextcolormap)
			break;

		// Set the first colormap to the one that comes after it.
		next = pat->mipmap->nextcolormap;
		pat->mipmap->nextcolormap = next->nextcolormap;

		// Free image data from memory.
		if (next->data)
			Z_Free(next->data);
		if (next->colormap)
			Z_Free(next->colormap);
		next->data = NULL;
		next->colormap = NULL;
		HWD.pfnDeleteTexture(next);

		// Free the old colormap mipmap from memory.
		free(next);
	}
}

static boolean FreeTextureCallback(void *mem)
{
	patch_t *patch = (patch_t *)mem;
	HWR_FreeTexture(patch);
	return false;
}

static boolean FreeColormapsCallback(void *mem)
{
	patch_t *patch = (patch_t *)mem;
	HWR_FreeTextureColormaps(patch);
	return false;
}

static void HWR_FreePatchCache(boolean freeall)
{
	boolean (*callback)(void *mem) = FreeTextureCallback;

	if (!freeall)
		callback = FreeColormapsCallback;

	Z_IterateTags(PU_PATCH, PU_PATCH_ROTATED, callback);
	Z_IterateTags(PU_SPRITE, PU_HUDGFX, callback);
}

// free all textures after each level
void HWR_ClearAllTextures(void)
{
	HWD.pfnClearMipMapCache(); // free references to the textures
	HWR_FreePatchCache(true);
}

void HWR_FreeColormapCache(void)
{
	HWR_FreePatchCache(false);
}

void HWR_InitMapTextures(void)
{
	gl_textures = NULL;
	gl_flats = NULL;
	gl_maptexturesloaded = false;
}

static void FreeMapTexture(GLMapTexture_t *tex)
{
	HWD.pfnDeleteTexture(&tex->mipmap);
	if (tex->mipmap.data)
		Z_Free(tex->mipmap.data);
	tex->mipmap.data = NULL;
	// the texture's colormap was never freed either (leak on every map change)
	if (tex->mipmap.colormap)
		Z_Free(tex->mipmap.colormap);
	tex->mipmap.colormap = NULL;
}

void HWR_FreeMapTextures(void)
{
	size_t i;

#ifdef __3DS__
	for (i = 0; i < gl_numtextures; i++)
	{
		if (gl_textures_p && gl_textures_p[i]) { FreeMapTexture(gl_textures_p[i]); free(gl_textures_p[i]); }
		if (gl_flats_p && gl_flats_p[i]) { FreeMapTexture(gl_flats_p[i]); free(gl_flats_p[i]); }
	}
	if (gl_textures_p) free(gl_textures_p);
	if (gl_flats_p) free(gl_flats_p);
	gl_textures_p = gl_flats_p = NULL;
#else
	for (i = 0; i < gl_numtextures; i++)
	{
		FreeMapTexture(&gl_textures[i]);
		FreeMapTexture(&gl_flats[i]);
	}
#endif

	// now the heap don't have any 'user' pointing to our
	// texturecache info, we can free it
	if (gl_textures)
		free(gl_textures);
	if (gl_flats)
		free(gl_flats);
	gl_textures = NULL;
	gl_flats = NULL;
	gl_numtextures = 0;
	gl_maptexturesloaded = false;
}

void HWR_LoadMapTextures(size_t pnumtextures)
{
	// we must free it since numtextures may have changed
	HWR_FreeMapTextures();

	gl_numtextures = pnumtextures;
#ifdef __3DS__
	gl_textures_p = (GLMapTexture_t **)calloc(gl_numtextures, sizeof(*gl_textures_p));
	gl_flats_p = (GLMapTexture_t **)calloc(gl_numtextures, sizeof(*gl_flats_p));
	if ((gl_textures_p == NULL) || (gl_flats_p == NULL))
		I_Error("HWR_LoadMapTextures: ran out of memory for OpenGL textures");
	gl_maptexturesloaded = true;
	return;
#endif
	gl_textures = (GLMapTexture_t *)calloc(gl_numtextures, sizeof(*gl_textures));
	gl_flats = (GLMapTexture_t *)calloc(gl_numtextures, sizeof(*gl_flats));

	if ((gl_textures == NULL) || (gl_flats == NULL))
		I_Error("HWR_LoadMapTextures: ran out of memory for OpenGL textures");

	gl_maptexturesloaded = true;
}

void HWR_SetPalette(RGBA_t *palette)
{
	HWD.pfnSetPalette(palette);

	// hardware driver will flush there own cache if cache is non paletized
	// now flush data texture cache so 32 bit texture are recomputed
	if (patchformat == GL_TEXFMT_RGBA || textureformat == GL_TEXFMT_RGBA)
	{
		Z_FreeTag(PU_HWRCACHE);
		Z_FreeTag(PU_HWRCACHE_UNLOCKED);
	}
}

// --------------------------------------------------------------------------
#ifdef __3DS__
#ifdef __3DS__
// DIAG [REGEN]: which textures are regenerated (CPU build + upload) and how much
// they cost. Suspicion: animated sky/textures of Balconies regenerated every frame.
static INT32 s_regenN[8192];
static UINT64 s_regenT[8192];
static tic_t s_regenLast = 0;
static void HWR_3DS_RegenNote(INT32 tex, UINT64 dt)
{
	if (tex >= 0 && tex < 8192) { s_regenN[tex]++; s_regenT[tex] += dt; }
	if (gametic - s_regenLast < 2*TICRATE)
		return;
	s_regenLast = gametic;
	INT32 tot = 0; UINT64 tt = 0;
	for (int i = 0; i < 8192 && i < numtextures; i++) { tot += s_regenN[i]; tt += s_regenT[i]; }
	RR_DIAGPRINT("[REGEN] 2s: %d regenerations, %.1f ms | sky=%d (%.8s) | top:", (int)tot,
		tt * 1000.0 / I_GetPrecisePrecision(), (int)texturetranslation[skytexture],
		textures[texturetranslation[skytexture]]->name);
	for (int k = 0; k < 6; k++)
	{
		int best = -1;
		for (int i = 0; i < 8192 && i < numtextures; i++)
			if (s_regenN[i] && (best < 0 || s_regenT[i] > s_regenT[best])) best = i;
		if (best < 0) break;
		CONS_Printf(" %.8s(%dx%d)=%dx/%.1fms", textures[best]->name, textures[best]->width, textures[best]->height,
			(int)s_regenN[best], s_regenT[best] * 1000.0 / I_GetPrecisePrecision());
		s_regenN[best] = 0; s_regenT[best] = 0;
	}
	CONS_Printf("\n");
	RR_DIAGPRINT("[REGEN] why: etc=%d | no etc: colormap=%d no_file=%d measured=%d | normal_palette=%d\n",
		(int)s_whyEtc, (int)s_whyCmap, (int)s_whyNoEtc, (int)s_whySize, I_3DS_PaletteIsDefault());
	s_whyEtc = s_whyCmap = s_whyNoEtc = s_whySize = 0;
	memset(s_regenN, 0, sizeof s_regenN); memset(s_regenT, 0, sizeof s_regenT);
}
#endif

// (MK7 style): the RAM copy of a wall/flat texture already uploaded to
// the GPU is freed RIGHT AWAY (the GPU keeps its own). It used to stay as a
// purgeable zone block interleaved with the others -> fragmented heap -> no 128 KB
// contiguous (MakeBlock crash on Balconies). If the GPU evicts it (LRU) or the
// palette changes, downloaded=0 and data=NULL -> it regenerates by itself.
static void HWR_3DS_DropRam(GLMipmap_t *m)
{
	if (!m->data)
		return;
	if (m->downloaded)
		Z_Free(m->data); // the "user" (m->data) becomes NULL
	else
		Z_ChangeTag(m->data, PU_HWRCACHE_UNLOCKED);
}
#else
#define HWR_3DS_DropRam(m) Z_ChangeTag((m)->data, PU_HWRCACHE_UNLOCKED)
#endif

// Make sure texture is downloaded and set it as the source
// --------------------------------------------------------------------------
#if defined(__3DS__) || defined(RR_PCGEN)
// Last texture request (to regenerate it from the static cache).
struct rr3ds_texreq_t { int kind; INT32 a, b; void *p; };
rr3ds_texreq_t rr3ds_texreq = { 0, 0, 0, NULL };
GLMapTexture_t *rr3ds_lastgrtex = NULL;
#define RR3DS_REQ(k,aa,bb,pp) do { rr3ds_texreq.kind = (k); rr3ds_texreq.a = (aa); rr3ds_texreq.b = (bb); rr3ds_texreq.p = (pp); } while (0)
#else
#define RR3DS_REQ(k,aa,bb,pp) do {} while (0)
#endif
GLMapTexture_t *HWR_GetTexture(INT32 tex, INT32 basetex)
{
	GLMapTexture_t *grtex;
#ifdef PARANOIA
	if ((unsigned)tex >= gl_numtextures)
		I_Error("HWR_GetTexture: tex >= numtextures\n");
#endif

	// Every texture in memory, stored in the
	// hardware renderer's bit depth format. Wow!
	grtex = GL_TEX(tex);

	// Generate texture if missing from the cache
	if (!grtex->mipmap.data && !grtex->mipmap.downloaded)
	{
#ifdef __3DS__
		const UINT64 rg0 = I_GetPreciseTime();
#endif
		HWR_GenerateTexture(grtex, tex, !R_TextureCanRemap(basetex));
#ifdef __3DS__
		HWR_3DS_RegenNote(tex, I_GetPreciseTime() - rg0);
#endif
	}

	// If hardware does not have the texture, then call pfnSetTexture to upload it
	if (!grtex->mipmap.downloaded)
	{
#ifdef __3DS__
		const UINT64 up0 = I_GetPreciseTime();
#endif
		HWD.pfnSetTexture(&grtex->mipmap);
#ifdef __3DS__
		{
			// slow GPU upload (>300 us): texture, size, how many times (max 30 lines)
			// summary every 5 s: how many uploads and their total time; the slowest one by name
			static UINT64 w0 = 0, wTot = 0, wMax = 0; static int wN = 0, wTex = 0;
			const UINT64 now = I_GetPreciseTime(), d = now - up0;
			if (!w0) w0 = now;
			wTot += d; wN++;
			if (d > wMax) { wMax = d; wTex = (int)tex; }
			if (now - w0 > 5 * I_GetPrecisePrecision())
			{
				if (RRCfg_On("diag"))
					RR_DIAGPRINT("[UPLOAD] 5s: n=%d total=%.0f ms | max=%.1f ms %.8s %dx%d\n", wN,
						wTot * 1000.0 / I_GetPrecisePrecision(), wMax * 1000.0 / I_GetPrecisePrecision(),
						textures[wTex]->name, (int)textures[wTex]->width, (int)textures[wTex]->height);
				w0 = now; wTot = wMax = 0; wN = 0;
			}
		}
#endif
	}
	HWR_SetCurrentTexture(&grtex->mipmap);

	// The system-memory data can be purged now.
	HWR_3DS_DropRam(&grtex->mipmap);

	if (R_GetTextureBrightmap(tex))
	{
		GLMapTexture_t *grtexbright;

		// Every texture in memory, stored in the
		// hardware renderer's bit depth format. Wow!
		grtexbright = GL_TEX(R_GetTextureBrightmap(tex));

		// Generate texture if missing from the cache
		if (!grtexbright->mipmap.data && !grtexbright->mipmap.downloaded)
			HWR_GenerateTexture(grtexbright, R_GetTextureBrightmap(tex), true);

		grtexbright->mipmap.flags |= TF_BRIGHTMAP;

		// If hardware does not have the texture, then call pfnSetTexture to upload it
		if (!grtexbright->mipmap.downloaded)
			HWD.pfnSetTexture(&grtexbright->mipmap);
		HWR_SetCurrentTexture(&grtexbright->mipmap);

		// The system-memory data can be purged now.
		HWR_3DS_DropRam(&grtexbright->mipmap);
	}

	RR3DS_REQ(1, tex, basetex, NULL);
	rr3ds_lastgrtex = grtex; // texture scale for sliding walls
	return grtex;
}

static void HWR_CacheFlat(GLMipmap_t *grMipmap, lumpnum_t flatlumpnum)
{
	UINT8 *flat;
	size_t steppy;
	size_t size, pflatsize;


	// setup the texture info
	grMipmap->format = GL_TEXFMT_P_8;
	grMipmap->flags = TF_WRAPXY|TF_CHROMAKEYED;

	size = W_LumpLength(flatlumpnum);

	switch (size)
	{
		case 4194304: // 2048x2048 lump
			pflatsize = 2048;
			break;
		case 1048576: // 1024x1024 lump
			pflatsize = 1024;
			break;
		case 262144:// 512x512 lump
			pflatsize = 512;
			break;
		case 65536: // 256x256 lump
			pflatsize = 256;
			break;
		case 16384: // 128x128 lump
			pflatsize = 128;
			break;
		case 1024: // 32x32 lump
			pflatsize = 32;
			break;
		default: // 64x64 lump
			pflatsize = 64;
			break;
	}

	grMipmap->width  = (UINT16)pflatsize;
	grMipmap->height = (UINT16)pflatsize;

	// the flat raw data needn't be converted with palettized textures
	W_ReadLump(flatlumpnum, Z_Malloc(W_LumpLength(flatlumpnum),
		PU_HWRCACHE, &grMipmap->data));

	flat = (UINT8*)grMipmap->data;
	for (steppy = 0; steppy < size; steppy++)
		if (flat[steppy] != HWR_PATCHES_CHROMAKEY_COLORINDEX)
			flat[steppy] = grMipmap->colormap->source[flat[steppy]];
}

static void HWR_CacheTextureAsFlat(GLMipmap_t *grMipmap, INT32 texturenum, boolean noencoremap)
{
	UINT8 *flat;
	UINT8 *converted;
	size_t size;
	size_t i;
	UINT8 *colormap = colormaps;

	if (!noencoremap && encoremap)
	{
		colormap += COLORMAP_REMAPOFFSET;
	}

	// setup the texture info
	grMipmap->format = GL_TEXFMT_P_8;
	grMipmap->flags = TF_WRAPXY|TF_CHROMAKEYED;

#ifdef __3DS__
	if (HWR_3DS_FromPack(grMipmap, texturenum, colormap, true))
		return;
#endif
	grMipmap->width  = (UINT16)textures[texturenum]->width;
	grMipmap->height = (UINT16)textures[texturenum]->height;
	size = (grMipmap->width * grMipmap->height);

	flat = (UINT8 *)Z_Malloc(size, PU_HWRCACHE, &grMipmap->data);
#ifdef __3DS__
	if (textures[texturenum]->patchcount == 0)
	{
		// Filler without patches: gray checkerboard (see HWR_GenerateTexture)
		for (i = 0; i < size; i++)
		{
			const size_t x = i % grMipmap->width, y = i / grMipmap->width;
			flat[i] = colormap[(((x >> 3) ^ (y >> 3)) & 1) ? 8 : 12];
		}
		grMipmap->flags &= ~TF_CHROMAKEYED;
		return;
	}
#endif
	converted = (UINT8 *)Picture_TextureToFlat(texturenum);
	for (i = 0; i < size; i++)
	{
		flat[i] = colormap[converted[i]];
	}
	Z_Free(converted);
}

// Download a Doom 'flat' to the hardware cache and make it ready for use
void HWR_GetRawFlat(lumpnum_t flatlumpnum, boolean noencoremap)
{
	RR3DS_REQ(3, (INT32)flatlumpnum, noencoremap, NULL);
	GLMipmap_t *grmip;
	patch_t *patch;

	UINT8 *colormap = colormaps;

	if (flatlumpnum == LUMPERROR)
		return;

	// console crash (v1.0.80, 765 last lap): a lumpnum arrived
	// broken (wad 99) -> wadfiles[99] NULL. Skip instead of crashing.
	if (WADFILENUM(flatlumpnum) >= numwadfiles || !wadfiles[WADFILENUM(flatlumpnum)]
		|| LUMPNUM(flatlumpnum) >= wadfiles[WADFILENUM(flatlumpnum)]->numlumps)
	{
		static int warnings = 0;
		if (warnings < 5) { warnings++; RR_DIAGPRINT("[BADFLAT] lumpnum=%08x invalid, skipping\n", (unsigned)flatlumpnum); }
		return;
	}

	patch = HWR_GetCachedGLPatch(flatlumpnum);
	grmip = ((GLPatch_t *)Patch_AllocateHardwarePatch(patch))->mipmap;

	if (!grmip->colormap)
	{
		if (!noencoremap && encoremap)
			colormap += COLORMAP_REMAPOFFSET;

		grmip->colormap = (GLColormap_t *)Z_Calloc(sizeof(*grmip->colormap), PU_HWRPATCHCOLMIPMAP, NULL);
		grmip->colormap->source = colormap;
		M_Memcpy(grmip->colormap->data, colormap, 256 * sizeof(UINT8));
	}

	if (!grmip->downloaded && !grmip->data)
	{
		HWR_CacheFlat(grmip, flatlumpnum);
	}

	// If hardware does not have the texture, then call pfnSetTexture to upload it
	if (!grmip->downloaded)
		HWD.pfnSetTexture(grmip);
	HWR_SetCurrentTexture(grmip);

	// The system-memory data can be purged now.
	Z_ChangeTag(grmip->data, PU_HWRCACHE_UNLOCKED);
}

void HWR_GetLevelFlat(levelflat_t *levelflat, boolean noencoremap)
{
	// Who knows?
	if (levelflat == NULL)
		return;

	if (levelflat->type == LEVELFLAT_FLAT)
		HWR_GetRawFlat(levelflat->u.flat.lumpnum, noencoremap);
	else if (levelflat->type == LEVELFLAT_TEXTURE)
	{
		GLMapTexture_t *grtex;
		INT32 texturenum = levelflat->u.texture.num;
#ifdef PARANOIA
		if ((unsigned)texturenum >= gl_numtextures)
			I_Error("HWR_GetLevelFlat: texturenum >= numtextures");
#endif

		// Who knows?
		if (texturenum == 0 || texturenum == -1)
			return;

		// Every texture in memory, stored as a 8-bit flat. Wow!
		grtex = GL_FLAT(texturenum);

		// Generate flat if missing from the cache
		if (!grtex->mipmap.data && !grtex->mipmap.downloaded)
		{
			HWR_CacheTextureAsFlat(&grtex->mipmap, texturenum, noencoremap);
		}

		// If hardware does not have the texture, then call pfnSetTexture to upload it
		if (!grtex->mipmap.downloaded)
			HWD.pfnSetTexture(&grtex->mipmap);
		HWR_SetCurrentTexture(&grtex->mipmap);

		// The system-memory data can be purged now.
		HWR_3DS_DropRam(&grtex->mipmap);

		if ((texturenum = R_GetTextureBrightmap(texturenum)))
		{
			// Every texture in memory, stored as a 8-bit flat. Wow!
			grtex = GL_FLAT(texturenum);

			// Generate flat if missing from the cache
			if (!grtex->mipmap.data && !grtex->mipmap.downloaded)
				HWR_CacheTextureAsFlat(&grtex->mipmap, texturenum, true);

			grtex->mipmap.flags |= TF_BRIGHTMAP;

			// If hardware does not have the texture, then call pfnSetTexture to upload it
			if (!grtex->mipmap.downloaded)
				HWD.pfnSetTexture(&grtex->mipmap);
			HWR_SetCurrentTexture(&grtex->mipmap);

			// The system-memory data can be purged now.
			HWR_3DS_DropRam(&grtex->mipmap);
		}
	}
	else if (levelflat->type == LEVELFLAT_PATCH)
	{
		patch_t *patch = (patch_t *)W_CachePatchNum(levelflat->u.flat.lumpnum, PU_CACHE);
		levelflat->width = (UINT16)(patch->width);
		levelflat->height = (UINT16)(patch->height);
		HWR_GetPatch(patch);
	}
#ifndef NO_PNG_LUMPS
	else if (levelflat->type == LEVELFLAT_PNG)
	{
		GLMipmap_t *mipmap = (GLMipmap_t *)levelflat->mipmap;

		// Cache the picture.
		if (!levelflat->mippic)
		{
			INT32 pngwidth = 0, pngheight = 0;
			void *pic = Picture_PNGConvert((const UINT8*)W_CacheLumpNum(levelflat->u.flat.lumpnum, PU_CACHE), PICFMT_FLAT, &pngwidth, &pngheight, NULL, NULL, W_LumpLength(levelflat->u.flat.lumpnum), NULL, (pictureflags_t)0);

			Z_ChangeTag(pic, PU_LEVEL);
			Z_SetUser(pic, &levelflat->mippic);

			levelflat->width = (UINT16)pngwidth;
			levelflat->height = (UINT16)pngheight;
		}

		// Make the mipmap.
		if (mipmap == NULL)
		{
			mipmap = (GLMipmap_t *)Z_Calloc(sizeof(GLMipmap_t), PU_STATIC, NULL);
			mipmap->format = GL_TEXFMT_P_8;
			mipmap->flags = TF_WRAPXY|TF_CHROMAKEYED;
			levelflat->mipmap = mipmap;
		}

		if (!mipmap->data && !mipmap->downloaded)
		{
			UINT8 *flat;
			size_t size;

			if (levelflat->mippic == NULL)
				I_Error("HWR_GetLevelFlat: levelflat->mippic == NULL");

			mipmap->width = levelflat->width;
			mipmap->height = levelflat->height;

			size = (mipmap->width * mipmap->height);
			flat = (UINT8 *)Z_Malloc(size, PU_LEVEL, &mipmap->data);
			M_Memcpy(flat, levelflat->mippic, size);
		}

		// Tell the hardware driver to bind the current texture to the flat's mipmap
		HWR_SetCurrentTexture(mipmap);
	}
#endif
	else // set no texture
		HWR_SetCurrentTexture(NULL);
	// a = current frame of the animated flat (the static cache changes it when drawing)
	RR3DS_REQ(2, levelflat->type == LEVELFLAT_TEXTURE ? levelflat->u.texture.num : (INT32)levelflat->u.flat.lumpnum, noencoremap, levelflat);
}

// --------------------+
// HWR_LoadPatchMipmap : Generates a patch into a mipmap, usually the mipmap inside the patch itself
// --------------------+
#ifdef __3DS__
extern "C" int I_3DS_SprGet(int idx, int* w, int* h, int* fmt); // r_citro3d.cpp 
extern "C" void I_3DS_TexSetSpr(GLMipmap_t* mip, int idx);
#endif
#ifdef __3DS__
// cost of building/uploading sprites during sdraw (mobj = rr3ds_sprmobj != NULL)
UINT64 rr3ds_sprMakeT = 0, rr3ds_sprUpT = 0;
UINT32 rr3ds_sprMakeN = 0, rr3ds_sprUpN = 0, rr3ds_sprMakeCm = 0;
extern mobj_t *rr3ds_sprmobj;
#endif
static void HWR_LoadPatchMipmap(patch_t *patch, GLMipmap_t *grMipmap)
{
	GLPatch_t *grPatch = (GLPatch_t *)patch->hardware;
#ifdef __3DS__
	// sprite without colormap already converted
	// to ETC1A4 -> no RGBA MakeBlock in RAM; the GPU reads it from the file.
	if (!grMipmap->downloaded && !grMipmap->data && grMipmap == grPatch->mipmap)
	{
		int sw, sh, sf;
		const int si = I_3DS_SprGet(patch->rr3ds_spr - 1, &sw, &sh, &sf);
		if (si >= 0)
		{
			HWR_MakePatch(patch, grPatch, grMipmap, false); // sizes and max_s/max_t
			grMipmap->width = (UINT16)sw;
			grMipmap->height = (UINT16)sh;
			Z_Malloc(4, PU_HWRCACHE, &grMipmap->data); // marker (data is in the file)
			grMipmap->flags |= TF_3DS_ETC;
			I_3DS_TexSetSpr(grMipmap, si);
		}
		else
			grMipmap->flags &= ~TF_3DS_ETC; // built as before (RGBA)
	}
#endif
#ifdef __3DS__
	if (rr3ds_sprmobj && !grMipmap->downloaded)
	{
		UINT64 t0 = I_GetPreciseTime();
		if (!grMipmap->data)
		{
			HWR_MakePatch(patch, grPatch, grMipmap, true);
			UINT64 t1 = I_GetPreciseTime();
			rr3ds_sprMakeT += t1 - t0; rr3ds_sprMakeN++;
			if (grMipmap->colormap) rr3ds_sprMakeCm++;
			t0 = t1;
		}
		HWD.pfnSetTexture(grMipmap);
		rr3ds_sprUpT += I_GetPreciseTime() - t0; rr3ds_sprUpN++;
	}
#endif
	if (!grMipmap->downloaded && !grMipmap->data)
		HWR_MakePatch(patch, grPatch, grMipmap, true);

	// If hardware does not have the texture, then call pfnSetTexture to upload it
	if (!grMipmap->downloaded)
		HWD.pfnSetTexture(grMipmap);
	HWR_SetCurrentTexture(grMipmap);

	// The system-memory data can be purged now.
	Z_ChangeTag(grMipmap->data, PU_HWRCACHE_UNLOCKED);
}

// ----------------------+
// HWR_UpdatePatchMipmap : Updates a mipmap.
// ----------------------+
static void HWR_UpdatePatchMipmap(patch_t *patch, GLMipmap_t *grMipmap)
{
	GLPatch_t *grPatch = (GLPatch_t *)patch->hardware;
	HWR_MakePatch(patch, grPatch, grMipmap, true);

	// If hardware does not have the texture, then call pfnSetTexture to upload it
	// If it does have the texture, then call pfnUpdateTexture to update it
	if (!grMipmap->downloaded)
		HWD.pfnSetTexture(grMipmap);
	else
		HWD.pfnUpdateTexture(grMipmap);
	HWR_SetCurrentTexture(grMipmap);

	// The system-memory data can be purged now.
	Z_ChangeTag(grMipmap->data, PU_HWRCACHE_UNLOCKED);
}

// -----------------+
// HWR_GetPatch     : Downloads a patch to the hardware cache and make it ready for use
// -----------------+
void HWR_GetPatch(patch_t *patch)
{
	if (!patch->hardware)
		Patch_CreateGL(patch);
	HWR_LoadPatchMipmap(patch, ((GLPatch_t *)patch->hardware)->mipmap);
}

// -------------------+
// HWR_GetMappedPatch : Same as HWR_GetPatch for sprite color
// -------------------+
mobj_t *rr3ds_sprmobj = NULL; // who requests the sprite with a colormap
static void rr3ds_cmaplog(const UINT8 *colormap, patch_t *patch, int kind)
{
	static const UINT8 *seen[48]; static int nseen = 0; static int nlog = 0;
	for (int i = 0; i < nseen; i++) if (seen[i] == colormap) return;
	if (nseen < 48) seen[nseen++] = colormap;
	if (nlog++ >= 48) return;
	mobj_t *m = rr3ds_sprmobj;
	RR_DIAGPRINT("[CMAP] k%d src=%p cmaps=%p remap=%d %dx%d mobj=%s type=%d spr=%d frame=%x color=%d colorized=%d skin=%d\n",
		kind, (const void *)colormap, (void *)colormaps, colormap == colormaps + COLORMAP_REMAPOFFSET,
		patch->width, patch->height, m ? "si" : "no", m ? (int)m->type : -1, m ? (int)m->sprite : -1,
		m ? (unsigned)m->frame : 0u, m ? (int)m->color : -1, m ? (int)m->colorized : -1,
		(m && m->skin) ? (int)((skin_t *)m->skin)->skinnum : -1);
}
void HWR_GetMappedPatch(patch_t *patch, const UINT8 *colormap)
{
	GLPatch_t *grPatch;
	GLMipmap_t *grMipmap, *newMipmap;

	if (!patch->hardware)
		Patch_CreateGL(patch);
	grPatch = (GLPatch_t *)patch->hardware;

	// Blatant hack for encore colormapping aside...
	if (colormap == colormaps || colormap == NULL || colormap == (const UINT8*)(COLORMAP_REMAPOFFSET))
	{
		// Load the default (green) color in hardware cache
		HWR_GetPatch(patch);
		return;
	}

	// search for the mipmap
	// skip the first (no colormap translated)
	for (grMipmap = grPatch->mipmap; grMipmap->nextcolormap; )
	{
		grMipmap = grMipmap->nextcolormap;
		if (grMipmap->colormap && grMipmap->colormap->source == colormap)
		{
			if (memcmp(grMipmap->colormap->data, colormap, 256 * sizeof(UINT8)))
			{
				rr3ds_cmaplog(colormap, patch, 2);
				M_Memcpy(grMipmap->colormap->data, colormap, 256 * sizeof(UINT8));
				HWR_UpdatePatchMipmap(patch, grMipmap);
			}
			else
				HWR_LoadPatchMipmap(patch, grMipmap);
			return;
		}
	}
	rr3ds_cmaplog(colormap, patch, 1);
	// not found, create it!
	// If we are here, the sprite with the current colormap is not already in hardware memory

	//BP: WARNING: don't free it manually without clearing the cache of harware renderer
	//              (it have a liste of mipmap)
	//    this malloc is cleared in HWR_FreeColormapCache
	//    (...) unfortunately z_malloc fragment alot the memory :(so malloc is better
	newMipmap = (GLMipmap_t *)calloc(1, sizeof (*newMipmap));
	if (newMipmap == NULL)
		I_Error("%s: Out of memory", "HWR_GetMappedPatch");
	grMipmap->nextcolormap = newMipmap;

	newMipmap->colormap = (GLColormap_t *)Z_Calloc(sizeof(*newMipmap->colormap), PU_HWRPATCHCOLMIPMAP, NULL);
	newMipmap->colormap->source = colormap;
	M_Memcpy(newMipmap->colormap->data, colormap, 256 * sizeof(UINT8));

	HWR_LoadPatchMipmap(patch, newMipmap);
}

#ifdef __3DS__
// is the sprite (with this colormap) already on the GPU? (without building or uploading anything)
boolean HWR_MappedPatchReady(patch_t *patch, const UINT8 *colormap)
{
	if (!patch || !patch->hardware)
		return false;
	GLMipmap_t *m = ((GLPatch_t *)patch->hardware)->mipmap;
	if (colormap == colormaps || colormap == NULL || colormap == (const UINT8*)(COLORMAP_REMAPOFFSET))
		return m && m->downloaded;
	for (; m && m->nextcolormap; )
	{
		m = m->nextcolormap;
		if (m->colormap && m->colormap->source == colormap)
			return m->downloaded && !memcmp(m->colormap->data, colormap, 256);
	}
	return false;
}
#endif

void HWR_UnlockCachedPatch(GLPatch_t *gpatch)
{
	if (!gpatch)
		return;

	Z_ChangeTag(gpatch->mipmap->data, PU_HWRCACHE_UNLOCKED);
}

static const INT32 picmode2GR[] =
{
	GL_TEXFMT_P_8,                // PALETTE
	0,                            // INTENSITY          (unsupported yet)
	GL_TEXFMT_ALPHA_INTENSITY_88, // INTENSITY_ALPHA    (corona use this)
	0,                            // RGB24              (unsupported yet)
	GL_TEXFMT_RGBA,               // RGBA32             (opengl only)
};

static void HWR_DrawPicInCache(UINT8 *block, INT32 pblockwidth, INT32 pblockheight,
	INT32 blockmodulo, pic_t *pic, INT32 bpp)
{
	INT32 i,j;
	fixed_t posx, posy, stepx, stepy;
	UINT8 *dest, *src, texel;
	UINT16 texelu16;
	INT32 picbpp;
	RGBA_t col;

	stepy = ((INT32)SHORT(pic->height)<<FRACBITS)/pblockheight;
	stepx = ((INT32)SHORT(pic->width)<<FRACBITS)/pblockwidth;
	picbpp = format2bpp((GLTextureFormat_t)picmode2GR[pic->mode]);
	posy = 0;
	for (j = 0; j < pblockheight; j++)
	{
		posx = 0;
		dest = &block[j*blockmodulo];
		src = &pic->data[(posy>>FRACBITS)*SHORT(pic->width)*picbpp];
		for (i = 0; i < pblockwidth;i++)
		{
			switch (pic->mode)
			{ // source bpp
				case PALETTE :
					texel = src[(posx+FRACUNIT/2)>>FRACBITS];
					switch (bpp)
					{ // destination bpp
						case 1 :
							*dest++ = texel; break;
						case 2 :
							texelu16 = (UINT16)(texel | 0xff00);
							memcpy(dest, &texelu16, sizeof(UINT16));
							dest += sizeof(UINT16);
							break;
						case 3 :
							col = V_GetColor(texel);
							memcpy(dest, &col, sizeof(RGBA_t)-sizeof(UINT8));
							dest += sizeof(RGBA_t)-sizeof(UINT8);
							break;
						case 4 :
							memcpy(dest, &V_GetColor(texel), sizeof(RGBA_t));
							dest += sizeof(RGBA_t);
							break;
					}
					break;
				case INTENSITY :
					*dest++ = src[(posx+FRACUNIT/2)>>FRACBITS];
					break;
				case INTENSITY_ALPHA : // assume dest bpp = 2
					memcpy(dest, src + ((posx+FRACUNIT/2)>>FRACBITS)*sizeof(UINT16), sizeof(UINT16));
					dest += sizeof(UINT16);
					break;
				case RGB24 :
					break;  // not supported yet
				case RGBA32 : // assume dest bpp = 4
					dest += sizeof(UINT32);
					memcpy(dest, src + ((posx+FRACUNIT/2)>>FRACBITS)*sizeof(UINT32), sizeof(UINT32));
					break;
			}
			posx += stepx;
		}
		posy += stepy;
	}
}

// -----------------+
// HWR_GetPic       : Download a Doom pic (raw row encoded with no 'holes')
// Returns          :
// -----------------+
patch_t *HWR_GetPic(lumpnum_t lumpnum)
{
	patch_t *patch = HWR_GetCachedGLPatch(lumpnum);
	GLPatch_t *grPatch = (GLPatch_t *)(patch->hardware);

	if (!grPatch->mipmap->downloaded && !grPatch->mipmap->data)
	{
		pic_t *pic;
		UINT8 *block;
		size_t len;

		pic = (pic_t*)W_CacheLumpNum(lumpnum, PU_CACHE);
		patch->width = SHORT(pic->width);
		patch->height = SHORT(pic->height);
		len = W_LumpLength(lumpnum) - sizeof (pic_t);

		grPatch->mipmap->width = (UINT16)patch->width;
		grPatch->mipmap->height = (UINT16)patch->height;

		if (pic->mode == PALETTE)
			grPatch->mipmap->format = (GLTextureFormat_t)textureformat; // can be set by driver
		else
			grPatch->mipmap->format = (GLTextureFormat_t)picmode2GR[pic->mode];

		Z_Free(grPatch->mipmap->data);

		// allocate block
		block = MakeBlock(grPatch->mipmap);

		if (patch->width  == SHORT(pic->width) &&
			patch->height == SHORT(pic->height) &&
			format2bpp(grPatch->mipmap->format) == format2bpp((GLTextureFormat_t)picmode2GR[pic->mode]))
		{
			// no conversion needed
			M_Memcpy(grPatch->mipmap->data, pic->data,len);
		}
		else
			HWR_DrawPicInCache(block, SHORT(pic->width), SHORT(pic->height),
			                   SHORT(pic->width)*format2bpp(grPatch->mipmap->format),
			                   pic,
			                   format2bpp(grPatch->mipmap->format));

		Z_Unlock(pic);
		Z_ChangeTag(block, PU_HWRCACHE_UNLOCKED);

		grPatch->mipmap->flags = 0;
		grPatch->max_s = grPatch->max_t = 1.0f;
	}
	HWD.pfnSetTexture(grPatch->mipmap);
	//CONS_Debug(DBG_RENDER, "picloaded at %x as texture %d\n",grPatch->mipmap->data, grPatch->mipmap->downloaded);

	return patch;
}

patch_t *HWR_GetCachedGLPatchPwad(UINT16 wadnum, UINT16 lumpnum)
{
	lumpcache_t *lumpcache = wadfiles[wadnum]->patchcache;
	if (!lumpcache[lumpnum])
	{
		patch_t *ptr = (patch_t *)Z_Calloc(sizeof(patch_t), PU_PATCH, &lumpcache[lumpnum]);
		Patch_Create(NULL, 0, ptr);
		Patch_AllocateHardwarePatch(ptr);
	}
	return (patch_t *)(lumpcache[lumpnum]);
}

patch_t *HWR_GetCachedGLPatch(lumpnum_t lumpnum)
{
	return HWR_GetCachedGLPatchPwad(WADFILENUM(lumpnum),LUMPNUM(lumpnum));
}

// Need to do this because they aren't powers of 2
static void HWR_DrawFadeMaskInCache(GLMipmap_t *mipmap, INT32 pblockwidth, INT32 pblockheight,
	lumpnum_t fademasklumpnum, UINT16 fmwidth, UINT16 fmheight)
{
	INT32 i,j;
	fixed_t posx, posy, stepx, stepy;
	UINT8 *block = (UINT8 *)mipmap->data; // places the data directly into here
	UINT8 *flat;
	UINT8 *dest, *src, texel;
	RGBA_t col;

	// Place the flats data into flat
	W_ReadLump(fademasklumpnum, Z_Malloc(W_LumpLength(fademasklumpnum),
		PU_HWRCACHE, &flat));

	stepy = ((INT32)fmheight<<FRACBITS)/pblockheight;
	stepx = ((INT32)fmwidth<<FRACBITS)/pblockwidth;
	posy = 0;
	for (j = 0; j < pblockheight; j++)
	{
		posx = 0;
		dest = &block[j*(mipmap->width)]; // 1bpp
		src = &flat[(posy>>FRACBITS)*SHORT(fmwidth)];
		for (i = 0; i < pblockwidth;i++)
		{
			// fademask bpp is always 1, and is used just for alpha
			texel = src[(posx)>>FRACBITS];
			col = V_GetColor(texel);
			*dest = col.s.red; // take the red level of the colour and use it for alpha, as fademasks do

			dest++;
			posx += stepx;
		}
		posy += stepy;
	}

	Z_Free(flat);
}

static void HWR_CacheFadeMask(GLMipmap_t *grMipmap, lumpnum_t fademasklumpnum)
{
	size_t size;
	UINT16 fmheight = 0, fmwidth = 0;

	// setup the texture info
	grMipmap->format = GL_TEXFMT_ALPHA_8; // put the correct alpha levels straight in so I don't need to convert it later
	grMipmap->flags = 0;

	size = W_LumpLength(fademasklumpnum);

	switch (size)
	{
		// None of these are powers of 2, so I'll need to do what is done for textures and make them powers of 2 before they can be used
		case 256000: // 640x400
			fmwidth = 640;
			fmheight = 400;
			break;
		case 64000: // 320x200
			fmwidth = 320;
			fmheight = 200;
			break;
		case 16000: // 160x100
			fmwidth = 160;
			fmheight = 100;
			break;
		case 4000: // 80x50 (minimum)
			fmwidth = 80;
			fmheight = 50;
			break;
		default: // Bad lump
			CONS_Alert(CONS_WARNING, "Fade mask lump of incorrect size, ignored\n"); // I should avoid this by checking the lumpnum in HWR_RunWipe
			break;
	}

	// Thankfully, this will still work for this scenario
	grMipmap->width  = fmwidth;
	grMipmap->height = fmheight;

	MakeBlock(grMipmap);

	HWR_DrawFadeMaskInCache(grMipmap, fmwidth, fmheight, fademasklumpnum, fmwidth, fmheight);

	// I DO need to convert this because it isn't power of 2 and we need the alpha
}


void HWR_GetFadeMask(lumpnum_t fademasklumpnum)
{
	patch_t *patch = HWR_GetCachedGLPatch(fademasklumpnum);
	GLMipmap_t *grmip = ((GLPatch_t *)Patch_AllocateHardwarePatch(patch))->mipmap;
	if (!grmip->downloaded && !grmip->data)
		HWR_CacheFadeMask(grmip, fademasklumpnum);

	HWD.pfnSetTexture(grmip);

	// The system-memory data can be purged now.
	Z_ChangeTag(grmip->data, PU_HWRCACHE_UNLOCKED);
}

#endif //HWRENDER
