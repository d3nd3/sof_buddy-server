#!/usr/bin/env python3
"""Server-clock timing: func-body braced vs while_cpu.cfg exec shape."""
import argparse
import re
import shutil
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bench_alias import SOFDIR, cvar_int, exec_cfg_wait, kill_spsv, rcon, server_sec, start_spsv

FUNC_SRC = (
    Path(__file__).resolve().parents[3]
    / "src/features/cpu_optimizations/cmd_cost/bench/sofbuddy_bench_time.func"
)
FUNC_DST = SOFDIR / "User/sofplus/addons/sofbuddy_bench_time.func"
LOG = SOFDIR / "User/sof.log"
FIX = Path(__file__).resolve().parent / "fixtures"


def time_cfg(name: str, src: Path, n: int, alias: bool = False) -> dict:
    body = "\n".join(
        ln.replace("2500", str(n))
        for ln in src.read_text().splitlines()
        if ln.strip() and not ln.lstrip().startswith("//")
    )
    dst = SOFDIR / "User" / name
    dst.write_text(body + "\n")
    r = exec_cfg_wait(
        name,
        target=n,
        alias="sp_sc_cvar_math_add bench_wcnt 1" if alias else None,
        timeout=7200.0,
    )
    return {
        "tag": name.replace(".cfg", "").replace("bench_time_", ""),
        "n": n,
        "sec_server": r["sec_server"],
        "sec_wall": r["sec_wall"],
        "cnt": r["cnt"],
        "rate_server": r["rate_server"] or None,
        "rate_wall": r["rate_wall"],
    }


def time_func(hashmap: int, n: int, target: int) -> list[dict]:
    FUNC_DST.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(FUNC_SRC, FUNC_DST)
    pos = LOG.stat().st_size if LOG.exists() else 0
    rcon("sp_sc_func_load_file sofplus/addons/sofbuddy_bench_time.func", timeout=15)
    rcon("sp_sc_func_exec sofbuddy_bench_time_init", timeout=15)
    rcon(f"set _bench_time_n {n}")
    rcon(f"set _bench_time_target {target}")
    rcon("sp_sc_func_exec sofbuddy_bench_time_run", timeout=7200.0)
    text = LOG.read_text(errors="replace")[pos:]
    rows = []
    for m in re.finditer(
        r"\[bench_time\] (func_\w+) n(\d+) rounds (\d+) sec (\d+) iter/s ([\d.]+) cnt (\d+)",
        text,
    ):
        rows.append(
            {
                "tag": m.group(1),
                "n": int(m.group(2)),
                "sec_server": int(m.group(4)),
                "cnt": int(m.group(6)),
                "rate_server": float(m.group(5)),
            }
        )
    return rows


def run_all(hashmap: int, n: int, target: int) -> None:
    kill_spsv()
    start_spsv(hashmap)
    print(f"\n=== hashmap={hashmap} n={n} ===", flush=True)
    func = time_func(hashmap, n, target)
    for r in func:
        print(
            f"  {r['tag']:11s} server {r['sec_server']}s  "
            f"{r['rate_server']:g} iter/s  cnt={r['cnt']}",
            flush=True,
        )
    for label, src in (
        ("cfg_inline", FIX / "bench_time_cfg_inline.cfg"),
        ("cfg_alias", FIX / "bench_time_cfg_alias.cfg"),
    ):
        r = time_cfg(f"bench_time_{label}.cfg", src, n, alias=label == "cfg_alias")
        rs = r["rate_server"]
        rw = r["rate_wall"]
        print(
            f"  {label:11s} server {r['sec_server']}s wall {r['sec_wall']:.1f}s  "
            f"rate={rs if rs else f'~{rw:.1f}'} iter/s  cnt={r['cnt']}",
            flush=True,
        )
    fi = next((x["rate_server"] for x in func if x["tag"] == "func_inline"), 0)
    ci = r if (r := time_cfg("x.cfg", FIX / "bench_time_cfg_inline.cfg", n)) else None
    # ratio printed in main


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--hashmap", type=int, choices=(0, 1), default=None)
    ap.add_argument("--n", type=int, default=2500)
    ap.add_argument("--target", type=int, default=3)
    args = ap.parse_args()
    modes = [args.hashmap] if args.hashmap is not None else [1, 0]
    results = {}
    for hm in modes:
        kill_spsv()
        start_spsv(hm)
        print(f"\n=== hashmap={hm} n={args.n} ===", flush=True)
        func = time_func(hm, args.n, args.target)
        cfg = {}
        for label, src in (
            ("cfg_inline", FIX / "bench_time_cfg_inline.cfg"),
            ("cfg_alias", FIX / "bench_time_cfg_alias.cfg"),
        ):
            cfg[label] = time_cfg(
                f"bench_time_{label}.cfg", src, args.n, alias=label == "cfg_alias"
            )
        results[hm] = {"func": func, "cfg": cfg}
        for r in func:
            print(
                f"  {r['tag']:11s} server {r['sec_server']}s  "
                f"{r['rate_server']:g} iter/s  cnt={r['cnt']}",
                flush=True,
            )
        for label, r in cfg.items():
            rs, rw = r["rate_server"], r["rate_wall"]
            rate_s = f"{rs:g}" if rs else f"~{rw:.1f} (wall)"
            print(
                f"  {label:11s} server {r['sec_server']}s wall {r['sec_wall']:.1f}s  "
                f"{rate_s} iter/s  cnt={r['cnt']}",
                flush=True,
            )
        fi = next((x["rate_server"] for x in func if x["tag"] == "func_inline"), 0)
        ci = cfg["cfg_inline"]["rate_server"] or cfg["cfg_inline"]["rate_wall"]
        if fi and ci:
            print(f"  => func_inline / cfg_inline: {fi/ci:.1f}x faster", flush=True)


if __name__ == "__main__":
    main()
