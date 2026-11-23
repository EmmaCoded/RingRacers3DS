// DR. ROBOTNIK'S RING RACERS - 3DS port: shared platform functions
#ifndef __I_3DS_H__
#define __I_3DS_H__

#ifdef __3DS__

#include <stdarg.h>
#include "../doomtype.h"

// Internal resolution. NATIVE 400x240 by default (1:1 pixels, HUD snapped to
// the edges with V_SNAPTO*). To go back to a stretched 320x200: -DI3DS_VIDWIDTH=320
// -DI3DS_VIDHEIGHT=200.
#ifndef I3DS_VIDWIDTH
#define I3DS_VIDWIDTH 400
#endif
#ifndef I3DS_VIDHEIGHT
#define I3DS_VIDHEIGHT 240
#endif

#ifdef __cplusplus
extern "C" {
#endif

void I_3DS_SetupVideoBuffer(void);
int I_3DS_VidW(void); // vid.width/height for the citro3d driver
int I_3DS_VidH(void);
void I_3DS_FinishFrame(void);
void I_3DS_PollInput(void);
void I_3DS_BottomUI_Activate(void);
// Info panel on the bottom screen
void I_3DS_Panel_Start(void);
int I_3DS_Bottom_Tick(int touching, int tx, int ty); // bottom_3ds.c
int I_3DS_Bottom_HasMap(void);
void I_3DS_PanelFeed(const char *fmt, va_list ap);
void I_3DS_RasterizeTwodee(UINT8 *dst, INT32 w, INT32 h);
void I_3DS_DiscardTwodee(void);

#ifdef __cplusplus
}
#endif

#endif // __3DS__
#endif // __I_3DS_H__
