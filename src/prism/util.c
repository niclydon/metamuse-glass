#include "util.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "crypto.h"

int64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t wall_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int make_hex32(char out[33]) {
    uint8_t b[16];
    if (random_bytes(b, 16)) return -1;
    hex_encode(b, 16, out);
    return 0;
}

void plog(const char *fmt, ...) {
    int64_t t = mono_ms();
    fprintf(stderr, "[%6lld.%03lld] ", (long long)(t / 1000), (long long)(t % 1000));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

char *read_file(const char *path, size_t cap, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = malloc(cap + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, cap + 1, f);
    fclose(f);
    if (n > cap) { free(buf); return NULL; }
    buf[n] = 0;
    if (len_out) *len_out = n;
    return buf;
}
