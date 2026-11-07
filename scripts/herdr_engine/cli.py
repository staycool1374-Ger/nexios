#!/usr/bin/env python3
"""cli (T4) — tick/status/selftest entry for the herdr engine fleet.

    python3 -m herdr_engine.cli tick [--rooms 47,507] [--once | --interval N | --dry-run]
    python3 -m herdr_engine.cli status [--rooms 47,507]
    python3 -m herdr_engine.cli selftest

tick: circle-of-rooms loop, one action per room per cycle. The engine-side
wake door (a Transport with a waker) sets `eng.wake` on demand-shaped
arrivals; the continuous loop parks on it instead of a busy poll —
replacing the legacy file-tail arrival trick. `--once` / `--interval N`
exit deterministically for scripting and tests.
"""

from __future__ import annotations

import argparse
import os
import sys

SCRIPTS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if SCRIPTS not in sys.path:
    sys.path.insert(0, SCRIPTS)

from herdr_engine.fsm import Engine, RoomRuntime  # noqa: E402
from herdr_engine.herdr import Herdr  # noqa: E402
from herdr_engine.transport import Transport  # noqa: E402


def parse_rooms(spec: str | None) -> list[int]:
    if not spec:
        return []
    return [int(x) for x in spec.split(",") if x.strip().isdigit()]


def build_engine(rooms: list[int], dry: bool) -> Engine:
    hc = None
    if not dry:
        hc = Herdr(timeout=10.0)
    eng = Engine(runtimes={r: RoomRuntime(herdr=hc) for r in rooms},
                 dry_run=dry, transport=None if dry else Transport())
    eng.rooms_arg = rooms  # explicit fleet wins over the namespace scan
    return eng


def engine_fleet(eng: Engine) -> list[int]:
    rooms = getattr(eng, "rooms_arg", None) or []
    return rooms if rooms else eng.fleet()


def _cycle(eng: Engine) -> int:
    # T8 live finding: "ok" is the absence of work, not work — counting it
    # as progress hot-loops the engine near 100% CPU and floods every
    # observer. Only non-ok actions count as cycle movement.
    before_ok = len([a for a in eng.actions if a.verb == "ok"])
    fresh = []
    for room in engine_fleet(eng):
        act = eng.tick(room)
        if act is not None and act.verb != "ok":
            fresh.append(act)
            print(("[dry] " if eng.dry_run else "") + act.text())
    # keep the log bounded; cycles print nothing when nothing changed
    if len(eng.actions) > 200:
        eng.actions = eng.actions[-50:]
    return len(fresh)


def cmd_tick(args) -> int:
    eng = build_engine(parse_rooms(args.rooms), args.dry_run)
    # engine-side arrival door: demand-shaped transport posts set eng.wake.
    global WAKE_DOOR
    WAKE_DOOR = Transport(waker=lambda *_a: eng.wake.set())
    if args.once:
        _cycle(eng)
        return 0
    if args.interval:
        for _ in range(max(1, args.interval)):
            _cycle(eng)
        return 0
    while True:
        moved = _cycle(eng)
        if not moved:
            eng.wake.wait(timeout=1.0)
            eng.wake.clear()


def cmd_status(args) -> int:
    rooms = parse_rooms(args.rooms)
    eng = Engine(runtimes={r: RoomRuntime() for r in rooms}, dry_run=True)
    print(eng.status(rooms or None))
    return 0


def cmd_selftest(args) -> int:
    import unittest
    loader = unittest.TestLoader()
    repo = os.path.dirname(SCRIPTS)  # repo root = parent of scripts/
    suite = loader.discover(os.path.join(repo, "tests", "herdr_engine"), pattern="test_*.py")
    res = unittest.TextTestRunner(verbosity=1).run(suite)
    return 0 if res.wasSuccessful() else 1


def main() -> int:
    ap = argparse.ArgumentParser(prog="herdr-engine")
    sub = ap.add_subparsers(dest="cmd", required=True)

    tp = sub.add_parser("tick", help="one FSM action per room per cycle")
    tp.add_argument("--rooms", default="", help="comma list; default = whole namespace")
    tp.add_argument("--once", action="store_true", help="single cycle then exit")
    tp.add_argument("--interval", type=int, default=0, help="run N cycles then exit")
    tp.add_argument("--dry-run", action="store_true", help="report actions, write nothing")
    tp.set_defaults(func=cmd_tick)

    st = sub.add_parser("status", help="rooms overview (phases, orders, NEEDs)")
    st.add_argument("--rooms", default="")
    st.set_defaults(func=cmd_status)

    sf = sub.add_parser("selftest", help="run the tests/herdr_engine corpus")
    sf.set_defaults(func=cmd_selftest)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
