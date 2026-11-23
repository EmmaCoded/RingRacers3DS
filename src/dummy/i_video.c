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

#include "i_3ds_log.h"
#include "../doomdef.h"
#include "../rr_config.h"  // single-file configuration
#include "../command.h"
#include "../i_video.h"
#ifdef __3DS__
#include <3ds.h>
#include <stdio.h>
#include "i_3ds.h"
#ifdef HWRENDER
#include "../hardware/hw_drv.h"
#include "../hardware/hw_main.h"
#include "../v_video.h"
void I_3DS_BindHWDriver(void);
#endif
#endif

rendermode_t rendermode = render_none;
rendermode_t chosenrendermode = render_none;

boolean highcolor = false;

boolean allow_fullscreen = false;

// cv_vidwait: defined in cvars.cpp - do not redefine here.

// graphics_started defined in i_system.c
extern UINT8 graphics_started;

void I_StartupGraphics(void)
{
#ifdef __3DS__
	// gfxInitDefault() and aptInit() were already called in main() (i_main.c).
	// 8-bit software renderer; presentation is done by I_3DS_FinishFrame()
	// (dummy/i_video_3ds.c) without citro3d/hwr2.
	rendermode = render_soft;
	chosenrendermode = render_soft;
#ifdef HWRENDER
	// 3D GPU renderer (hardware/r_citro3d): opt-in with sdmc:/3ds/rr_gpu3d
	{
		extern int g_3ds_gpu3d;
		if (RRCfg_OnDef("gpu3d", 1)) // rr_config.txt; with no key = GPU (without the GPU, textures do not show)
		{
			g_3ds_gpu3d = 1;
			I_3DS_BindHWDriver();
			vid.glstate = HWD.pfnInit() ? VID_GL_LIBRARY_LOADED : VID_GL_LIBRARY_ERROR;
			if (vid.glstate == VID_GL_LIBRARY_LOADED)
			{
				rendermode = render_opengl;
				chosenrendermode = render_opengl;
				I_3DS_Both("[3DS] renderer: GPU (citro3d)\n");
			}
			else
				g_3ds_gpu3d = 0;
		}
	}
#endif
	I_3DS_SetupVideoBuffer();
	graphics_started = 1;
#endif
}
void I_ShutdownGraphics(void)
{
#ifdef __3DS__
	if (graphics_started)
	{
		gfxExit();
		graphics_started = 0;
	}
#endif
}

void VID_StartupOpenGL(void){}

void I_SetPalette(RGBA_t *palette)
{
	(void)palette;
}

INT32 VID_NumModes(void)
{
	return 0;
}

INT32 VID_GetModeForSize(INT32 w, INT32 h)
{
	(void)w;
	(void)h;
	return 0;
}

void VID_PrepareModeList(void){}

INT32 VID_SetMode(INT32 modenum)
{
	(void)modenum;
	// Fixed resolution: the mode never changes (see I_3DS_SetupVideoBuffer).
	return 1;
}

boolean VID_CheckRenderer(void)
{
	return false;
}

#ifdef HWRENDER
// VID_BeginLegacyGLRenderPass (i_video_common.cpp) calls this on 3DS. The GPU frame
// opens r_citro3d on the first draw and the presentation closes it (2D on top).
void I_3DS_BeginLegacyGLPass(void)
{
	static boolean started = false;
	if (!started && rendermode == render_opengl)
	{
		// First time: initialize the HW renderer with the game already loaded.
		started = true;
		HWR_Switch(); // HWR_Startup was already called in D_SRB2Main
		V_SetPalette(0);
	}
}
#endif

void VID_CheckGLLoaded(rendermode_t oldrender)
{
	(void)oldrender;
}

const char *VID_GetModeName(INT32 modenum)
{
	(void)modenum;
	return NULL;
}

void I_UpdateNoBlit(void){}

// I_FinishUpdate: defined in i_video_common.cpp - do not redefine here.

void I_UpdateNoVsync(void) {}

void I_WaitVBL(INT32 count)
{
	(void)count;
}

void I_ReadScreen(UINT8 *scr)
{
	(void)scr;
}

void I_BeginRead(void){}

void I_EndRead(void){}

