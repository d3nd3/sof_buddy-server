#!/usr/bin/env python3
"""Drive safe catalog commands via rcon, dump cmd_cost, A/B cpu_optimizations."""
from __future__ import annotations

import argparse
import re
import socket
import time
from pathlib import Path

HOST, PORT = "127.0.0.1", 28921
PW = "testpw123"
SOFDIR = Path(
    "/home/dinda/wprefix/sof-server/drive_c/users/dinda/Soldier of Fortune"
)
LOG = SOFDIR / "User" / "sof.log"
N = 8

# Bare names that recurse / change session / need a client. Never send these.
SKIP = {
    "map", "gamemap", "connect", "exec", "wait", "screenshot", "tgashot", "winstart",
    "rcon_password", "rcon_password_logger",
    "sp_sc_exec_file", "sp_sc_exec_cvar", "sp_sc_func_load_file", "sp_sc_func_load_cvar",
    "sp_sc_timer", "sp_sc_on_change", "sp_sc_func_exec", "sp_sc_func_alias",
    "sp_sc_cvar_save", "sp_sc_cvar_big_text",
    "cmdlist", "alias", "sp_sc_alias", "spectator", "console", "link",
    "sp_sc_cvar_list",
    ".fps", ".kill", ".no", ".players", ".run", ".timelimit", ".vote", ".yes",
    "sp_sv_client_blue", "sp_sv_client_check", "sp_sv_client_cvar_set",
    "sp_sv_client_play", "sp_sv_client_red", "sp_sv_client_spec", "sp_sv_client_swap",
    "sp_sv_print_broadcast", "sp_sv_print_client", "sp_sv_say_mute", "sp_sv_say_unmute",
    "sp_sv_sound_broadcast", "sp_sv_sound_client",
    # sv_maplist -> Cbuf_ExecuteText EXEC_NOW -> __alloca_probe stack overflow
    "sv_maplist", "sv_maplistfile",
    "sp_cl_console", "sp_cl_info_map", "sp_cl_info_pos", "sp_cl_info_skins",
}

LINE = {
    "echo": "echo cmdcost_bench",
    "set": "set _cmdcost_t 0",
    "status": "status",
    "serverstatus": "serverstatus",
    "sp_sc_cvar_append": "sp_sc_cvar_append _cmdcost_t x",
    "sp_sc_cvar_append_newline": "sp_sc_cvar_append_newline _cmdcost_t",
    "sp_sc_cvar_copy": "sp_sc_cvar_copy _cmdcost_d _cmdcost_t",
    "sp_sc_cvar_copy_latched": "sp_sc_cvar_copy_latched _cmdcost_d hostname",
    "sp_sc_cvar_escape": "sp_sc_cvar_escape _cmdcost_d _cmdcost_t",
    "sp_sc_cvar_find": "sp_sc_cvar_find _cmdcost_f _sofbuddy_",
    "sp_sc_cvar_hex": "sp_sc_cvar_hex _cmdcost_d _cmdcost_t",
    "sp_sc_cvar_len": "sp_sc_cvar_len _cmdcost_d hostname",
    "sp_sc_cvar_math_abs": "sp_sc_cvar_math_abs _cmdcost_t",
    "sp_sc_cvar_math_add": "sp_sc_cvar_math_add _cmdcost_t 1",
    "sp_sc_cvar_math_ceil": "sp_sc_cvar_math_ceil _cmdcost_t",
    "sp_sc_cvar_math_div": "sp_sc_cvar_math_div _cmdcost_t 2",
    "sp_sc_cvar_math_floor": "sp_sc_cvar_math_floor _cmdcost_t",
    "sp_sc_cvar_math_mod": "sp_sc_cvar_math_mod _cmdcost_t 3",
    "sp_sc_cvar_math_mul": "sp_sc_cvar_math_mul _cmdcost_t 1",
    "sp_sc_cvar_math_sqrt": "sp_sc_cvar_math_sqrt _cmdcost_t",
    "sp_sc_cvar_math_sub": "sp_sc_cvar_math_sub _cmdcost_t 1",
    "sp_sc_cvar_nbsp": "sp_sc_cvar_nbsp _cmdcost_d _cmdcost_t",
    "sp_sc_cvar_no_color": "sp_sc_cvar_no_color _cmdcost_d _cmdcost_t",
    "sp_sc_cvar_random_float": "sp_sc_cvar_random_float _cmdcost_t 0 1",
    "sp_sc_cvar_random_int": "sp_sc_cvar_random_int _cmdcost_t 0 10",
    "sp_sc_cvar_replace": "sp_sc_cvar_replace _cmdcost_d _cmdcost_t a b",
    "sp_sc_cvar_split": "sp_sc_cvar_split _cmdcost_s _ hostname",
    "sp_sc_cvar_sset": "sp_sc_cvar_sset _cmdcost_t 1",
    "sp_sc_cvar_substr": "sp_sc_cvar_substr _cmdcost_d hostname 0 3",
    "sp_sc_cvar_unescape": "sp_sc_cvar_unescape _cmdcost_d _cmdcost_t",
    "sp_sc_cvar_unhex": "sp_sc_cvar_unhex _cmdcost_d _cmdcost_t",
    "sp_sc_file_find": "sp_sc_file_find _cmdcost_f *.cfg",
    "sp_sc_flow_if": 'sp_sc_flow_if number cvar _cmdcost_t = val 0 "echo if_true" "echo if_false"',
    "sp_sc_flow_while": 'sp_sc_flow_while number cvar _cmdcost_w < val 8 "sp_sc_cvar_math_add _cmdcost_w 1"',
    "sp_sc_func_list": "sp_sc_func_list",
    "sp_sc_info_net": "sp_sc_info_net",
    "sp_sc_info_net_reset": "sp_sc_info_net_reset",
    "sp_sc_info_time": "sp_sc_info_time",
    "sp_sc_uptime": "sp_sc_uptime",
    "sp_sv_info_client": "sp_sv_info_client",
    "sp_sv_info_client_mod": "sp_sv_info_client_mod",
    "sp_sv_info_frames": "sp_sv_info_frames",
    "sp_sv_players": "sp_sv_players",
    "allow_download": "allow_download",
    "hostname": "hostname",
    "sv_violence": "sv_violence",
}

GAUGE = [
    "_sofbuddy_tickpace_cbuf_max",
    "_sofbuddy_tickpace_late_avg",
    "_sofbuddy_tickpace_late_max",
    "_sofbuddy_tickpace_defers",
    "_sofbuddy_tickpace_saved",
    "_sofbuddy_tickpace_cbuf_fill_max",
    "_sofbuddy_highclamps",
    "_sofbuddy_clamp_avg",
    "_sofbuddy_clamp_lost_ms",
    "_sofbuddy_lowclamp_checks",
    "_sofbuddy_cmdcost_max",
    "_sofbuddy_cmdcost_name",
    "_sofbuddy_cmdcost_ema",
    "_sofbuddy_cmdcost_ema_name",
    "_sofbuddy_cbuf_insert_us",
    "_sofbuddy_cbuf_inserts",
    "_sofbuddy_cbuf_insert_max",
    "_sofbuddy_hashmap",
    "_sofbuddy_cbuf_insert",
    "_sofbuddy_tickpace",
    "_sofbuddy_tickpace_reserve_ms",
    "_sofbuddy_zpool",
    "_sofbuddy_dotcmd_defer",
    "_sofbuddy_cmdcost",
]


def rcon(cmd: str, timeout=0.15, max_pkts=2) -> str:
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
    return "".join(chunks).strip()


def cvar_num(text: str) -> float | None:
    m = re.search(r'"([^"]*)"\s+is\s+"([^"]*)"', text)
    if m:
        try:
            return float(m.group(2))
        except ValueError:
            return None
    m = re.search(r"is\s+\"([^\"]+)\"", text)
    if m:
        try:
            return float(m.group(1))
        except ValueError:
            return None
    return None


def catalog() -> list[str]:
    h = Path(__file__).resolve().parents[3] / "build" / "generated_cmd_catalog.h"
    names = []
    for line in h.read_text().splitlines():
        line = line.strip()
        if line.startswith('"') and line.endswith('",'):
            names.append(line[1:-2])
    return names


def dump_table() -> list[tuple]:
    # gi.dprintf from rcon-handled commands returns in the UDP reply; it does not
    # land in sof.log (unlike server-thread echo / some tick-path prints).
    text = rcon("sofbuddy_cmdcost_dump", timeout=2.0, max_pkts=64)
    rows = []
    for line in text.splitlines():
        if not line.startswith("[cmdcost] ") or line.startswith("[cmdcost] begin") or line.startswith("[cmdcost] end"):
            continue
        p = line.split()
        if len(p) < 6:
            continue
        rows.append((p[1], int(p[2]), float(p[3]), float(p[4]), float(p[5])))
    rows.sort(key=lambda r: -r[3])
    return rows


def snap(label: str) -> dict:
    out = {"_label": label}
    print(f"\n== {label} ==")
    for k in GAUGE:
        v = rcon(k, timeout=0.35)
        out[k] = v
        n = cvar_num(v)
        shown = f"{n:g}" if n is not None else v.replace("\n", " | ")[:100]
        print(f"  {k}: {shown}")
    return out


def run_catalog(names: list[str]) -> None:
    rcon("set _cmdcost_t 0")
    rcon("set _cmdcost_w 0")
    rcon("sofbuddy_cmdcost_reset")
    time.sleep(0.05)
    skipped, ran = [], []
    for name in names:
        if name in SKIP or name not in LINE:
            skipped.append(name)
            continue
        line = LINE[name]
        for _ in range(N):
            rcon(line, timeout=0.08, max_pkts=1)
        ran.append(name)
    print(f"catalog ran {len(ran)} skipped {len(skipped)}")
    print("  ran:", ", ".join(ran))
    if skipped:
        print("  skip:", ", ".join(skipped))


def print_rows(rows, title, n=30):
    print(f"\n-- {title} (ema ms, top {n}) --")
    print(f"{'name':32} {'n':>5} {'max':>8} {'ema':>8} {'avg':>8}")
    for name, c, mx, ema, avg in rows[:n]:
        print(f"{name:32} {c:5d} {mx:8.4f} {ema:8.4f} {avg:8.4f}")


def pick(rows, *want):
    return [(r[0], r[2], r[3]) for r in rows if r[0] in want]


def while_burst(iters: int) -> None:
    rcon("set _cmdcost_w 0")
    rcon(
        f'sp_sc_flow_while number cvar _cmdcost_w < val {iters} '
        f'"sp_sc_cvar_math_add _cmdcost_w 1"',
        timeout=0.5,
        max_pkts=1,
    )
    time.sleep(max(0.15, iters * 0.002))


def restore_defaults() -> None:
    rcon("_sofbuddy_cbuf_insert 0")
    rcon("_sofbuddy_tickpace 1")
    rcon("_sofbuddy_tickpace_reserve_ms 3")
    rcon("_sofbuddy_tickpace_defer_max_ms 200")
    rcon("_sofbuddy_dotcmd_defer 1")
    rcon("_sofbuddy_cmdcost 1")


def ab_while(label: str, setup: list[str], iters: int) -> tuple[list, dict]:
    for s in setup:
        rcon(s)
    rcon("sofbuddy_cmdcost_reset")
    time.sleep(0.05)
    while_burst(iters)
    rows = dump_table()
    s = snap(label)
    print("  while/add:", pick(rows, "sp_sc_flow_while", "sp_sc_cvar_math_add", "?"))
    return rows, s


def client_slot(status: str) -> int | None:
    # status table: "  0     0   36 cmdcost_bot ..."
    for line in status.splitlines():
        parts = line.split()
        if len(parts) >= 5 and parts[0].isdigit() and parts[3].isdigit():
            # num score ping name — ping is digits; name follows
            try:
                return int(parts[0])
            except ValueError:
                pass
    # looser: first line starting with digits after header
    seen_hdr = False
    for line in status.splitlines():
        if "----" in line:
            seen_hdr = True
            continue
        if not seen_hdr:
            continue
        parts = line.split()
        if parts and parts[0].isdigit():
            return int(parts[0])
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--catalog-only", action="store_true")
    ap.add_argument("--ab-only", action="store_true")
    args = ap.parse_args()
    ping = rcon("status", timeout=1.0, max_pkts=4)
    if ping in ("", "<no reply>") or "No rcon" in ping:
        print("rcon failed:", ping[:200] or "(empty)")
        return 1
    names = catalog()
    slot = client_slot(ping)
    print("catalog", len(names), "log", LOG, "client_slot", slot)
    print("status:", ping.splitlines()[0][:80] if ping else "?")

    rcon("_sofbuddy_cmdcost 1")
    rcon("sofbuddy_cmdcost_wrap")
    if not args.ab_only:
        run_catalog(names)
        rcon("sofbuddy_cmdcost_reset")
        time.sleep(0.1)
        before_spin = LOG.stat().st_size if LOG.exists() else 0
        rcon("sofbuddy_cmdcost_spinall", timeout=20.0, max_pkts=1)
        # spinall can take >10s under Wine QPC; wait for last math spin line
        deadline = time.time() + 45.0
        while time.time() < deadline:
            tail = LOG.read_text(encoding="latin1", errors="replace")[before_spin:]
            if "spin sp_sc_cvar_sset" in tail or "spin sp_sc_cvar_math_sub" in tail:
                break
            time.sleep(0.4)
        time.sleep(0.2)
        if slot is not None:
            rcon(f"sofbuddy_cmdcost_slots {slot} 4", timeout=8.0, max_pkts=1)
            time.sleep(1.0)
        else:
            print("no client — skip sofbuddy_cmdcost_slots")
        rows = dump_table()
        print_rows(rows, "spinall+slots samples")
        # Prefer spin log lines (aggregate) over TimedCmd (Wine ~16ms quantum → 0)
        spin_lines = [
            ln for ln in LOG.read_text(encoding="latin1", errors="replace")[before_spin:].splitlines()
            if ln.startswith("[cmdcost] spin ")
        ]
        print(f"\n-- spin log ({len(spin_lines)}) --")
        for ln in spin_lines:
            print(" ", ln)
        snap("after catalog/spin")
        if args.catalog_only:
            return 0

    restore_defaults()
    idle = snap("idle before A/B")

    # cbuf_insert: same while unroll
    ab_while("cbuf_insert=0 while 400", ["_sofbuddy_cbuf_insert 0"], 400)
    ab_while("cbuf_insert=1 while 400", ["_sofbuddy_cbuf_insert 1"], 400)

    # tickpace on/off (settle+reserve)
    rcon("_sofbuddy_cbuf_insert 0")
    ab_while("tickpace=0 reserve=3 while 250", ["_sofbuddy_tickpace 0"], 250)
    ab_while("tickpace=1 reserve=0 while 250", ["_sofbuddy_tickpace 1", "_sofbuddy_tickpace_reserve_ms 0"], 250)
    ab_while("tickpace=1 reserve=3 while 250", ["_sofbuddy_tickpace_reserve_ms 3"], 250)
    ab_while("tickpace=1 reserve=8 while 250", ["_sofbuddy_tickpace_reserve_ms 8"], 250)

    ab_while("reserve=3 defer_max=50 while 250",
             ["_sofbuddy_tickpace_reserve_ms 3", "_sofbuddy_tickpace_defer_max_ms 50"], 250)
    ab_while("reserve=3 defer_max=200 while 250", ["_sofbuddy_tickpace_defer_max_ms 200"], 250)

    print("\n-- dotcmd_defer / hashmap: need restart (+set) for hashmap; client .COMMAND for defer --")
    print("  defaults stay: _sofbuddy_dotcmd_defer 1, _sofbuddy_hashmap 0 (load-time)")

    restore_defaults()
    snap("restored defaults")
    print("\nidle highclamps was", idle.get("_sofbuddy_highclamps", "")[:80])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
