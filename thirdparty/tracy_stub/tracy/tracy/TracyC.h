// No-op stub of the Tracy C API (TracyC.h), for the 3DS.
// Real Tracy does not build for bare metal (see TracySystem.cpp:96, "Unsupported platform!").
// These macros cover exactly the ones used by the project: TracyCAlloc, TracyCFree,
// TracyCZone, TracyCZoneEnd (z_zone.cpp, p_saveg.cpp, p_setup.cpp).
#ifndef __TRACYC_STUB_H__
#define __TRACYC_STUB_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ___tracy_c_zone_context { int _unused; } TracyCZoneCtx;

#ifdef __cplusplus
}
#endif

#define TracyCZone(ctx, active) TracyCZoneCtx ctx; (void)(ctx._unused); (void)(active)
#define TracyCZoneEnd(ctx) (void)(ctx._unused)
#define TracyCAlloc(ptr, size) ((void)(ptr), (void)(size))
#define TracyCFree(ptr) ((void)(ptr))

#endif // __TRACYC_STUB_H__
