/* prismd: clocks, ids, logging. */
#ifndef PRISM_UTIL_H
#define PRISM_UTIL_H
#include <stdint.h>
#include <stddef.h>

int64_t mono_ms(void);   /* CLOCK_MONOTONIC in ms */
int64_t wall_ms(void);   /* CLOCK_REALTIME in ms (may be wrong on a device without RTC) */
/* 32 lowercase hex chars from 16 random bytes; used for frame ids and nonces. */
int make_hex32(char out[33]);
/* One log line to stderr with a monotonic timestamp. Never pass a key or token. */
void plog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Read a whole file (cap bytes max) into a malloc'd NUL-terminated buffer; NULL on error. */
char *read_file(const char *path, size_t cap, size_t *len_out);
#endif
