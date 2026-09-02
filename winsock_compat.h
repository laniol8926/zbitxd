// Windows port sketch: shared socket-API compatibility layer for the
// five files that do real socket work -- logbook.c (UDP QSO broadcast
// to a logger), modems.c (fldigi TCP bridge), hamlib.c/remote.c (each
// hosts a small TCP command server), rig_generic.c (rigctld TCP
// client). One shared header instead of five separate, inevitably-
// inconsistent #ifdef blocks.
//
// Real behavioral differences from POSIX sockets here, not just header
// renames -- read before assuming a file that #includes this is fully
// ported just because it now compiles:
//
//  - Windows sockets need one-time process startup/shutdown
//    (WSAStartup()/WSACleanup()) that POSIX sockets never needed at
//    all. winsock_compat_init()/winsock_compat_cleanup() below wrap
//    that (no-ops on non-Windows) -- winsock_compat_init() must be
//    called once, early in main() (sbitx_daemon.c), before ANY socket
//    call anywhere in the program; not wired in by this header alone.
//
//  - socket()/accept() return SOCKET (an unsigned handle type) on
//    Windows, not a plain int, and their failure sentinel is
//    INVALID_SOCKET -- NOT a negative value the way POSIX's -1 is.
//    Every one of this project's own socket()/accept() call sites
//    stores the result in a plain `int` and checks `< 0` for failure.
//    That happens to still work correctly for a 32-bit Windows build
//    specifically (SOCKET is 32-bit there, matching int's own width,
//    and INVALID_SOCKET's all-ones bit pattern reads back as -1 when
//    reinterpreted as a signed 32-bit int) -- confirmed by reasoning
//    through the actual bit representation, not assumed. It is NOT a
//    generally-portable pattern (a 64-bit Windows build, where SOCKET
//    is 64-bit, isn't covered by that same reasoning) -- flagged at
//    each call site with a comment rather than silently left
//    unexplained, so a future 64-bit build doesn't quietly inherit a
//    real bug here.
//
//  - Errors come from WSAGetLastError(), not errno.
//
//  - close() doesn't work on a socket handle at all on Windows --
//    closesocket() does. #define'd below; safe to blanket-apply within
//    any file that includes this header specifically because each of
//    the five files above was checked, file-by-file, to only ever call
//    close() on a socket (real file descriptors in all five go through
//    fclose()/pclose() instead, a distinct C function this doesn't
//    touch).
//
//  - Non-blocking mode: POSIX uses
//    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); Windows uses
//    ioctlsocket(fd, FIONBIO, &mode) instead -- a real function-shape
//    difference (different arguments entirely), not a rename, so
//    deliberately NOT blanket-handled here. hamlib.c and remote.c both
//    call fcntl() this way for their own listening sockets -- still
//    open, not done as part of this header.

#ifndef WINSOCK_COMPAT_H
#define WINSOCK_COMPAT_H

#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>
#define close closesocket

// SOCK_NONBLOCK (a Linux-specific socket()-creation flag, combining
// socket()+fcntl(O_NONBLOCK) into one call) doesn't exist on Windows at
// all -- defined here as a harmless 0 (ORing in 0 changes nothing) so
// hamlib.c/remote.c's own `socket(PF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)`
// calls keep compiling unchanged; winsock_set_nonblocking() below is the
// real fix, called explicitly right after socket()/accept() instead.
#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 0
#endif

static inline int winsock_compat_init(void){
	WSADATA wsa;
	return WSAStartup(MAKEWORD(2, 2), &wsa);
}
static inline void winsock_compat_cleanup(void){
	WSACleanup();
}

// Real function-shape difference, not a rename: POSIX sets non-blocking
// mode via fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); Windows
// has no fcntl() on sockets at all, ioctlsocket(fd, FIONBIO, &mode)
// instead (mode != 0 enables non-blocking). Wrapped here once, used at
// both call sites this matters for in hamlib.c/remote.c (the listening
// socket, previously via SOCK_NONBLOCK above, and the accepted
// connection socket, previously via an explicit fcntl() call) instead
// of duplicating an #ifdef at each one.
static inline void winsock_set_nonblocking(int fd){
	u_long mode = 1;
	ioctlsocket(fd, FIONBIO, &mode);
}

#else

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h> // TCP_NODELAY -- distinct from netinet/in.h, easy to drop by mistake
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>

static inline int winsock_compat_init(void){ return 0; }
static inline void winsock_compat_cleanup(void){ }

static inline void winsock_set_nonblocking(int fd){
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

#endif

#endif // WINSOCK_COMPAT_H
