#!/usr/bin/env python3
"""Scale string sofplus cmds vs _cmdcost_blob length (and append piece size)."""
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
OUT = Path(os.environ.get("CMDCOST_OUT", Path(__file__).resolve().parent / "results_strlen.txt"))

# Ops that read/process blob (idempotent on src).
BLOB_CMDS = [
    "sp_sc_cvar_len",
    "sp_sc_cvar_copy",
    "sp_sc_cvar_escape",
    "sp_sc_cvar_unescape",
    "sp_sc_cvar_hex",
    "sp_sc_cvar_unhex",
    "sp_sc_cvar_nbsp",
    "sp_sc_cvar_no_color",
    "sp_sc_cvar_replace",
    "sp_sc_cvar_substr",
    "sp_sc_cvar_split",
    "sp_sc_cvar_append",  # +1 char onto dest=blob (prep each call)
    "sp_sc_cvar_append_newline",
    "sp_sc_cvar_save",
]

LENGTHS = [0, 8, 16, 32, 64, 128, 256]  # hard cap — longer cvars crash sofplus
PIECES = [1, 8, 32, 64]  # append piece size at dest=0


def rcon(cmd: str, timeout=0.35) -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    s.sendto(
        b"\xff\xff\xff\xffrcon " + PW.encode() + b" " + cmd.encode("latin1") + b"\n",
        (HOST, PORT),
    )
    chunks = []
    try:
        while True:
            d, _ = s.recvfrom(65535)
            if d.startswith(b"\xff\xff\xff\xff"):
                d = d[4:]
            if d.startswith(b"print\n"):
                d = d[6:]
            chunks.append(d.decode("latin1", "replace"))
    except Exception:
        pass
    s.close()
    return "".join(chunks)


def parse_spins(text: str) -> dict[str, tuple[int, float, bool]]:
    out = {}
    for line in text.splitlines():
        m = re.search(
            r"spin (\S+) n=(\d+) total=([\d.]+)ms per=([\d.]+)ms \(([\d.]+)us\)(.*)",
            line,
        )
        if m:
            out[m.group(1)] = (int(m.group(2)), float(m.group(5)), "upper" in m.group(6))
    return out


def wait_spin(before: int, name: str, timeout=90.0) -> dict[str, tuple[int, float, bool]]:
    deadline = time.time() + timeout
    while time.time() < deadline:
        # Re-read full file and only keep spins whose line starts after `before`
        # (byte offset), so mid-line slices still work via regex on the remnant.
        raw = LOG.read_bytes()
        text = raw[before:].decode("latin1", "replace")
        spins = parse_spins(text)
        if name in spins:
            return spins
        time.sleep(0.05)
    raw = LOG.read_bytes()
    return parse_spins(raw[before:].decode("latin1", "replace"))


def wait_blob(before: int, length: int, timeout=30.0) -> None:
    deadline = time.time() + timeout
    needle = f"blob len={length}"
    while time.time() < deadline:
        text = LOG.read_bytes()[before:].decode("latin1", "replace")
        if needle in text:
            return
        time.sleep(0.05)


def main() -> int:
    ping = rcon("status", timeout=1.5)
    if not ping or "No rcon" in ping:
        print("rcon failed:", ping[:120])
        return 1
    rcon("_sofbuddy_cmdcost 1")
    rcon("sofbuddy_cmdcost_wrap")

    # results[cmd][len] = (per_us, upper_bound)
    grid: dict[str, dict[int, tuple[float, bool]]] = {c: {} for c in BLOB_CMDS}
    piece_rows: list[tuple[int, int, float, bool]] = []

    for length in LENGTHS:
        print(f"== blob len={length} ==", flush=True)
        before = LOG.stat().st_size if LOG.exists() else 0
        rcon(f"sofbuddy_cmdcost_blob {length} 1")
        wait_blob(before, length, timeout=30.0)
        for cmd in BLOB_CMDS:
            # hex output is 2× len — stay under cvar cap
            if cmd in ("sp_sc_cvar_hex", "sp_sc_cvar_unhex") and length > 128:
                continue
            if cmd == "sp_sc_cvar_save" and length > 128:
                continue
            b2 = LOG.stat().st_size
            rcon(f"sofbuddy_cmdcost_spin {cmd} 0 0")  # adaptive until QPC resolves
            spins = wait_spin(b2, cmd, timeout=90.0)
            if cmd in spins:
                n, us, up = spins[cmd]
                grid[cmd][length] = (us, up)
                print(f"  {cmd:28} n={n:7d}  {us:10.3f} us{'  <=' if up else ''}", flush=True)
            else:
                print(f"  {cmd:28} NO SAMPLE", flush=True)

    print("== append piece size (blob=0) ==", flush=True)
    for piece in PIECES:
        before = LOG.stat().st_size
        rcon(f"sofbuddy_cmdcost_blob 0 {piece}")
        wait_blob(before, 0)
        b2 = LOG.stat().st_size
        rcon("sofbuddy_cmdcost_spin sp_sc_cvar_append 0 0")
        spins = wait_spin(b2, "sp_sc_cvar_append")
        n, us, up = spins.get("sp_sc_cvar_append", (0, float("nan"), False))
        piece_rows.append((piece, n, us, up))
        print(f"  piece={piece:2d} n={n} {us:.3f} us{' <=' if up else ''}", flush=True)

    def cell(v: tuple[float, bool] | None) -> str:
        if v is None:
            return f"{'—':>10}"
        us, up = v
        s = f"{us:.3f}"
        return f"{('<' + s) if up else s:>10}"

    lines = [
        "string-length scaling (µs/call)  hashmap="
        + os.environ.get("CMDCOST_HASHMAP", "?"),
        "blob = 'A'*len in _cmdcost_blob (capped 256). append dest pre-copied from blob.",
        "'<x' = flood/cap hit before one QPC tick (upper bound, not zero).",
        "",
        f"{'command':28} " + " ".join(f"{L:>10}" for L in LENGTHS),
        "-" * (28 + 11 * len(LENGTHS)),
    ]
    for cmd in BLOB_CMDS:
        lines.append(f"{cmd:28} " + " ".join(cell(grid[cmd].get(L)) for L in LENGTHS))
    lines += ["", "append piece size @ dest=0:", f"{'piece':>8} {'n':>8} {'per_us':>12} {'note':>8}"]
    for piece, n, us, up in piece_rows:
        lines.append(f"{piece:8d} {n:8d} {us:12.3f} {'<=upper' if up else 'ok':>8}")
    lines += ["", "Compare columns for O(n). hex/unhex/save skipped at 256 (2× len would overflow)."]
    OUT.write_text("\n".join(lines) + "\n")
    print("\n".join(lines))
    print("wrote", OUT)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
