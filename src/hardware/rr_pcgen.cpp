// PC layer of the map package (.rr3) generator and of the static cache.
// On 3DS this is done by r_citro3d.cpp (vertex pool in linear memory / GPU).
// Here: the same pool in regular RAM. In Low-End with OpenGL it is DRAWN (r_opengl.cpp
// DrawStaticPool, VBO re-uploaded only if the pool changed: s_ver).
#include <stdlib.h>
#include <string.h>
#include "../doomdef.h"
#include "hw_defs.h"
#include "hw_data.h"
#include "hw_drv.h"
#include "../doomstat.h"
#include "../i_video.h"

namespace {
struct SVert { float x, y, z, s, t; unsigned int c; }; // 24 B, same as r_citro3d.cpp
static_assert(sizeof(SVert) == 24, "SVert must be 24 B (.rr3 format)");
enum { kStaticVerts = 65535 };
SVert *s_spool = NULL;
size_t s_spoolUsed = 0;
UINT32 s_ver = 1; // changes with every write to the pool
}

extern "C" {
int g_3ds_dbgflat = 0;

void *linearAlloc(size_t size) { return malloc(size); }
void linearFree(void *mem) { free(mem); }

int I_3DS_StaticAdd(const FOutVector *v, int n, unsigned int rgba)
{
	if (!s_spool) s_spool = (SVert *)malloc(sizeof(SVert) * kStaticVerts);
	if (!s_spool || n < 3 || s_spoolUsed + (size_t)n > kStaticVerts) return -1;
	const size_t first = s_spoolUsed;
	SVert *d = s_spool + first;
	for (int i = 0; i < n; i++)
	{
		d[i].x = v[i].x; d[i].y = v[i].y; d[i].z = v[i].z;
		d[i].s = v[i].s; d[i].t = v[i].t; d[i].c = rgba;
	}
	s_spoolUsed += (size_t)n;
	s_ver++;
	return (int)first;
}
void I_3DS_StaticShiftST(int first, int n, float ds, float dt)
{
	if (!s_spool || first < 0 || n <= 0 || (size_t)(first + n) > s_spoolUsed) return;
	for (int i = 0; i < n; i++) { s_spool[first + i].s += ds; s_spool[first + i].t += dt; }
	s_ver++;
}
void I_3DS_StaticSetYT(int first, int n, const float *y, const float *t)
{
	if (!s_spool || first < 0 || n <= 0 || (size_t)(first + n) > s_spoolUsed) return;
	for (int i = 0; i < n; i++) { s_spool[first + i].y = y[i]; s_spool[first + i].t = t[i]; }
	s_ver++;
}
void I_3DS_StaticSetY(int first, int n, float y)
{
	if (!s_spool || first < 0 || n <= 0 || (size_t)(first + n) > s_spoolUsed) return;
	for (int i = 0; i < n; i++) s_spool[first + i].y = y;
	s_ver++;
}
int I_3DS_StaticUsed(void) { return (int)s_spoolUsed; }
const void *I_3DS_StaticVerts(void) { return s_spool; }
void I_3DS_StaticTruncate(int used) { if (used >= 0 && (size_t)used <= s_spoolUsed) { s_spoolUsed = (size_t)used; s_ver++; } }
int I_3DS_StaticReset(void) { s_spoolUsed = 0; s_ver++; return 1; }
boolean HWR_UseShaderPC(void); // hw_main.cpp 
// 1 = drawn (or nothing to draw), 0 = texture lost (recapture), -1 = could not.
int I_3DS_StaticDraw(GLMipmap_t *mip, GLMipmap_t *bright, FBITFIELD flags, const UINT16 *idx, int n)
{
	if (rendermode != render_opengl || !HWD.pfnDrawStaticPool || !HWD.pfnSetTexture)
		return 1; // preparer without GL: does not draw (as before)
	if (!s_spool || !s_spoolUsed || n < 3)
		return -1;
	if (mip && !(flags & PF_NoTexture))
	{
		if (!mip->downloaded && !mip->data)
			return 0;
		HWD.pfnSetTexture(mip);
		if (bright && (bright->flags & TF_BRIGHTMAP) && (bright->downloaded || bright->data))
			HWD.pfnSetTexture(bright);
	}
	HWD.pfnDrawStaticPool(s_spool, (INT32)s_spoolUsed, s_ver, idx, n, flags, HWR_UseShaderPC());
	return 1;
}
void I_3DS_StaticEnd(void) {}
}
