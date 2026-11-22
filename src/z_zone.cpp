// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 2025 by Kart Krew.
// Copyright (C) 2020 by Sonic Team Junior.
// Copyright (C) 2006 by Graue.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  z_zone.cpp
/// \brief Zone memory allocation.
///        This file does zone memory allocation. Each allocation is done with a
///        tag, and this file keeps track of all the allocations made. Later, you
///        can purge everything with a given tag.
///
///        Some tags (PU_CACHE, for example) may be automatically purged whenever
///        the space is needed, so memory allocated with these tags is no longer
///        guaranteed to be valid after another call to Z_Malloc().
///
///        The original implementation allocated a large block (48 MB, as of the
///        last version of SRB2 that did this) upfront, and Z_Malloc() carved
///        pieces out of that. Unfortunately, this had the effect of masking a
///        lot of read/write past end of buffer type bugs which we have since
///        caught with this direct-malloc version. We also suspected that SRB2's
///        allocator was fragmenting badly. Finally, this version is a bit
///        simpler (about half the lines of code).

#include <stddef.h>
#include <string.h>
#include <stdalign.h>

#include <tracy/tracy/TracyC.h>

#include "core/memory.h"
#include "doomdef.h"
#include "doomstat.h"
#include "r_patch.h"
#include "r_picformats.h"
#include "i_system.h" // I_GetFreeMem
#include "i_video.h" // rendermode
#include "z_zone.h"
#ifdef __3DS__
#include "rr_config.h" // zone_arena_mb
#endif
#include "m_misc.h" // M_Memcpy
#include "lua_script.h"

#ifdef HWRENDER
#include "hardware/hw_main.h" // For hardware memory info
#endif

#ifdef HAVE_VALGRIND
#include "valgrind.h"
static boolean Z_calloc = false;
#include "memcheck.h"
#endif

#define ZONEID 0xa441d13d

typedef struct memblock_s
{
	void **user;
	INT32 tag; // purgelevel
	UINT32 id; // Should be ZONEID

	size_t size; // including the header and blocks
	size_t realsize; // size of real data only

	const char *ownerfile;
	INT32 ownerline;

	struct memblock_s *next, *prev;
} memblock_t;

#define ALIGNPAD (((sizeof (memblock_t) + (alignof (max_align_t) - 1)) & ~(alignof (max_align_t) - 1)) - sizeof (memblock_t))
#define MEMORY(x) (void *)((uintptr_t)(x) + sizeof(memblock_t) + ALIGNPAD)
#define MEMBLOCK(x) (memblock_t *)((uintptr_t)(x) - ALIGNPAD - sizeof(memblock_t))

// both the head and tail of the zone memory block list
static memblock_t head;

static constexpr size_t kLevelLargePoolBlockSize = sizeof(mobj_t);
static constexpr size_t kLevelMedPoolBlockSize = sizeof(precipmobj_t);
static constexpr size_t kLevelSmallPoolBlockSize = 128;
static constexpr size_t kLevelTinyPoolBlockSize = 64;

#ifdef __3DS__
// the pools reserve a whole CHUNK as soon as the first block is requested. The "med" one was
// sizeof(precipmobj_t) x 32768 = ~8 MB at once (memory.cpp:76: 10 MB in 5 blocks in
// 765Stadium). Small chunks: they grow on their own if needed (same behavior, less RAM).
static srb2::PoolAllocator g_level_large_pool { kLevelLargePoolBlockSize, 128, PU_LEVEL };
static srb2::PoolAllocator g_level_med_pool { kLevelMedPoolBlockSize, 256, PU_LEVEL };
static srb2::PoolAllocator g_level_small_pool { kLevelSmallPoolBlockSize, 512, PU_LEVEL };
static srb2::PoolAllocator g_level_tiny_pool { kLevelTinyPoolBlockSize, 1024, PU_LEVEL };
#else
static srb2::PoolAllocator g_level_large_pool { kLevelLargePoolBlockSize, 1024, PU_LEVEL };
static srb2::PoolAllocator g_level_med_pool { kLevelMedPoolBlockSize, 32768, PU_LEVEL };
static srb2::PoolAllocator g_level_small_pool { kLevelSmallPoolBlockSize, 4096, PU_LEVEL };
static srb2::PoolAllocator g_level_tiny_pool { kLevelTinyPoolBlockSize, 8192, PU_LEVEL };
#endif

//
// Function prototypes
//
static void Command_Memfree_f(void);
static void Command_Memdump_f(void);

// --------------------------
// Zone memory initialisation
// --------------------------

/** Initialises zone memory.
  * Used at game startup.
  *
  * \sa I_GetFreeMem, Command_Memfree_f, Command_Memdump_f
  */
void Z_Init(void)
{
	UINT32 total, memfree;

	memset(&head, 0x00, sizeof(head));

	head.next = head.prev = &head;

	memfree = I_GetFreeMem(&total)>>20;
	CONS_Printf("System memory: %uMB - Free: %uMB\n", total>>20, memfree);

	// Note: This allocates memory. Watch out.
	COM_AddDebugCommand("memfree", Command_Memfree_f);
	COM_AddDebugCommand("memdump", Command_Memdump_f);
}


// ----------------------
// Zone memory allocation
// ----------------------

#ifdef __3DS__
// Plan B when the normal heap is fragmented (28 MB free but
// no contiguous 270 KB block). The block is requested from "linear" memory
// (another region of the 3DS) and Z_Free knows how to return it there by its address.
extern "C" {
	void *linearAlloc(size_t size);
	void linearFree(void *mem);
	extern uint32_t __ctru_linear_heap;
	extern uint32_t __ctru_linear_heap_size;
}
static inline bool Z_3DS_IsLinear(const void *p)
{
	uintptr_t a = (uintptr_t)p;
	return a >= __ctru_linear_heap && a < (uintptr_t)__ctru_linear_heap + __ctru_linear_heap_size;
}

// zone ARENA (idea from MK7's heap): a single large contiguous block requested
// at startup (with the heap still clean) from which ALL Z_Malloc blocks come.
// That way the zone (12-25 MB) is not mixed with std::vector/new on the normal heap and
// does not fragment it. If the arena fills up or the request fails, it falls back to malloc as before.
// Allocator: blocks with a header (size + previous size), free lists by
// log2 of the size, best fit within the list and merging with neighbors on free.
// Turn off: `zone_arena_mb=0` in rr_config.txt. Size: `zone_arena_mb=N` (default 20).
extern "C" {
	void *__real_malloc(size_t);
	void svcSleepThread(long long ns);
	extern char *fake_heap_end;
}
#include <malloc.h>
#include <unistd.h>
namespace za
{
	struct Hdr { uint32_t size; uint32_t prev; };            // size = block bytes | 1 if used; prev = bytes of the previous block (0 = first)
	struct Fre { Hdr h; Fre *next, *prv; };
	constexpr uint32_t kUsed = 1, kMin = sizeof(Fre), kBins = 28;
	static_assert(sizeof(Fre) == 16, "Fre must be 16 B (32 bits)");
	static uint8_t *base = nullptr, *end = nullptr;
	static Fre *bins[kBins];
	static bool tried = false;
	static uint32_t usedB = 0, peakB = 0, nAlloc = 0, nFree = 0, nMiss = 0, nBlocksFree = 0;
	static inline uint32_t bsz(const Hdr *h) { return h->size & ~kUsed; }
	static inline int binOf(uint32_t s) { return 31 - __builtin_clz(s); }
	static void link(Fre *f)
	{
		const int b = binOf(f->h.size);
		f->prv = nullptr; f->next = bins[b];
		if (f->next) f->next->prv = f;
		bins[b] = f; nBlocksFree++;
	}
	static void unlink(Fre *f)
	{
		const int b = binOf(f->h.size);
		if (f->prv) f->prv->next = f->next; else bins[b] = f->next;
		if (f->next) f->next->prv = f->prv;
		nBlocksFree--;
	}
	// SINGLE ARENA (root cause of the fragmentation): by default the arena takes ALL the free
	// heap at startup minus `small_heap_mb` (default 4 MB; measured usage ~0.3 MB) for small requests
	// (<4 KB), those from other threads and newlib's internal ones. The zone comes out of the arena
	// And all the large malloc/new/realloc (memtrack_3ds.cpp). Before: fixed 20 MB and
	// large blocks went to the normal heap, which fragmented (largest block 50-100 KB).
	// `zone_arena_mb=N` sets the size by hand; 0 = no arena (as before).
	static uint32_t fixedB = 0;
	static void init(void)
	{
		tried = true;
		const int cfg = RRCfg_Int("zone_arena_mb", -1);
		if (cfg == 0) return;
		size_t want;
		const size_t kMB = 1u << 20;
		if (cfg > 0)
			want = (size_t)cfg * kMB;
		else
		{
			const char *top = (const char *)sbrk(0);
			const size_t unused = (fake_heap_end && top && fake_heap_end > top) ? (size_t)(fake_heap_end - top) : 0;
			const size_t freeAmount = unused + (size_t)mallinfo().fordblks;
			int small = RRCfg_Int("small_heap_mb", 4);
			if (small < 2) small = 2;
			const size_t keep = (size_t)small * kMB;
			want = freeAmount > keep + 8 * kMB ? freeAmount - keep : 8 * kMB;
			want &= ~(size_t)0xFFFF;
		}
		for (; want >= 8 * kMB; want -= kMB)
		{
			base = (uint8_t *)__real_malloc(want);
			if (base) break;
		}
		if (!base) { CONS_Printf("[ZARENA] could not reserve the arena: using malloc\n"); return; }
		const uint32_t sz = (uint32_t)want;
		fixedB = sz;
		end = base + sz;
		Fre *f = (Fre *)base;
		f->h.size = sz; f->h.prev = 0;
		link(f);
		CONS_Printf("[ZARENA] single arena: %u KB at %p (zone + big mallocs)\n", (unsigned)(sz >> 10), (void *)base);
	}
	// the arena is now also used by large malloc/new calls; the music thread may
	// free an arena block: simple lock (there is almost never any waiting).
	static volatile int lk = 0;
	static inline void lock(void) { while (__atomic_test_and_set(&lk, __ATOMIC_ACQUIRE)) svcSleepThread(20000); }
	static inline void unlock(void) { __atomic_clear(&lk, __ATOMIC_RELEASE); }
	static inline bool contains(const void *p) { return base && (const uint8_t *)p >= base && (const uint8_t *)p < end; }
	static void *alloc_nl(size_t n)
	{
		if (!base || n > (size_t)(end - base)) return nullptr;
		uint32_t need = ((uint32_t)n + sizeof(Hdr) + 7u) & ~7u;
		if (need < kMin) need = kMin;
		int b = binOf(need);
		Fre *best = nullptr;
		for (Fre *f = bins[b]; f; f = f->next)
			if (f->h.size >= need && (!best || f->h.size < best->h.size)) { best = f; if (f->h.size == need) break; }
		for (int bb = b + 1; !best && bb < (int)kBins; bb++)
			best = bins[bb];
		if (!best) { nMiss++; return nullptr; }
		const uint32_t total = best->h.size;
		unlink(best);
		if (total - need >= kMin)
		{
			Fre *rem = (Fre *)((uint8_t *)best + need);
			rem->h.size = total - need; rem->h.prev = need;
			Hdr *nx = (Hdr *)((uint8_t *)rem + rem->h.size);
			if ((uint8_t *)nx < end) nx->prev = rem->h.size;
			link(rem);
			best->h.size = need | kUsed;
		}
		else
			best->h.size = total | kUsed;
		usedB += bsz(&best->h); if (usedB > peakB) peakB = usedB;
		nAlloc++;
		return (uint8_t *)best + sizeof(Hdr);
	}
	static void release_nl(void *p)
	{
		Hdr *h = (Hdr *)((uint8_t *)p - sizeof(Hdr));
		uint32_t size = bsz(h);
		usedB -= size; nFree++;
		Hdr *nx = (Hdr *)((uint8_t *)h + size);
		if ((uint8_t *)nx < end && !(nx->size & kUsed)) { size += bsz(nx); unlink((Fre *)nx); }
		if (h->prev)
		{
			Hdr *pv = (Hdr *)((uint8_t *)h - h->prev);
			if (!(pv->size & kUsed)) { unlink((Fre *)pv); size += h->prev; h = pv; }
		}
		h->size = size;
		Hdr *nn = (Hdr *)((uint8_t *)h + size);
		if ((uint8_t *)nn < end) nn->prev = size;
		link((Fre *)h);
	}
	// for large mallocs: the free block with the HIGHEST ADDRESS that is big enough, cut
	// from its end. The zone cuts from the bottom (best fit) and malloc blocks
	// stack on top: long-lived ones do not end up in the middle of the level data.
	static uint32_t nTop = 0, topB = 0;
	static void *alloc_top_nl(size_t n)
	{
		if (!base || n > (size_t)(end - base)) return nullptr;
		uint32_t need = ((uint32_t)n + sizeof(Hdr) + 7u) & ~7u;
		if (need < kMin) need = kMin;
		Fre *best = nullptr;
		for (int bb = binOf(need); bb < (int)kBins; bb++)
			for (Fre *f = bins[bb]; f; f = f->next)
				if (f->h.size >= need && f > best) best = f;
		if (!best) return nullptr;
		const uint32_t total = best->h.size;
		unlink(best);
		Hdr *blk;
		if (total - need >= kMin)
		{
			best->h.size = total - need; // the lower part stays free
			link(best);
			blk = (Hdr *)((uint8_t *)best + (total - need));
			blk->size = need | kUsed; blk->prev = total - need;
			Hdr *nx = (Hdr *)((uint8_t *)blk + need);
			if ((uint8_t *)nx < end) nx->prev = need;
		}
		else
		{
			blk = &best->h;
			blk->size = total | kUsed;
		}
		usedB += bsz(blk); if (usedB > peakB) peakB = usedB;
		nAlloc++; nTop++;
		return (uint8_t *)blk + sizeof(Hdr);
	}
	static void *alloc(size_t n)
	{
		if (!tried) init();
		lock(); void *p = alloc_nl(n); unlock();
		return p;
	}
	static void *alloc_top(size_t n)
	{
		if (!tried) return nullptr; // the arena is built in i_main (after the 64mb reservation), not on the 1st malloc
		lock(); void *p = alloc_top_nl(n); unlock();
		return p;
	}
	static void release(void *p) { lock(); release_nl(p); unlock(); }
	static inline size_t usable(const void *p) { const Hdr *h = (const Hdr *)((const uint8_t *)p - sizeof(Hdr)); return bsz(h) - sizeof(Hdr); }
	static uint32_t biggest(void)
	{
		uint32_t big = 0;
		lock();
		for (uint32_t b = 0; b < kBins; b++)
			for (Fre *f = bins[b]; f; f = f->next) if (f->h.size > big) big = f->h.size;
		unlock();
		return big;
	}
	static void report(void)
	{
		if (!base) return;
		const uint32_t big = biggest();
		CONS_Printf("[ZARENA] used=%uKB peak=%uKB free=%uKB largest-free-block=%uKB free-chunks=%u | requests=%u freed=%u no-room=%u\n",
			(unsigned)(usedB >> 10), (unsigned)(peakB >> 10), (unsigned)(((uint32_t)(end - base) - usedB) >> 10), (unsigned)(big >> 10),
			(unsigned)nBlocksFree, (unsigned)nAlloc, (unsigned)nFree, (unsigned)nMiss);
	}
}
namespace za
{
	// Integrity check (HOME / load-level crashes in alloc_nl/release_nl = corrupted free list).
	// Walks every block (size, prev link) and every free list. Prints the FIRST anomaly with
	// the previous used block (if it is a zone block: tag + file:line of its owner).
	static void describe(const char *tag, const uint8_t *q)
	{
		if (!q) { CONS_Printf("[ARENACHK]   %s: (none)\n", tag); return; }
		const Hdr *h = (const Hdr *)q;
		const memblock_t *mb = (const memblock_t *)(q + sizeof(Hdr));
		if ((h->size & kUsed) && mb->id == ZONEID)
			CONS_Printf("[ARENACHK]   %s: %p size=%u USED zone tag=%d real=%u owner=%s:%d\n", tag, (const void *)q, (unsigned)bsz(h), (int)mb->tag, (unsigned)mb->realsize, mb->ownerfile ? mb->ownerfile : "?", (int)mb->ownerline);
		else
			CONS_Printf("[ARENACHK]   %s: %p size=%u %s (malloc/other)\n", tag, (const void *)q, (unsigned)bsz(h), (h->size & kUsed) ? "USED" : "FREE");
	}
	static int verify(const char *where)
	{
		if (!base) return 1;
		lock();
		const uint8_t *p = base, *prevp = nullptr, *prevUsed = nullptr;
		uint32_t prevsz = 0, nblk = 0, nfree = 0;
		int ok = 1;
		while (p < end)
		{
			const Hdr *h = (const Hdr *)p;
			const uint32_t sz = bsz(h);
			if (sz < kMin || (sz & 7) || sz > (uint32_t)(end - p) || h->prev != prevsz)
			{
				CONS_Printf("[ARENACHK] %s: CORRUPT block header at %p (offset %u, block #%u): size=0x%x prev=%u expected_prev=%u\n",
					where, (const void *)p, (unsigned)(p - base), (unsigned)nblk, (unsigned)h->size, (unsigned)h->prev, (unsigned)prevsz);
				describe("previous block", prevp);
				describe("previous USED block", prevUsed);
				ok = 0; break;
			}
			if (!(h->size & kUsed))
			{
				const Fre *f = (const Fre *)p;
				nfree++;
				if ((f->next && !contains(f->next)) || (f->prv && !contains(f->prv)) || (f->next && f->next->prv != f))
				{
					CONS_Printf("[ARENACHK] %s: CORRUPT free-list links at %p size=%u next=%p prv=%p (use after free?)\n", where, (const void *)p, (unsigned)sz, (void *)f->next, (void *)f->prv);
					describe("previous block", prevp);
					describe("previous USED block", prevUsed);
					ok = 0; break;
				}
			}
			else prevUsed = p;
			prevp = p; prevsz = sz; p += sz; nblk++;
		}
		if (ok && nfree != nBlocksFree)
		{
			CONS_Printf("[ARENACHK] %s: free block count differs: chain=%u lists=%u\n", where, (unsigned)nfree, (unsigned)nBlocksFree);
			ok = 0;
		}
		unlock();
		if (ok) CONS_Printf("[ARENACHK] %s: ok (%u blocks, %u free)\n", where, (unsigned)nblk, (unsigned)nfree);
		return ok;
	}
}
extern "C" int RR3DS_ArenaVerify(const char *where) { return za::verify(where); }
// API for memtrack_3ds.cpp (large malloc/new/realloc go to the arena)
extern "C" void Z_3DS_ArenaInit(void) { if (!za::tried) za::init(); }
static int s_arenaMalloc = -1; // `arena_malloc=0` in rr_config = large mallocs go to the heap as before (for comparison)
extern "C" void *RR3DS_ArenaAlloc(size_t n)
{
	if (s_arenaMalloc < 0) { if (!za::tried) return nullptr; s_arenaMalloc = RRCfg_Int("arena_malloc", 1) ? 1 : 0; }
	return s_arenaMalloc ? za::alloc_top(n) : nullptr;
}
extern "C" void RR3DS_ArenaFree(void *p) { za::release(p); }
extern "C" int RR3DS_ArenaContains(const void *p) { return za::contains(p) ? 1 : 0; }
extern "C" size_t RR3DS_ArenaUsable(const void *p) { return za::usable(p); }
extern "C" void RR3DS_ArenaInfo(unsigned *totalKB, unsigned *freeKB, unsigned *bigKB, unsigned *topN)
{
	*totalKB = za::fixedB >> 10;
	*freeKB = za::base ? (unsigned)((za::fixedB - za::usedB) >> 10) : 0;
	*bigKB = za::base ? (unsigned)(za::biggest() >> 10) : 0;
	*topN = za::nTop;
}
static inline void Z_3DS_RawFree(void *p)
{
	if (za::contains(p)) za::release(p);
	else if (Z_3DS_IsLinear(p)) linearFree(p); else free(p);
}
#endif

/** Frees allocated memory.
  *
  * \param ptr A pointer to allocated memory,
  *             assumed to have been allocated with Z_Malloc/Z_Calloc.
  * \sa Z_FreeTags
  */
void Z_Free2(void *ptr, const char *file, INT32 line)
{
	memblock_t *block;

	if (ptr == NULL)
		return;

/*
// Sal: There's a print exactly like this just below?
#ifdef ZDEBUG
	CONS_Debug(DBG_MEMORY, "Z_Free %s:%d\n", file, line);
#endif
*/

	block = MEMBLOCK(ptr);
#ifdef PARANOIA
	if (block->id != ZONEID)
		I_Error("Z_Free at %s:%d: wrong id", file, line);
#endif

	// Write every Z_Free call to a debug file.
	CONS_Debug(DBG_MEMORY, "Z_Free at %s:%d\n", file, line);

	// anything that isn't by lua gets passed to lua just in case.
	if (block->tag != PU_LUA)
		LUA_InvalidateUserdata(ptr);

	// TODO: if zdebugging, make sure no other block has a user
	// that is about to be freed.

	// Clear the user's mark.
	if (block->user != NULL)
		*block->user = NULL;

#ifdef VALGRIND_DESTROY_MEMPOOL
	VALGRIND_DESTROY_MEMPOOL(block);
#endif
	block->prev->next = block->next;
	block->next->prev = block->prev;
	TracyCFree(block);
#ifdef __3DS__
	Z_3DS_RawFree(block);
#else
	free(block);
#endif
}

/** malloc() that doesn't accept failure.
  *
  * \param size Amount of memory to be allocated, in bytes.
  * \return A pointer to the allocated memory.
  */
static void *xm(size_t size, const char *file, INT32 line)
{
	const size_t padedsize = size+sizeof (size_t);
	void *p;

	if (padedsize < size)/* overflow check */
		I_Error("You are allocating memory too large!");
#ifdef __3DS__
	p = za::alloc(padedsize); // the zone arena first
	if (p) return p;
	p = __real_malloc(padedsize); // the arena was already tried; straight to the small heap (no memtrack)
#else
	p = malloc(padedsize);
#endif

	if (p == NULL)
	{
		// Oh crumbs: we're out of heap. Try purging the cache and reallocating.
		Z_FreeTags(PU_PURGELEVEL, INT32_MAX);
#ifdef __3DS__
		p = za::alloc(padedsize);
		if (p) return p;
		p = __real_malloc(padedsize);
#else
		p = malloc(padedsize);
#endif

		if (p == NULL)
		{
			// added file/line of the caller (already available via
			// the Z_Malloc/Z_MallocAlign macros) to be able to pinpoint the exact
			// call site that runs out of memory, instead of guessing by elimination.
#ifdef __3DS__
			p = linearAlloc(padedsize);
			if (p != NULL)
			{
				CONS_Printf("[ZONE] fragmented heap: %s bytes to linear (%s:%d)\n", sizeu1(size), file, line);
				return p;
			}
			Z_3DS_ReportTags("out of memory");
#endif
			I_Error("Out of memory allocating %s bytes (%s:%d)", sizeu1(size), file, line);
		}
	}

	return p;
}

/** The Z_MallocAlign function.
  * Allocates a block of memory, adds it to a linked list so we can keep track of it.
  *
  * \param size Amount of memory to be allocated, in bytes.
  * \param tag Purge tag.
  * \param user The address of a pointer to the memory to be allocated.
  *             When the memory is freed by Z_Free later,
  *             the pointer at this address will then be automatically set to NULL.
  * \param alignbits The alignment of the memory to be allocated, in bits. Can be 0.
  * \note You can pass Z_Malloc() a NULL user if the tag is less than PU_PURGELEVEL.
  * \sa Z_CallocAlign, Z_ReallocAlign
  */
void *Z_Malloc2(size_t size, INT32 tag, void *user, INT32 alignbits,
	const char *file, INT32 line)
{
	memblock_t *block;
	void *ptr;

	(void)(alignbits); // no longer used, so silence warnings. TODO we should figure out a solution for this

#ifdef ZDEBUG
	CONS_Debug(DBG_MEMORY, "Z_Malloc %s:%d\n", file, line);
#endif

	block = (memblock_t*)xm(sizeof (memblock_t) + ALIGNPAD + size, file, line);
	TracyCAlloc(block, sizeof (memblock_t) + ALIGNPAD + size);
	ptr = MEMORY(block);
	I_Assert((intptr_t)ptr % alignof (max_align_t) == 0);

#ifdef HAVE_VALGRIND
	Z_calloc = false;
#endif

	block->next = head.next;
	block->prev = &head;
	head.next = block;
	block->next->prev = block;

	block->tag = tag;
	block->user = NULL;
	block->ownerline = line;
	block->ownerfile = file;
	block->size = sizeof (memblock_t) + size;
	block->realsize = size;

#ifdef VALGRIND_CREATE_MEMPOOL
	VALGRIND_CREATE_MEMPOOL(block, size, Z_calloc);
#endif

	block->id = ZONEID;

	if (user != NULL)
	{
		block->user = (void**)user;
		*(void **)user = ptr;
	}
	else if (tag >= PU_PURGELEVEL)
		I_Error("Z_Malloc: attempted to allocate purgable block "
			"(size %s) with no user", sizeu1(size));

	return ptr;
}

/** The Z_CallocAlign function.
  * Allocates a block of memory, adds it to a linked list so we can keep track of it.
  * Unlike Z_MallocAlign, this also initialises the bytes to zero.
  *
  * \param size Amount of memory to be allocated, in bytes.
  * \param tag Purge tag.
  * \param user The address of a pointer to the memory to be allocated.
  *             When the memory is freed by Z_Free later,
  *             the pointer at this address will then be automatically set to NULL.
  * \param alignbits The alignment of the memory to be allocated, in bits. Can be 0.
  * \note You can pass Z_Calloc() a NULL user if the tag is less than PU_PURGELEVEL.
  * \sa Z_MallocAlign, Z_ReallocAlign
  */
void *Z_Calloc2(size_t size, INT32 tag, void *user, INT32 alignbits, const char *file, INT32 line)
{
#ifdef VALGRIND_MEMPOOL_ALLOC
	Z_calloc = true;
#endif
	return memset(Z_Malloc2    (size, tag, user, alignbits, file, line), 0, size);
}

/** The Z_ReallocAlign function.
  * Reallocates a block of memory with a new size.
  *
  * \param ptr A pointer to allocated memory,
  *             assumed to have been allocated with Z_Malloc/Z_Calloc.
  *             If NULL, this function instead acts as a wrapper for Z_CallocAlign.
  * \param size New size of memory block, in bytes.
  *             If zero, then the memory is freed and NULL is returned.
  * \param tag New purge tag.
  * \param user The address of a pointer to the memory to be reallocated.
  *             This can be a different user to the one originally assigned to the memory block.
  * \param alignbits The alignment of the memory to be allocated, in bits. Can be 0.
  * \return A pointer to the reallocated memory. Can be NULL if memory was freed.
  * \note You can pass Z_Realloc() a NULL user if the tag is less than PU_PURGELEVEL.
  * \sa Z_MallocAlign, Z_CallocAlign
  */
void *Z_Realloc2(void *ptr, size_t size, INT32 tag, void *user, INT32 alignbits, const char *file, INT32 line)
{
	void *rez;
	memblock_t *block;
	size_t copysize;

#ifdef ZDEBUG
	CONS_Debug(DBG_MEMORY, "Z_Realloc %s:%d\n", file, line);
#endif

	if (!size)
	{
		Z_Free(ptr);
		return NULL;
	}

	if (!ptr)
	{
		return Z_Calloc2(size, tag, user, alignbits, file , line);
	}

	block = MEMBLOCK(ptr);
#ifdef PARANOIA
	if (block->id != ZONEID)
		I_Error("Z_ReallocAlign at %s:%d: wrong id", file, line);
#endif

	if (block == NULL)
		return NULL;

	// Write every Z_Realloc call to a debug file.
	DEBFILE(va("Z_Realloc at %s:%d\n", file, line));
	rez = Z_Malloc2(size, tag, user, alignbits, file, line);

	if (size < block->realsize)
		copysize = size;
	else
		copysize = block->realsize;

	M_Memcpy(rez, ptr, copysize);

	Z_Free2(ptr, file, line);

	// Need to set the user in case the old block had the same one, in
	// which case the Z_Free will just have NULLed it out.
	if (user)
		*((void**)user) = rez;

	if (size > copysize)
		memset((char*)rez+copysize, 0x00, size-copysize);

	return rez;
}

/** Frees all memory for a given set of tags.
  *
  * \param lowtag The lowest tag to consider.
  * \param hightag The highest tag to consider.
  */
#ifdef __3DS__
// "coalesce the holes". If malloc runs out of blocks, BEFORE using linear memory
// the purgeable cache is released (it reloads itself when used again).
// It never used to happen: memtrack's safety net returned linear memory and xm never got to purge.
static volatile int s_purging = 0;
static unsigned s_purges = 0, s_purgedKB = 0;
extern "C" size_t RR3DS_PurgeCache(void)
{
	if (s_purging) return 0;
	s_purging = 1;
	const size_t before = Z_TagsUsage(PU_PURGELEVEL, INT32_MAX);
	if (before) Z_FreeTags(PU_PURGELEVEL, INT32_MAX);
	s_purging = 0;
	if (before) { s_purges++; s_purgedKB += (unsigned)(before >> 10); }
	return before;
}
extern "C" void RR3DS_PurgeStats(unsigned *n, unsigned *kb) { *n = s_purges; *kb = s_purgedKB; }
// SPRITE RELIEF VALVE. Sprite patches (PU_SPRITE + their PU_PATCH_DATA columns) are never
// released during a map: on CityEscape they grow to ~2.8 MB and the arena (34 MB) fills up.
// At the end of each frame (2D queue already drawn, nobody holds sprite pointers) it checks how much
// is really left (free + purgeable cache); if it is less than `relief_valve_kb` (default 1536) ALL
// sprites are released: those still on screen reload on their own (W_CachePatchNum).
extern "C" void Patch_FreeTags(INT32 lowtag, INT32 hightag); // r_patch.cpp
extern "C" size_t RR3DS_ArenaFreeBytes(void) { return za::base ? (size_t)(za::end - za::base) - za::usedB : (size_t)64 * 1024 * 1024; }
extern "C" void RR3DS_ValveSprites(void)
{
	static unsigned frameNo = 0, times = 0, last = 0;
	static int threshold = -1;
	if (!za::base) return;
	frameNo++;
	if ((frameNo & 7) || frameNo - last < 70) return; // check every 8 frames; do not repeat before 70
	if (threshold < 0) threshold = RRCfg_Int("relief_valve_kb", 1536) * 1024;
	if (!threshold) return;
	const size_t freeAmount = (size_t)(za::end - za::base) - za::usedB;
	if (freeAmount >= (size_t)threshold) return;
	if (freeAmount + Z_TagsUsage(PU_PURGELEVEL, INT32_MAX) >= (size_t)threshold) return;
	const uint32_t before = za::usedB;
	Patch_FreeTags(PU_SPRITE, PU_SPRITE);
	last = frameNo; times++;
	CONS_Printf("[VALVE] sprites released (time %u): arena used %u -> %u KB (real free was %u KB)\n", times,
		(unsigned)(before >> 10), (unsigned)(za::usedB >> 10), (unsigned)(freeAmount >> 10));
}
// Contiguous block for one-off big work buffers (Argon2 password hash, 8 MB). The arena is
// fragmented after races (free 13 MB but biggest block 3.4 MB): drop the purgeable cache and
// the reloadable patches, retry. Returns NULL (never I_Error) so the caller can fail softly.
extern "C" void *RR3DS_ArenaAllocBig(size_t n)
{
	if (!za::base) return nullptr;
	void *p = za::alloc_top(n);
	if (p) return p;
	RR3DS_PurgeCache();
	p = za::alloc_top(n);
	if (p) return p;
	Patch_FreeTags(PU_PATCH_LOWPRIORITY, PU_SPRITE);
	p = za::alloc_top(n);
	CONS_Printf("[ZARENA] big block %uKB: %s (largest free now %uKB)\n", (unsigned)(n >> 10), p ? "ok after releasing cache and patches" : "DOES NOT FIT", (unsigned)(za::biggest() >> 10));
	return p;
}
#endif

void Z_FreeTags(INT32 lowtag, INT32 hightag)
{
	memblock_t *block, *next;
	TracyCZone(__zone, true);

	Z_CheckHeap(420);

	// First, release all pools, since they can make allocations in zones.
	if (PU_LEVEL >= lowtag && PU_LEVEL <= hightag)
	{
		g_level_large_pool.release();
		g_level_med_pool.release();
		g_level_small_pool.release();
		g_level_tiny_pool.release();
	}

	for (block = head.next; block != &head; block = next)
	{
		next = block->next; // get link before freeing
		if (block->tag >= lowtag && block->tag <= hightag)
			Z_Free(MEMORY(block));
	}

	TracyCZoneEnd(__zone);
}

/** Iterates through all memory for a given set of tags.
  *
  * \param lowtag The lowest tag to consider.
  * \param hightag The highest tag to consider.
  * \param iterfunc The iterator function.
  */
void Z_IterateTags(INT32 lowtag, INT32 hightag, boolean (*iterfunc)(void *))
{
	memblock_t *block, *next;
	TracyCZone(__zone, true);

	if (!iterfunc)
		I_Error("Z_IterateTags: no iterator function was given");

	for (block = head.next; block != &head; block = next)
	{
		next = block->next; // get link before possibly freeing

		if (block->tag >= lowtag && block->tag <= hightag)
		{
			void *mem = MEMORY(block);
			boolean free = iterfunc(mem);
			if (free)
				Z_Free(mem);
		}
	}

	TracyCZoneEnd(__zone);
}

// -----------------
// Utility functions
// -----------------

// starting value of nextcleanup
#define CLEANUPCOUNT 2000

// number of function calls left before next cleanup
static INT32 nextcleanup = CLEANUPCOUNT;

/** This was in Z_Malloc, but was freeing data at
  * unsafe times. Now it is only called when it is safe
  * to cleanup memory.
  *
  * \todo Currently blocks >= PU_PURGELEVEL are freed every
  *       CLEANUPCOUNT. It might be better to keep track of
  *       the total size of all purgable memory and free it when the
  *       size exceeds some value.
  */
void Z_CheckMemCleanup(void)
{
	if (nextcleanup-- == 0)
	{
		nextcleanup = CLEANUPCOUNT;
		Z_FreeTags(PU_PURGELEVEL, INT32_MAX);
	}
}


/** Checks the heap, as well as the memhdr_ts, for any corruption or
  * other problems.
  * \param i Identifies from where in the code Z_CheckHeap was called.
  * \author Graue <graue@oceanbase.org>
  */
void Z_CheckHeap(INT32 i)
{
	memblock_t *block;
	UINT32 blocknumon = 0;
	void *given;

	for (block = head.next; block != &head; block = block->next)
	{
		blocknumon++;
		given = MEMORY(block);
#ifdef ZDEBUG
		CONS_Debug(DBG_MEMORY, "block %u owned by %s:%d\n",
			blocknumon, block->ownerfile, block->ownerline);
#endif
#ifdef VALGRIND_MEMPOOL_EXISTS
		if (!VALGRIND_MEMPOOL_EXISTS(block))
		{
			I_Error("Z_CheckHeap %d: block %u"
				"(owned by %s:%d)"
				" should not exist", i, blocknumon,
				" should not exist", i, blocknumon,
				block->ownerfile, block->ownerline
			);
		}
#endif
		if (block->user != NULL && *(block->user) != given)
		{
			I_Error("Z_CheckHeap %d: block %u"
				"(owned by %s:%d)"
				" doesn't have a proper user", i, blocknumon,
				block->ownerfile, block->ownerline
			);
		}
		if (block->next->prev != block)
		{
			I_Error("Z_CheckHeap %d: block %u"
				"(owned by %s:%d)"
				" lacks proper backlink", i, blocknumon,
				block->ownerfile, block->ownerline
			);
		}
		if (block->prev->next != block)
		{
			I_Error("Z_CheckHeap %d: block %u"
				"(owned by %s:%d)"
				" lacks proper forward link", i, blocknumon,
				block->ownerfile, block->ownerline
			);
		}
#ifdef VALGRIND_MAKE_MEM_DEFINED
		VALGRIND_MAKE_MEM_DEFINED(hdr, sizeof *hdr);
#endif
		if (block->id != ZONEID)
		{
			I_Error("Z_CheckHeap %d: block %u"
				"(owned by %s:%d)"
				" have the wrong ID", i, blocknumon,
				block->ownerfile, block->ownerline
			);
		}
	}
}

// ------------------------
// Zone memory modification
// ------------------------

/** Changes a memory block's purge tag.
  *
  * \param ptr A pointer to allocated memory,
  *             assumed to have been allocated with Z_Malloc/Z_Calloc.
  * \param tag The new tag.
  * \sa Z_SetUser
  */
#ifdef PARANOIA
void Z_ChangeTag2(void *ptr, INT32 tag, const char *file, INT32 line)
#else
void Z_ChangeTag(void *ptr, INT32 tag)
#endif
{
	memblock_t *block;

	if (ptr == NULL)
		return;

	block = MEMBLOCK(ptr);

#ifdef PARANOIA
	if (block->id != ZONEID) I_Error("Z_ChangeTag at %s:%d: wrong id", file, line);
#endif

	if (tag >= PU_PURGELEVEL && block->user == NULL)
		I_Error("Internal memory management error: "
			"tried to make block purgable but it has no owner");

	block->tag = tag;
}

/** Changes a memory block's user.
  *
  * \param ptr A pointer to allocated memory,
  *             assumed to have been allocated with Z_Malloc/Z_Calloc.
  * \param newuser The new user for the memory block.
  * \sa Z_ChangeTag
  */
#ifdef PARANOIA
void Z_SetUser2(void *ptr, void **newuser, const char *file, INT32 line)
#else
void Z_SetUser(void *ptr, void **newuser)
#endif
{
	memblock_t *block;

	if (ptr == NULL)
		return;

	block = MEMBLOCK(ptr);

#ifdef PARANOIA
	if (block->id != ZONEID) I_Error("Z_SetUser at %s:%d: wrong id", file, line);
#endif

	if (block->tag >= PU_PURGELEVEL && newuser == NULL)
		I_Error("Internal memory management error: "
			"tried to make block purgable but it has no owner");

	block->user = (void**)newuser;
	*newuser = ptr;
}

// -----------------
// Zone memory usage
// -----------------

/** Calculates memory usage for a given set of tags.
  *
  * \param lowtag The lowest tag to consider.
  * \param hightag The highest tag to consider.
  * \return Number of bytes currently allocated in the heap for the
  *         given tags.
  */
size_t Z_TagsUsage(INT32 lowtag, INT32 hightag)
{
	size_t cnt = 0;
	memblock_t *rover;

	for (rover = head.next; rover != &head; rover = rover->next)
	{
		if (rover->tag < lowtag || rover->tag > hightag)
			continue;
		cnt += rover->size + sizeof *rover;
	}

	return cnt;
}

#ifdef __3DS__
#include <malloc.h>
// split of Z_Malloc memory by tag groups (KB).
extern "C" void RR3DS_MemTrackReport(const char *why); // dummy/memtrack_3ds.cpp
void Z_3DS_ReportTags(const char *why)
{
	za::report();
	static const struct { INT32 lo, hi; const char *name; } g[] = {
		{PU_STATIC, PU_STATIC, "static"}, {PU_LUA, PU_LUA, "lua"},
		{PU_SOUND, PU_MUSIC, "audio"}, {PU_PATCH, PU_PATCH_DATA, "patch"},
		{PU_SPRITE, PU_SPRITE, "sprite"}, {PU_HUDGFX, PU_HUDGFX, "hud"},
		{PU_HWRPATCHINFO, PU_HWRMODELTEXTURE, "hwrinfo"}, {PU_HWRCACHE, PU_HWRCACHE, "hwrcache"},
		{PU_CACHE, PU_CACHE, "cache"}, {PU_LEVEL, PU_LEVEL, "level"}, {PU_LEVSPEC, PU_LEVSPEC, "levspec"},
		{PU_HWRPLANE, PU_PURGELEVEL - 1, "hwrplane+"}, {PU_PURGELEVEL, INT32_MAX, "purgable"},
	};
	size_t kb[sizeof g / sizeof g[0]] = {0};
	size_t total = 0, other = 0; size_t i;
	memblock_t *rover;
	for (rover = head.next; rover != &head; rover = rover->next)
	{
		size_t sz = rover->size + sizeof *rover;
		total += sz;
		for (i = 0; i < sizeof g / sizeof g[0]; i++)
			if (rover->tag >= g[i].lo && rover->tag <= g[i].hi) { kb[i] += sz; break; }
		if (i == sizeof g / sizeof g[0]) other += sz;
	}
	char buf[512]; int o = snprintf(buf, sizeof buf, "[ZONE] %s: total=%uKB", why, (unsigned)(total >> 10));
	for (i = 0; i < sizeof g / sizeof g[0] && o < (int)sizeof buf - 24; i++)
		if (kb[i] >= 1024) o += snprintf(buf + o, sizeof buf - o, " %s=%u", g[i].name, (unsigned)(kb[i] >> 10));
	if (o < (int)sizeof buf - 24) o += snprintf(buf + o, sizeof buf - o, " other=%u", (unsigned)(other >> 10));
	struct mallinfo mi = mallinfo();
	CONS_Printf("%s | malloc used=%uKB free=%uKB\n", buf, (unsigned)(mi.uordblks >> 10), (unsigned)(mi.fordblks >> 10));
	// patches (patch_t from w_wad MakePatch) by owner tag: n and KB of data (tag17)
	{
		static size_t pn[32], pb[32];
		memset(pn, 0, sizeof pn); memset(pb, 0, sizeof pb);
		for (rover = head.next; rover != &head; rover = rover->next)
		{
			if (rover->tag < 0 || rover->tag >= 32 || rover->size != sizeof(memblock_t) + sizeof(patch_t)) continue;
			if (!rover->ownerfile || !strstr(rover->ownerfile, "w_wad")) continue;
			patch_t *pp = (patch_t *)MEMORY(rover);
			size_t d = 0;
			if (pp->columns) d += ((memblock_t *)((uintptr_t)pp->columns - sizeof(memblock_t) - ALIGNPAD))->size - sizeof(memblock_t);
			if (pp->columnofs) d += ((memblock_t *)((uintptr_t)pp->columnofs - sizeof(memblock_t) - ALIGNPAD))->size - sizeof(memblock_t);
			pn[rover->tag]++; pb[rover->tag] += d;
		}
		char pbuf[300]; int po = snprintf(pbuf, sizeof pbuf, "[PATCHTAG] %s:", why);
		for (int t = 0; t < 32 && po < (int)sizeof pbuf - 40; t++)
			if (pn[t]) po += snprintf(pbuf + po, sizeof pbuf - po, " tag%d n=%u data=%uKB |", t, (unsigned)pn[t], (unsigned)(pb[t] >> 10));
		CONS_Printf("%s\n", pbuf);
	}
	// WHO holds the zone: top 16 by file:line (all tags)
	{
		static struct { const char *f; INT32 l; INT32 tag; size_t sz; UINT32 n; } own[384];
		size_t no = 0;
		for (rover = head.next; rover != &head; rover = rover->next)
		{
			size_t k = 0;
			while (k < no && !(own[k].f == rover->ownerfile && own[k].l == rover->ownerline)) k++;
			if (k == no) { if (no == 384) continue; own[no].f = rover->ownerfile; own[no].l = rover->ownerline; own[no].tag = rover->tag; own[no].sz = 0; own[no].n = 0; no++; }
			own[k].sz += rover->size; own[k].n++;
		}
		for (size_t a = 0; a < no && a < 16; a++)
			for (size_t b = a + 1; b < no; b++)
				if (own[b].sz > own[a].sz) { auto t = own[a]; own[a] = own[b]; own[b] = t; }
		for (size_t a = 0; a < no && a < 16; a++)
		{
			const char *f = own[a].f ? own[a].f : "?";
			const char *sl = strrchr(f, '/'); if (sl) f = sl + 1;
			CONS_Printf("[ZOWN] %s: %s:%d tag=%d %uKB (%u)\n", why, f, (int)own[a].l, (int)own[a].tag, (unsigned)(own[a].sz >> 10), (unsigned)own[a].n);
		}
	}
	// who holds the memory OUTSIDE the zone (dummy/memtrack_3ds.cpp)
	RR3DS_MemTrackReport(why);
}
#endif

// -----------------------
// Miscellaneous functions
// -----------------------

/** The function called by the "memfree" console command.
  * Prints the memory being used by each part of the game to the console.
  */
static void Command_Memfree_f(void)
{
	UINT32 freebytes, totalbytes;

	Z_CheckHeap(-1);
	CONS_Printf("\x82%s", M_GetText("Memory Info\n"));
	CONS_Printf(M_GetText("Total heap used        : %7s KB\n"), sizeu1(Z_TotalUsage()>>10));
	CONS_Printf(M_GetText("Static                 : %7s KB\n"), sizeu1(Z_TagUsage(PU_STATIC)>>10));
	CONS_Printf(M_GetText("Static (sound)         : %7s KB\n"), sizeu1(Z_TagUsage(PU_SOUND)>>10));
	CONS_Printf(M_GetText("Static (music)         : %7s KB\n"), sizeu1(Z_TagUsage(PU_MUSIC)>>10));
	CONS_Printf(M_GetText("Patches                : %7s KB\n"), sizeu1(Z_TagUsage(PU_PATCH)>>10));
	CONS_Printf(M_GetText("Patches (low priority) : %7s KB\n"), sizeu1(Z_TagUsage(PU_PATCH_LOWPRIORITY)>>10));
	CONS_Printf(M_GetText("Patches (rotated)      : %7s KB\n"), sizeu1(Z_TagUsage(PU_PATCH_ROTATED)>>10));
	CONS_Printf(M_GetText("Sprites                : %7s KB\n"), sizeu1(Z_TagUsage(PU_SPRITE)>>10));
	CONS_Printf(M_GetText("HUD graphics           : %7s KB\n"), sizeu1(Z_TagUsage(PU_HUDGFX)>>10));
	CONS_Printf(M_GetText("Locked cache           : %7s KB\n"), sizeu1(Z_TagUsage(PU_CACHE)>>10));
	CONS_Printf(M_GetText("Level                  : %7s KB\n"), sizeu1(Z_TagUsage(PU_LEVEL)>>10));
	CONS_Printf(M_GetText("Special thinker        : %7s KB\n"), sizeu1(Z_TagUsage(PU_LEVSPEC)>>10));
	CONS_Printf(M_GetText("All purgable           : %7s KB\n"),
		sizeu1(Z_TagsUsage(PU_PURGELEVEL, INT32_MAX)>>10));

#ifdef HWRENDER
	if (rendermode == render_opengl)
	{
		CONS_Printf(M_GetText("Patch info headers     : %7s KB\n"), sizeu1(Z_TagUsage(PU_HWRPATCHINFO)>>10));
		CONS_Printf(M_GetText("Cached textures        : %7s KB\n"), sizeu1(Z_TagUsage(PU_HWRCACHE)>>10));
		CONS_Printf(M_GetText("Texture colormaps      : %7s KB\n"), sizeu1(Z_TagUsage(PU_HWRPATCHCOLMIPMAP)>>10));
		CONS_Printf(M_GetText("Model textures         : %7s KB\n"), sizeu1(Z_TagUsage(PU_HWRMODELTEXTURE)>>10));
		CONS_Printf(M_GetText("Plane polygons         : %7s KB\n"), sizeu1(Z_TagUsage(PU_HWRPLANE)>>10));
		CONS_Printf(M_GetText("All GPU textures       : %7d KB\n"), HWR_GetTextureUsed()>>10);
	}
#endif

	CONS_Printf("\x82%s", M_GetText("System Memory Info\n"));
	freebytes = I_GetFreeMem(&totalbytes);
	CONS_Printf(M_GetText("    Total physical memory: %7u KB\n"), totalbytes>>10);
	CONS_Printf(M_GetText("Available physical memory: %7u KB\n"), freebytes>>10);
}

/** The function called by the "memdump" console command.
  * Prints zone memory debugging information (i.e. tag, size, location in code allocated).
  * Can be all memory allocated in game, or between a set of tags (if -min/-max args used).
  */
static void Command_Memdump_f(void)
{
	memblock_t *block;
	INT32 mintag = 0, maxtag = INT32_MAX;
	INT32 i;

	if ((i = COM_CheckParm("-min")))
		mintag = atoi(COM_Argv(i + 1));

	if ((i = COM_CheckParm("-max")))
		maxtag = atoi(COM_Argv(i + 1));

	for (block = head.next; block != &head; block = block->next)
		if (block->tag >= mintag && block->tag <= maxtag)
		{
			const char *filename = strrchr(block->ownerfile, PATHSEP[0]);
			CONS_Printf("[%3d] %s (%s) bytes @ %s:%d\n", block->tag, sizeu1(block->size), sizeu2(block->realsize), filename ? filename + 1 : block->ownerfile, block->ownerline);
		}
}

/** Creates a copy of a string.
  *
  * \param s The string to be copied.
  * \return A copy of the string, allocated in zone memory.
  */
char *Z_StrDup(const char *s)
{
	return strcpy((char*)ZZ_Alloc(strlen(s) + 1), s);
}

void* Z_LevelPoolMalloc(size_t size)
{
	void* p = nullptr;
	if (size <= kLevelTinyPoolBlockSize)
	{
		p = g_level_tiny_pool.allocate();
	}
	else if (size <= kLevelSmallPoolBlockSize)
	{
		p = g_level_small_pool.allocate();
	}
	else if (size <= kLevelMedPoolBlockSize)
	{
		p = g_level_med_pool.allocate();
	}
	else if (size <= kLevelLargePoolBlockSize)
	{
		p = g_level_large_pool.allocate();
	}

	if (p == nullptr)
	{
		p = Z_Malloc(size, PU_LEVEL, nullptr);
	}

	return p;
}

void* Z_LevelPoolCalloc(size_t size)
{
	void* p = Z_LevelPoolMalloc(size);
	memset(p, 0, size);
	return p;
}

void Z_LevelPoolFree(void* p, size_t size)
{
	if (size <= kLevelTinyPoolBlockSize)
	{
		return g_level_tiny_pool.deallocate(p);
	}
	if (size <= kLevelSmallPoolBlockSize)
	{
		return g_level_small_pool.deallocate(p);
	}
	if (size <= kLevelMedPoolBlockSize)
	{
		return g_level_med_pool.deallocate(p);
	}
	if (size <= kLevelLargePoolBlockSize)
	{
		return g_level_large_pool.deallocate(p);
	}
	return Z_Free(p);
}
