// Minimal rigctld TCP client: connects out to a running rigctld (already
// confirmed working against a QMX via `rigctl -m 2 -r 127.0.0.1:4532`) and
// sends the same short-form wire commands rigctld's own clients use. This is
// the CAT half of the generic-rig backend; sound_generic.c is the audio half.
// Mirrors hamlib.c's socket handling but as a client instead of a server --
// zbitxd's existing hamlib.c goes the opposite direction (it lets external
// hamlib apps control the zBitx), which isn't reusable here.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <signal.h>
#include <errno.h>
#include <complex.h>
#include <math.h>
#include <fftw3.h>
#include "sdr.h"
#include "rig_generic.h"

static int rig_sock = -1;
static pid_t rigctld_pid = -1;

static int rig_connect(void)
{
	struct addrinfo hints, *res = NULL;
	char port_str[16];
	int s, one = 1;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	snprintf(port_str, sizeof(port_str), "%d", generic_rigctld_port);

	if (getaddrinfo(generic_rigctld_host, port_str, &hints, &res) != 0)
		return -1;

	s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (s < 0) {
		freeaddrinfo(res);
		return -1;
	}
	if (connect(s, res->ai_addr, res->ai_addrlen) < 0) {
		close(s);
		freeaddrinfo(res);
		return -1;
	}
	freeaddrinfo(res);

	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	return s;
}

// (Re)connects on demand rather than tracking staleness elsewhere -- a
// rigctld TCP connection is cheap to reopen, and every command path already
// has to handle a failed send/recv, so a dropped link just reconnects on
// the next call.
static int rig_ensure_connected(void)
{
	if (rig_sock >= 0)
		return rig_sock;

	rig_sock = rig_connect();
	if (rig_sock < 0)
		fprintf(stderr, "rig_generic: could not connect to rigctld at %s:%d\n",
			generic_rigctld_host, generic_rigctld_port);
	return rig_sock;
}

static void rig_drop(void)
{
	if (rig_sock >= 0)
		close(rig_sock);
	rig_sock = -1;
}

static void rig_send_command(const char *cmd)
{
	char buf[128];
	int n;

	if (rig_ensure_connected() < 0)
		return;

	n = snprintf(buf, sizeof(buf), "%s\n", cmd);
	if (send(rig_sock, buf, n, 0) != n) {
		rig_drop();
		return;
	}

	n = recv(rig_sock, buf, sizeof(buf) - 1, 0);
	if (n <= 0) {
		rig_drop();
		return;
	}
	buf[n] = 0;

	if (strncmp(buf, "RPRT 0", 6))
		fprintf(stderr, "rig_generic: unexpected reply to '%s': %s", cmd, buf);
}

void rig_generic_init(void)
{
	rig_ensure_connected();
}

// `rigctl --list` output is a fixed-width table, but manufacturer/model
// names can themselves contain single spaces (e.g. "N2ADR James
// Ahlstrom", "FT-1000MP MARK-V"), so columns are found by runs of 2+
// spaces rather than fixed offsets -- more robust against hamlib
// changing its exact column widths across versions.
void rig_generic_list(char *out, size_t out_size)
{
	FILE *pf;
	char line[256];
	size_t used = 0;
	int first = 1;

	out[0] = 0;
	pf = popen("rigctl --list", "r");
	if (!pf)
		return;

	while (fgets(line, sizeof(line), pf)) {
		if (first) {
			first = 0;
			continue; // header row
		}

		char *p = line;
		while (*p == ' ')
			p++;
		if (*p < '0' || *p > '9')
			continue; // not a data row

		int id = atoi(p);
		while (*p >= '0' && *p <= '9')
			p++;
		while (*p == ' ')
			p++; // skip to Mfg -- don't count this run as a column boundary

		char name[128];
		int ni = 0, boundaries = 0, space_run = 0;
		for (; *p && *p != '\n'; p++) {
			if (*p == ' ') {
				space_run++;
				if (space_run == 2)
					boundaries++;
				if (boundaries >= 2)
					break;
			} else {
				if (space_run >= 1 && ni > 0)
					name[ni++] = ' ';
				space_run = 0;
				if (ni < (int)sizeof(name) - 1)
					name[ni++] = *p;
			}
		}
		name[ni] = 0;

		char entry[192];
		int n = snprintf(entry, sizeof(entry), "%d %s\n", id, name);
		if (n <= 0)
			continue;
		if (used + (size_t)n >= out_size)
			break;
		memcpy(out + used, entry, (size_t)n);
		used += (size_t)n;
	}
	out[used] = 0;

	pclose(pf);
}

static void rigctld_stop(void)
{
	if (rigctld_pid > 0) {
		kill(rigctld_pid, SIGTERM);
		waitpid(rigctld_pid, NULL, 0);
		rigctld_pid = -1;
	}
}

// /dev/serial/by-id/* gives stable, device-identity-based names (survive
// reboots/replugging in a different order); /dev/ttyACM0-style names are
// assigned by enumeration order and can point at a different physical
// device from one boot to the next. Lists by-id entries if any exist,
// otherwise falls back to raw /dev/ttyACM*/ttyUSB* so there's still
// something to pick from.
void rig_generic_list_serial_devices(char *out, size_t out_size)
{
	DIR *d;
	struct dirent *e;
	size_t used = 0;

	out[0] = 0;

	d = opendir("/dev/serial/by-id");
	if (d) {
		while ((e = readdir(d))) {
			if (e->d_name[0] == '.')
				continue;
			char entry[320];
			int n = snprintf(entry, sizeof(entry), "/dev/serial/by-id/%s\n", e->d_name);
			if (n <= 0 || used + (size_t)n >= out_size)
				continue;
			memcpy(out + used, entry, (size_t)n);
			used += (size_t)n;
		}
		closedir(d);
	}

	if (used == 0) {
		d = opendir("/dev");
		if (!d)
			return;
		while ((e = readdir(d))) {
			if (strncmp(e->d_name, "ttyACM", 6) && strncmp(e->d_name, "ttyUSB", 6))
				continue;
			char entry[280];
			int n = snprintf(entry, sizeof(entry), "/dev/%s\n", e->d_name);
			if (n <= 0 || used + (size_t)n >= out_size)
				continue;
			memcpy(out + used, entry, (size_t)n);
			used += (size_t)n;
		}
		closedir(d);
	}
	out[used] = 0;
}

// `aplay -l`'s simple "card N: NAME [...]" listing turned into ready-to-use
// plughw:NAME,0 device strings for the Capture/Playback Device fields.
void rig_generic_list_audio_devices(char *out, size_t out_size)
{
	FILE *pf;
	char line[256];
	size_t used = 0;

	out[0] = 0;
	pf = popen("aplay -l 2>/dev/null", "r");
	if (!pf)
		return;

	while (fgets(line, sizeof(line), pf)) {
		char *p = strstr(line, "card ");
		if (p != line)
			continue; // only lines starting with "card "
		char *colon = strchr(p, ':');
		if (!colon)
			continue;
		char *name_start = strchr(colon, ' ');
		if (!name_start)
			continue;
		name_start++;
		// the bare card name ends at the first space or '[' -- what
		// follows (e.g. "[USB Interface mchf], device 0: ...") is a
		// human-readable description, not part of the actual card
		// identifier plughw: expects
		char *name_end = name_start;
		while (*name_end && *name_end != ' ' && *name_end != '[' && *name_end != ',')
			name_end++;
		if (name_end == name_start)
			continue;

		char card[128];
		size_t len = (size_t)(name_end - name_start);
		if (len >= sizeof(card))
			len = sizeof(card) - 1;
		memcpy(card, name_start, len);
		card[len] = 0;

		char entry[192];
		int n = snprintf(entry, sizeof(entry), "plughw:%s,0\n", card);
		if (n <= 0 || used + (size_t)n >= out_size)
			continue;
		memcpy(out + used, entry, (size_t)n);
		used += (size_t)n;
	}
	out[used] = 0;

	pclose(pf);
}

void rig_generic_connect(const char *model, const char *device, const char *baud)
{
	char port_str[16];
	char *argv[10];
	int i = 0;

	rig_drop();
	rigctld_stop();

	snprintf(port_str, sizeof(port_str), "%d", generic_rigctld_port);

	argv[i++] = "rigctld";
	argv[i++] = "-m";
	argv[i++] = (char *)model;
	argv[i++] = "-r";
	argv[i++] = (char *)device;
	if (baud && baud[0]) {
		argv[i++] = "-s";
		argv[i++] = (char *)baud;
	}
	argv[i++] = "-t";
	argv[i++] = port_str;
	argv[i] = NULL;

	rigctld_pid = fork();
	if (rigctld_pid == 0) {
		execvp("rigctld", argv);
		// only reached if execvp() itself failed
		fprintf(stderr, "rig_generic: failed to exec rigctld: %s\n", strerror(errno));
		_exit(1);
	} else if (rigctld_pid < 0) {
		fprintf(stderr, "rig_generic: fork() failed, could not start rigctld\n");
		rigctld_pid = -1;
		return;
	}

	// give rigctld a moment to open its listening socket rather than
	// assuming a fixed delay is long enough
	for (int attempt = 0; attempt < 20; attempt++) {
		usleep(100000);
		if (rig_ensure_connected() >= 0)
			return;
	}
	fprintf(stderr, "rig_generic: rigctld started (pid %d) but did not accept a connection\n",
		rigctld_pid);
}

void rig_generic_set_freq(long freq_hz)
{
	char cmd[64];
	snprintf(cmd, sizeof(cmd), "F %ld", freq_hz);
	rig_send_command(cmd);
}

void rig_generic_set_ptt(int on)
{
	rig_send_command(on ? "T 1" : "T 0");
}
