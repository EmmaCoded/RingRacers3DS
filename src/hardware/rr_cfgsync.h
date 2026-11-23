// Ring Racers E (PC edition): its own config (ringconfigE.cfg) synchronized with the original Ring Racers config.
// PC only: not compiled on 3DS.
#ifndef __RR_CFGSYNC_H__
#define __RR_CFGSYNC_H__

#ifdef __cplusplus
extern "C" {
#endif

// / `configfile` arrives with the path of ringconfig.cfg (the original game's) and leaves with that of ringconfigE.cfg.
// / First it synchronizes the NON-graphics options between the two (the most recently modified wins).
// / Graphics options (renderer, gfxapi, resolution, gamma, gr_*...) are never copied.
void RR_CfgSync(char *configfile, unsigned long size);

#ifdef __cplusplus
}
#endif

#endif
