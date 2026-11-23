// per-function profiler (only with RR_PROF=1 at cmake configure time).
// : measures SELF time per function
// inside P_Ticker. The numbers are inflated (every call pays for the hook), but
// they are good enough for ranking. Printed as [PROF] addr ... and resolved with
// tools\prof_resolver.py.
#ifdef RR_PROF
#include <3ds.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../doomtype.h"
#include "../command.h"
#define NF __attribute__((no_instrument_function))
#define PROF_HASH 4096
#define PROF_DEPTH 256
typedef struct { void *fn; u32 calls; u64 self; } pe_t;
typedef struct { void *fn; u64 t0; u64 child; } pf_t;
static pe_t tab[PROF_HASH];
static pf_t stk[PROF_DEPTH];
static int sp = 0, active = 0, over = 0;
extern void P_Ticker(boolean run);
#ifdef RR_PROFD
// DRAW profile (RR_PROFD=1): the root is D_Display instead of P_Ticker
extern void *RR3DS_DisplayFn(void);
#endif

NF static pe_t *find(void *fn)
{
	u32 h = ((u32)(uintptr_t)fn >> 2) * 2654435761u >> 20; // 12 bits
	for (u32 i = 0; i < PROF_HASH; i++)
	{
		pe_t *e = &tab[(h + i) & (PROF_HASH - 1)];
		if (!e->fn) { e->fn = fn; return e; }
		if (e->fn == fn) return e;
	}
	return &tab[0];
}
NF void __cyg_profile_func_enter(void *fn, void *site)
{
	(void)site;
	if (!active)
	{
#ifdef RR_PROFD
		if (fn != RR3DS_DisplayFn()) return;
#else
		if (fn != (void *)P_Ticker) return;
#endif
		active = 1; sp = 0; over = 0;
	}
	if (sp >= PROF_DEPTH) { over++; return; }
	stk[sp].fn = fn; stk[sp].child = 0; stk[sp].t0 = svcGetSystemTick(); sp++;
}
NF void __cyg_profile_func_exit(void *fn, void *site)
{
	(void)site;
	if (!active) return;
	if (over) { over--; return; }
	if (sp <= 0) return;
	u64 now = svcGetSystemTick();
	pf_t *f = &stk[--sp];
	u64 total = now - f->t0, self = total - f->child;
	pe_t *e = find(f->fn);
	e->calls++; e->self += self;
	if (sp > 0) stk[sp - 1].child += total;
	if (sp == 0) active = 0;
}
// Prints the top 40 by self time (us per tic) and resets. n = accumulated tics.
NF static int cmp_self(const void *a, const void *b)
{
	u64 x = tab[*(const int *)a].self, y = tab[*(const int *)b].self;
	return x < y ? 1 : (x > y ? -1 : 0);
}
NF void RR3DS_ProfReport(unsigned n)
{
	static int idx[PROF_HASH];
	int saved = active, nt = 0;
	active = 0;
	for (int i = 0; i < PROF_HASH; i++) if (tab[i].fn) idx[nt++] = i;
	qsort(idx, nt, sizeof idx[0], cmp_self);
	for (int j = 0; j < nt && j < 40; j++)
	{
		pe_t *e = &tab[idx[j]];
		CONS_Printf("[PROF] addr=%08x us/tic=%.0f calls/tic=%.1f\n",(unsigned)(uintptr_t)e->fn,
			(double)e->self * 1000000.0 / 268111856.0 / n, (double)e->calls / n);
	}
	memset(tab, 0, sizeof tab);
	active = saved;
}
#endif
