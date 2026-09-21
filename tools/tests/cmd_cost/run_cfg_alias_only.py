#!/usr/bin/env python3
import sys
import time

sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parent))
from bench_alias import (  # noqa: E402
    CFG_INLINE,
    CFG_WRAP,
    SOFDIR,
    kill_spsv,
    rcon,
    start_spsv,
    cvar_int,
)


def main() -> None:
    for hm in (1, 0):
        print(f"\n=== CFG hashmap={hm} ===", flush=True)
        kill_spsv()
        start_spsv(hm)
        for label, src in (("inline", CFG_INLINE), ("alias", CFG_WRAP)):
            dst = SOFDIR / "User" / src.name
            dst.write_text(src.read_text())
            rcon("set bench_wcnt 0")
            t0 = time.time()
            rcon(f"exec {src.name}", timeout=3600.0, max_pkts=8)
            sec = time.time() - t0
            cnt = cvar_int("bench_wcnt")
            rate = (cnt or 0) / sec if sec > 0 else 0.0
            print(
                f"  {label:6s} wall={sec:7.1f}s cnt={cnt} rate={rate:.0f} iter/s",
                flush=True,
            )


if __name__ == "__main__":
    main()
