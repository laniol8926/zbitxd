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
"""

import glob
import os
import re
import socket
import subprocess
import sys
import time

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
JT9_TIMEOUT_SEC = 12
# One connect/send/close per decoded line, not one connection carrying
# several -- remote.c's own recv() loop (sbitx_daemon.c/remote.c)
# truncates at the *first* \r or \n it sees in whatever arrived in a
# single recv() call and silently drops the rest, so several lines
# queued into one TCP stream can lose data depending on OS/network
# timing. A fresh connection per line sidesteps that entirely instead
# of trying to time sends around it -- trivial overhead on loopback.
MAX_PENDING_FILES = 6
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
# decoded.txt.
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

    try:
        result = subprocess.run(
            ["jt9", JT9_MODE_FLAG[mode], path],
            capture_output=True, text=True, timeout=JT9_TIMEOUT_SEC,
            cwd=JT9_CWD,
        )
    except (OSError, subprocess.TimeoutExpired) as e:
        log(f"jt9 failed on {path}: {e}")
        return

    n_sent = 0
    for raw in result.stdout.splitlines():
        dm = DECODE_RE.match(raw)
        if not dm:
            continue
        snr, dt, freq, msg = dm.groups()
        # TIME DT SNR FREQ ~ MSG -- see this file's own module comment
        if send_ft8continue(f"{real_time} {dt} {snr} {freq} ~ {msg}"):
            n_sent += 1
        time.sleep(0.02)  # trivial spacing, not required for correctness -- just avoids a tight burst of connects
    if n_sent:
        log(f"{os.path.basename(path)}: {n_sent} decode(s) sent")


def prune_backlog():
    # See MAX_PENDING_FILES's own comment -- self-healing bound on
    # unprocessed accumulation, independent of the main per-file
    # processing loop below.
    paths = sorted(glob.glob(WAV_GLOB), key=lambda p: os.path.getmtime(p) if os.path.exists(p) else 0)
    if len(paths) <= MAX_PENDING_FILES:
        return paths
    stale = paths[: len(paths) - MAX_PENDING_FILES]
    for p in stale:
        log(f"backlog too deep, discarding unprocessed: {p}")
        try:
            os.remove(p)
        except OSError:
            pass
    return paths[len(paths) - MAX_PENDING_FILES:]


def main():
    os.makedirs(JT9_CWD, exist_ok=True)
    log(f"watching {WAV_GLOB}, feeding {ZBITXD_HOST}:{ZBITXD_PORT}")
    while True:
        for path in prune_backlog():
            if not os.path.exists(path):
                continue
            process_file(path)
            try:
                os.remove(path)
            except OSError:
                pass
        time.sleep(POLL_INTERVAL_SEC)


if __name__ == "__main__":
    main()
