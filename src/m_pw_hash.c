// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2025 by Kart Krew.
// Copyright (C) 2025 by James Robert Roman.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------

#include "modp_b64/modp_b64.h"
#include "monocypher/monocypher.h"

#include "doomdef.h"
#include "m_pw_hash.h"
#ifdef __3DS__
#include "z_zone.h"
#include "i_system.h"
extern void *RR3DS_ArenaAllocBig(size_t n); // z_zone.cpp: NULL if no contiguous block
extern void RR3DS_ArenaFree(void *p);
#endif

UINT8 *M_HashPassword(UINT8 hash[M_PW_HASH_SIZE], const char *key, const UINT8 salt[M_PW_SALT_SIZE])
{
	size_t memory = 8*1024*1024;
#ifdef __3DS__
	// 3DS: the C heap is tiny (the zone arena owns almost all RAM), so the 8 MB
	// Argon2 work buffer must come from the zone. In-level free arena is too small:
	// then the hash fails (the password menu is only usable outside a race).
	void *int_buf = RR3DS_ArenaAllocBig(memory);
	if (!int_buf)
	{
		CONS_Printf("Password hash: not enough free memory (8 MB needed)\n");
		return NULL;
	}
#else
	void *int_buf = malloc(memory);
	if (!int_buf)
		return NULL;
#endif

	// https://monocypher.org/manual/argon2
	crypto_argon2_config config = {
		CRYPTO_ARGON2_ID, // algorithm
		memory / 1024, // nb_blocks
		2, // nb_passes
		1, // nb_lanes
	};

	crypto_argon2_inputs inputs = {
		(const UINT8*)key, // pass
		salt, // salt
		strlen(key), // pass_size
		M_PW_SALT_SIZE, // salt_size
	};

	crypto_argon2_extras extras = {
		NULL, // key
		NULL, // ad
		0, // key_size
		0, // ad_size
	};

#ifdef __3DS__
	const precise_t rr_t0 = I_GetPreciseTime();
#endif
	crypto_argon2(hash, M_PW_HASH_SIZE, int_buf, config, inputs, extras);
#ifdef __3DS__
	CONS_Printf("[PASSWORD] Argon2 8 MB: %u ms\n",(unsigned)((I_GetPreciseTime() - rr_t0) * 1000 / I_GetPrecisePrecision()));
#endif
#ifdef __3DS__
	RR3DS_ArenaFree(int_buf);
#else
	free(int_buf);
#endif
	return hash;
}
