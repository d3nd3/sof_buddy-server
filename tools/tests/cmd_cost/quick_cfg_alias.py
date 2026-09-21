#!/usr/bin/env python3
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bench_alias import SOFDIR, exec_cfg_wait, kill_spsv, start_spsv

ITERS = 250


def main() -> None:
    inline = (
        f'sp_sc_flow_while number cvar bench_wcnt < val {ITERS} '
        f'"sp_sc_cvar_math_add bench_wcnt 1"\n'
    )
    wrap = f"sp_sc_flow_while number cvar bench_wcnt < val {ITERS} bench_cga\n"
    for hm in (1, 0):
        print(f"\n=== cfg {ITERS} iters hashmap={hm} ===", flush=True)
        kill_spsv()
        start_spsv(hm)
        for label, body, alias in (
            ("inline", inline, None),
            ("alias", wrap, "sp_sc_cvar_math_add bench_wcnt 1"),
        ):
            name = f"bench_quick_{label}.cfg"
            (SOFDIR / "User" / name).write_text(body)
            r = exec_cfg_wait(name, target=ITERS, alias=alias, timeout=600.0)
            rate = r["rate_server"] or r["rate_wall"]
            print(
                f"  {label:6s} {r['sec_wall']:6.1f}s cnt={r['cnt']} "
                f"rate={rate:.0f}/s done={r['done']}",
                flush=True,
            )


if __name__ == "__main__":
    main()
