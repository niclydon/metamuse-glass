/* prismd: the two numbers presence carries, read from the paths glass-os's HUD reads
 * on the real unit. Each returns 0 and fills *out, or -1 when the path is absent (the
 * host harness, or a unit without the driver loaded). Nothing else is read. */
#ifndef PRISM_TELEM_H
#define PRISM_TELEM_H

#define TELEM_BATTERY_UV "/sys/class/power_supply/bq27520-0/voltage_now"  /* microvolts */
#define TELEM_WIRELESS   "/proc/net/wireless"

int telem_battery_mv(int *out);
int telem_rssi_dbm(const char *ifname, int *out);
/* First line of a small file without its newline; 0 on success. */
int telem_read_line(const char *path, char *out, int cap);

#endif
