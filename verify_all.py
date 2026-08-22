#!/usr/bin/env python3
"""Union several verify.py runs.

A routine is proven when *some* run reached it and every run that reached it
agreed.  No single route reaches everything: a game started with the mouse
never runs the keyboard input path, the attract demo never runs from a level
snapshot, and the animated bricks are ten minutes of play away from the menu.
Measuring the union is the only honest number, and it is also the only way to
see that a routine which fails on one route passes on another - which would be
a real finding rather than a coverage gap.

    venv/bin/python verify_all.py --snapshot DIR/level07_*.snap

Each route runs as a subprocess so one crash does not take the rest with it.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PY = os.path.join(HERE, "venv", "bin", "python")


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seconds", type=float, default=120.0,
                    help="how long each route plays")
    ap.add_argument("--snapshot", action="append", default=[],
                    metavar="FILE",
                    help="also run from this sidebyside.py snapshot; repeat "
                         "for as many as you have")
    ap.add_argument("--skip-base", action="store_true",
                    help="only run the snapshots")
    ap.add_argument("--keep", metavar="DIR",
                    help="keep the per-route JSON here instead of a temp dir")
    args = ap.parse_args()

    routes = []
    if not args.skip_base:
        routes = [("play", []), ("menu", ["--menu"]),
                  ("keyboard", ["--keyboard"])]
    routes += [(os.path.basename(s), ["--resume", s]) for s in args.snapshot]

    out = args.keep or tempfile.mkdtemp(prefix="verifyall")
    os.makedirs(out, exist_ok=True)

    runs = []
    for name, extra in routes:
        j = os.path.join(out, f"{name}.json")
        print(f"--- {name} ({args.seconds:.0f}s)", flush=True)
        r = subprocess.run(
            [PY, os.path.join(HERE, "verify.py"),
             "--seconds", str(args.seconds), "--json", j] + extra,
            capture_output=True, text=True)
        tail = [l for l in r.stdout.splitlines() if l.startswith("  FAIL")]
        for l in tail:
            print(l)
        if not os.path.exists(j):
            print(f"    (no result: {r.stderr.strip().splitlines()[-1:]})")
            continue
        runs.append((name, json.load(open(j))))
        d = runs[-1][1]["routines"]
        ok = sum(1 for v in d.values() if v["checked"] and not v["mismatched"])
        print(f"    {ok} agreed, "
              f"{sum(1 for v in d.values() if v['mismatched'])} differed")

    if not runs:
        raise SystemExit("no route produced a result")

    every = {}
    for name, d in runs:
        for off, v in d["routines"].items():
            e = every.setdefault(off, {"name": v["name"], "checked": 0,
                                       "did_work": 0, "bad": {}})
            e["checked"] += v["checked"]
            e["did_work"] += v["did_work"]
            if v["mismatched"]:
                e["bad"][name] = v["why"]

    proven = {o: e for o, e in every.items()
              if e["checked"] and e["did_work"] and not e["bad"]}
    shallow = {o: e for o, e in every.items()
               if e["checked"] and not e["did_work"] and not e["bad"]}
    failing = {o: e for o, e in every.items() if e["bad"]}
    never = {o: e for o, e in every.items() if not e["checked"] and not e["bad"]}

    n = len(every)
    print(f"\n=== union of {len(runs)} routes, {n} dispatched routines ===")
    print(f"  {len(proven):3d} proven      - reached, did work, agreed "
          f"everywhere")
    print(f"  {len(shallow):3d} shallow     - agreed, but every call was an "
          f"early return")
    print(f"  {len(failing):3d} differing")
    print(f"  {len(never):3d} unreached")
    for o, e in sorted(failing.items()):
        for route, why in e["bad"].items():
            print(f"  FAIL {e['name']} ({o}) on {route}: {why}")
    if shallow:
        print("\nreached but unproven (early return every time): " +
              ", ".join(f"{e['name']} ({o})"
                        for o, e in sorted(shallow.items())))
    if never:
        print("\nnever reached by any route: " +
              ", ".join(f"{e['name']} ({o})"
                        for o, e in sorted(never.items())))
    if args.keep:
        print(f"\nper-route JSON in {out}")
    return 1 if failing else 0


if __name__ == "__main__":
    sys.exit(main())
