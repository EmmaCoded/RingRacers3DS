// DR. ROBOTNIK'S RING RACERS — 3DS
// Exit traps: log to ringracers_log.txt WHY the
// process closed when it does not go through I_Error/I_Quit (abort, std::terminate,
// new out of memory, direct exit()).
#ifdef __3DS__
#include <3ds.h>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <csignal>
#include <exception>
#include <new>
#include <typeinfo>
#include <cxxabi.h>
#include <malloc.h>
#include <cstdint>
#include "i_3ds_log.h"

static void trap_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void trap_log(const char *fmt, ...)
{
	FILE *f = fopen(I_3DS_LogPath(), "a");
	if (!f) return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fflush(f);
	fclose(f);
}

static void trap_mem(void)
{
	trap_log("[TRAP] app RAM free=%u KB | linear free=%u KB\n",
		(unsigned)(osGetMemRegionFree(MEMREGION_APPLICATION) >> 10),
		(unsigned)(linearSpaceFree() >> 10));
}

// Walks the stack and records addresses that fall in .text (0x100000..0x5FFFFF):
// they are candidate return addresses -> addr2line with ringracers.elf.
static void trap_stack(void)
{
	const uint32_t *sp = (const uint32_t *)__builtin_frame_address(0);
	char line[256]; int o = 0, found = 0;
	o += snprintf(line + o, sizeof line - o, "[TRAP] stack:");
	for (int i = 0; i < 2048 && found < 40; i++)
	{
		uint32_t v = sp[i];
		if (v >= 0x100000 && v < 0x600000)
		{
			o += snprintf(line + o, sizeof line - o, " %lx", (unsigned long)v);
			found++;
			if (o > 200) { trap_log("%s\n", line); o = snprintf(line, sizeof line, "[TRAP] stack:"); }
		}
	}
	trap_log("%s\n", line);
}

extern "C" void Z_FreeTags(int32_t lowtag, int32_t hightag);
extern "C" size_t Z_TagsUsage(int32_t lowtag, int32_t hightag);
extern "C" void Patch_FreeTags(int32_t lowtag, int32_t hightag); // r_patch.cpp
extern "C" size_t I_3DS_EtcCacheBytes(void); // r_citro3d.cpp 
extern "C" void I_3DS_EtcCacheFree(void);

extern "C" void Z_3DS_ReportTags(const char *why);
static inline void Z_3DS_ReportTags_C(const char *w) { Z_3DS_ReportTags(w); }

extern size_t g_3ds_new_fail_sz; extern void *g_3ds_new_fail_caller;
static void trap_new_handler(void)
{
	// new does not purge the Z_Malloc cache (Z_Malloc does).
	// Before giving up, free everything purgeable (PU_PURGELEVEL=100+) and
	// let operator new retry.
	{
		// First free the ETC block cache in RAM (re-read from the SD card if needed)
		const size_t ec = I_3DS_EtcCacheBytes();
		if (ec > 0)
		{
			trap_log("[TRAP] new out of memory: freed ETC cache %u KB, retrying\n", (unsigned)(ec >> 10));
			I_3DS_EtcCacheFree();
			return;
		}
	}
	{
		size_t purgable = Z_TagsUsage(100, 0x7fffffff);
		if (purgable > 0)
		{
			trap_log("[TRAP] new out of memory: purged %u KB of cache, retrying\n", (unsigned)(purgable >> 10));
			Z_FreeTags(100, 0x7fffffff);
			return;
		}
	}
	// crash when opening the video menu (bad_alloc, fragmented heap, purgeable=0). Rotated patches and
	// low-priority ones regenerate on their own: free them too before giving up.
	{
		static int s_attempt = 0;
		const size_t extra = Z_TagsUsage(15, 16);
		if (extra > 0 && s_attempt < 4)
		{
			s_attempt++;
			trap_log("[TRAP] new out of memory: freed %u KB of rotated/low-priority patches, retrying\n", (unsigned)(extra >> 10));
			Patch_FreeTags(15, 16);
			return;
		}
	}
	trap_log("[TRAP] new OUT OF MEMORY: requested %u KB, from %p\n", (unsigned)(g_3ds_new_fail_sz >> 10), g_3ds_new_fail_caller);
	{ Z_3DS_ReportTags_C("CRASH"); } // who holds the memory
	struct mallinfo mi = mallinfo();
	trap_log("[TRAP] malloc: arena=%u KB used=%u KB free=%u KB largest-free-block~%u KB\n",
		(unsigned)(mi.arena >> 10), (unsigned)(mi.uordblks >> 10), (unsigned)(mi.fordblks >> 10), (unsigned)(mi.keepcost >> 10));
	trap_stack();
	trap_mem();
	std::set_new_handler(nullptr); // let it throw bad_alloc -> terminate logs it
}

static void trap_terminate(void)
{
	trap_log("[TRAP] std::terminate (LR=%p)\n", __builtin_return_address(0));
	std::exception_ptr ep = std::current_exception();
	if (ep)
	{
		std::type_info *t = abi::__cxa_current_exception_type();
		try { std::rethrow_exception(ep); }
		catch (const std::exception &e) { trap_log("[TRAP]   exception %s: %s\n", t ? t->name() : "?", e.what()); }
		catch (...) { trap_log("[TRAP]   non-std exception %s\n", t ? t->name() : "?"); }
	}
	trap_mem();
	trap_log("[TRAP] process stopped; Azahar stays open to read PC/LR\n");
	while (1) svcSleepThread(1000000000LL);
}

static void trap_sigabrt(int)
{
	trap_log("[TRAP] abort() / assert failed (LR=%p)\n", __builtin_return_address(0));
	trap_mem();
	while (1) svcSleepThread(1000000000LL);
}

static void trap_atexit(void)
{
	trap_log("[TRAP] exit() called without I_Quit/I_Error\n");
	trap_mem();
}

namespace {
struct CrashTrapInit
{
	CrashTrapInit()
	{
		std::set_new_handler(trap_new_handler);
		std::set_terminate(trap_terminate);
		std::signal(SIGABRT, trap_sigabrt);
		std::atexit(trap_atexit);
	}
} s_crashTrapInit;
}
#endif
