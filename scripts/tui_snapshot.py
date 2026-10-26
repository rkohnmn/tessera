#!/usr/bin/env python3
"""
Records a Tessera terminal session and replays it into a text grid.

    python3 scripts/tui_snapshot.py --cols 120 --rows 34 -- ./build/tessera data/sales.csv

Keys can be sent before the final frame with --keys (e.g. --keys "/region=EU\\n"),
using Python escape syntax. The replay is a small ANSI interpreter that handles
cursor positioning, erasing, SGR (for optional colour output) and UTF-8, which is
enough for visual regression checks in a headless environment.
"""
from __future__ import annotations

import argparse
import codecs
import fcntl
import os
import pty
import re
import select
import signal
import struct
import sys
import termios
import time

CUP = re.compile(r"\x1b\[(\d+);(\d+)H")
ERASE_ALL = re.compile(r"\x1b\[2J")
SGR = re.compile(r"\x1b\[([0-9;]*)m")
OTHER_CSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")
OSC = re.compile(r"\x1b\][^\x07]*\x07")


def run_session(argv: list[str], cols: int, rows: int, keys: bytes, settle: float, hold: bool) -> bytes:
    pid, fd = pty.fork()
    if pid == 0:  # child
        os.environ["TERM"] = "xterm-256color"
        os.environ["COLORTERM"] = "truecolor"
        os.environ["LINES"] = str(rows)
        os.environ["COLUMNS"] = str(cols)
        try:
            os.execv(argv[0], argv)
        finally:
            os._exit(127)

    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    output = bytearray()

    def pump(seconds: float) -> None:
        deadline = time.time() + seconds
        while time.time() < deadline:
            ready, _, _ = select.select([fd], [], [], 0.05)
            if ready:
                try:
                    chunk = os.read(fd, 65536)
                except OSError:
                    return
                if not chunk:
                    return
                output.extend(chunk)

    pump(0.6)
    for key in keys.split(b"|") if keys else []:
        os.write(fd, key)
        pump(settle)
    pump(settle)
    # Terminate with a signal rather than typing 'q': a modal overlay may treat
    # 'q' as "close", which would capture the wrong frame.
    try:
        os.kill(pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        os.close(fd)
    except OSError:
        pass
    try:
        os.waitpid(pid, 0)
    except ChildProcessError:
        pass
    return bytes(output)


def replay(data: bytes, cols: int, rows: int, color: bool) -> tuple[list[list[str]], list[list[str]]]:
    text = [[" " for _ in range(cols)] for _ in range(rows)]
    colours = [["" for _ in range(cols)] for _ in range(rows)]
    row = col = 0
    style = ""
    stream = data.decode("utf-8", "replace")

    index = 0
    while index < len(stream):
        if stream[index] == "\x1b":
            rest = stream[index:]
            match = CUP.match(rest)
            if match:
                row = int(match.group(1)) - 1
                col = int(match.group(2)) - 1
                index += match.end()
                continue
            if ERASE_ALL.match(rest):
                text = [[" " for _ in range(cols)] for _ in range(rows)]
                index += ERASE_ALL.match(rest).end()
                continue
            match = SGR.match(rest)
            if match:
                style = match.group(1)
                index += match.end()
                continue
            match = OTHER_CSI.match(rest)
            if match:
                index += match.end()
                continue
            match = OSC.match(rest)
            if match:
                index += match.end()
                continue
            index += 1
            continue

        character = stream[index]
        index += 1
        if character == "\r":
            col = 0
        elif character == "\n":
            row += 1
            col = 0
        elif character == "\b":
            col = max(0, col - 1)
        elif character >= " ":
            if 0 <= row < rows and 0 <= col < cols:
                text[row][col] = character
                colours[row][col] = style
            col += 1
    return text, colours


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cols", type=int, default=120)
    parser.add_argument("--rows", type=int, default=34)
    parser.add_argument("--keys", default="")
    parser.add_argument("--settle", type=float, default=0.55)
    parser.add_argument("--hold", action="store_true", help="do not quit after the last frame")
    parser.add_argument("--colour", action="store_true", help="emit ANSI colour per cell")
    parser.add_argument("--json", action="store_true", help="emit a JSON grid with per-cell styles")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()

    command = args.command
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        parser.error("provide a command after --")

    keys = args.keys.encode("utf-8").decode("unicode_escape").encode("latin1")
    raw = run_session(command, args.cols, args.rows, keys, args.settle, args.hold)
    grid, colours = replay(raw, args.cols, args.rows, args.colour)

    if args.json:
        import json
        payload = {
            "cols": args.cols,
            "rows": args.rows,
            "cells": [[[colours[y][x], grid[y][x]] for x in range(args.cols)] for y in range(args.rows)],
        }
        print(json.dumps(payload))
        return 0

    if args.colour:
        for y in range(args.rows):
            line = ""
            current = ""
            for x in range(args.cols):
                style = colours[y][x]
                if style != current:
                    line += f"\x1b[{style}m" if style else "\x1b[0m"
                    current = style
                line += grid[y][x]
            print(line + "\x1b[0m")
    else:
        for line in grid:
            print("".join(line).rstrip())
    return 0


if __name__ == "__main__":
    sys.exit(main())
