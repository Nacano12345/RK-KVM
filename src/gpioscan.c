/*
 * gpioscan - sequential jumper finder, one pin at a time, durable log
 *
 * Usage: gpioscan <start> <end>
 *
 * Logs every action to /userdata/gpioscan.log and fsync()s it, so that if a
 * pin wedges the board the log shows exactly which pin caused it and the
 * scan can be resumed after it.
 *
 * For pin A in order: export A, set output, drive low -> sample all exported
 * pins; drive high -> sample all; release.  A pin B reading 1 when A is high
 * and 0 when A is low is shorted to A.
 *
 * To limit the blast radius it only ever exports pins within [start,end].
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#define MAXG 256
static int pins[MAXG];
static int fds[MAXG];
static int n;
static int logfd = -1;

static void logf(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	int len = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (len < 0)
		return;
	if (logfd >= 0) {
		write(logfd, buf, len);
		fsync(logfd);
	}
	write(1, buf, len);
}

static int wfile(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	int r = write(fd, val, strlen(val));
	close(fd);
	return r < 0 ? -1 : 0;
}
static void wnum(const char *path, int v)
{ char s[16]; snprintf(s, sizeof s, "%d", v); wfile(path, s); }
static int gval(int fd)
{ char b[8]; int r = pread(fd, b, sizeof b - 1, 0); if (r <= 0) return -1; b[r] = 0; return atoi(b); }
static void set_dir(int g, const char *d)
{ char p[128]; snprintf(p, sizeof p, "/sys/class/gpio/gpio%d/direction", g); wfile(p, d); }
static void set_val(int g, const char *v)
{ char p[128]; snprintf(p, sizeof p, "/sys/class/gpio/gpio%d/value", g); wfile(p, v); }

int main(int argc, char **argv)
{
	int start = argc > 1 ? atoi(argv[1]) : 32;
	int end = argc > 2 ? atoi(argv[2]) : 63;
	char p[128];

	logfd = open("/userdata/gpioscan.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
	logf("==== scan %d..%d ====\n", start, end);

	for (int g = start; g <= end && n < MAXG; g++) {
		logf("export gpio%d\n", g);
		snprintf(p, sizeof p, "/sys/class/gpio/gpio%d", g);
		if (access(p, F_OK) != 0) {
			wnum("/sys/class/gpio/export", g);
			if (access(p, F_OK) != 0) {
				logf("  gpio%d busy/invalid, skip\n", g);
				continue;
			}
		}
		set_dir(g, "in");
		snprintf(p, sizeof p, "/sys/class/gpio/gpio%d/value", g);
		int fd = open(p, O_RDWR);
		if (fd < 0) {
			logf("  gpio%d open fail, skip\n", g);
			continue;
		}
		pins[n] = g;
		fds[n] = fd;
		n++;
	}
	logf("ready: %d pins\n", n);

	int hi[MAXG], lo[MAXG];
	for (int a = 0; a < n; a++) {
		logf("test gpio%d ...", pins[a]);
		set_dir(pins[a], "out");
		set_val(pins[a], "0");
		usleep(4000);
		for (int i = 0; i < n; i++)
			lo[i] = gval(fds[i]);
		set_val(pins[a], "1");
		usleep(4000);
		for (int i = 0; i < n; i++)
			hi[i] = gval(fds[i]);
		set_dir(pins[a], "in");
		int found = 0;
		for (int b = 0; b < n; b++) {
			if (b == a)
				continue;
			if (lo[b] == 0 && hi[b] == 1) {
				logf("  *** SHORT gpio%d <-> gpio%d ***", pins[a], pins[b]);
				found = 1;
			}
		}
		logf(" %s\n", found ? "FOUND" : "ok");
	}

	for (int i = 0; i < n; i++) {
		set_dir(pins[i], "in");
		close(fds[i]);
		wnum("/sys/class/gpio/unexport", pins[i]);
	}
	logf("==== done %d..%d ====\n", start, end);
	return 0;
}
