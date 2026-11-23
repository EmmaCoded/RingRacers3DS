// Local stub: SRB2_CONFIG_TRACY is OFF in the 3DS port, so these macros do not need to do anything.
// Covers the standard public Tracy API. If a .cpp reports "use of undeclared identifier 'TracyXxx'",
// add one more #define here with the same signature (nothing real is needed behind it).
#pragma once

#define ZoneScoped
#define ZoneScopedC(x)
#define ZoneScopedN(x)
#define ZoneScopedNC(x,y)
#define ZoneName(x,y)
#define ZoneNameV(x,y,z)
#define ZoneText(x,y)
#define ZoneTextV(x,y,z)
#define ZoneValue(x)
#define ZoneValueV(x,y)
#define ZoneIsActive false
#define ZoneIsActiveV(x) false

#define FrameMark
#define FrameMarkNamed(x)
#define FrameMarkStart(x)
#define FrameMarkEnd(x)
#define FrameImage(x,y,z,w,a)

#define TracyAlloc(x,y)
#define TracyFree(x)
#define TracySecureAlloc(x,y)
#define TracySecureFree(x)
#define TracyAllocN(x,y,z)
#define TracyFreeN(x,y)

#define TracyMessage(x,y)
#define TracyMessageL(x)
#define TracyMessageC(x,y,z)
#define TracyMessageLC(x,y)
#define TracyAppInfo(x,y)

#define TracyPlot(x,y)
#define TracyPlotConfig(x,...)

#define TracyLockable(type, varname) type varname
#define TracyLockableN(type, varname, desc) type varname
#define TracySharedLockable(type, varname) type varname
#define TracySharedLockableN(type, varname, desc) type varname
#define LockableBase(type) type
#define SharedLockableBase(type) type
#define LockMark(x)
#define LockableName(x,y,z)

#define TracyCZoneEnd(x)
#define TracyCZone(x,y)
#define TracyCZoneN(x,y,z)

#define TracySetThreadName(x)
