#!/usr/bin/env python3
import re
import shutil
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bench_alias import SOFDIR, kill_spsv, rcon, start_spsv

FUNC_SRC = (
    Path(__file__).resolve().parents[3]
    / "src/features/cpu_optimizations/cmd_cost/bench/sofbuddy_bench_math.func"
)
FUNC_DST = SOFDIR / "User/sofplus/addons/sofbuddy_bench_math.func"
LOG = SOFDIR / "User/sof.log"


def main() -> None:
    kill_spsv()
    start_spsv(0)
    FUNC_DST.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(FUNC_SRC, FUNC_DST)
    pos = LOG.stat().st_size if LOG.exists() else 0
    rcon("sp_sc_func_load_file sofplus/addons/sofbuddy_bench_math.func", timeout=15)
    rcon("sp_sc_func_exec sofbuddy_bench_math_init", timeout=15)
    rcon("set _sofbuddy_bench_math_target 3")
    rcon("set _sofbuddy_cmdcost 0")
    print("hashmap:", rcon("_sofbuddy_hashmap", timeout=5).strip(), flush=True)
    t0 = time.time()
    rcon("sp_sc_func_exec sofbuddy_bench_math_run", timeout=600)
    print(f"done in {time.time() - t0:.0f}s", flush=True)
    text = LOG.read_text(errors="replace")[pos:] if LOG.exists() else ""
    for line in text.splitlines():
        if "[sofbuddy_bench_math]" in line:
            print(line.strip().lstrip("\x04"), flush=True)
    m = re.search(r"S/M ratio.*x\s+([\d.]+)", text)
    if m:
        print(f"\nS/M ratio: {m.group(1)}x", flush=True)


if __name__ == "__main__":
    main()
