// DR. ROBOTNIK'S RING RACERS — 3DS hw cvars stub
// With HWRENDER=OFF, hardware/hw_main.h declares these cvars as extern
// but hw_main.c is not compiled. This file provides the definitions.
// menus/options-video-advanced.c references them in menu tables.

#include "../command.h"

// hardware/hw_main.h:102
consvar_t cv_glshaders        = CVAR_INIT("gr_shaders",        "Off", CV_SAVE, CV_OnOff, NULL);
consvar_t cv_glallowshaders   = CVAR_INIT("gr_allowshaders",   "Off", CV_SAVE, CV_OnOff, NULL);

// hardware/hw_main.h:103
consvar_t cv_glmodels         = CVAR_INIT("gr_models",         "Off", CV_SAVE, CV_OnOff, NULL);

// hardware/hw_main.h:116
consvar_t cv_glanisotropicmode = CVAR_INIT("gr_anisotropicmode", "1", CV_SAVE, NULL,     NULL);

// hardware/hw_main.h:119
consvar_t cv_glshearing       = CVAR_INIT("gr_shearing",       "Off", CV_SAVE, CV_OnOff, NULL);

// menus/options-video-advanced.o references cv_glspritebillboarding
// Look it up in hw_main.h to confirm the exact name if it fails
consvar_t cv_glspritebillboarding = CVAR_INIT("gr_spritebillboarding", "Off", CV_SAVE, CV_OnOff, NULL);
