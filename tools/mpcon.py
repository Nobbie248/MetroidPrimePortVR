#!/usr/bin/env python3
"""Client for the debug command console (MP_CONSOLE=<port>, any build).

    mpcon.py status                      # one command
    mpcon.py 'warp chozo 492CBF4A' 'objs eyeball' shot
    mpcon.py                             # interactive prompt
    mpcon.py -f script.txt               # one command per line, '#' comments

Each command prints the game's reply. Exits 1 if any command fails, 2 if the
game cannot be reached. --port (default 4777) or MPCON_PORT picks the port;
--wait <s> keeps retrying the connection while the game starts up.
"""
import argparse
import os
import socket
import sys
import time


def connect(port, wait):
    deadline = time.monotonic() + wait
    while True:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=5)
        except OSError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.5)


def run(sock, reader, line):
    sock.sendall((line + "\n").encode())
    ok = True
    while True:
        reply = reader.readline()
        if not reply:
            raise ConnectionError("the game closed the connection")
        reply = reply.rstrip("\n")
        print(reply, flush=True)
        if reply.startswith("=> "):
            ok = reply == "=> ok"
            break
    return ok


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=int(os.environ.get("MPCON_PORT", "4777")))
    parser.add_argument("--wait", type=float, default=0, help="seconds to keep retrying the connection")
    parser.add_argument("--timeout", type=float, default=600, help="seconds to wait for one reply")
    parser.add_argument("-f", "--file", help="read commands from a file")
    parser.add_argument("commands", nargs="*")
    args = parser.parse_args()

    try:
        sock = connect(args.port, args.wait)
    except OSError as e:
        print(f"mpcon: cannot reach the game on 127.0.0.1:{args.port}: {e}", file=sys.stderr)
        return 2
    sock.settimeout(args.timeout)
    reader = sock.makefile("r", encoding="utf-8", errors="replace")

    commands = list(args.commands)
    if args.file:
        with open(args.file, encoding="utf-8") as f:
            commands += [l.strip() for l in f if l.strip() and not l.lstrip().startswith("#")]

    try:
        if commands:
            ok = True
            for line in commands:
                if not args.file or len(commands) > 1:
                    print(f"> {line}", flush=True)
                ok = run(sock, reader, line) and ok
            return 0 if ok else 1
        while True:
            try:
                line = input("mp> ").strip()
            except EOFError:
                return 0
            if line in ("exit", "bye"):
                return 0
            if line:
                run(sock, reader, line)
    except (ConnectionError, socket.timeout) as e:
        print(f"mpcon: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
