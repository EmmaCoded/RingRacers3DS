// i_3ds_log.h - message to the bottom screen AND to the SD log.
// printf only reaches the screen; this also keeps it in ringracers_log.txt.
#ifndef I_3DS_LOG_H
#define I_3DS_LOG_H
#include <stdio.h>
#ifdef __3DS__
#ifdef __cplusplus
extern "C" {
#endif
void I_3DS_Both(const char *fmt, ...);
const char *I_3DS_LogPath(void);  // log/diag with a name unique per boot (avoids stale/cached files)
const char *I_3DS_DiagPath(void);
#ifdef __cplusplus
}
#endif
#else
#define I_3DS_Both printf
#endif
#endif
