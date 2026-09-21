#!/usr/bin/env python3
"""Run sofbuddy_bench_alias.func; optionally restart spsv for hashmap on/off."""
from __future__ import annotations

import argparse
import os
import re
import socket
import subprocess
import time
from pathlib import Path

HOST, PORT, PW = "127.0.0.1", 28921, "testpw123"
WINEPREFIX = Path("/home/dinda/wprefix/sof-server")
SOFDIR = WINEPREFIX / "drive_c/users/dinda/Soldier of Fortune"
LOG = SOFDIR / "User/sof.log"
FUNC_SRC = (
    Path(__file__).resolve().parents[3]
    / "src/features/cpu_optimizations/cmd_cost/bench/sofbuddy_bench_alias.func"
)
FUNC_DST = SOFDIR / "User/sofplus/addons/sofbuddy_bench_alias.func"
CFG_INLINE = Path(__file__).resolve().parent / "fixtures/bench_alias_inline.cfg"
CFG_WRAP = Path(__file__).resolve().parent / "fixtures/bench_alias_wrap.cfg"
TIMEOUT = 600.0


def rcon(cmd: str, timeout: float = 30.0, max_pkts: int = 64) -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    pkt = b"\xff\xff\xff\xffrcon " + PW.encode() + b" " + cmd.encode("latin1") + b"\n"
    s.sendto(pkt, (HOST, PORT))
    chunks = []
    try:
        while len(chunks) < max_pkts:
            data, _ = s.recvfrom(65535)
            if data.startswith(b"\xff\xff\xff\xff"):
                data = data[4:]
            if data.startswith(b"print\n"):
                data = data[6:]
            chunks.append(data.decode("latin1", "replace"))
    except socket.timeout:
        pass
    s.close()
    return "".join(chunks)


def wait_rcon(timeout: float = 120.0) -> bool:
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            rcon("status", timeout=2.0, max_pkts=4)
            return True
        except OSError:
            time.sleep(1.0)
    return False


def kill_spsv() -> None:
    subprocess.run(["pkill", "-f", "SoF-spsv.exe"], check=False)
    time.sleep(2.0)


def start_spsv(hashmap: int) -> None:
    env = {
        "WINEDEBUG": "-all",
        "WINEPREFIX": str(WINEPREFIX),
        "DISPLAY": ":99",
    }
    cmd = [
        "wine",
        "SoF-spsv.exe",
        "+set",
        "dedicated",
        "1",
        "+set",
        "hostport",
        str(PORT),
        "+set",
        "logfile",
        "3",
        "+set",
        "rcon_password",
        PW,
        "+set",
        f"_sofbuddy_hashmap",
        str(hashmap),
        "+set",
        "_sofbuddy_cmdcost",
        "0",
        "+exec",
        "dedicated.cfg",
        "+map",
        "dm/nycctf1",
    ]
    subprocess.Popen(
        cmd,
        cwd=SOFDIR,
        env={**os.environ, **env},
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if not wait_rcon():
        raise SystemExit("server did not come up")


def tail_bench(pos: int) -> str:
    if not LOG.exists():
        return ""
    return LOG.read_text(errors="replace")[pos:]


def cvar_int(name: str, timeout: float = 5.0) -> int | None:
    m = re.search(r'is\s+"([^"]+)"', rcon(name, timeout=timeout))
    return int(float(m.group(1))) if m else None


def server_sec() -> int:
    rcon("sp_sc_info_time", timeout=5.0)
    v = cvar_int("_sp_sc_info_time_sec")
    return v if v is not None else 0


def exec_cfg_wait(
    rel_cfg: str,
    *,
    counter: str = "bench_wcnt",
    target: int,
    alias: str | None = None,
    timeout: float = 3600.0,
    poll: float = 0.05,
) -> dict:
    """Fire exec and poll counter; rcon exec has no completion reply."""
    if alias:
        rcon(f'alias bench_cga "{alias}"', timeout=5.0)
    rcon(f"set {counter} 0", timeout=5.0)
    t0 = server_sec()
    wall0 = time.time()
    rcon(f"exec {rel_cfg}", timeout=0.5, max_pkts=1)
    cnt = 0
    while time.time() - wall0 < timeout:
        c = cvar_int(counter, timeout=0.35)
        if c is not None:
            cnt = c
        if cnt >= target:
            break
        time.sleep(poll)
    wall = time.time() - wall0
    sec = max(server_sec() - t0, 0)
    rate_s = cnt / sec if sec > 0 else 0.0
    rate_w = cnt / wall if wall > 0 else 0.0
    return {
        "cnt": cnt,
        "target": target,
        "sec_server": sec,
        "sec_wall": wall,
        "done": cnt >= target,
        "rate_server": rate_s,
        "rate_wall": rate_w,
    }


def run_cfg_shape() -> dict:
    """Production while_cpu.cfg shape via exec (string-form flow_while)."""
    out: dict[str, str | float | int] = {}
    iters = 2500
    for key, src, alias in (
        ("I_cfg", CFG_INLINE, None),
        ("A_cfg", CFG_WRAP, "sp_sc_cvar_math_add bench_wcnt 1"),
    ):
        dst = SOFDIR / "User" / src.name
        dst.write_text(
            "\n".join(
                ln
                for ln in src.read_text().splitlines()
                if ln.strip() and not ln.lstrip().startswith("//")
            )
            + "\n"
        )
        r = exec_cfg_wait(
            src.name,
            target=iters,
            alias=alias,
            timeout=1800.0,
        )
        sec = r["sec_server"] or r["sec_wall"]
        out[f"{key}_sec"] = sec
        out[f"{key}_cnt"] = r["cnt"]
        out[f"{key}_want"] = iters
        out[f"{key}_rate"] = r["rate_server"] or r["rate_wall"]
    if out.get("I_cfg_sec") and out.get("A_cfg_sec") and out["A_cfg_sec"] > 0:
        out["r_cfg"] = out["I_cfg_sec"] / out["A_cfg_sec"]
    return out


def run_once() -> dict:
    FUNC_DST.parent.mkdir(parents=True, exist_ok=True)
    FUNC_DST.write_text(FUNC_SRC.read_text())
    pos = LOG.stat().st_size if LOG.exists() else 0
    rcon("sp_sc_func_load_file sofplus/addons/sofbuddy_bench_alias.func", timeout=10.0)
    rcon("sp_sc_func_exec sofbuddy_bench_alias_init", timeout=10.0)
    rcon("set bench_acnt 0")
    rcon("set bench_mcnt 0")
    rcon("sp_sc_func_exec sofbuddy_bench_alias_run", timeout=TIMEOUT)
    time.sleep(1.0)
    text = tail_bench(pos)
    out: dict[str, str | float] = {}
    m = re.search(r"_sofbuddy_hashmap =\s*(\d+)", text)
    if m:
        out["hashmap"] = int(m.group(1))
    for tag in ("I_add", "A_add", "I_math", "A_math"):
        m = re.search(
            rf"\[sofbuddy_bench_alias\]\s+{tag}\s+rounds\s+\d+\s+iters\s+(\d+)\s+in\s+(\d+)\s+s\s+=\s+([\d.]+)\s+iter/s\s+loop\s+(\d+)\s+cnt\s+(\d+)",
            text,
        )
        if m:
            out[f"{tag}_iters"] = int(m.group(1))
            out[f"{tag}_sec"] = int(m.group(2))
            out[f"{tag}_rate"] = float(m.group(3))
            out[f"{tag}_loop"] = int(m.group(4))
            out[f"{tag}_cnt"] = int(m.group(5))
    for tag, key in (("add", "r_add"), ("math", "r_math")):
        m = re.search(rf"A/I {tag} x\s+([\d.]+)", text)  # inline/alias slowdown
        if m:
            out[key] = float(m.group(1))
    return out


def fmt(d: dict) -> str:
    hm = d.get("hashmap", "?")
    lines = [f"hashmap={hm}"]
    for p in ("I_add", "A_add", "I_math", "A_math"):
        if f"{p}_rate" in d:
            lines.append(
                f"  {p:6s} {d[f'{p}_rate']:>12g} iter/s  "
                f"cnt={d.get(f'{p}_cnt','?')} loop={d.get(f'{p}_loop','?')}"
            )
    if "r_add" in d:
        lines.append(f"  A/I add  = {d['r_add']:.3f}x slower with alias")
    if "r_math" in d:
        lines.append(f"  A/I math = {d['r_math']:.3f}x slower with alias")
    for p in ("I_cfg", "A_cfg"):
        if f"{p}_rate" in d:
            lines.append(
                f"  {p:6s} {d[f'{p}_rate']:>12g} iter/s  "
                f"cnt={d.get(f'{p}_cnt','?')}/{d.get(f'{p}_want','?')} "
                f"in {d.get(f'{p}_sec',0):.2f}s"
            )
    if "r_cfg" in d:
        lines.append(
            f"  cfg inline {d.get('I_cfg_sec','?')}s  alias {d.get('A_cfg_sec','?')}s  "
            f"alias {d['r_cfg']:.2f}x slower (while_cpu.cfg)"
        )
    return "\n".join(lines)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--hashmap", type=int, choices=(0, 1), help="single run only")
    ap.add_argument("--no-restart", action="store_true", help="use running server")
    args = ap.parse_args()

    results = []
    modes = [args.hashmap] if args.hashmap is not None else [1, 0]
    for hm in modes:
        if not args.no_restart:
            print(f"\n=== restart hashmap={hm} ===")
            kill_spsv()
            start_spsv(hm)
        print(f"\n=== bench hashmap={hm} ===")
        d = run_once()
        d.update(run_cfg_shape())
        results.append(d)
        print(fmt(d))

    if len(results) == 2:
        print("\n=== compare ===")
        for p in ("add", "math"):
            k = f"r_{p}"
            a, b = results[0].get(k), results[1].get(k)
            if a is not None and b is not None:
                print(f"  {p}: hashmap=1 A/I={a:.3f}x  hashmap=0 A/I={b:.3f}x")


if __name__ == "__main__":
    main()
