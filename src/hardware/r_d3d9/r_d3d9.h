// DR. ROBOTNIK'S RING RACERS (Ring Racers E, PC edition)
// 3D hardware backend on Direct3D 9 / 9Ex (PC, Windows). See r_d3d9.cpp.
#ifndef __R_D3D9_H__
#define __R_D3D9_H__

#ifdef _WIN32

#include "../hw_drv.h"

#ifdef __cplusplus
extern "C++" {
#endif

// / Fills the HWD table with the Direct3D 9 functions.
void RR_D3D9_FillHWD(hwdriver_s* hwd);

// / Equivalent to OglSdlSurface: reports the size of the drawing surface and resets the state.
void RR_D3D9_Surface(INT32 w, INT32 h);

#ifdef __cplusplus
}
#endif

#endif // _WIN32

#endif
