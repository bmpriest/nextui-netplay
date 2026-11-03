#!/usr/bin/env python3
"""Reads run-pair.sh's output directories and says, per pair, whether the two
builds compute the same game and can exchange states.

    compare.py <pair dir>...

For each comparison it reports the first frame where game RAM, video, audio
and the serialized state differ ("same" if never). RAM is the outcome that
matters: equal RAM on every frame means the two builds played the same game.
Video and audio can differ while RAM agrees (a renderer or resampler change),
and the state blob can differ while everything else agrees (a format change).
"""

import os
import sys

FIELDS = ("ram", "video", "audio", "state")


def read_trace(path):
    rows = {}
    try:
        with open(path) as f:
            for line in f:
                p = line.split()
                if len(p) == 6:
                    rows[int(p[0])] = (p[1], p[2], p[3], p[4], int(p[5]))
    except FileNotFoundError:
        pass
    return rows


def first_diff(x, y, start=0):
    """First frame >= start where each field differs, or None; and the frame
    count compared."""
    frames = sorted(f for f in x if f > start and f in y)
    out = {}
    for i, name in enumerate(FIELDS):
        out[name] = next((f for f in frames if x[f][i] != y[f][i]), None)
    return out, len(frames)


def rc(d, name):
    try:
        return int(open(os.path.join(d, name + ".rc")).read().strip())
    except (FileNotFoundError, ValueError):
        return None


def log_line(d, name, key):
    try:
        for line in open(os.path.join(d, name + ".log")):
            if line.startswith(key):
                return line.split(":", 1)[1].strip()
    except FileNotFoundError:
        pass
    return ""


def describe(diff, n):
    if n == 0:
        return "no frames to compare"
    return "  ".join(f"{k}={'same' if v is None else 'differs@' + str(v)}" for k, v in diff.items())


def main(dirs):
    for d in dirs:
        a_core, b_core, rom, fs = open(os.path.join(d, "pair.txt")).read().splitlines()[:4]
        frames, save_at = (int(v) for v in fs.split())
        t = {n: read_trace(os.path.join(d, n + ".trace")) for n in ("a", "a2", "b", "b2", "aa", "bb", "ab", "ba")}
        print(f"== {os.path.basename(d.rstrip('/'))}: {os.path.basename(rom)}")
        print(f"   A {log_line(d, 'a', 'core')}   ({a_core})")
        print(f"   B {log_line(d, 'b', 'core')}   ({b_core})")
        print(f"   state size A {log_line(d, 'a', 'state')}, B {log_line(d, 'b', 'state')}")
        for name, x, y, start, label in (
            ("A vs A", t["a"], t["a2"], 0, "A twice from power-on"),
            ("B vs B", t["b"], t["b2"], 0, "B twice from power-on"),
            ("A vs B", t["a"], t["b"], 0, "same inputs, A and B from power-on"),
            ("A<-A", t["a"], t["aa"], save_at, "A loads its own state, against A"),
            ("B<-B", t["b"], t["bb"], save_at, "B loads its own state, against B"),
            ("A<-B", t["b"], t["ab"], save_at, "A loads B's state, against B"),
            ("B<-A", t["a"], t["ba"], save_at, "B loads A's state, against A"),
        ):
            loader = {"A<-A": "aa", "B<-B": "bb", "A<-B": "ab", "B<-A": "ba"}.get(name)
            if loader and rc(d, loader) != 0:
                load = log_line(d, loader, "load") or log_line(d, loader, "dlopen") or f"exit {rc(d, loader)}"
                print(f"   {name:7} {label}: load failed - {load}")
                continue
            diff, n = first_diff(x, y, start)
            print(f"   {name:7} {label}: {describe(diff, n)}")
        print()


if __name__ == "__main__":
    main(sys.argv[1:])
