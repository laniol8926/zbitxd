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

// hamlib model id of whichever rig is currently connected (set in
// rig_generic_connect()) -- rig_generic_set_mode() needs this to know
// which CAT mode string actually means "digital" for this specific rig.
static int rig_generic_model_id = 0;

// QRP Labs QMX (hamlib model 2057): the only rig looked at so far with a
// dedicated digital CAT mode (PKTUSB) distinct from voice USB -- confirmed
// via `rigctl --dump-caps -m 2057`, mode list is CW/CWR/PKTUSB/PKTLSB
// only, no plain USB/LSB at all (as of hamlib 4.7.1/4.7.2/current master).
// This app's own generic-rig backend always captures from the QMX's own
// built-in USB audio interface, which is what PKTUSB corresponds to --
// running digital via an external interface (e.g. a Digirig wired to the
// QMX's audio-out/mic jacks, CAT still over the QMX's own USB) would be a
// different setup this backend doesn't use, so that distinction doesn't
// change this mapping.
#define RIG_MODEL_QRPLABS_QMX_ID 2057

// A system-packaged libhamlib4 (e.g. Debian's, often years behind) and a
// newer from-source build can both register under the identical
// "libhamlib.so.4" SONAME; the dynamic linker's cache then picks
// whichever was indexed first, silently, regardless of which one a
// given rigctld binary was actually built/tested against -- confirmed on
// the dev box: a plain `rigctld -m 2057 ...` resolved to the apt lib and
// reported "Unknown rig num 2057" even though a from-source build with
// real QMX support was sitting right there in /usr/local. Forcing this
// specific binary + its own lib dir sidesteps that regardless of
// whatever else is installed system-wide. Falls back to a bare PATH
// lookup (previous behaviour) if this build isn't present, e.g. on a
// box that only has the system package.
#define RIGCTLD_PREFERRED_PATH "/usr/local/bin/rigctld"
#define RIGCTLD_PREFERRED_LIBDIR "/usr/local/lib"

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

// Reads the rig's actual current VFO frequency via CAT ("f") -- a
// separate function from rig_send_command() since GET-style rigctld
// commands reply with the value itself, not "RPRT 0" (which
// rig_send_command() specifically checks for and would misreport as
// an "unexpected reply" for every successful query). Returns -1 on
// any failure (not connected, send/recv error, unparseable reply).
long rig_generic_get_freq(void)
{
	char buf[128];
	int n;

	if (rig_ensure_connected() < 0)
		return -1;

	n = snprintf(buf, sizeof(buf), "f\n");
	if (send(rig_sock, buf, n, 0) != n) {
		rig_drop();
		return -1;
	}

	n = recv(rig_sock, buf, sizeof(buf) - 1, 0);
	if (n <= 0) {
		rig_drop();
		return -1;
	}
	buf[n] = 0;

	char *end;
	long freq = strtol(buf, &end, 10);
	if (end == buf)
		return -1; // not a number -- e.g. an "RPRT -N" error reply

	return freq;
}

void rig_generic_set_mode(const char *app_mode)
{
	char cmd[64];
	const char *cat_mode;

	if (!strcmp(app_mode, "FT8") || !strcmp(app_mode, "FT4")) {
		// QMX: dedicated digital mode. Every other rig looked at so far
		// (RS-978: no separate digital mode, shares USB with voice;
		// QDX: no voice capability at all, so no USB/PKTUSB distinction
		// to make in the first place) just uses USB for digital too.
		cat_mode = (rig_generic_model_id == RIG_MODEL_QRPLABS_QMX_ID) ? "PKTUSB" : "USB";
	} else {
		// voice (app mode "USB") -- sent as-is. Not every connected rig
		// can actually do this (the QMX's hamlib capability table has
		// no voice mode at all as of this writing, pending either a
		// hamlib update or a local ts480.c patch; QDX has no SSB
		// modulator/mic input in hardware at all) -- this layer sends
		// the CAT command regardless and lets the radio/hamlib backend
		// reject it if it can't actually be done, rather than trying
		// to hide that per-rig limitation here.
		cat_mode = "USB";
	}

	// passband 0 = let rigctld use that mode's own default passband
	// width for the connected rig, rather than guessing a width here
	snprintf(cmd, sizeof(cmd), "M %s 0", cat_mode);
	rig_send_command(cmd);
}

void rig_generic_init(void)
{
	rig_ensure_connected();
}

// `rigctl --list` output is a fixed-width table, but manufacturer/model
// names can themselves contain single spaces (e.g. "N2ADR James
// Ahlstrom", "FT-1000MP MARK-V"). Anchored from the END of the line
// instead of counting column-separator runs from the front: the
// trailing Version/Status/Macro fields are always single space-free
// tokens in hamlib's own output (confirmed across a real, full ~311-rig
// catalog), so stripping exactly those 3 trailing tokens leaves
// "Mfg Model" regardless of internal spacing. A front-counted
// "runs of 2+ spaces = column boundary" approach was tried first and
// broke on real data: a long Mfg name (e.g. "DTTS Microwave Society")
// can overflow its allotted column width enough to compress the
// Mfg/Model gap down to a single space -- indistinguishable from the
// name's own internal word-spaces -- which shifted the front-counted
// boundary by one column and swallowed the Version field into the
// name (confirmed live: "23003 DTTS Microwave Society DttSP IPC
// 20200319.0" instead of stopping at "...DttSP IPC"). Anchoring from
// the end sidesteps this entirely since it never depends on any
// column's padding actually reaching the 2-space threshold.
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

		size_t len = strlen(line);
		while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
			line[--len] = 0;
		char *end = line + len;

		// strip exactly 3 trailing whitespace-separated tokens
		// (Macro, Status, Version)
		for (int t = 0; t < 3 && end > p; t++) {
			while (end > p && *(end-1) == ' ')
				end--;
			while (end > p && *(end-1) != ' ')
				end--;
		}
		while (end > p && *(end-1) == ' ')
			end--; // trailing spaces before the (now excluded) Version token

		while (*p >= '0' && *p <= '9')
			p++;
		while (*p == ' ')
			p++;

		if (p >= end)
			continue; // malformed row -- fewer than 3 trailing tokens

		char name[128];
		int ni = 0, space_run = 0;
		for (char *q = p; q < end; q++) {
			if (*q == ' ') {
				space_run++;
			} else {
				if (space_run >= 1 && ni > 0 && ni < (int)sizeof(name) - 1)
					name[ni++] = ' ';
				space_run = 0;
				if (ni < (int)sizeof(name) - 1)
					name[ni++] = *q;
			}
		}
		name[ni] = 0;
		if (ni == 0)
			continue;

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
// device from one boot to the next. Lists by-id entries (preferred),
// plus any /dev/ttyACM*/ttyUSB* not already represented by one -- not an
// either/or fallback (a device that never gets a by-id entry, e.g. some
// generic/no-serial-number USB-serial chips, would otherwise be
// completely invisible here the moment ANY other attached device does
// get one, since the old code treated "by-id has ANY entries" as "by-id
// covers everything").
void rig_generic_list_serial_devices(char *out, size_t out_size)
{
	DIR *d;
	struct dirent *e;
	size_t used = 0;
	// basenames (e.g. "ttyACM0") already represented by a by-id symlink,
	// resolved via readlink() -- checked against the raw /dev scan below
	// so it can skip them instead of assuming by-id is all-or-nothing
	char covered[16][64];
	int n_covered = 0;

	out[0] = 0;

	d = opendir("/dev/serial/by-id");
	if (d) {
		while ((e = readdir(d))) {
			if (e->d_name[0] == '.')
				continue;
			char entry[320];
			int n = snprintf(entry, sizeof(entry), "/dev/serial/by-id/%s\n", e->d_name);
			if (n > 0 && used + (size_t)n < out_size) {
				memcpy(out + used, entry, (size_t)n);
				used += (size_t)n;
			}

			if (n_covered < 16) {
				char link_path[320], resolved[64];
				snprintf(link_path, sizeof(link_path), "/dev/serial/by-id/%s", e->d_name);
				ssize_t rl = readlink(link_path, resolved, sizeof(resolved) - 1);
				if (rl > 0) {
					resolved[rl] = 0;
					// symlink target is normally relative (e.g.
					// "../../ttyACM0") -- basename it so it compares
					// directly against the bare names found below
					char *base = strrchr(resolved, '/');
					base = base ? base + 1 : resolved;
					snprintf(covered[n_covered++], sizeof(covered[0]), "%s", base);
				}
			}
		}
		closedir(d);
	}

	d = opendir("/dev");
	if (d) {
		while ((e = readdir(d))) {
			if (strncmp(e->d_name, "ttyACM", 6) && strncmp(e->d_name, "ttyUSB", 6))
				continue;

			int already_listed = 0;
			for (int i = 0; i < n_covered; i++)
				if (!strcmp(covered[i], e->d_name)) {
					already_listed = 1;
					break;
				}
			if (already_listed)
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

		// a single physical card can expose more than one PCM device
		// (e.g. a multi-input/output USB interface) -- aplay -l repeats
		// the "card N: id [...]" header once per device, with the real
		// index after ", device ". Previously hardcoded to ",0"
		// regardless, which duplicated the first device's entry and
		// made any device beyond 0 on such a card unreachable here --
		// not yet seen on real hardware (every rig tested so far
		// exposes exactly one device), but a real latent bug.
		int device_index = 0;
		char *dev_marker = strstr(colon, ", device ");
		if (dev_marker)
			device_index = atoi(dev_marker + 9);

		char entry[192];
		int n = snprintf(entry, sizeof(entry), "plughw:%s,%d\n", card, device_index);
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

	rig_generic_model_id = atoi(model);
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
		if (access(RIGCTLD_PREFERRED_PATH, X_OK) == 0) {
			setenv("LD_LIBRARY_PATH", RIGCTLD_PREFERRED_LIBDIR, 1);
			execv(RIGCTLD_PREFERRED_PATH, argv);
		} else {
			execvp("rigctld", argv);
		}
		// only reached if exec itself failed
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

int rig_generic_is_connected(void)
{
	return rig_sock >= 0;
}
