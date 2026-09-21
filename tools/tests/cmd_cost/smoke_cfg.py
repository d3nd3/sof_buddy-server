#!/usr/bin/env python3
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bench_alias import SOFDIR, exec_cfg_wait, kill_spsv, start_spsv

n = int(sys.argv[1]) if len(sys.argv) > 1 else 5
kill_spsv()
start_spsv(1)
(SOFDIR / "User" / "t1.cfg").write_text(
    f'sp_sc_flow_while number cvar bench_wcnt < val {n} '
    f'"sp_sc_cvar_math_add bench_wcnt 1"\n'
)
print("exec start", flush=True)
r = exec_cfg_wait("t1.cfg", target=n, timeout=120.0)
print(
    f"server={r['sec_server']}s wall={r['sec_wall']:.1f}s "
    f"cnt={r['cnt']} want={n} done={r['done']} "
    f"rate={r['rate_server'] or r['rate_wall']:.1f}/s",
    flush=True,
)
