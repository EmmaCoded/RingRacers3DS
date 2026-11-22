// DR. ROBOTNIK'S RING RACERS — 3DS
// OUT-OF-ZONE MEMORY TRACKER (Z_Malloc already has its own report).
// On some maps the game dies out of memory with the zone at ~20 MB: the rest is
// direct malloc/new calls that nobody was counting. This records every block >= 1 KB with
// WHO requested it (return address) and every [ZONE] report prints the
// 20 with the most live memory: [MEMT] 0xADDRESS=KB (n blocks).
// The address is resolved with addr2line against ringracers.elf.
// Hooked in through the linker: -Wl,--wrap=malloc,--wrap=free,--wrap=realloc,--wrap=calloc
// and by replacing operator new / new[] (to see the calling C++ function, not "operator new").
#ifdef __3DS__
#include <3ds.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <new>
#include <malloc.h>

extern "C" {
void *__real_malloc(size_t);
void __real_free(void *);
void *__real_realloc(void *, size_t);
void *__real_calloc(size_t, size_t);
void CONS_Printf(const char *fmt, ...);
size_t RR3DS_PurgeCache(void); // (z_zone.cpp)
// single arena (z_zone.cpp)
void *RR3DS_ArenaAlloc(size_t n);
void RR3DS_ArenaFree(void *p);
int RR3DS_ArenaContains(const void *p);
size_t RR3DS_ArenaUsable(const void *p);
}

size_t g_3ds_new_fail_sz = 0; void *g_3ds_new_fail_caller = 0; // what the failing new requested
namespace {
constexpr size_t kMin = 1024;          // only blocks >= 1 KB
constexpr uint32_t kCap = 16384;       // power of 2
struct Ent { void *p; uint32_t size; void *caller; };
Ent g_tab[kCap];                       // p==0 free, p==1 deleted
uint32_t g_used = 0, g_dropped = 0;
volatile int g_lock = 0;
void *const kTomb = (void *)1;

// UNIVERSAL SAFETY NET. If the heap (malloc) has no contiguous block,
// the request is served from LINEAR MEMORY (the GPU's; usually 3-4 MB spare),
// always leaving kLinKeep free for textures/commands. free/realloc recognize
// the block by its address. Before: every vector that grew mid-race = crash.
constexpr size_t kLinKeep = 1408u * 1024u; // 1024 -> 1408 KB. The fade requests 1 contiguous MB and at the end of the race 934 KB were left
volatile uint32_t g_linN = 0, g_linBytes = 0, g_linPeak = 0;
inline bool is_lin(const void *p)
{
	const uintptr_t a = (uintptr_t)p;
	return (a >= 0x14000000u && a < 0x1C000000u) || (a >= 0x30000000u && a < 0x40000000u);
}
volatile int g_linBusy = 0; // linearAlloc calls malloc internally (MemBlock): avoid recursion
void *lin_alloc(size_t sz)
{
	if (__atomic_test_and_set(&g_linBusy, __ATOMIC_ACQUIRE)) return nullptr;
	void *p = (linearSpaceFree() >= sz + kLinKeep) ? linearAlloc(sz) : nullptr;
	__atomic_clear(&g_linBusy, __ATOMIC_RELEASE);
	if (p) { g_linN++; g_linBytes += (uint32_t)linearGetSize(p); if (g_linBytes > g_linPeak) g_linPeak = g_linBytes; }
	return p;
}
void lin_free(void *p) { const uint32_t z = (uint32_t)linearGetSize(p); g_linBytes = g_linBytes > z ? g_linBytes - z : 0; linearFree(p); } // no underflow (free() of foreign linear blocks)

// main thread only (threadGetCurrent()==NULL); the music thread's
// core 1 never touches the zone.
inline bool purge() { return threadGetCurrent() == nullptr && RR3DS_PurgeCache() > 0; }

inline void lock()   { while (__atomic_test_and_set(&g_lock, __ATOMIC_ACQUIRE)) svcSleepThread(50000); }
inline void unlock() { __atomic_clear(&g_lock, __ATOMIC_RELEASE); }
inline uint32_t hsh(void *p) { uint32_t x = (uint32_t)(uintptr_t)p >> 3; x ^= x >> 15; x *= 0x2c1b3c6d; x ^= x >> 12; return x & (kCap - 1); }

void add(void *p, size_t sz, void *caller)
{
	if (!p || sz < kMin) return;
	lock();
	if (g_used >= kCap - kCap / 8) { g_dropped++; unlock(); return; }
	uint32_t i = hsh(p);
	while (g_tab[i].p && g_tab[i].p != kTomb) i = (i + 1) & (kCap - 1);
	g_tab[i].p = p; g_tab[i].size = (uint32_t)sz; g_tab[i].caller = caller;
	g_used++;
	unlock();
}

void del_tab(void *p)
{
	lock();
	uint32_t i = hsh(p);
	for (uint32_t n = 0; n < kCap && g_tab[i].p; n++, i = (i + 1) & (kCap - 1))
		if (g_tab[i].p == p) { g_tab[i].p = kTomb; g_used--; break; }
	unlock();
}

void del(void *p)
{
	if (!p || is_lin(p)) return;
	if (RR3DS_ArenaContains(p)) { del_tab(p); return; }
	// small blocks are never recorded (add() discards them): exit NOW, without locking or
	// walking the table. Before, every free (thousands per tic) walked up to 16K slots
	// (deleted slots never become empty again) -> the game slowed down.
	if (malloc_usable_size(p) < kMin) return;
	del_tab(p);
}

// SINGLE ARENA: requests >= kBig from the main thread come out of the arena (z_zone.cpp),
// not from newlib's heap. The heap is kept for small allocations, so a large vector/buffer never
// depends on finding a contiguous hole in a fragmented heap. If the arena has no
// room the heap is tried; if the (small) heap fills up, the arena is used for small ones.
// Last resort: purge the zone cache and linear memory.
constexpr size_t kBig = 4096;
inline bool main_thread() { return threadGetCurrent() == nullptr; }
void *alloc_any(size_t sz)
{
	const bool mt = main_thread();
	void *p = nullptr;
	if (mt && sz >= kBig) p = RR3DS_ArenaAlloc(sz);
	if (!p) p = __real_malloc(sz);
	if (!p && mt && sz < kBig) p = RR3DS_ArenaAlloc(sz);
	if (!p && purge())
	{
		p = mt ? RR3DS_ArenaAlloc(sz) : nullptr;
		if (!p) p = __real_malloc(sz);
	}
	return p;
}
inline size_t usable_any(void *p)
{
	if (is_lin(p)) return linearGetSize(p);
	if (RR3DS_ArenaContains(p)) return RR3DS_ArenaUsable(p);
	return malloc_usable_size(p);
}

void *new_impl(size_t sz, void *caller)
{
	if (sz == 0) sz = 1;
	for (;;)
	{
		void *p = alloc_any(sz);
		if (p) { add(p, sz, caller); return p; }
		if ((p = lin_alloc(sz))) return p;
		std::new_handler h = std::get_new_handler();
		g_3ds_new_fail_sz = sz; g_3ds_new_fail_caller = caller;
		if (!h) throw std::bad_alloc();
		h();
	}
}
} // namespace

extern "C" {
void *__wrap_malloc(size_t sz)
{
	if (!sz) return __real_malloc(0);
	// setvbuf(f, NULL, ...) requests its buffer with malloc (wrapped) but fclose/setvbuf
	// released with newlib's internal _free_r (unwrapped): that buffer MUST come from the heap.
	// It was the only case in the binary (objdump: callers of _free_r/_realloc_r).
	if ((uintptr_t)__builtin_return_address(0) - ((uintptr_t)&setvbuf & ~(uintptr_t)1) < 0x300u)
		return __real_malloc(sz);
	void *p = alloc_any(sz);
	if (!p) return lin_alloc(sz);
	add(p, sz, __builtin_return_address(0));
	return p;
}
void __wrap_free(void *p)
{
	if (!p) return;
	if (is_lin(p)) { lin_free(p); return; }
	if (RR3DS_ArenaContains(p)) { del_tab(p); RR3DS_ArenaFree(p); return; }
	del(p);
	__real_free(p);
}
void *__wrap_realloc(void *p, size_t sz)
{
	if (!p) return __wrap_malloc(sz);
	if (sz == 0) { __wrap_free(p); return nullptr; }
	const bool lin = is_lin(p), ar = !lin && RR3DS_ArenaContains(p);
	if (!lin && !ar && !(sz >= kBig && main_thread()))
	{
		// small heap block that stays small (or music thread): regular realloc
		void *q = __real_realloc(p, sz);
		if (q) { del(p); add(q, sz, __builtin_return_address(0)); return q; }
	}
	// linear or arena block, or one that grows large -> new block + copy
	const size_t old = usable_any(p);
	if (sz <= old && (lin || ar)) return p;
	void *q = alloc_any(sz);
	if (q) add(q, sz, __builtin_return_address(0));
	else if (!(q = lin_alloc(sz))) return nullptr;
	memcpy(q, p, old < sz ? old : sz);
	__wrap_free(p);
	return q;
}
void *__wrap_calloc(size_t n, size_t sz)
{
	if (!n || !sz) return __real_calloc(n, sz);
	if (sz && n > (size_t)-1 / sz) return nullptr;
	const size_t t = n * sz;
	void *p = alloc_any(t);
	if (!p) p = lin_alloc(t);
	else add(p, t, __builtin_return_address(0));
	if (p) memset(p, 0, t);
	return p;
}
// for the RAM table ([RAM] / ram_table.csv)
void RR3DS_LinFallbackStats(unsigned *n, unsigned *bytes, unsigned *peak)
{
	*n = g_linN; *bytes = g_linBytes; *peak = g_linPeak;
}

// Report: the 20 with the most live memory (everything outside the zone shows up;
// the zone shows up as the Z_Malloc caller -> xm in z_zone.cpp).
void RR3DS_MemTrackReport(const char *why)
{
	static struct { void *c; uint32_t kb_bytes; uint32_t n; } agg[512];
	uint32_t na = 0, total = 0, blocks = 0;
	lock();
	for (uint32_t i = 0; i < kCap; i++)
	{
		if (!g_tab[i].p || g_tab[i].p == kTomb) continue;
		total += g_tab[i].size; blocks++;
		uint32_t k = 0;
		while (k < na && agg[k].c != g_tab[i].caller) k++;
		if (k == na) { if (na == 512) continue; agg[na].c = g_tab[i].caller; agg[na].kb_bytes = 0; agg[na].n = 0; na++; }
		agg[k].kb_bytes += g_tab[i].size; agg[k].n++;
	}
	const uint32_t dropped = g_dropped;
	unlock();
	// partial order: top 20
	for (uint32_t a = 0; a < na && a < 20; a++)
		for (uint32_t b = a + 1; b < na; b++)
			if (agg[b].kb_bytes > agg[a].kb_bytes) { auto t = agg[a]; agg[a] = agg[b]; agg[b] = t; }
	CONS_Printf("[MEMT] %s: live>=1KB total=%u KB in %u blocks (%u callers, unannotated=%u)\n",
		why, (unsigned)(total >> 10), (unsigned)blocks, (unsigned)na, (unsigned)dropped);
	char line[256]; int o = 0;
	for (uint32_t a = 0; a < na && a < 20; a++)
	{
		o += snprintf(line + o, sizeof line - o, " %lx=%uKB(%u)", (unsigned long)(uintptr_t)agg[a].c,
			(unsigned)(agg[a].kb_bytes >> 10), (unsigned)agg[a].n);
		if (o > 180 || a + 1 == na || a == 19) { CONS_Printf("[MEMT] %s\n", line); o = 0; }
	}
}
} // extern "C"

void *operator new(size_t sz) { return new_impl(sz, __builtin_return_address(0)); }
void *operator new[](size_t sz) { return new_impl(sz, __builtin_return_address(0)); }
void operator delete(void *p) noexcept { __wrap_free(p); }
void operator delete[](void *p) noexcept { __wrap_free(p); }
void operator delete(void *p, size_t) noexcept { __wrap_free(p); }
void operator delete[](void *p, size_t) noexcept { __wrap_free(p); }
#endif

// [LINALLOC] log linear allocations >= 128 KB with caller address (: who holds ~6 MB before the map loads?)
// resolve with: arm-none-eabi-addr2line -f -e build/bin/ringracers.elf <addr>
extern "C" {
void *__real_linearAlloc(size_t);
void *__real_linearMemAlign(size_t, size_t);
void CONS_Printf(const char *fmt, ...);
void *__wrap_linearAlloc(size_t sz)
{
	void *p = __real_linearAlloc(sz);
	if (sz >= 128 * 1024) CONS_Printf("[LINALLOC] %uKB ra=%p ok=%d\n", (unsigned)(sz >> 10), __builtin_return_address(0), p != nullptr);
	return p;
}
void *__wrap_linearMemAlign(size_t sz, size_t al)
{
	void *p = __real_linearMemAlign(sz, al);
	if (sz >= 128 * 1024) CONS_Printf("[LINALLOC] %uKB ra=%p ok=%d\n", (unsigned)(sz >> 10), __builtin_return_address(0), p != nullptr);
	return p;
}
}