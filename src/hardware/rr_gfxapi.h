// rr_gfxapi.h: graphics APIs of the PC edition. PC only: not compiled on 3DS.
// "Detected" = the hardware/driver supports it (probe). "Implemented" = a backend has been written.
// The game starts in Software; the API is chosen in Video Options (cv_gfxapi).
#ifndef __RR_GFXAPI_H__
#define __RR_GFXAPI_H__

#ifdef __cplusplus
extern "C" {
#endif

enum { RRGFX_OPENGL, RRGFX_DX9, RRGFX_DX9EX, RRGFX_DX11, RRGFX_DX12, RRGFX_VULKAN, RRGFX_COUNT };

int RRGfx_Detected(int api);     // 1 = this PC supports it (probed once and logged as [GFXAPI])
int RRGfx_Implemented(int api);  // 1 = a backend has been written in the game
const char *RRGfx_Name(int api);
int RRGfx_Active(void);          // API this process started with
void RRGfx_SetActive(int api);
void RRGfx_SpawnSelf(void);      // launches another copy of the game with the same command line (to restart)
int RRGfx_LiveOK(int api, int renderer); int RRGfx_GLStale(void);       // 1 = can switch to that API without restarting (defined in sdl/i_video.cpp)
void RRGfx_LiveSwitch(int api, int renderer); // requests a live API switch (renderer 1 = Software, 2 = Legacy GL); performed at the start of the next frame

// Vulkan and DirectX 12 = the game's OpenGL translated by Mesa (Zink / GLOn12), mesa\ folder next to the exe.
int RRGfx_IsMesa(int api);           // 1 = that API comes from Mesa (Vulkan, DX12)
int RRGfx_MesaAvailable(void);       // 1 = mesa\opengl32.dll and libgallium_wgl.dll exist (64-bit only)
const char *RRGfx_MesaPrepare(int api); // prepares Mesa for that API; returns the path of mesa\opengl32.dll (NULL = unavailable)
int RRGfx_MesaHookSDL(const char *mesa_gl);
// DirectX 11 = ANGLE's OpenGL ES (angle folder next to the exe). Software mode only for now.
int RRGfx_IsAngle(int api);          // 1 = that API comes from ANGLE (DirectX 11)
int RRGfx_AngleAvailable(void);      // 1 = angle/libEGL.dll and angle/libGLESv2.dll exist (64-bit only)
int RRGfx_AnglePrepare(int api);     // DLL folder + variables so SDL loads ANGLE with D3D11 or Vulkan
void RRGfx_VkHideUserSettings(int hide); // hides vkconfig's config while Vulkan starts (1) / restores it (0)
int RRGfx_AngleVulkanAvailable(void); // 1 = angle folder with vulkan-1.dll (Chrome's ANGLE): Vulkan through ANGLE
// live API switch: undo what was done for Mesa / ANGLE (with their window and context already destroyed)
void RRGfx_MesaRelease(void);
int RRGfx_MesaLoadedApi(void);       // driver Mesa was already started with (-1 = never loaded); a different one = restart
void RRGfx_AngleRelease(void); // moves SDL2's pixel format and SwapBuffers (gdi32) to Mesa; returns how many

#ifdef __cplusplus
}
#endif

#endif
