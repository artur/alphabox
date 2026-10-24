#!/usr/bin/env python3
"""Drive the SRM console of a running Alphabox over its telnet serial port.

Connects, waits for the P00>>> prompt, runs console commands one by one, then
optionally stops the emulator. Everything received (NULs removed) goes to the
log; the output of the commands alone goes to --cmd-log when given.

Status lines on stdout:
  status=prompt                      prompt reached
  status=noconnect / status=noprompt
  status=no-prompt-after: <cmd>      a command didn't return to the prompt
                                     (expected for commands that start guest
                                     code, e.g. "boot")
  status=exited after N.Ns           the emulator process ended (--pid)
  status=still-running after N.Ns

Stopping (--after, needs --pid):
  none                 leave it running (default)
  sigterm              send SIGTERM at the prompt / after the commands
  disconnect-sigterm   close the telnet connection, wait 3 s, then SIGTERM
  sigusr1              send SIGUSR1 (a snapshot, docs/headless.md) with the
                       connection still open, and wait for the process to
                       end (with ALPHABOX_SNAPSHOT_EXIT=1 it exits after it)
  wait-exit            only wait for the process to end (e.g. a guest HALT
                       with exit_on_pal_halt)

--wake sends a carriage return once connected, and again after every 3 s
without output, until the prompt comes: a machine resumed from a snapshot
(ALPHABOX_RESTORE) printed its prompt before it was saved.
"""
import argparse
import os
import signal
import socket
import sys
import time

PROMPT = b"P00>>>"
# A single-processor console prints a bare ">>>" instead. Only at the end of
# what has arrived, and only on a line of its own: the consoles also write
# ">>>init" inside sentences telling you what to type.
PROMPT_ALT = b"\n>>>"


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--log", required=True, help="full console log")
    ap.add_argument("--cmd-log", help="output of the commands only")
    ap.add_argument("--timeout", type=int, default=300, help="wait for the first prompt")
    ap.add_argument("--cmd", action="append", default=[], help="console command (repeatable)")
    ap.add_argument("--cmd-timeout", type=int, default=30)
    ap.add_argument("--pid", type=int)
    ap.add_argument("--after", default="none",
                    choices=["none", "sigterm", "disconnect-sigterm", "sigusr1",
                             "wait-exit"])
    ap.add_argument("--wake", action="store_true",
                    help="send a carriage return until the first prompt")
    ap.add_argument("--exit-timeout", type=int, default=20)
    args = ap.parse_args()
    if args.after != "none" and not args.pid:
        ap.error("--after needs --pid")

    log = open(args.log, "wb")
    cmd_log = open(args.cmd_log, "wb") if args.cmd_log else None

    # Retry quickly: the emulator opens its telnet port a moment after it
    # starts, and a one-second retry quantised every timed run to whole
    # seconds -- which is a quarter of what cpu_bench.sh used to measure.
    s = None
    for _ in range(800):
        try:
            s = socket.create_connection(("127.0.0.1", args.port), timeout=2)
            break
        except OSError:
            time.sleep(0.05)
    if s is None:
        print("status=noconnect")
        return 1
    s.settimeout(1)

    # The port escapes a data byte 0xff as IAC IAC (RFC 854), as a telnet
    # client expects; undo that, so a log shows what the guest sent. Other
    # telnet commands pass through as they always have. An IAC that ends a
    # read waits for the next one.
    iac_pending = [False]

    def unescape(d):
        out = bytearray()
        for b in d:
            if iac_pending[0]:
                iac_pending[0] = False
                out.append(0xff)
                if b == 0xff:
                    continue
            if b == 0xff:
                iac_pending[0] = True
                continue
            out.append(b)
        return bytes(out)

    def pump(secs, sink=None, wake=False):
        """Read until the prompt (True), EOF/timeout (False)."""
        buf = b""
        end = time.time() + secs
        quiet = time.time()
        if wake:
            s.sendall(b"\r")
        while time.time() < end:
            try:
                d = s.recv(4096)
            except socket.timeout:
                if wake and time.time() - quiet >= 3:
                    s.sendall(b"\r")
                    quiet = time.time()
                continue
            except OSError:
                return False
            if not d:
                return False
            quiet = time.time()
            d = unescape(bytes(b for b in d if b != 0))
            log.write(d)
            log.flush()
            if sink:
                sink.write(d)
                sink.flush()
            buf += d
            if PROMPT in buf[-64:] or buf[-64:].rstrip().endswith(PROMPT_ALT):
                return True
        return False

    if not pump(args.timeout, wake=args.wake):
        print("status=noprompt")
        return 1
    print("status=prompt")

    # --wake sends a carriage return every 3 s until a prompt: a console slow
    # to answer (an EV7's, after a restore) leaves a second prompt coming.
    # Let the line go quiet, or that prompt ends the first command's wait.
    if args.wake:
        while True:
            try:
                d = s.recv(4096)
            except OSError:  # a second of quiet (socket.timeout), or gone
                break
            if not d:
                break
            log.write(unescape(bytes(b for b in d if b != 0)))
            log.flush()

    for c in args.cmd:
        s.sendall(c.encode() + b"\r")
        if not pump(args.cmd_timeout, cmd_log):
            print("status=no-prompt-after: " + c)
            break

    if args.after == "none":
        return 0
    t0 = time.time()
    if args.after == "sigterm":
        os.kill(args.pid, signal.SIGTERM)
    elif args.after == "sigusr1":
        os.kill(args.pid, signal.SIGUSR1)
    elif args.after == "disconnect-sigterm":
        s.close()
        time.sleep(3)
        t0 = time.time()
        os.kill(args.pid, signal.SIGTERM)
    end = time.time() + args.exit_timeout
    while time.time() < end and alive(args.pid):
        time.sleep(0.2)
    state = "still-running" if alive(args.pid) else "exited"
    print("status=%s after %.1fs" % (state, time.time() - t0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
