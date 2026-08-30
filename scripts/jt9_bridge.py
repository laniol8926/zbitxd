#!/usr/bin/env python3
"""
jt9 decoder bridge for zbitxd -- decoder-merge task (2026-08-29).

Runs jt9 as a second, independent FT8/FT4 decoder alongside zbitxd's own,
and feeds its results back in to be merged (unioned) with our own decodes
-- the union approach validated offline the night before against real
captured 20M audio (jt9 86/18 CQs, our own decoder 69/15 CQs, merged
union 102/21 CQs).

Deliberately a completely separate process, not linked into zbitxd at
all: if this hangs or crashes, zbitxd is entirely unaffected and keeps
running. The two sides only ever communicate through:

  - INPUT: zbitxd itself dumps one WAV file per completed FT8/FT4 slot
    to /tmp/zbitxd_jt9_slot_<toggle>_<HHMMSS>.wav (jt9_dump_slot_wav(),
    modem_ft8.c) -- already exactly 12kHz mono 16-bit PCM, the same
    format jt9 itself expects, no resampling needed. <HHMMSS> is the
    slot's own real wall-clock start time (jt9 has no time reference of
    its own from a bare WAV file -- its own decode output always shows
    a placeholder "000000").

  - OUTPUT: zbitxd's existing remote-command TCP port (remote.c,
    127.0.0.1:8081, no auth -- the same one used for manual/automated
    testing throughout this project) already accepts an "FT8CONTINUE
    <message>" command that feeds a decoded message into ft8_process()
    -- the exact same code path a real decode from our own decoder goes
    through. No new code was needed on zbitxd's receiving end at all.

Field order note: zbitxd's own message shape is
    TIME DT SNR FREQ ~ MSG
(modem_ft8.c's own snprintf: " %+4.1f %+03d %4d ~ ", time_sec, snr, freq)
but jt9's real stdout output is
    TIME SNR DT FREQ ~  MSG
-- SNR and DT are swapped relative to each other. This script corrects
that, and substitutes the WAV filename's own embedded real time for
jt9's "000000" placeholder.

Cleanup: each WAV file is deleted right after this script is done with
it (success or failure alike) -- file *existence* is the only "still
needs processing" signal, nothing else to track. A small self-healing
cap also discards (without processing) anything older than a few cycles
if a backlog ever builds up (jt9 fell behind, or this script wasn't
running for a while) -- bounded disk usage regardless of what state the
consumer was in.

Concurrency: real report, live (2026-08-29, Quadra comparison rig) --
jt9 takes ~15-25s per file against a 15s slot cadence, so even with an
otherwise-idle CPU, strictly sequential processing (one jt9 at a time)
structurally can't keep up: any slot whose decode runs long pushes
every later slot's *processing* behind by that same amount, with no way
to claw it back. Multiple jt9 invocations now run concurrently (see
MAX_CONCURRENT_JT9) so a slow slot no longer blocks the next one from
starting. Each concurrent invocation gets its own scratch subdirectory
(see process_file()'s own comment) -- jt9 writes its own working files
(decoded.txt, FFTW wisdom) relative to cwd, and two jt9s sharing one cwd
would corrupt each other's.
"""

import glob
import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

WAV_GLOB = "/tmp/zbitxd_jt9_slot_*.wav"
# Real bug, caught before it ever shipped: this originally always ran
# "jt9 -8" (FT8) regardless of which mode a slot was actually decoded
# in -- FT4 audio fed through FT8's own symbol timing decodes nothing.
# jt9's real flag for FT4 is "-5"/"--ft4" ("-4" is the unrelated older
# JT4 mode, an easy trap). modem_ft8.c now embeds the mode in the
# filename (jt9_dump_slot_wav()'s own comment) for exactly this.
FILENAME_RE = re.compile(r"zbitxd_jt9_slot_\d+_(\d{2})(\d{2})(\d{2})_(FT4|FT8)\.wav$")
JT9_MODE_FLAG = {"FT8": "-8", "FT4": "-5"}
ZBITXD_HOST = "127.0.0.1"
ZBITXD_PORT = 8081
POLL_INTERVAL_SEC = 0.5
# Real gap, caught live (2026-08-29): on the actual FT8 calling
# frequency (14.074 -- far busier than the quieter samples this was
# first tuned against), a real decode measured 28-32s on this Pi's
# hardware, well past the old 12s timeout -- subprocess.run() was
# killing real, still-in-progress decodes outright instead of just
# delivering them a slot or two late. User's own real-world WSJT-Z GUI
# experience confirms this is normal, not a bug to route around: the
# GUI doesn't finish within one slot either on a busy band, Deep mode
# especially (multi-pass) -- it isn't a problem there because nothing
# times out the computation. 40s gives real margin above the measured
# worst case without being so long a genuinely hung jt9 process (e.g.
# a corrupt WAV) blocks the queue indefinitely -- MAX_PENDING_FILES
# below still caps how far behind a busy frequency can make this
# bridge fall, independent of this timeout.
JT9_TIMEOUT_SEC = 40
# One connect/send/close per decoded line, not one connection carrying
# several -- remote.c's own recv() loop (sbitx_daemon.c/remote.c)
# truncates at the *first* \r or \n it sees in whatever arrived in a
# single recv() call and silently drops the rest, so several lines
# queued into one TCP stream can lose data depending on OS/network
# timing. A fresh connection per line sidesteps that entirely instead
# of trying to time sends around it -- trivial overhead on loopback.
MAX_PENDING_FILES = 6
# How many jt9 processes may run at once -- see this file's own
# "Concurrency" module comment. Sized to leave real headroom for
# zbitxd's own native decode thread (always-on, one full core) rather
# than claiming every core for jt9 alone; tune down on smaller hardware
# (e.g. the Pi Zero 2 W this originally ran on solo) if zbitxd's own
# decoding starts starving.
MAX_CONCURRENT_JT9 = max(1, (os.cpu_count() or 4) - 1)
# Real bug, caught live (2026-08-29): jt9 writes its own scratch file
# (./decoded.txt, relative to whatever its current working directory
# is) as part of a normal decode pass -- subprocess.run() below doesn't
# set cwd, so it inherited whatever directory this script happened to
# be launched from. Running as the zbitxd system user (which this
# bridge needs to, to be able to delete zbitxd's own WAV dumps -- see
# this file's own module comment) with that directory owned by a
# different user (confirmed live: /home/pi) makes jt9 unable to write
# it at all -- "Fortran runtime error: Cannot write to file opened for
# READ" -- and it silently returns just the one decode that happened to
# complete before that crash instead of the real ~15-19 a healthy run
# finds on the exact same audio (confirmed directly: 1 vs 15 on the
# identical WAV file, cwd was the only variable). A dedicated directory
# (not bare /tmp) avoids any chance of two concurrent jt9 runs -- this
# script's own, or an operator's manual test -- colliding on the same
# decoded.txt. Now that this script itself runs jt9 concurrently (see
# "Concurrency" above), each invocation gets its own subdirectory
# under this one instead of sharing it directly -- see process_file().
JT9_CWD = "/tmp/zbitxd_jt9_bridge_scratch"

# jt9 -8 <file> stdout, one decode per line, e.g.:
#   000000 -10  0.1 1501 ~  CS7BHA WX7P R-11
# groups: (snr, dt, freq, message)
# The FT4-capable build (WSJT-Z 2.0.18) uses "+" instead of "~" as the
# field separator for FT4 decodes specifically -- FT8 decodes from the
# same binary still use "~". Confirmed directly: same binary, same
# flags otherwise, -8 output used "~" throughout, -5 output used "+"
# throughout on a real live-captured sample of each.
DECODE_RE = re.compile(r"^\d{6}\s+([+-]?\d+)\s+([+-]?[\d.]+)\s+(\d+)\s+[~+]\s+(\S.*\S|\S)\s*$")


def log(msg):
    print(f"jt9_bridge: {msg}", file=sys.stderr, flush=True)


def send_ft8continue(message):
    try:
        with socket.create_connection((ZBITXD_HOST, ZBITXD_PORT), timeout=5) as sock:
            sock.sendall(f"FT8CONTINUE {message}\r\n".encode("ascii", "replace"))
    except OSError as e:
        log(f"couldn't reach zbitxd on {ZBITXD_HOST}:{ZBITXD_PORT}: {e}")
        return False
    return True


def process_file(path):
    m = FILENAME_RE.search(path)
    if not m:
        log(f"unrecognized filename, skipping: {path}")
        return
    hh, mm, ss, mode = m.groups()
    real_time = hh + mm + ss

    # Own scratch subdirectory per invocation -- see JT9_CWD's own
    # comment. Named from the WAV's own basename, which is already
    # unique (toggle+HHMMSS+mode), so concurrent invocations never
    # collide. Best-effort cleanup in finally -- a leftover directory
    # here is harmless clutter, not a correctness problem, so a failed
    # rmtree (e.g. NFS oddities) is not worth crashing the worker over.
    scratch = os.path.join(JT9_CWD, os.path.basename(path))
    os.makedirs(scratch, exist_ok=True)

    # Streaming, not subprocess.run(): real finding, live (2026-08-30,
    # user's own recollection from an earlier ft8modem integration,
    # confirmed by direct measurement here) -- jt9 writes decoded lines
    # to stdout progressively as it finds them, not all at once at exit.
    # A real captured slot measured 20/21 decodes arriving in the first
    # 4.2s of a 9.2s total run -- the remaining ~5s of runtime produced
    # exactly one more line. subprocess.run()'s capture_output=True only
    # hands back output once the process fully exits, silently sitting
    # on decodes that were already ready seconds earlier. Popen + reading
    # stdout line-by-line sends each one the instant it appears instead,
    # while still letting the process run to natural completion for
    # whatever stragglers show up late -- full latency win for the
    # common case, zero data loss for the rare slow one.
    #
    # timeout is enforced by a watchdog thread (Popen has no built-in
    # timeout the way run() does) rather than by giving up on stragglers
    # early -- killing the process here still lets the for loop below
    # see EOF and exit cleanly, same outcome subprocess.run()'s own
    # TimeoutExpired path had, just without discarding lines already read.
    try:
        proc = subprocess.Popen(
            # -w 0: FFTW3 planning patience, default is 1 -- measured live
            # (real backlogged WAV, same file, both runs): 30.3s at the
            # default vs 21.1s at 0, byte-identical decode output (32/32
            # lines, no diff) -- pure planning-time waste at the default,
            # not a sensitivity tradeoff. -m (FFT threads) tested
            # separately and gave no real gain (30.0s vs 30.3s baseline)
            # -- jt9's cost is dominated by single-threaded FT8/LDPC work,
            # not the large-FFT step -m parallelizes.
            ["jt9", "-w", "0", JT9_MODE_FLAG[mode], path],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            text=True, bufsize=1,  # line-buffered
            cwd=scratch,
        )
    except OSError as e:
        log(f"jt9 failed on {path}: {e}")
        shutil.rmtree(scratch, ignore_errors=True)
        return

    timed_out = threading.Event()

    def _kill_on_timeout():
        timed_out.set()
        proc.kill()

    watchdog = threading.Timer(JT9_TIMEOUT_SEC, _kill_on_timeout)
    watchdog.start()

    n_sent = 0
    try:
        for raw in proc.stdout:
            dm = DECODE_RE.match(raw.rstrip("\n"))
            if not dm:
                continue
            snr, dt, freq, msg = dm.groups()
            # TIME DT SNR FREQ ~ MSG -- see this file's own module comment
            if send_ft8continue(f"{real_time} {dt} {snr} {freq} ~ {msg}"):
                n_sent += 1
    finally:
        watchdog.cancel()
        proc.wait()
        shutil.rmtree(scratch, ignore_errors=True)

    if timed_out.is_set():
        log(f"jt9 timed out on {path} after {JT9_TIMEOUT_SEC}s ({n_sent} decode(s) sent before kill)")
    if n_sent:
        log(f"{os.path.basename(path)}: {n_sent} decode(s) sent")


def process_and_remove(path):
    try:
        process_file(path)
    finally:
        try:
            os.remove(path)
        except OSError:
            pass


def prune_backlog(in_flight):
    # See MAX_PENDING_FILES's own comment -- self-healing bound on
    # unprocessed accumulation, independent of the main per-file
    # processing loop below. in_flight (files a worker already owns)
    # are excluded from both the count and the discard candidates --
    # they're being actively worked, not backlog.
    paths = sorted(glob.glob(WAV_GLOB), key=lambda p: os.path.getmtime(p) if os.path.exists(p) else 0)
    pending = [p for p in paths if p not in in_flight]
    if len(pending) <= MAX_PENDING_FILES:
        return pending
    stale = pending[: len(pending) - MAX_PENDING_FILES]
    for p in stale:
        log(f"backlog too deep, discarding unprocessed: {p}")
        try:
            os.remove(p)
        except OSError:
            pass
    return pending[len(pending) - MAX_PENDING_FILES:]


def main():
    os.makedirs(JT9_CWD, exist_ok=True)
    log(f"watching {WAV_GLOB}, feeding {ZBITXD_HOST}:{ZBITXD_PORT}, up to {MAX_CONCURRENT_JT9} jt9(s) at once")
    in_flight = {}  # path -> Future
    with ThreadPoolExecutor(max_workers=MAX_CONCURRENT_JT9) as pool:
        while True:
            done = [p for p, fut in in_flight.items() if fut.done()]
            for p in done:
                del in_flight[p]

            for path in prune_backlog(set(in_flight)):
                if not os.path.exists(path) or path in in_flight:
                    continue
                if len(in_flight) >= MAX_CONCURRENT_JT9:
                    break
                in_flight[path] = pool.submit(process_and_remove, path)

            time.sleep(POLL_INTERVAL_SEC)


if __name__ == "__main__":
    main()
