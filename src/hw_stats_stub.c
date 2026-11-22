/* Stub definitions for hw perf-stat variables when HWRENDER is disabled.
   The real definitions live in hardware/hw_main.cpp (not compiled without HWRENDER). */
#include "doomtype.h"
#include "i_time.h"

precise_t ps_hw_skyboxtime    = 0;
precise_t ps_hw_nodesorttime  = 0;
precise_t ps_hw_nodedrawtime  = 0;
precise_t ps_hw_spritesorttime = 0;
precise_t ps_hw_spritedrawtime = 0;
precise_t ps_hw_batchsorttime = 0;
precise_t ps_hw_batchdrawtime = 0;
int ps_hw_numpolys    = 0;
int ps_hw_numverts    = 0;
int ps_hw_numcalls    = 0;
int ps_hw_numshaders  = 0;
int ps_hw_numtextures = 0;
int ps_hw_numpolyflags = 0;
int ps_hw_numcolors   = 0;
