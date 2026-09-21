#!/usr/bin/env python3
"""Remeasure cmds that previously reported 0.000us (Wine QPC batch/adaptive)."""
from __future__ import annotations

import os
import re
import socket
import time
from pathlib import Path

HOST, PORT, PW = "127.0.0.1", 28921, "testpw123"
LOG = Path(
    "/home/dinda/wprefix/sof-server/drive_c/users/dinda/Soldier of Fortune/User/sof.log"
)
OUT = Path(os.environ.get("CMDCOST_OUT", Path(__file__).resolve().parent / "results_sofplus.txt"))

# Seed from last good partial run
SEED = {
    "sp_sc_cvar_big_text": (1000, 32.0, False),
    "sp_sc_info_net": (10000, 3.2, False),
    "sp_sc_info_time": (10000, 8.0, False),
    "sp_sc_uptime": (10000, 4.8, False),
    "sp_sv_client_check": (10000, 6.4, False),
    "sp_sv_info_client": (10000, 14.4, False),
    "sp_sv_info_client_mod": (10000, 4.8, False),
    "sp_sv_players": (8, 2000.0, True),
    "sp_sv_print_broadcast": (4, 4000.0, True),
}

NEED = [
    "sp_sv_client_cvar_set",
    "sp_sv_print_client",
    "sp_sv_sound_broadcast",
    "sp_sv_sound_client",
    "sp_sc_alias",
    "sp_sc_cvar_list",
    "sp_sc_exec_cvar",
    "sp_sc_exec_file",
    "sp_sc_flow_if",
    "sp_sc_flow_while",
    "sp_sc_func_alias",
    "sp_sc_func_exec",
    "sp_sc_func_load_cvar",
    "sp_sc_func_load_file",
    "sp_sc_on_change",
    "sp_sc_timer",
    "sp_sv_client_blue",
    "sp_sv_client_play",
    "sp_sv_client_red",
    "sp_sv_client_spec",
    "sp_sv_client_swap",
    "sp_sv_say_mute",
    "sp_sv_say_unmute",
    "sp_sc_file_find",
    "sp_sc_cvar_save",
    "sp_sc_func_list",
]


def rcon(cmd: str, timeout=0.3) -> None:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    s.sendto(
        b"\xff\xff\xff\xffrcon " + PW.encode() + b" " + cmd.encode("latin1") + b"\n",
        (HOST, PORT),
    )
    try:
        while True:
            s.recvfrom(65535)
    except Exception:
        pass
    s.close()


def alive() -> bool:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(0.4)
    try:
        s.sendto(b"\xff\xff\xff\xffrcon testpw123 status\n", (HOST, PORT))
        s.recvfrom(32)
        return True
    except Exception:
        return False
    finally:
        s.close()


def parse(text: str) -> dict[str, tuple[int, float, bool]]:
    out: dict[str, tuple[int, float, bool]] = {}
    for line in text.splitlines():
        m = re.search(
            r"spin (\S+) n=(\d+) total=([\d.]+)ms per=([\d.]+)ms \(([\d.]+)us\)(.*)",
            line,
        )
        if m:
            out[m.group(1)] = (int(m.group(2)), float(m.group(5)), "upper" in m.group(6))
    return out


def wait_spin(before: int, name: str, timeout=45.0) -> dict[str, tuple[int, float, bool]]:
    deadline = time.time() + timeout
    while time.time() < deadline:
        sp = parse(LOG.read_bytes()[before:].decode("latin1", "replace"))
        if name in sp:
            return sp
        if not alive():
            return {}
        time.sleep(0.05)
    return parse(LOG.read_bytes()[before:].decode("latin1", "replace"))


def main() -> int:
    if not alive():
        print("rcon failed")
        return 1
    rcon("_sofbuddy_cmdcost 1")
    rcon("sofbuddy_cmdcost_wrap")
    rcon("set _cmdcost_exec echo")
    rcon("set _cmdcost_fnbody echo")
    rcon('sp_sc_func_alias _cmdcost_fn "echo x"')
    time.sleep(0.2)

    results = dict(SEED)
    for name in NEED:
        if not alive():
            print("server dead at", name, flush=True)
            break
        print(">>", name, flush=True)
        before = LOG.stat().st_size
        rcon(f"sofbuddy_cmdcost_spin {name} 0 0")
        sp = wait_spin(before, name)
        if name in sp:
            n, us, up = sp[name]
            results[name] = (n, us, up)
            print(f"   n={n} {us:.3f}us{' [upper]' if up else ''}", flush=True)
        else:
            print("   NO SAMPLE", flush=True)

    rcon("sp_sv_say_unmute 0")
    rcon("sp_sv_client_play 0")

    rows = sorted(results.items(), key=lambda kv: -kv[1][1])
    lines = [
        "sofplus remasure — batch QPC / adaptive N  hashmap="
        + os.environ.get("CMDCOST_HASHMAP", "?"),
        f"{'command':40} {'n':>8} {'per_us':>12} {'note':14}",
        "-" * 78,
    ]
    for name, (n, us, up) in rows:
        lines.append(
            f"{name:40} {n:8d} {us:12.3f} {'<=qpc-upper' if up else 'ok':14}"
        )
    for n in NEED:
        if n not in results:
            lines.append(f"{n:40} {'-':>8} {'-':>12} {'no sample':14}")
    lines += [
        "",
        "ok = batch total/n; <=qpc-upper = flooder capped below one Wine QPC tick",
    ]
    OUT.write_text("\n".join(lines) + "\n")
    print("\n".join(lines), flush=True)
    print("wrote", OUT, flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
