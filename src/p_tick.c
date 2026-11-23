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
/// \file  p_tick.c
/// \brief Archiving: SaveGame I/O, Thinker, Ticker

#include "d_think.h"
#include "rr_config.h"  // single-file configuration
#include "doomstat.h"
#include "d_main.h"
#include "g_game.h"
#include "g_input.h"
#include "p_local.h"
#include "z_zone.h"
#include "s_sound.h"
#include "st_stuff.h"
#include "p_polyobj.h"
#include "m_random.h"
#include "m_cond.h" // gamedata->playtime
#include "lua_script.h"
#include "lua_hook.h"
#include "m_perfstats.h"
#include "i_system.h" // I_GetPreciseTime
#include "i_video.h" // rendermode
#include "r_main.h"
#include "r_fps.h"
#include "d_clisrv.h" // UpdateChallenges
#include "p_link.h"

// Object place
#include "m_cheat.h"

// SRB2Kart
#include "k_kart.h"
#include "k_race.h"
#include "k_battle.h"
#include "k_boss.h"
#include "k_waypoint.h"
#include "k_director.h"
#include "k_specialstage.h"
#include "acs/interface.h"
#include "k_objects.h"
#include "music.h"
#include "k_dialogue.h"
#include "m_easing.h"
#include "k_hud.h" // messagetimer
#include "k_endcam.h"
#include "lua_profile.h"
#include "deh_tables.h" // MOBJTYPE_LIST

tic_t leveltime;
boolean thinkersCompleted;

UINT32 thinker_era = 0;

static boolean g_freezeCheat;
static boolean g_freezeLevel;

boolean P_LevelIsFrozen(void)
{
	return (g_freezeLevel || g_freezeCheat || K_EndCameraIsFreezing());
}

boolean P_FreezeCheat(void)
{
	return (g_freezeLevel || g_freezeCheat || K_EndCameraIsFreezing());
}

void P_SetFreezeCheat(boolean value)
{
	g_freezeCheat = value;
}

void P_SetFreezeLevel(boolean value)
{
	g_freezeLevel = value;
}

boolean P_MobjIsFrozen(mobj_t *mobj)
{
	if (g_freezeCheat == true)
	{
		// freeze cheat
		switch (mobj->type)
		{
			case MT_PLAYER:
			{
				break;
			}
			default:
			{
				return true;
			}
		}
	}

	if (g_freezeLevel == true || K_EndCameraIsFreezing())
	{
		// level totally frozen
		return true;
	}

	if ((mobj->eflags & MFE_PAUSED) == MFE_PAUSED)
	{
		// hitlag
		return true;
	}

	// manual
	return mobj->frozen;
}

INT32 P_AltFlip(INT32 n, tic_t tics)
{
	return leveltime % (2 * tics) < tics ? n : -(n);
}

// Please read p_tick.h
INT32 P_LerpFlip(INT32 n, tic_t tics)
{
	const tic_t w = 2 * tics;

	return P_AltFlip(((leveltime % w) - tics) * n, w);
}

//
// THINKERS
// All thinkers should be allocated by Z_Calloc
// so they can be operated on uniformly.
// The actual structures will vary in size,
// but the first element must be thinker_t.
//

// The entries will behave like both the head and tail of the lists.
thinker_t thlist[NUM_THINKERLISTS];

void Command_Numthinkers_f(void)
{
	INT32 num;
	INT32 count = 0;
	actionf_p1 action;
	thinker_t *think;
	thinklistnum_t start = 0;
	thinklistnum_t end = NUM_THINKERLISTS - 1;
	thinklistnum_t i;

	if (G_GamestateUsesLevel() == false)
	{
		CONS_Printf(M_GetText("You must be in a level to use this.\n"));
		return;
	}

	if (COM_Argc() < 2)
	{
		CONS_Printf(M_GetText("numthinkers <#>: Count number of thinkers\n"));
		CONS_Printf(
			"\t1: P_MobjThinker\n"
			"\t2: P_NullPrecipThinker\n"
			"\t3: T_Friction\n"
			"\t4: T_Pusher\n"
			"\t5: P_RemoveThinkerDelayed\n");
		return;
	}

	num = atoi(COM_Argv(1));

	switch (num)
	{
		case 1:
			start = end = THINK_MOBJ;
			action = (actionf_p1)P_MobjThinker;
			CONS_Printf(M_GetText("Number of %s: "), "P_MobjThinker");
			break;
		case 2:
			start = end = THINK_PRECIP;
			action = (actionf_p1)P_NullPrecipThinker;
			CONS_Printf(M_GetText("Number of %s: "), "P_NullPrecipThinker");
			break;
		case 3:
			start = end = THINK_MAIN;
			action = (actionf_p1)T_Friction;
			CONS_Printf(M_GetText("Number of %s: "), "T_Friction");
			break;
		case 4:
			start = end = THINK_MAIN;
			action = (actionf_p1)T_Pusher;
			CONS_Printf(M_GetText("Number of %s: "), "T_Pusher");
			break;
		case 5:
			action = (actionf_p1)P_RemoveThinkerDelayed;
			CONS_Printf(M_GetText("Number of %s: "), "P_RemoveThinkerDelayed");
			break;
		default:
			CONS_Printf(M_GetText("That is not a valid number.\n"));
			return;
	}

	for (i = start; i <= end; i++)
	{
		for (think = thlist[i].next; think != &thlist[i]; think = think->next)
		{
			if (think->function.acp1 != action)
				continue;

			count++;
		}
	}

	CONS_Printf("%d\n", count);
}

void Command_CountMobjs_f(void)
{
	thinker_t *th;
	mobjtype_t i;
	INT32 count;

	if (G_GamestateUsesLevel() == false)
	{
		CONS_Printf(M_GetText("You must be in a level to use this.\n"));
		return;
	}

	if (COM_Argc() >= 2)
	{
		size_t j;
		for (j = 1; j < COM_Argc(); j++)
		{
			i = atoi(COM_Argv(j));
			if (i >= NUMMOBJTYPES)
			{
				CONS_Printf(M_GetText("Object number %d out of range (max %d).\n"), i, NUMMOBJTYPES-1);
				continue;
			}

			count = 0;

			for (th = thlist[THINK_MOBJ].next; th != &thlist[THINK_MOBJ]; th = th->next)
			{
				if (th->function.acp1 == (actionf_p1)P_RemoveThinkerDelayed)
					continue;

				if (((mobj_t *)th)->type == i)
					count++;
			}

			const char *name;
			if (i >= MT_FIRSTFREESLOT)
				name = FREE_MOBJS[i-MT_FIRSTFREESLOT];
			else
				name = MOBJTYPE_LIST[i];

			CONS_Printf(M_GetText("There are %d objects of type %d (%s) currently in the level.\n"), count, i, name);
		}
		return;
	}

	CONS_Printf(M_GetText("Count of active objects in level:\n"));

	for (i = 0; i < NUMMOBJTYPES; i++)
	{
		count = 0;

		for (th = thlist[THINK_MOBJ].next; th != &thlist[THINK_MOBJ]; th = th->next)
		{
			if (th->function.acp1 == (actionf_p1)P_RemoveThinkerDelayed)
				continue;

			if (((mobj_t *)th)->type == i)
				count++;
		}

		if (count > 0) // Don't bother displaying if there are none of this type!
		{
			const char *name;
			if (i >= MT_FIRSTFREESLOT)
				name = FREE_MOBJS[i-MT_FIRSTFREESLOT];
			else
				name = MOBJTYPE_LIST[i];
			CONS_Printf(" * %d (%s): %d\n", i, name, count);
		}
	}
}

//
// P_InitThinkers
//
void P_InitThinkers(void)
{
	UINT8 i;

	P_InvalidateThinkersWithoutInit();

	for (i = 0; i < NUM_THINKERLISTS; i++)
	{
		thlist[i].prev = thlist[i].next = &thlist[i];
	}

	iquehead = iquetail = 0;

	waypointcap = NULL;
	trackercap = NULL;

	titlemapcam.mobj = NULL;

	for (i = 0; i <= 15; i++)
	{
		skyboxcenterpnts[i] = skyboxviewpnts[i] = NULL;
	}

	P_InitMobjPointers();
}

//
// P_InvalidateThinkersWithoutInit
//

void P_InvalidateThinkersWithoutInit(void)
{
	thinker_era++;
}

// Adds a new thinker at the end of the list.
void P_AddThinker(const thinklistnum_t n, thinker_t *thinker)
{
#ifdef PARANOIA
	I_Assert(n < NUM_THINKERLISTS);
#endif

	thlist[n].prev->next = thinker;
	thinker->next = &thlist[n];
	thinker->prev = thlist[n].prev;
	thlist[n].prev = thinker;

	thinker->references = 0;    // killough 11/98: init reference counter to 0

#ifdef PARANOIA
	thinker->debug_mobjtype = MT_NULL;
#endif
}

#ifdef PARANOIA
static const char *MobjTypeName(const mobj_t *mobj)
{
	actionf_p1 p1 = mobj->thinker.function.acp1;

	if (p1 == (actionf_p1)P_MobjThinker)
	{
		return MOBJTYPE_LIST[mobj->type];
	}
	else if (p1 == (actionf_p1)P_RemoveThinkerDelayed)
	{
		if (mobj->thinker.debug_mobjtype != MT_NULL)
		{
			return MOBJTYPE_LIST[mobj->thinker.debug_mobjtype];
		}
	}

	return "<Not a mobj>";
}

static const char *MobjThinkerName(const mobj_t *mobj)
{
	actionf_p1 p1 = mobj->thinker.function.acp1;

	if (p1 == (actionf_p1)P_MobjThinker)
	{
		return "P_MobjThinker";
	}
	else if (p1 == (actionf_p1)P_RemoveThinkerDelayed)
	{
		return "P_RemoveThinkerDelayed";
	}

	return "<Unknown Thinker>";
}
#endif

//
// killough 11/98:
//
// Make currentthinker external, so that P_RemoveThinkerDelayed
// can adjust currentthinker when thinkers self-remove.

static thinker_t *currentthinker;

//
// P_RemoveThinkerDelayed()
//
// Called automatically as part of the thinker loop in P_RunThinkers(),
// on nodes which are pending deletion.
//
// If this thinker has no more pointers referencing it indirectly,
// remove it, and set currentthinker to one node preceeding it, so
// that the next step in P_RunThinkers() will get its successor.
//
void P_RemoveThinkerDelayed(thinker_t *thinker)
{
	if (thinker->references != 0)
	{
#ifdef PARANOIA
		// Removed mobjs can be the target of another mobj. In
		// that case, the other mobj will manage its reference
		// to the removed mobj in P_MobjThinker. However, if
		// the removed mobj is removed after the other object
		// thinks, the reference management is delayed by one
		// tic (or two?)
		const UINT8 delay = 2;

		if (thinker->debug_time > leveltime)
		{
			thinker->debug_time = leveltime + delay + 1; // do not print errors again
		}
		else if ((thinker->debug_time + delay) <= leveltime)
		{
			CONS_Printf(
					"PARANOIA/P_RemoveThinkerDelayed: %p %s references=%d\n",
					(void*)thinker,
					MobjTypeName((mobj_t*)thinker),
					thinker->references
			);

			thinker->debug_time = leveltime + delay + 1; // do not print this error again
		}
#endif
		return;
	}

	R_DestroyLevelInterpolators(thinker);

	/* Note that currentthinker is guaranteed to point to us,
	* and since we're freeing our memory, we had better change that. So
	* point it to thinker->prev, so the iterator will correctly move on to
	* thinker->prev->next = thinker->next */
	currentthinker = thinker->prev;

	/* Remove from main thinker list */
	P_UnlinkThinker(thinker);
}

//
// P_UnlinkThinker()
//
// Actually removes thinker from the list and frees its memory.
//
void P_UnlinkThinker(thinker_t *thinker)
{
	thinker_t *next = thinker->next;

	I_Assert(thinker->references == 0);

	(next->prev = thinker->prev)->next = next;
	if (thinker->alloctype == TAT_LEVELPOOL)
	{
		Z_LevelPoolFree(thinker, thinker->size);
	}
	else
	{
		Z_Free(thinker);
	}
}

//
// P_RemoveThinker
//
// Deallocation is lazy -- it will not actually be freed
// until its thinking turn comes up.
//
// killough 4/25/98:
//
// Instead of marking the function with -1 value cast to a function pointer,
// set the function to P_RemoveThinkerDelayed(), so that later, it will be
// removed automatically as part of the thinker process.
//
void P_RemoveThinker(thinker_t *thinker)
{
	LUA_InvalidateUserdata(thinker);
	thinker->function.acp1 = (actionf_p1)P_RemoveThinkerDelayed;
}

/*
 * P_SetTarget
 *
 * This function is used to keep track of pointer references to mobj thinkers.
 * In Doom, objects such as lost souls could sometimes be removed despite
 * their still being referenced. In Boom, 'target' mobj fields were tested
 * during each gametic, and any objects pointed to by them would be prevented
 * from being removed. But this was incomplete, and was slow (every mobj was
 * checked during every gametic). Now, we keep a count of the number of
 * references, and delay removal until the count is 0.
 */

mobj_t *P_SetTarget2(mobj_t **mop, mobj_t *targ
#ifdef PARANOIA
		, const char *source_file, int source_line
#endif
)
{
	if (*mop) // If there was a target already, decrease its refcount
	{
		(*mop)->thinker.references--;

#ifdef PARANOIA
		if ((*mop)->thinker.references < 0)
		{
			CONS_Printf(
					"PARANOIA/P_SetTarget: %p %s %s references=%d, references go negative! (%s:%d)\n",
					(void*)*mop,
					MobjTypeName(*mop),
					MobjThinkerName(*mop),
					(*mop)->thinker.references,
					source_file,
					source_line
			);
		}

		(*mop)->thinker.debug_time = leveltime;
#endif
	}

	if (targ != NULL) // Set new target and if non-NULL, increase its counter
	{
		targ->thinker.references++;

#ifdef PARANOIA
		targ->thinker.debug_time = leveltime;
#endif
	}

	*mop = targ;

	return targ;
}

//
// P_RunThinkers
//
// killough 4/25/98:
//
// Fix deallocator to stop using "next" pointer after node has been freed
// (a Doom bug).
//
// Process each thinker. For thinkers which are marked deleted, we must
// load the "next" pointer prior to freeing the node. In Doom, the "next"
// pointer was loaded AFTER the thinker was freed, which could have caused
// crashes.
//
// But if we are not deleting the thinker, we should reload the "next"
// pointer after calling the function, in case additional thinkers are
// added at the end of the list.
//
// killough 11/98:
//
// Rewritten to delete nodes implicitly, by making currentthinker
// external and using P_RemoveThinkerDelayed() implicitly.
//
#ifdef __3DS__
// Per-mobj-type profile: time and count per MT_ in the THINK_MOBJ list.
static UINT64 rr3ds_mtTime[NUMMOBJTYPES];
static UINT32 rr3ds_mtCount[NUMMOBJTYPES];
// breakdown of MT_THOK by state (who spawns them)
#define RR3DS_THOKSLOTS 24
static INT32 rr3ds_thokState[RR3DS_THOKSLOTS];
static UINT64 rr3ds_thokTime[RR3DS_THOKSLOTS];
static UINT32 rr3ds_thokCount[RR3DS_THOKSLOTS];
static void rr3ds_ThokAdd(INT32 st, UINT64 dt)
{
	int i;
	for (i = 0; i < RR3DS_THOKSLOTS; i++)
	{
		if (rr3ds_thokCount[i] == 0) rr3ds_thokState[i] = st;
		if (rr3ds_thokState[i] == st) { rr3ds_thokTime[i] += dt; rr3ds_thokCount[i]++; return; }
	}
}
int RR3DS_DiagOn(void); // hw_main.cpp 
// MK7: counters defined in p_mobj.c
extern UINT32 rr3ds_mk7_ringFast, rr3ds_mk7_floatSkip, rr3ds_mk7_scenSkip,
	rr3ds_mk7_capsNoLink, rr3ds_mk7_tripSkip;
static void rr3ds_RunMobjListProfiled(void)
{
	for (currentthinker = thlist[THINK_MOBJ].next; currentthinker != &thlist[THINK_MOBJ]; currentthinker = currentthinker->next)
	{
		if (currentthinker->rr3ds_sleep
			&& currentthinker->function.acp1 == (actionf_p1)P_MobjThinker
			&& ((leveltime + (UINT32)((uintptr_t)currentthinker >> 6)) & 31)) /* check 1 of 32 (before 8) */
		{
			rr3ds_ringSlept++; // sleeping ring
			continue;
		}
		if (currentthinker->function.acp1 == (actionf_p1)P_MobjThinker)
		{
			const mobjtype_t t = ((mobj_t *)currentthinker)->type;
			const INT32 st = (t == MT_THOK && ((mobj_t *)currentthinker)->state)
				? (INT32)(((mobj_t *)currentthinker)->state - states) : -1;
			const UINT64 t0 = I_GetPreciseTime();
			currentthinker->function.acp1(currentthinker);
			if ((unsigned)t < NUMMOBJTYPES)
			{
				if (st >= 0) rr3ds_ThokAdd(st, I_GetPreciseTime() - t0);
				rr3ds_mtTime[t] += I_GetPreciseTime() - t0;
				rr3ds_mtCount[t]++;
			}
		}
		else
			currentthinker->function.acp1(currentthinker);
	}
}

static void rr3ds_ReportMobjTypes(UINT32 ntics)
{
	enum { TOP = 45 }; // 15 -> 45 (see the tail, not just the first ones)
	int best[TOP]; int k, j; size_t t;
	const double us = 1000000.0 / (double)I_GetPrecisePrecision() / ntics;
	UINT32 total = 0;
	for (k = 0; k < TOP; k++) best[k] = -1;
	for (t = 0; t < NUMMOBJTYPES; t++)
	{
		total += rr3ds_mtCount[t];
		if (!rr3ds_mtTime[t]) continue;
		for (k = 0; k < TOP; k++)
		{
			if (best[k] < 0 || rr3ds_mtTime[t] > rr3ds_mtTime[best[k]])
			{
				for (j = TOP - 1; j > k; j--) best[j] = best[j - 1];
				best[k] = (int)t;
				break;
			}
		}
	}
	CONS_Printf("[MT] top types (us/tic, count/tic) | mobjs/tic=%u\n", (unsigned)(total / ntics));
	for (k = 0; k < TOP && best[k] >= 0; k++)
	{
		// MK7: print the type number (MOBJTYPE_LIST seems misaligned with info.h)
		// and doomednum/spawnstate to identify it without trusting the name.
		const char *nm = (best[k] >= 0 && best[k] < NUMMOBJTYPES) ? MOBJTYPE_LIST[best[k]] : NULL;
		CONS_Printf("[MT]  #%-4d %-26s dn=%-5d st=%-5d fl=%08lx %6.0f us  %5u\n", best[k],
			nm ? nm : "?", (int)mobjinfo[best[k]].doomednum, (int)mobjinfo[best[k]].spawnstate, (unsigned long)mobjinfo[best[k]].flags,
			rr3ds_mtTime[best[k]] * us, (unsigned)(rr3ds_mtCount[best[k]] / ntics));
	}
	for (k = 0; k < RR3DS_THOKSLOTS && rr3ds_thokCount[k]; k++)
		if (rr3ds_thokTime[k] * us >= 100.0)
			CONS_Printf("[THOK] state=%ld %6.0f us %5u\n", (long)rr3ds_thokState[k],
				rr3ds_thokTime[k] * us, (unsigned)(rr3ds_thokCount[k] / ntics));
	{ // [CP] types that request P_CheckPosition the most
		extern UINT32 rr3ds_cpCount[NUMMOBJTYPES];
		extern UINT32 rr3ds_cheapMoveN;
		int n;
		CONS_Printf("[CP] cheap_move=%u per tic\n", (unsigned)(rr3ds_cheapMoveN / ntics));
		rr3ds_cheapMoveN = 0;
		for (n = 0; n < 14; n++)
		{
			int bt = -1;
			for (t = 0; t < NUMMOBJTYPES; t++)
				if (rr3ds_cpCount[t] && (bt < 0 || rr3ds_cpCount[t] > rr3ds_cpCount[bt])) bt = (int)t;
			if (bt < 0) break;
			CONS_Printf("[CP]  #%-4d %-26s %6.1f per tic\n", bt, MOBJTYPE_LIST[bt], (double)rr3ds_cpCount[bt] / (double)ntics);
			rr3ds_cpCount[bt] = 0;
		}
		memset(rr3ds_cpCount, 0, sizeof rr3ds_cpCount);
	}
	memset(rr3ds_thokCount, 0, sizeof rr3ds_thokCount);
	memset(rr3ds_thokTime, 0, sizeof rr3ds_thokTime);
	memset(rr3ds_mtTime, 0, sizeof rr3ds_mtTime);
	memset(rr3ds_mtCount, 0, sizeof rr3ds_mtCount);
}
#endif

static void P_RunThinkers(void)
{
	size_t i;

#ifdef RR_LOWEND_CODE
	{
		static int wasRead = 0;
		if (!wasRead) { wasRead = 1; rr3ds_ringSleepOn = RRCfg_Int("sleeping_rings", 1); rr3ds_farSleepOn = RRCfg_Int("far_sleep", 1); { extern int rr3ds_flingNoCol; rr3ds_flingNoCol = RRCfg_Int("fling_nocol", 1); } }
	}
#endif
	for (i = 0; i < NUM_ACTIVETHINKERLISTS; i++)
	{
		ps_thlist_times[i] = I_GetPreciseTime();
#ifdef __3DS__
		if (i == THINK_MOBJ && RR3DS_DiagOn()) // per-mobj profile ONLY with diagnostics (it cost 1.5-4 ms per tic)
		{
			rr3ds_RunMobjListProfiled();
			ps_thlist_times[i] = I_GetPreciseTime() - ps_thlist_times[i];
			continue;
		}
#endif
#ifdef RR_LOWEND_CODE
		if (i == THINK_MOBJ && RR_LOWEND) // on PC only in low-end mode
		{
			// sleeping rings (data only): skipped without calling the thinker.
			// 1 of every 8 tics (staggered) they are checked anyway for safety.
			for (currentthinker = thlist[i].next; currentthinker != &thlist[i]; currentthinker = currentthinker->next)
			{
				if (currentthinker->rr3ds_sleep
					&& currentthinker->function.acp1 == (actionf_p1)P_MobjThinker
					&& ((leveltime + (UINT32)((uintptr_t)currentthinker >> 6)) & 31)) /* check 1 of 32 (before 8) */
				{
					rr3ds_ringSlept++;
#ifndef __3DS__
					RR3DS_RingSyncFrame((mobj_t *)currentthinker); // PC: Software does not do this when drawing
#endif
					continue;
				}
				currentthinker->function.acp1(currentthinker);
			}
			ps_thlist_times[i] = I_GetPreciseTime() - ps_thlist_times[i];
			continue;
		}
#endif
		for (currentthinker = thlist[i].next; currentthinker != &thlist[i]; currentthinker = currentthinker->next)
		{
#ifdef PARANOIA
			I_Assert(currentthinker->function.acp1 != NULL);
#endif
			currentthinker->function.acp1(currentthinker);
		}
		ps_thlist_times[i] = I_GetPreciseTime() - ps_thlist_times[i];
	}

	if (gametyperules & GTR_CIRCUIT)
		K_RunFinishLineBeam();

	if (gametyperules & GTR_PAPERITEMS)
		K_RunPaperItemSpawners();

	if ((gametyperules & GTR_OVERTIME) && battleovertime.enabled)
		K_RunBattleOvertime();

	ps_acs_time = I_GetPreciseTime();
	ACS_Tick();
	ps_acs_time = I_GetPreciseTime() - ps_acs_time;
}

static inline void P_DeviceRumbleTick(void)
{
	UINT8 i;

	for (i = 0; i <= splitscreen; i++)
	{
		player_t *player = &players[g_localplayers[i]];
		UINT16 low = 0;
		UINT16 high = 0;

		if (player->mo != NULL && !player->exiting)
		{
			if ((player->mo->eflags & MFE_DAMAGEHITLAG) && player->mo->hitlag)
			{
				low = high = 65536 / 2;
			}
			else if (player->sneakertimer > (sneakertime-(TICRATE/2)) || player->panelsneakertimer > (sneakertime-(TICRATE/2)) || player->weaksneakertimer > (sneakertime-(TICRATE/2)))
			{
				low = high = 65536 / (3+player->numsneakers+player->numpanelsneakers+player->numweaksneakers);
			}
			else if (((player->boostpower < FRACUNIT) || (player->stairjank > 8))
				&& P_IsObjectOnGround(player->mo) && player->speed != 0)
			{
				low = high = 65536 / 32;
			}
		}

		G_PlayerDeviceRumble(i, low, high);
	}
}

void P_RunChaseCameras(void)
{
	UINT8 i;

	for (i = 0; i <= r_splitscreen; i++)
	{
		if (camera[i].chase)
		{
			player_t *p = &players[displayplayers[i]];
			camera_t *cam = &camera[i];

			if (p->mo && p->cmd.throwdir != 0)
			{
				if (p->speed < 6 * p->mo->scale && abs(cam->dpad_y_held) < 2*TICRATE)
					cam->dpad_y_held += intsign(p->cmd.throwdir);
			}
			else
				cam->dpad_y_held = 0;

			P_MoveChaseCamera(p, cam, false);
		}
	}

	K_EndCameraGC();
}

static fixed_t P_GetDarkness(tic_t start, tic_t end)
{
	if (leveltime <= end)
	{
		tic_t fade = end - DARKNESS_FADE_TIME;
		tic_t t;

		if (leveltime < fade) // dark or darkening
		{
			t = leveltime - start;
			t = min(t, DARKNESS_FADE_TIME);
		}
		else // lightening
		{
			t = end - leveltime;
		}

		return Easing_Linear((t * FRACUNIT) / DARKNESS_FADE_TIME, 0,
			(gametyperules & GTR_BUMPERS) ? FRACUNIT/3 : FRACUNIT/7);
	}

	return 0;
}

static void P_TickDarkness(void)
{
	const fixed_t globalValue = P_GetDarkness(g_darkness.start, g_darkness.end);
	UINT8 i;

	for (i = 0; i <= r_splitscreen; ++i)
	{
		const player_t *p = &players[displayplayers[i]];
		fixed_t value = P_GetDarkness(p->darkness_start, p->darkness_end);

		g_darkness.value[i] = value ? value : globalValue;
	}
}

static void P_TickMusicFade(void)
{
	if (leveltime >= g_musicfade.start && leveltime <= g_musicfade.end)
	{
		INT32 half = (g_musicfade.end - g_musicfade.start) / 2;
		INT32 fade = max(1, g_musicfade.fade);
		INT32 mid = half - fade;
		INT32 t = abs((INT32)leveltime - (INT32)(g_musicfade.start + half));
		Music_LevelVolume((max(t, mid) - mid) * 100 / fade);
	}
	else if (!g_musicfade.ticked)
		Music_LevelVolume(100);

	g_musicfade.ticked = true;
}

static void P_StartLevelMusic(void)
{
	if (!Music_Playing("level_nosync"))
	{
		// Do not stop level_nosync
		Music_Play(P_UseContinuousLevelMusic() ? "level_nosync" : "level");
	}
}

#ifdef __3DS__
// ==========================================================================
// 3DS: BENCH MODE. If sdmc:/3ds/rr_bench exists the game starts by itself
// on the map and the camera drives the track following the waypoints (no
// kart physics). After completing 1 lap it writes [BENCH] and exits.
// ==========================================================================
boolean rr3ds_bench = false;
static waypoint_t *rr_bwp = NULL, *rr_bfinish = NULL;
static fixed_t rr_bdist = 0;
static UINT32 rr_btics = 0;
static size_t rr_bframe0 = 0;
static precise_t rr_bt0 = 0;
static boolean rr_bstarted = false;

// [WORST]: worst case per map for the per-tic ms BUDGET.
// Only collects data while the bench is running. 1 ms histograms (cap 1023).
// tic = whole P_Ticker (logic); frame = a full pass of D_SRB2Loop; draw = D_Display.
// worst_sec = worst average of 35 consecutive tics (1 s of gameplay): the sustained value, not an isolated spike.
#define RR_PH 1024
static UINT16 rr_ph_tic[RR_PH], rr_ph_frm[RR_PH], rr_ph_dib[RR_PH];
static double rr_pt_sum, rr_pt_max, rr_pf_sum, rr_pf_max, rr_pd_sum, rr_pd_max;
static UINT32 rr_pt_n, rr_pf_n;
static double rr_pw[TICRATE], rr_pw_sum, rr_pw_max; static UINT32 rr_pw_i;
static UINT32 rr_pm_max, rr_pm_sum, rr_pm_n;
static precise_t rr_p_last; // last partial [WORST]

static void RR3DS_WorstReset(void)
{
	memset(rr_ph_tic, 0, sizeof rr_ph_tic); memset(rr_ph_frm, 0, sizeof rr_ph_frm); memset(rr_ph_dib, 0, sizeof rr_ph_dib);
	rr_pt_sum = rr_pt_max = rr_pf_sum = rr_pf_max = rr_pd_sum = rr_pd_max = 0;
	rr_pt_n = rr_pf_n = 0;
	memset(rr_pw, 0, sizeof rr_pw); rr_pw_sum = rr_pw_max = 0; rr_pw_i = 0;
	rr_pm_max = rr_pm_sum = rr_pm_n = 0;
	rr_p_last = I_GetPreciseTime();
}

static void rr_ph_add(UINT16 *h, double ms)
{
	int b = (int)ms;
	if (b < 0) b = 0;
	if (b >= RR_PH) b = RR_PH - 1;
	if (h[b] < 0xFFFF) h[b]++;
}

static int rr_ph_pct(const UINT16 *h, UINT32 n, int pct)
{
	UINT32 meta = (UINT32)(((UINT64)n * pct + 99) / 100), acc = 0;
	int b;
	if (!n) return 0;
	for (b = 0; b < RR_PH; b++) { acc += h[b]; if (acc >= meta) return b + 1; }
	return RR_PH;
}

static void RR3DS_WorstPrint(const char *que)
{
	CONS_Printf("[WORST] %s map=%s tics=%u tic_ms avg=%.1f p95=%d p99=%d max=%.1f worst_sec=%.1f"
		" | frame_ms avg=%.1f p95=%d max=%.1f | draw_ms avg=%.1f p95=%d max=%.1f | mobjs avg=%u max=%u\n",
		que, G_BuildMapName(gamemap), (unsigned)rr_pt_n,
		rr_pt_n ? rr_pt_sum / rr_pt_n : 0.0, rr_ph_pct(rr_ph_tic, rr_pt_n, 95), rr_ph_pct(rr_ph_tic, rr_pt_n, 99), rr_pt_max,
		rr_pw_max / TICRATE,
		rr_pf_n ? rr_pf_sum / rr_pf_n : 0.0, rr_ph_pct(rr_ph_frm, rr_pf_n, 95), rr_pf_max,
		rr_pf_n ? rr_pd_sum / rr_pf_n : 0.0, rr_ph_pct(rr_ph_dib, rr_pf_n, 95), rr_pd_max,
		rr_pm_n ? rr_pm_sum / rr_pm_n : 0, rr_pm_max);
}

// Called at the end of P_Ticker (only if run)
static void RR3DS_WorstTic(double ms)
{
	if (!rr3ds_bench || !rr_bstarted) return;
	rr_ph_add(rr_ph_tic, ms);
	rr_pt_sum += ms; rr_pt_n++;
	if (ms > rr_pt_max) rr_pt_max = ms;
	rr_pw_sum += ms - rr_pw[rr_pw_i]; rr_pw[rr_pw_i] = ms;
	rr_pw_i = (rr_pw_i + 1) % TICRATE;
	if (rr_pt_n >= TICRATE && rr_pw_sum > rr_pw_max) rr_pw_max = rr_pw_sum;
	if (rr_pt_n % TICRATE == 0)
	{
		// how many mobjs there are (once per game second, cheap)
		thinker_t *th; UINT32 c = 0;
		for (th = thlist[THINK_MOBJ].next; th != &thlist[THINK_MOBJ]; th = th->next)
			if (th->function.acp1 != (actionf_p1)P_RemoveThinkerDelayed) c++;
		rr_pm_sum += c; rr_pm_n++;
		if (c > rr_pm_max) rr_pm_max = c;
	}
	// partial every ~30 real seconds (if Azahar hangs or is slow, the script uses the latest)
	if ((double)(I_GetPreciseTime() - rr_p_last) / (double)I_GetPrecisePrecision() > 30.0)
	{
		rr_p_last = I_GetPreciseTime();
		RR3DS_WorstPrint("partial");
	}
}

// Called from D_SRB2Loop (d_main.cpp) for every complete frame
void RR3DS_WorstFrame(double total_ms, double draw_ms)
{
	if (!rr3ds_bench || !rr_bstarted) return;
	rr_ph_add(rr_ph_frm, total_ms); rr_ph_add(rr_ph_dib, draw_ms);
	rr_pf_sum += total_ms; rr_pd_sum += draw_ms; rr_pf_n++;
	if (total_ms > rr_pf_max) rr_pf_max = total_ms;
	if (draw_ms > rr_pd_max) rr_pd_max = draw_ms;
}

// 7 s wait standing still (black zone card) and N consecutive races
// (sdmc:/3ds/RingRacers3ds/rr_laps, default 1) before closing.
static UINT32 rr_bwait = 0;
static int rr_benchLaps = 0, rr_benchDone = 0;

static void RR3DS_BenchTick(void)
{
	player_t *p = &players[consoleplayer];
	mobj_t *mo = p->mo;
	waypoint_t *next;
	fixed_t ax, ay, az, bx, by, bz, seg, t;

	if (!rr3ds_bench || gamestate != GS_LEVEL || !mo || P_MobjWasRemoved(mo))
		return;

	if (!rr_bstarted)
	{
		if (rr_benchLaps == 0)
		{
			rr_benchLaps = RRCfg_Int("laps", 1); // rr_config.txt
			if (rr_benchLaps < 1) rr_benchLaps = 1;
			if (rr_benchLaps > 10) rr_benchLaps = 10;
		}
		if (rr_bwait < 7 * TICRATE)
		{
			rr_bwait++; // standing still while the zone card shows
			mo->momx = mo->momy = mo->momz = 0;
			return;
		}
		rr_bfinish = K_GetFinishLineWaypoint();
		if (!rr_bfinish || !rr_bfinish->mobj || rr_bfinish->numnextwaypoints == 0)
			return;
		rr_bwp = rr_bfinish;
		rr_bdist = 0;
		rr_btics = 0;
		rr_bframe0 = framecount;
		rr_bt0 = I_GetPreciseTime(); { extern UINT32 rr3ds_dbg_real, rr3ds_dbg_cap; rr3ds_dbg_real = 0; rr3ds_dbg_cap = 0; }
		RR3DS_WorstReset();
		rr_bstarted = true;
		CONS_Printf("[BENCH] start on %s (race %d of %d)\n", G_BuildMapName(gamemap), rr_benchDone + 1, rr_benchLaps);
	}

	// in special stages the path can break (waypoint without a next
	// or without a mobj) -> before it read NULL (Unmapped Read in Azahar). Now it ends the bench.
	if (!rr_bwp || !rr_bwp->mobj || rr_bwp->numnextwaypoints == 0 || !rr_bwp->nextwaypoints)
		goto bench_fin;
	next = rr_bwp->nextwaypoints[0];
	if (!next || !next->mobj)
		goto bench_fin;

	ax = rr_bwp->mobj->x; ay = rr_bwp->mobj->y; az = rr_bwp->mobj->z;
	bx = next->mobj->x;   by = next->mobj->y;   bz = next->mobj->z;
	seg = R_PointToDist2(ax, ay, bx, by);

	rr_bdist += 48 * mapobjectscale; // ~race speed
	while (seg > 0 && rr_bdist >= seg)
	{
		rr_bdist -= seg;
		rr_bwp = next;
		if (rr_bwp == rr_bfinish || rr_btics > 35*240)
		{
bench_fin:
			{
			const double secs = (double)(I_GetPreciseTime() - rr_bt0) / (double)I_GetPrecisePrecision();
			const UINT32 frames = (UINT32)(framecount - rr_bframe0);
			CONS_Printf("[BENCH] end: tics=%u frames=%u seconds=%.1f avg_fps=%.2f ms_per_frame=%.1f\n",
				(unsigned)rr_btics, (unsigned)frames, secs, frames / secs, secs * 1000.0 / (frames ? frames : 1));
			{ extern UINT32 rr3ds_dbg_real, rr3ds_dbg_cap; CONS_Printf("[TICLOSS] tics_requested=%u frames>8tics=%u\n", (unsigned)rr3ds_dbg_real, (unsigned)rr3ds_dbg_cap); }
			RR3DS_WorstPrint("fin");
			{ extern void RR3DS_PreSave(void); RR3DS_PreSave(); }
			rr_benchDone++;
			if (rr_benchDone < rr_benchLaps && rr_bfinish && rr_bfinish->mobj)
			{
				// another unfinished race: go back to the finish line and measure again
				rr_bstarted = false;
				return;
			}
			rr3ds_bench = false;
			I_Quit();
			return;
			}
		}
		if (!rr_bwp->mobj || rr_bwp->numnextwaypoints == 0 || !rr_bwp->nextwaypoints
			|| !rr_bwp->nextwaypoints[0] || !rr_bwp->nextwaypoints[0]->mobj)
		{
			CONS_Printf("[BENCH] waypoint path cut; ending here\n");
			goto bench_fin;
		}
		next = rr_bwp->nextwaypoints[0];
		ax = rr_bwp->mobj->x; ay = rr_bwp->mobj->y; az = rr_bwp->mobj->z;
		bx = next->mobj->x;   by = next->mobj->y;   bz = next->mobj->z;
		seg = R_PointToDist2(ax, ay, bx, by);
	}

	t = seg > 0 ? FixedDiv(rr_bdist, seg) : 0;
	{
		const fixed_t x = ax + FixedMul(bx - ax, t);
		const fixed_t y = ay + FixedMul(by - ay, t);
		const fixed_t z = az + FixedMul(bz - az, t);
		const angle_t ang = R_PointToAngle2(ax, ay, bx, by);
		if (rr_btics == 0) P_SetOrigin(mo, x, y, z);
		else P_MoveOrigin(mo, x, y, z);
		mo->momx = mo->momy = mo->momz = 0;
		mo->angle = ang;
		p->angleturn = ang;
		p->drawangle = ang;
	}
	rr_btics++;
}

// OFF (manual) mode. You play; when the race starts a fixed item is given;
// measured from the start until 1 lap is completed, [BENCH] is written and it exits.
#include <strings.h>
boolean rr3ds_benchmanual = false;
static SINT8 rr_bitem = KITEM_SNEAKER;
static UINT8 rr_bitemamt = 1;
static boolean rr_bgave = false;

void RR3DS_BenchSetItem(const char *name)
{
	static const struct { const char *n; SINT8 v; } tbl[] = {
#define FOREACH( name, n ) { #name, n }
		KART_ITEM_ITERATOR,
#undef FOREACH
	};
	size_t i;
	for (i = 0; i < sizeof(tbl)/sizeof(tbl[0]); i++)
	{
		if (!strcasecmp(tbl[i].n, name))
		{
			rr_bitem = tbl[i].v;
			return;
		}
	}
	CONS_Printf("[BENCH] unknown item '%s', using SNEAKER\n", name);
	rr_bitem = KITEM_SNEAKER;
}

static void RR3DS_BenchManualTick(void)
{
	player_t *p = &players[consoleplayer];
	mobj_t *mo = p->mo;

	if (!rr3ds_benchmanual || gamestate != GS_LEVEL || !mo || P_MobjWasRemoved(mo))
		return;

	// fixed item at race start
	if (!rr_bgave && leveltime >= starttime)
	{
		if (rr_bitem > KITEM_NONE)
		{
			p->itemtype = rr_bitem;
			p->itemamount = rr_bitemamt;
		}
		rr_bgave = true;
	}

	if (!rr_bstarted)
	{
		if (leveltime < starttime)
			return;
		rr_btics = 0;
		rr_bframe0 = framecount;
		rr_bt0 = I_GetPreciseTime(); { extern UINT32 rr3ds_dbg_real, rr3ds_dbg_cap; rr3ds_dbg_real = 0; rr3ds_dbg_cap = 0; }
		rr_bstarted = true;
		CONS_Printf("[BENCH] manual start on %s\n", G_BuildMapName(gamemap));
	}

	rr_btics++;

	// end: complete the 1st lap (laps becomes 2) or finish the race
	if (p->laps >= 2 || p->exiting)
	{
		const double secs = (double)(I_GetPreciseTime() - rr_bt0) / (double)I_GetPrecisePrecision();
		const UINT32 frames = (UINT32)(framecount - rr_bframe0);
		CONS_Printf("[BENCH] end (manual): tics=%u frames=%u seconds=%.1f avg_fps=%.2f ms_per_frame=%.1f\n",
			(unsigned)rr_btics, (unsigned)frames, secs, frames / secs, secs * 1000.0 / (frames ? frames : 1));
		rr3ds_benchmanual = false;
		I_Quit();
	}
}
#endif

//
// P_Ticker
//
#ifdef __3DS__
// time per P_Ticker segment (us/tic in the [TIC] seg=)
static precise_t rr_seg[9], rr_segLast;
#define RR_SEG(k) do { precise_t n_ = I_GetPreciseTime(); rr_seg[k] += n_ - rr_segLast; rr_segLast = n_; } while (0)
#else
#define RR_SEG(k) ((void)0)
#endif

#ifdef __3DS__
UINT64 rr3ds_m_ptick = 0; // [METER]: time inside P_Ticker (read/zeroed in d_main.cpp)
#endif

void P_Ticker(boolean run)
{
	quake_t *quake = NULL;
	INT32 i;
#ifdef __3DS__
	const precise_t rr_pt0 = I_GetPreciseTime(); // total P_Ticker time
#endif

	thinkersCompleted = false;
#ifdef __3DS__
	rr_segLast = rr_pt0;
#endif

	// Increment jointime and quittime even if paused
	for (i = 0; i < MAXPLAYERS; i++)
	{
		if (playeringame[i])
		{
			players[i].jointime++;
		}
	}

	if (objectplacing)
	{
		if (OP_FreezeObjectplace())
		{
			P_MapStart();
			R_UpdateMobjInterpolators();
			OP_ObjectplaceMovement(&players[0]);
			P_MoveChaseCamera(&players[0], &camera[0], false);
			R_UpdateViewInterpolation();
			P_MapEnd();
			return;
		}
	}

	// Check for pause or menu up in single player
	if (paused || P_AutoPause())
	{
		P_RunChaseCameras(); // special case: allow freecam to MOVE during pause!
		return;
	}

	for (i = 0; i <= r_splitscreen; i++)
		postimgtype[i] = postimg_none;

	P_MapStart();

	if (run)
	{
		R_UpdateMobjInterpolators();

		if (demo.recording)
		{
			if (!G_ConsiderEndingDemoWrite())
			{
				G_WriteDemoExtraData();
				for (i = 0; i < MAXPLAYERS; i++)
					if (playeringame[i])
						G_WriteDemoTiccmd(&players[i].cmd, i);
			}
		}
		if (demo.playback && !demo.waitingfortally)
		{
			if (!G_ConsiderEndingDemoRead())
			{
				G_ReadDemoExtraData();
				for (i = 0; i < MAXPLAYERS; i++)
					if (playeringame[i])
						G_ReadDemoTiccmd(&players[i].cmd, i);
			}
		}

		LUA_ResetTicTimers();

		ps_lua_mobjhooks = 0;
		ps_checkposition_calls = 0;

		LUA_HOOK(PreThinkFrame);

		ps_playerthink_time = I_GetPreciseTime();

		RR_SEG(0);
		K_UpdateAllPlayerPositions();
		RR_SEG(1);

		// OK! Now that we got all of that sorted, players can think!
		for (i = 0; i < MAXPLAYERS; i++)
		{
			if (!(playeringame[i] && players[i].mo && !P_MobjWasRemoved(players[i].mo)))
				continue;
			P_PlayerThink(&players[i]);
			K_KartPlayerHUDUpdate(&players[i]);
		}

		ps_playerthink_time = I_GetPreciseTime() - ps_playerthink_time;
		RR_SEG(2);

		if (gamedata && gamestate == GS_LEVEL && !demo.playback)
		{
			mapheader_t *mapheader;

			mapheader = mapheaderinfo[gamemap - 1];

			// Keep track of how long they've been playing!
			gamedata->totalplaytime++;

			// Map playtime
			if (mapheader)
			{
				mapheader->records.timeplayed++;
			}

			// Netgame total time
			if (netgame)
			{
				gamedata->totalnetgametime++;

				if (mapheader)
				{
					mapheader->records.netgametimeplayed++;
				}
			}

			// Per-skin total playtime for all machine-local players
			for (i = 0; i < MAXPLAYERS; i++)
			{
				skin_t *playerskin;

				if (!P_IsMachineLocalPlayer(&players[i]))
				{
					continue;
				}

				if (!(playeringame[i] && players[i].mo && !P_MobjWasRemoved(players[i].mo)))
				{
					continue;
				}

				if (players[i].skin >= numskins)
				{
					continue;
				}

				playerskin = skins[players[i].skin];

				playerskin->records.timeplayed++;
			}

			if (gametype != GT_TUTORIAL)
			{
				INT32 mode = M_GameDataGameType(gametype, battleprisons);

				// Gamedata mode playtime
				if (mode >= 0 && mode < GDGT_MAX)
				{
					gamedata->modeplaytime[mode]++;
					if (mapheader)
					{
						mapheader->records.modetimeplayed[mode]++;
					}
				}

				// Attacking mode playtime
				if (modeattacking != ATTACKING_NONE)
				{
					if (encoremode) // ((modeattacking & ATTACKING_SPB) != 0)
					{
						gamedata->spbattackingtotaltime++;
						if (mapheader)
						{
							mapheader->records.spbattacktimeplayed++;
						}
					}
					//else
					{
						gamedata->timeattackingtotaltime++;
						if (mapheader)
						{
							mapheader->records.timeattacktimeplayed++;
						}
					}
				}

				// Per-skin mode playtime
				for (i = 0; i < MAXPLAYERS; i++)
				{
					skin_t *playerskin;

					if (!P_IsMachineLocalPlayer(&players[i]))
					{
						continue;
					}

					if (!(playeringame[i] && players[i].mo && !P_MobjWasRemoved(players[i].mo)))
					{
						continue;
					}

					if (players[i].skin >= numskins)
					{
						continue;
					}

					playerskin = skins[players[i].skin];
					playerskin->records.modetimeplayed[mode]++;
				}
			}

			// TODO would this be laggy with more conditions in play...
			if (
				(leveltime > introtime
					&& M_UpdateUnlockablesAndExtraEmblems(true, false))
				|| gamedata->deferredsave
			)
			{
#ifdef __3DS__
				// saving mid-race = a ~2 s stall on the 3DS.
				// It is flagged and saved when the map finishes (G_DoCompleted / exit to menu).
				gamedata->deferredsave = true;
#else
				G_SaveGameData();
#endif
			}
		}
	}

	if (run)
	{
		RR_SEG(2);
		ps_thinkertime = I_GetPreciseTime();
		P_RunThinkers();
		ps_thinkertime = I_GetPreciseTime() - ps_thinkertime;
		thinkersCompleted = true;

		// Run any "after all the other thinkers" stuff
		{
			player_t *finishingPlayers[MAXPLAYERS];
			UINT8 numingame = 0, numFinishingPlayers = 0;

			for (i = 0; i < MAXPLAYERS; i++)
			{
				if (playeringame[i] && players[i].mo && !P_MobjWasRemoved(players[i].mo))
				{
					P_PlayerAfterThink(&players[i]);

					if (players[i].spectator == true)
						continue;

					numingame++;

					// Check for the number of ties for first place after every player has thunk run for this tic
					if (players[i].exiting == 1 && players[i].position == 1 &&
							(players[i].pflags & (PF_HITFINISHLINE|PF_NOCONTEST)) == PF_HITFINISHLINE)
					{
						finishingPlayers[numFinishingPlayers++] = &players[i];
					}
				}
			}

			if ((netgame) // Antigrief is supposed to apply?
				&& !(K_Cooperative() || timelimitintics > 0 || g_pointlimit > 0) // There are rules that will punish a griefing player
				&& (gametyperules & GTR_CIRCUIT) && (leveltime > starttime) && K_GetNumWaypoints()) // The following only detects race griefing
			{
				for (i = 0; i < MAXPLAYERS; i++)
				{
					if (playeringame[i] && players[i].mo && !P_MobjWasRemoved(players[i].mo))
					{
						if (players[i].spectator == true)
							continue;

						if (players[i].exiting || (players[i].pflags & PF_NOCONTEST))
							continue;

						if (players[i].bot == true)
							continue;

						P_CheckRaceGriefing(&players[i], (numingame > 1));
					}
				}
			}

			// Apply rumble to local players
			if (!demo.playback)
			{
				P_DeviceRumbleTick();
			}

			if (numFinishingPlayers > 1)
			{
				for (i = 0; i < numFinishingPlayers; i++)
				{
					P_SetupSignExit(finishingPlayers[i], true);
				}
			}
			else if (numFinishingPlayers == 1)
			{
				P_SetupSignExit(finishingPlayers[0], false);
			}
		}

		if (musiccountdown > 0)
		{
			// Music is controlled by completion sequence
		}
		else if (K_CheckBossIntro() == true)
		{
			// Bosses have a punchy start, so no position.
			if (leveltime == 1)
			{
				P_StartLevelMusic();
			}
		}
		else if (leveltime < starttime + TICRATE)
		{
			if (leveltime == (starttime + (TICRATE/2)))
			{
				// Plays the music after the starting countdown.
				P_StartLevelMusic();
			}
			else if (starttime != introtime)
			{
				// Start countdown/music handling
				if (leveltime == starttime-(3*TICRATE))
				{
					S_StartSound(NULL, sfx_s3ka7); // 3,
				}
				else if ((leveltime == starttime-(2*TICRATE))
					|| (leveltime == starttime-TICRATE))
				{
					S_StartSound(NULL, sfx_s3ka7); // 2, 1,
				}
				else if (leveltime == starttime)
				{
					S_StartSound(NULL, sfx_s3kad); // GO!
				}

				// POSITION!! music
				if (modeattacking == ATTACKING_NONE || !P_UseContinuousLevelMusic())
				{
					P_StartPositionMusic(true); // exact times only
				}
			}
		}

		if (modeattacking != ATTACKING_NONE && P_UseContinuousLevelMusic())
		{
			if (leveltime == 4)
			{
				P_StartLevelMusic();
			}
		}

		ps_lua_thinkframe_time = I_GetPreciseTime();
		LUA_HookThinkFrame();
		ps_lua_thinkframe_time = I_GetPreciseTime() - ps_lua_thinkframe_time;
	}

	if (run)
		UpdateChallenges();
	RR_SEG(4);

	// Run shield positioning
	P_RunOverlays();
	RR_SEG(5);

	P_UpdateSpecials();
	RR_SEG(6);
	P_RespawnSpecials();
	RR_SEG(7);

	// Lightning, rain sounds, etc.
	P_PrecipitationEffects();
	RR_SEG(8);

#ifdef __3DS__
	// Logic profile: average per tic every 175 tics.
	if (run)
	{
		static precise_t acc[NUM_ACTIVETHINKERLISTS], accPlayer, accThink, accAcs, accLua, accPtick;
		static UINT32 accCheckpos, accHooks, n;
		size_t li;
		for (li = 0; li < NUM_ACTIVETHINKERLISTS; li++) acc[li] += ps_thlist_times[li];
		accPlayer += ps_playerthink_time; accThink += ps_thinkertime;
		accAcs += ps_acs_time; accLua += ps_lua_thinkframe_time; accPtick += I_GetPreciseTime() - rr_pt0;
		RR3DS_WorstTic((double)(I_GetPreciseTime() - rr_pt0) * 1000.0 / (double)I_GetPrecisePrecision());
		rr3ds_m_ptick += I_GetPreciseTime() - rr_pt0; // [METER]
		accCheckpos += ps_checkposition_calls; accHooks += ps_lua_mobjhooks;
		if (++n >= 175)
		{
			const double us = 1000000.0 / (double)I_GetPrecisePrecision() / n;
			char buf[160]; size_t o = 0;
			for (li = 0; li < NUM_ACTIVETHINKERLISTS && o < sizeof buf - 16; li++)
				o += snprintf(buf + o, sizeof buf - o, " L%u=%.0f", (unsigned)li, acc[li] * us);
			CONS_Printf("[TIC] us/tic: players=%.0f thinkers=%.0f (%s ) acs=%.0f luaframe=%.0f | checkpos=%u luahooks=%u ptick=%.0f per tic | seg pre=%.0f post=%.0f play+gd=%.0f (n/a=%.0f) thk+post+chal=%.0f ovl=%.0f upd=%.0f resp=%.0f precip=%.0f\n",
				accPlayer * us, accThink * us, buf, accAcs * us, accLua * us,
				(unsigned)(accCheckpos / n), (unsigned)(accHooks / n), accPtick * us,
					rr_seg[0] * us, rr_seg[1] * us, rr_seg[2] * us, rr_seg[3] * us, rr_seg[4] * us, rr_seg[5] * us, rr_seg[6] * us, rr_seg[7] * us, rr_seg[8] * us);
				memset(rr_seg, 0, sizeof rr_seg);
			if (RR3DS_DiagOn()) rr3ds_ReportMobjTypes(n);
#ifdef RR_PROF
			{ extern void RR3DS_ProfReport(unsigned); RR3DS_ProfReport(n); }
#endif
			// MK7: per-tic averages of the shortcuts (diagnostics only)
			if (RR3DS_DiagOn())
				CONS_Printf("[MK7] per tic: far=%u asleep=%u rings=%u float=%u scenery=%u capsule=%u tripwire=%u\n",
					(unsigned)(rr3ds_farSlept / n), (unsigned)(rr3ds_ringSlept / n), (unsigned)(rr3ds_mk7_ringFast / n), (unsigned)(rr3ds_mk7_floatSkip / n),
					(unsigned)(rr3ds_mk7_scenSkip / n), (unsigned)(rr3ds_mk7_capsNoLink / n),
					(unsigned)(rr3ds_mk7_tripSkip / n));
			rr3ds_ringSlept = rr3ds_farSlept = 0;
			rr3ds_mk7_ringFast = rr3ds_mk7_floatSkip = rr3ds_mk7_scenSkip = 0;
			rr3ds_mk7_capsNoLink = rr3ds_mk7_tripSkip = 0;
			memset(acc, 0, sizeof acc);
			accPlayer = accThink = accAcs = accLua = accPtick = 0; accCheckpos = accHooks = n = 0;
		}
	}
#endif
#ifdef __3DS__
	// [BOTS] every 5 s (diag=1): speed, distance to finish and "stuck" counters of each bot.
	if (run && gamestate == GS_LEVEL && (leveltime % 175) == 0 && leveltime > starttime && RRCfg_On("diag"))
	{
		for (INT32 bi = 0; bi < MAXPLAYERS; bi++)
		{
			const player_t *bp = &players[bi];
			if (!playeringame[bi] || !bp->bot || !bp->mo || bp->spectator) continue;
			RR_DIAGPRINT("[BOTS] t=%u p%d pos=%d vel=%d dist=%u respconf=%u turnconf=%d spinconf=%u buttons=%x\n",
				(unsigned)leveltime, (int)bi, (int)bp->position,
				(int)(FixedHypot(bp->mo->momx, bp->mo->momy) >> FRACBITS), (unsigned)bp->distancetofinish,
				(unsigned)bp->botvars.respawnconfirm, (int)bp->botvars.turnconfirm, (unsigned)bp->botvars.spindashconfirm,
				(unsigned)bp->cmd.buttons);
		}
	}
#endif
	if (run)
	{
		leveltime++;

		if (starttime > introtime && leveltime == starttime)
		{
			ACS_RunPositionScript();
		}

		if ((gametyperules & GTR_OVERTIME) && !battleprisons &&
			timelimitintics > 0 && leveltime == (timelimitintics + starttime + 1))
		{
			ACS_RunOvertimeScript();
		}
	}

	if (g_fast_forward == 0 || demo.simplerewind)
	{
		timeinmap++;
	}

	if (run)
	{
		if (racecountdown > 1)
			racecountdown--;

		P_TickDarkness();
		P_TickMusicFade();

		if (exitcountdown >= 1)
		{
			boolean run_exit_countdown = true;
			for (i = 0; i < MAXPLAYERS; i++)
			{
				if (playeringame[i] == false)
				{
					continue;
				}

				player_t *player = &players[i];
				if (player->spectator == false && K_PlayerTallyActive(player) == true && player->tally.done == false)
				{
					run_exit_countdown = false;
					break;
				}
			}

			if (run_exit_countdown == true)
			{
				if (exitcountdown > 1)
				{
					exitcountdown--;
				}

				if (exitcountdown == 1)
				{
					if (demo.playback)
						G_FinishExitLevel();
					else if (server)
						SendNetXCmd(XD_EXITLEVEL, NULL, 0);
				}
			}
		}

		K_RunItemCooldowns();

		K_BossInfoTicker();

		K_TickSpecialStage();

		if ((gametyperules & GTR_POINTLIMIT))
		{
			if (wantedcalcdelay && --wantedcalcdelay <= 0)
				K_CalculateBattleWanted();
		}

		if (bombflashtimer)
			bombflashtimer--;	// Bomb seizure prevention

		// Tick quake effects
		quake = g_quakes;
		while (quake != NULL)
		{
			if (quake->time <= 0)
			{
				// Time out, remove this effect
				quake_t *remove = quake;
				quake = quake->next;
				P_FreeQuake(remove);
				continue;
			}

			quake->time--;

			if (quake->epicenter != NULL && quake->mobj != NULL && P_MobjWasRemoved(quake->mobj) == false)
			{
				quake->epicenter->x = quake->mobj->x;
				quake->epicenter->y = quake->mobj->y;
				quake->epicenter->z = quake->mobj->z + (quake->mobj->height / 2);
			}

			quake = quake->next;
		}

		if (demo.recording)
		{
			if (cv_recordmultiplayerdemos.value && demo.savebutton && demo.savebutton + 3*TICRATE < leveltime)
				G_CheckDemoTitleEntry();

			if (!G_ConsiderEndingDemoWrite())
			{
				G_WriteAllGhostTics();
			}
		}
		else if (demo.playback && !demo.waitingfortally)
		{
			if (!G_ConsiderEndingDemoRead())
			{
				// Use Ghost data for consistency checks.
				G_ConsAllGhostTics();
			}
		}

		if (modeattacking)
		{
			if (!demo.waitingfortally)
				G_GhostTicker();
			if (!demo.playback)
				G_TickTimeStickerMedals();
		}

		if (mapreset > 1
			&& --mapreset <= 1
			&& server) // Remember: server uses it for mapchange, but EVERYONE ticks down for the animation
				D_MapChange(gamemap, gametype, encoremode, true, 0, false, false);

		if (cv_kartdebugwaypoints.value != 0)
		{
			K_DebugWaypointsVisualise();
		}
	}

#ifdef __3DS__
	if (run && rr3ds_bench)
		RR3DS_BenchTick();
	if (run && rr3ds_benchmanual)
		RR3DS_BenchManualTick();
#endif
	if (gamestate == GS_LEVEL)
	{
		// Move the camera during levels.
		K_UpdateDirector();
		P_RunChaseCameras();
#ifdef RR_LOWEND_CODE
		if (run && RR_LOWEND && !levelloading)
			RR3DS_PrecipFollow(); // rain only near the cameras (p_mobj.c)
#endif
	}

	if (run && !levelloading && leveltime)
	{
		K_TickDialogue();
		K_TickMessages();
	}

	if (run)
	{
		LUA_HOOK(PostThinkFrame);

		R_UpdateLevelInterpolators();

		// Hack: ensure newview is assigned every tic.
		// Ensures view interpolation is T-1 to T in poor network conditions
		// We need a better way to assign view state decoupled from game logic
		if (rendermode != render_none)
		{
			for (i = 0; i <= r_splitscreen; i++)
			{
				player_t *player = &players[displayplayers[i]];
				if (!player->mo)
					continue;
				const boolean skybox = (player->skybox.viewpoint && cv_skybox.value); // True if there's a skybox object and skyboxes are on
				if (skybox)
				{
					R_SkyboxFrame(i);
				}
				R_SetupFrame(i);
			}
		}
	}

	P_MapEnd();

	for (i = 0; i < MAXPLAYERS; i++)
	{
		G_CopyTiccmd(&players[i].oldcmd, &players[i].cmd, 1);
	}

	if (D_IsDeferredStartTitle())
	{
		D_StartTitle();
	}
//	Z_CheckMemCleanup();
}
