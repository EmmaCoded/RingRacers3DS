// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2025 by Sally "TehRealSalt" Cochenour
// Copyright (C) 2025 by Kart Krew
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  k_brightmap.c
/// \brief Brightmap texture loading.

#include "k_brightmap.h"

#include "doomdata.h"
#include "doomdef.h"
#include "doomtype.h"
#include "fastcmp.h"
#include "r_textures.h"
#include "w_wad.h"
#include "z_zone.h"

static brightmapStorage_t *brightmapStorage = NULL;
static size_t maxBrightmapStorage = 0;
// Startup speed: the storage used to grow by 1 entry per Z_Realloc (copying the whole array each time)
// and every texture was looked up with a linear scan = quadratic. On the 3DS that was ~10 s of boot.
// Now: capacity doubling + a hash index (open addressing; stores index+1, 0 = empty).
static size_t capBrightmapStorage = 0;
static UINT32 *brightmapIndex = NULL;
static size_t brightmapIndexMask = 0;

static void K_BrightmapIndexInsert(size_t idx)
{
	size_t h = brightmapStorage[idx].textureHash & brightmapIndexMask;
	while (brightmapIndex[h] != 0)
		h = (h + 1) & brightmapIndexMask;
	brightmapIndex[h] = (UINT32)(idx + 1);
}

static void K_BrightmapIndexRebuild(void)
{
	size_t size = 1024, i;
	while (size < capBrightmapStorage * 2)
		size <<= 1;
	free(brightmapIndex);
	brightmapIndex = (UINT32 *)calloc(size, sizeof(UINT32));
	brightmapIndexMask = size - 1;
	if (brightmapIndex == NULL)
		return;
	for (i = 0; i < maxBrightmapStorage; i++)
		K_BrightmapIndexInsert(i);
}

/*--------------------------------------------------
	static brightmapStorage_t *K_NewBrightmap(void)

		Increases the size of maxBrightmapStorage by 1.

	Input Arguments:-
		None

	Return:-
		The new brightmap storage struct.
--------------------------------------------------*/
static brightmapStorage_t *K_NewBrightmap(void)
{
	maxBrightmapStorage++;
	if (maxBrightmapStorage + 1 > capBrightmapStorage)
	{
		capBrightmapStorage = (capBrightmapStorage == 0) ? 256 : capBrightmapStorage * 2;
		brightmapStorage = (brightmapStorage_t *)Z_Realloc(brightmapStorage, sizeof(brightmapStorage_t) * (capBrightmapStorage + 1), PU_STATIC, &brightmapStorage);
		// the entries already stored are valid, the new one (index max-1) is not hashed yet
		maxBrightmapStorage--;
		K_BrightmapIndexRebuild();
		maxBrightmapStorage++;
	}
	return &brightmapStorage[ maxBrightmapStorage - 1 ];
}

/*--------------------------------------------------
	static brightmapStorage_t *K_GetBrightmapStorageByIndex(size_t checkIndex)

		See header file for description.
--------------------------------------------------*/
static brightmapStorage_t *K_GetBrightmapStorageByIndex(size_t checkIndex)
{
	if (checkIndex >= maxBrightmapStorage)
	{
		return NULL;
	}

	return &brightmapStorage[checkIndex];
}

/*--------------------------------------------------
	static brightmapStorage_t *K_GetBrightmapStorageByTextureName(const char *checkName)

		See header file for description.
--------------------------------------------------*/
static brightmapStorage_t *K_GetBrightmapStorageByTextureName(const char *checkName)
{
	UINT32 checkHash = quickncasehash(checkName, 8);
	size_t i;

	if (maxBrightmapStorage == 0)
	{
		return NULL;
	}

	if (brightmapIndex != NULL)
	{
		size_t h = checkHash & brightmapIndexMask;
		while (brightmapIndex[h] != 0)
		{
			brightmapStorage_t *bms = &brightmapStorage[brightmapIndex[h] - 1];
			if (checkHash == bms->textureHash && !strncmp(checkName, bms->textureName, 8))
				return bms;
			h = (h + 1) & brightmapIndexMask;
		}
		return NULL;
	}

	for (i = 0; i < maxBrightmapStorage; i++)
	{
		brightmapStorage_t *bms = &brightmapStorage[i];

		if (checkHash == bms->textureHash && !strncmp(checkName, bms->textureName, 8))
		{
			// Name matches.
			return bms;
		}
	}

	return NULL;
}

/*--------------------------------------------------
	static boolean K_BRIGHTLumpParser(char *data, size_t size)

		Parses inputted lump data as a BRIGHT lump.

	Input Arguments:-
		data - Pointer to lump data.
		size - The length of the lump data.

	Return:-
		false if any errors occured, otherwise true.
--------------------------------------------------*/
static boolean K_BRIGHTLumpParser(char *data, size_t size)
{
	char *tkn = M_GetToken((char *)data);
	size_t pos = 0;

	while (tkn && (pos = M_GetTokenPos()) < size)
	{
		boolean valid = true;

		if (stricmp(tkn, "texture") == 0)
		{
			Z_Free(tkn);
			tkn = M_GetToken(NULL);
			pos = M_GetTokenPos();

			if (tkn && pos <= size)
			{
				brightmapStorage_t *bms = K_GetBrightmapStorageByTextureName(tkn);

				if (bms == NULL)
				{
					bms = K_NewBrightmap();
					strncpy(bms->textureName, tkn, 8);
					bms->textureHash = quickncasehash(tkn, 8);
					if (brightmapIndex != NULL)
						K_BrightmapIndexInsert(maxBrightmapStorage - 1);
				}

				Z_Free(tkn);
				tkn = M_GetToken(NULL);
				pos = M_GetTokenPos();

				if (tkn && pos <= size)
				{
					strncpy(bms->brightmapName, tkn, 8);
					bms->brightmapHash = quickncasehash(tkn, 8);
				}
				else
				{
					CONS_Alert(CONS_ERROR, "No brightmap for brightmap definition.\n");
					valid = false;
				}
			}
			else
			{
				CONS_Alert(CONS_ERROR, "No texture for brightmap definition.\n");
				valid = false;
			}
		}
		// todo: SPRITE brightmaps?!
		else
		{
			CONS_Alert(CONS_ERROR, "Unknown keyword '%s' found in BRIGHT lump.\n", tkn);
			valid = false;
		}

		Z_Free(tkn);

		if (valid == false)
		{
			return false;
		}

		tkn = M_GetToken(NULL);
	}

	Z_Free(tkn);
	return true;
}

/*--------------------------------------------------
	void K_InitBrightmapsPwad(INT32 wadNum)

		See header file for description.
--------------------------------------------------*/
void K_InitBrightmapsPwad(INT32 wadNum)
{
	UINT16 lumpNum;
	size_t i;

	I_Assert(brightmapStorage == NULL);

	// Find BRIGHT lump in the WAD
	lumpNum = W_CheckNumForNamePwad("BRIGHT", wadNum, 0);

	while (lumpNum != INT16_MAX)
	{
		UINT8 *data = (UINT8 *)W_CacheLumpNumPwad(wadNum, lumpNum, PU_CACHE);

		if (data != NULL)
		{
			lumpinfo_t *lump_p = &wadfiles[wadNum]->lumpinfo[lumpNum];
			size_t size = W_LumpLengthPwad(wadNum, lumpNum);
			char *datacopy;

			size_t nameLength = strlen(wadfiles[wadNum]->filename) + 1 + strlen(lump_p->fullname); // length of file name, '|', and lump name
			char *name = malloc(nameLength + 1);

			sprintf(name, "%s|%s", wadfiles[wadNum]->filename, lump_p->fullname);
			name[nameLength] = '\0';

			size = W_LumpLengthPwad(wadNum, lumpNum);

			CONS_Printf(M_GetText("Loading BRIGHT from %s\n"), name);

			datacopy = (char *)Z_Malloc((size+1)*sizeof(char),PU_STATIC,NULL);
			memmove(datacopy,data,size);
			datacopy[size] = '\0';

			Z_Free(data);

			K_BRIGHTLumpParser(datacopy, size);

			Z_Free(datacopy);

			free(name);
		}

		lumpNum = W_CheckNumForNamePwad("BRIGHT", (UINT16)wadNum, lumpNum + 1);
	}

	if (maxBrightmapStorage == 0)
	{
		// No brightmaps were defined.
		return;
	}

	for (i = 0; i < maxBrightmapStorage; i++)
	{
		brightmapStorage_t *bms = K_GetBrightmapStorageByIndex(i);
		INT32 texNum, bmNum;

		if (bms == NULL)
		{
			// Shouldn't happen.
			break;
		}

		texNum = R_CheckTextureNumForName(bms->textureName);
		if (texNum != -1)
		{
			bmNum = R_CheckTextureNumForName(bms->brightmapName);
			R_UpdateTextureBrightmap(texNum, (bmNum == -1 ? 0 : bmNum));
		}
	}

	R_ClearTextureNumCache(false);

	// Clear brightmapStorage now that we're done with it.
	Z_Free(brightmapStorage);
	brightmapStorage = NULL;
	maxBrightmapStorage = 0;
	capBrightmapStorage = 0;
	free(brightmapIndex);
	brightmapIndex = NULL;
	brightmapIndexMask = 0;
}

/*--------------------------------------------------
	void K_InitBrightmaps(void)

		See header file for description.
--------------------------------------------------*/
void K_InitBrightmaps(void)
{
	INT32 wadNum;

	for (wadNum = 0; wadNum < numwadfiles; wadNum++)
	{
		K_InitBrightmapsPwad(wadNum);
	}
}
