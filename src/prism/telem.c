#include "telem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int telem_read_line(const char *path, char *out, int cap) {
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int ok = fgets(out, cap, f) != NULL;
    fclose(f);
    if (!ok) return -1;
    out[strcspn(out, "\r\n")] = 0;
    return 0;
}

int telem_battery_mv(int *out) {
    char line[32];
    if (telem_read_line(TELEM_BATTERY_UV, line, sizeof line)) return -1;
    long uv = strtol(line, NULL, 10);
    if (uv <= 0) return -1;
    *out = (int)(uv / 1000);
    return 0;
}

/* /proc/net/wireless: two header lines, then per interface
 *   wlan0: 0000   70.  -35.  -256        0      0      0      0      0        0
 * (status, link, level, noise, ...). level is dBm on bcmdhd; the trailing dot is
 * the "updated" flag. */
int telem_rssi_dbm(const char *ifname, int *out) {
    FILE *f = fopen(TELEM_WIRELESS, "r");
    if (!f) return -1;
    char line[256];
    int rc = -1;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ') p++;
        size_t n = strlen(ifname);
        if (strncmp(p, ifname, n) || p[n] != ':') continue;
        p += n + 1;
        char *end;
        strtol(p, &end, 16);          /* status */
        p = end;
        strtod(p, &end);              /* link quality */
        p = end;
        double level = strtod(p, &end);
        if (end == p) break;
        *out = (int)level;
        rc = 0;
        break;
    }
    fclose(f);
    return rc;
}
