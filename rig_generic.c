// Minimal rigctld TCP client: connects out to a running rigctld (already
// confirmed working against a QMX via `rigctl -m 2 -r 127.0.0.1:4532`) and
// sends the same short-form wire commands rigctld's own clients use. This is
// the CAT half of the generic-rig backend; sound_generic.c is the audio half.
// Mirrors hamlib.c's socket handling but as a client instead of a server --
// zbitxd's existing hamlib.c goes the opposite direction (it lets external
// hamlib apps control the zBitx), which isn't reusable here.

#include <stdio.h>
#include <string.h>
#include <unistd.h>
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

static void rigctld_stop(void)
{
	if (rigctld_pid > 0) {
		kill(rigctld_pid, SIGTERM);
		waitpid(rigctld_pid, NULL, 0);
		rigctld_pid = -1;
	}
}

void rig_generic_connect(const char *model, const char *device)
{
	char port_str[16];

	rig_drop();
	rigctld_stop();

	snprintf(port_str, sizeof(port_str), "%d", generic_rigctld_port);

	rigctld_pid = fork();
	if (rigctld_pid == 0) {
		execlp("rigctld", "rigctld", "-m", model, "-r", device, "-t", port_str, (char *)NULL);
		// only reached if execlp() itself failed
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
