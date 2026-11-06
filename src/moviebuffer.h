#ifndef NV_MOVIEBUFFER_H
#define NV_MOVIEBUFFER_H
#include <stddef.h>
#include "dts/dts_engine.h"
/* Main-thread lifecycle; workers never touch player state or graphics. */
int moviebuffer_url(const char *source, const char *headers, int mib, char *out, size_t n);
void moviebuffer_stop(void);
/* Exact non-consuming read, <=MOVIEBUFFER_COPIA_MAX, for the original source
 * URL only. Copied in <=64 KiB pieces so the ring lock is held briefly.
 * Returns n if every byte is cached in one generation, otherwise 0. Never
 * waits for bytes, moves the video cursor, or opens a network request.
 * The caller ignores dst on a miss. Safe against moviebuffer_stop. */
#define MOVIEBUFFER_COPIA_MAX (1024 * 1024)
int moviebuffer_copiar(const char *source,int64_t offset,unsigned char *dst,int n);
/* 1 cache active, -1 origin cannot be cached (redirected), 0 inactive. */
int moviebuffer_info(DtsBufferInfo *info);
#endif
