#!/usr/bin/env python3
"""Time while_cpu.cfg-shaped exec; poll counter (rcon exec has no done reply)."""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bench_alias import SOFDIR, exec_cfg_wait, kill_spsv, start_spsv

FIX = Path(__file__).resolve().parent / "fixtures"
DEPLOY = SOFDIR / "User/sofplus/addons"


def deploy_cfg(name: str, src: Path, n: int) -> str:
    DEPLOY.mkdir(parents=True, exist_ok=True)
    body = "\n".join(
        ln.replace("2500", str(n))
        for ln in src.read_text().splitlines()
        if ln.strip() and not ln.lstrip().startswith("//")
    )
    (DEPLOY / name).write_text(body + "\n")
    return f"sofplus/addons/{name}"


def time_cfg(n: int, label: str, src: Path, alias: bool) -> dict:
    name = f"bench_time_{label}.cfg"
    rel = deploy_cfg(name, src, n)
    r = exec_cfg_wait(
        rel,
        target=n,
        alias="sp_sc_cvar_math_add bench_wcnt 1" if alias else None,
        timeout=7200.0,
    )
    rate = r["rate_server"] or r["rate_wall"]
    return {
        "label": label,
        "n": n,
        "sec": r["sec_server"] or r["sec_wall"],
        "wall": r["sec_wall"],
        "cnt": r["cnt"],
        "done": r["done"],
        "rate": rate,
        "used_wall": r["sec_server"] == 0,
    }


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--hashmap", type=int, choices=(0, 1), default=None)
    ap.add_argument("-n", type=int, default=2500)
    args = ap.parse_args()
    modes = [args.hashmap] if args.hashmap is not None else [1, 0]
    for hm in modes:
        kill_spsv()
        start_spsv(hm)
        print(f"\nhashmap={hm} n={args.n}", flush=True)
        for label, src, alias in (
            ("cfg_inline", FIX / "bench_time_cfg_inline.cfg", False),
            ("cfg_alias", FIX / "bench_time_cfg_alias.cfg", True),
        ):
            r = time_cfg(args.n, label, src, alias)
            src_tag = "wall" if r["used_wall"] else "server"
            t = r["sec"] if not r["used_wall"] else r["wall"]
            print(
                f"  {label:10s} {src_tag}={t:.1f}s  "
                f"cnt={r['cnt']}/{r['n']} done={r['done']} rate={r['rate']:.1f}/s",
                flush=True,
            )


if __name__ == "__main__":
    main()
