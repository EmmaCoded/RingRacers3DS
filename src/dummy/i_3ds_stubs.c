// DR. ROBOTNIK'S RING RACERS — 3DS port stubs
// Symbols not provided by dummy/i_system.c, dummy/i_video.c or i_video_common.cpp.

#include <time.h>
#include "../i_system.h"
#include "../i_joy.h"
#include "../i_video.h"
#include "../k_bans.h"

// ---------------------------------------------------------------------------
// i_system.h:188
// ---------------------------------------------------------------------------
void I_StartupInput(void) {}

// i_system.h:179 / 183
void I_JoyScale3(void) {}
void I_JoyScale4(void) {}

// i_system.h:39 - global variable; console.c uses it to avoid output
// during the exit signal handler (irrelevant on 3DS, always false)
boolean g_in_exiting_signal_handler = false;

// ---------------------------------------------------------------------------
// i_joy.h:62-67 - gamepad (3DS has no external gamepad with rumble/index)
// ---------------------------------------------------------------------------
void I_SetGamepadPlayerIndex(INT32 device_id, INT32 index)
{
	(void)device_id;
	(void)index;
}

void I_SetGamepadIndicatorColor(INT32 device_id, UINT8 red, UINT8 green, UINT8 blue)
{
	(void)device_id;
	(void)red;
	(void)green;
	(void)blue;
}

void I_GamepadRumble(INT32 device_id, UINT16 low_strength, UINT16 high_strength)
{
	(void)device_id;
	(void)low_strength;
	(void)high_strength;
}

void I_GamepadRumbleTriggers(INT32 device_id, UINT16 left_strength, UINT16 right_strength)
{
	(void)device_id;
	(void)left_strength;
	(void)right_strength;
}

// i_joy.h:64-65
void I_GetGamepadGuid(INT32 device_id, char *out, int out_len)
{
	(void)device_id;
	if (out && out_len > 0)
		out[0] = '\0';
}

void I_GetGamepadName(INT32 device_id, char *out, int out_len)
{
	(void)device_id;
	if (out && out_len > 0)
		out[0] = '\0';
}

// ---------------------------------------------------------------------------
// i_video.h:169 - 3DS screen runs at 60 Hz (top screen)
// ---------------------------------------------------------------------------
UINT32 I_GetRefreshRate(void)
{
	return 60;
}

// ---------------------------------------------------------------------------
// k_bans.h:52
// ---------------------------------------------------------------------------
void SV_BanPlayer(int pnum, time_t duration, char *reason)
{
	(void)pnum;
	(void)duration;
	(void)reason;
}
