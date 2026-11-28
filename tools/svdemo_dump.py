#!/usr/bin/env python3
"""Inspect a .sbd server-side state demo.

This is the reference parser for the container documented in
src/features/server_demo/README.md. It is dependency-free and intentionally
small: a browser/TypeScript renderer can port the same offsets directly.

Usage:
    python3 tools/svdemo_dump.py match.sbd
    python3 tools/svdemo_dump.py match.sbd --slot 2 --samples 20
    python3 tools/svdemo_dump.py match.sbd --quiet
"""

from __future__ import annotations

import argparse
import datetime as _dt
import struct
import sys
from dataclasses import dataclass, field

MAGIC = b"SOFDEMO\0"
VERSION = 1
PS_SIZE = 200
ES_SIZE = 116

CHUNK_CLIENT_FRAME = 1
CHUNK_ROSTER = 2
CHUNK_EVENT = 3
CHUNK_END = 4
CHUNK_CONFIGSTRING = 5

# SoF q_sh_interface.h / configstring.h indexes.
CS_SKY = 2
CS_MODELS = 35
CS_PLAYERSKINS = 1415

# Player state offsets (see server_demo_format.h).
PS_ORIGIN = 4
PS_VELOCITY = 10
PS_VIEWANGLES = 28
PS_VIEWOFFSET = 40
PS_FOV = 148
PS_STATS = 164

# entity_state_t offsets.
ES_ORIGIN = 4
ES_ANGLES = 16
ES_MODELINDEX = 32
ES_FRAME = 40

STAT_HEALTH = 1
STAT_ARMOR = 5
STAT_FRAGS = 10


class FormatError(Exception):
    pass


class Reader:
    def __init__(self, data: bytes):
        self.data = data
        self.pos = 0

    def take(self, n: int) -> bytes:
        if self.pos + n > len(self.data):
            raise FormatError("truncated file")
        out = self.data[self.pos : self.pos + n]
        self.pos += n
        return out

    def u8(self) -> int:
        return self.take(1)[0]

    def u16(self) -> int:
        return struct.unpack("<H", self.take(2))[0]

    def u32(self) -> int:
        return struct.unpack("<I", self.take(4))[0]

    def string16(self) -> str:
        return self.take(self.u16()).decode("utf-8", "replace")

    def remaining(self) -> int:
        return len(self.data) - self.pos


@dataclass
class Header:
    flags: int
    ps_size: int
    es_size: int
    max_clients: int
    tick_ms: int
    created_unix: int
    map_name: str
    level_name: str
    gamedir: str
    server_name: str


@dataclass
class ClientFrame:
    frame: int
    time_ms: int
    slot: int
    flags: int
    ps: bytes
    es: bytes | None


@dataclass
class Roster:
    frame: int
    time_ms: int
    slot: int
    flags: int
    name: str
    userinfo: str


@dataclass
class Event:
    frame: int
    time_ms: int
    kind: int
    text: str


@dataclass
class End:
    first_frame: int
    last_frame: int
    client_frames: int
    max_clients: int
    duration_ms: int
    reason: int


@dataclass
class Demo:
    header: Header
    frames: list[ClientFrame] = field(default_factory=list)
    rosters: dict[int, Roster] = field(default_factory=dict)
    events: list[Event] = field(default_factory=list)
    configstrings: dict[int, str] = field(default_factory=dict)
    end: End | None = None


def read_header(r: Reader) -> Header:
    if r.take(8) != MAGIC:
        raise FormatError("not a .sbd file (bad magic)")
    version = r.u16()
    if version != VERSION:
        raise FormatError(f"unsupported .sbd version {version}")
    header_size = r.u16()
    flags = r.u32()
    ps_size = r.u32()
    es_size = r.u32()
    max_clients = r.u32()
    tick_ms = r.u32()
    created_unix = r.u32()
    r.u32()
    r.u32()
    r.u32()
    if ps_size != PS_SIZE or es_size != ES_SIZE:
        raise FormatError(f"unexpected state sizes ps={ps_size} es={es_size}")
    map_name = r.string16()
    level_name = r.string16()
    gamedir = r.string16()
    server_name = r.string16()
    if header_size != r.pos:
        raise FormatError(f"header size {header_size} != parsed {r.pos}")
    return Header(
        flags, ps_size, es_size, max_clients, tick_ms, created_unix,
        map_name, level_name, gamedir, server_name,
    )


def parse_frame(payload: bytes) -> ClientFrame:
    frame, time_ms, slot, flags, _reserved = struct.unpack_from("<IIBBH", payload, 0)
    ps = payload[12 : 12 + PS_SIZE]
    if len(ps) != PS_SIZE:
        raise FormatError("client frame: short player state")
    es = None
    if flags & 1:
        es = payload[12 + PS_SIZE : 12 + PS_SIZE + ES_SIZE]
        if len(es) != ES_SIZE:
            raise FormatError("client frame: short entity state")
        if len(payload) != 12 + PS_SIZE + ES_SIZE:
            raise FormatError("client frame: trailing bytes")
    elif len(payload) != 12 + PS_SIZE:
        raise FormatError("client frame: trailing bytes")
    return ClientFrame(frame, time_ms, slot, flags, ps, es)


def parse_roster(payload: bytes) -> Roster:
    r = Reader(payload)
    frame = r.u32()
    time_ms = r.u32()
    slot = r.u8()
    flags = r.u8()
    name = r.string16()
    userinfo = r.string16()
    if r.remaining():
        raise FormatError("roster: trailing bytes")
    return Roster(frame, time_ms, slot, flags, name, userinfo)


def parse_event(payload: bytes) -> Event:
    r = Reader(payload)
    frame = r.u32()
    time_ms = r.u32()
    kind = r.u8()
    r.u8()
    text = r.string16()
    if r.remaining():
        raise FormatError("event: trailing bytes")
    return Event(frame, time_ms, kind, text)


def parse_configstring(payload: bytes) -> tuple[int, str]:
    r = Reader(payload)
    index = r.u16()
    value = r.string16()
    if r.remaining():
        raise FormatError("configstring: trailing bytes")
    return index, value


def parse_end(payload: bytes) -> End:
    first, last, frames, max_clients, duration, reason, _r8, _r16 = struct.unpack_from(
        "<IIIIIBBH", payload, 0
    )
    if len(payload) != 24:
        raise FormatError("end: trailing bytes")
    return End(first, last, frames, max_clients, duration, reason)


def parse(data: bytes) -> Demo:
    r = Reader(data)
    demo = Demo(read_header(r))
    while r.remaining():
        ctype = r.u8()
        _flags = r.u8()
        size = r.u16()
        payload = r.take(size)
        if ctype == CHUNK_CLIENT_FRAME:
            demo.frames.append(parse_frame(payload))
        elif ctype == CHUNK_ROSTER:
            roster = parse_roster(payload)
            demo.rosters[roster.slot] = roster
        elif ctype == CHUNK_EVENT:
            demo.events.append(parse_event(payload))
        elif ctype == CHUNK_END:
            demo.end = parse_end(payload)
        elif ctype == CHUNK_CONFIGSTRING:
            index, value = parse_configstring(payload)
            demo.configstrings[index] = value
        else:
            # Unknown chunk types are skipped rather than rejected, so a v1
            # reader keeps working when a later side channel adds chunks.
            pass
    return demo


def origin(ps: bytes) -> tuple[float, float, float]:
    return tuple(v / 8.0 for v in struct.unpack_from("<3h", ps, PS_ORIGIN))


def velocity(ps: bytes) -> tuple[float, float, float]:
    return tuple(v / 8.0 for v in struct.unpack_from("<3h", ps, PS_VELOCITY))


def viewangles(ps: bytes) -> tuple[float, float, float]:
    return struct.unpack_from("<3f", ps, PS_VIEWANGLES)


def viewoffset(ps: bytes) -> tuple[float, float, float]:
    return struct.unpack_from("<3f", ps, PS_VIEWOFFSET)


def stats(ps: bytes) -> tuple[int, ...]:
    return struct.unpack_from("<16h", ps, PS_STATS)


def reason_name(reason: int) -> str:
    return {
        0: "stopped",
        1: "map change",
        2: "disabled",
        3: "size cap",
        4: "shutdown",
        5: "error",
        6: "idle",
    }.get(reason, f"unknown({reason})")


def fmt_vec(v: tuple[float, ...]) -> str:
    return "(" + ", ".join(f"{x:.1f}" for x in v) + ")"


def print_summary(demo: Demo, samples: int, slot_filter: int | None) -> None:
    h = demo.header
    print(f"map:       {h.map_name}  ({h.level_name})")
    print(f"gamedir:   {h.gamedir}")
    print(f"server:    {h.server_name}")
    created = (
        _dt.datetime.fromtimestamp(h.created_unix).isoformat(sep=" ", timespec="seconds")
        if h.created_unix
        else "?"
    )
    print(f"created:   {created}")
    print(f"format:    v{VERSION}, ps={h.ps_size}, es={h.es_size}, tick={h.tick_ms} ms")
    print(f"entities:  {'yes' if h.flags & 1 else 'no'}")
    print(f"maxclients:{h.max_clients}")
    print(f"chunks:    frames={len(demo.frames)} rosters={len(demo.rosters)} "
          f"events={len(demo.events)} end={'yes' if demo.end else 'no'}")
    if demo.frames:
        print(f"frames:    {demo.frames[0].frame}..{demo.frames[-1].frame} "
              f"({demo.frames[-1].time_ms / 1000.0:.1f}s)")
    if demo.rosters:
        print("roster:")
        for slot in sorted(demo.rosters):
            r = demo.rosters[slot]
            flags = " spectator" if r.flags & 2 else ""
            print(f"  slot {slot}: {r.name or '-'}{flags}")
    if demo.configstrings:
        print(f"configstrings: {len(demo.configstrings)} non-empty")
        sky = demo.configstrings.get(CS_SKY)
        if sky:
            print(f"  sky: {sky}")
        models = sorted(
            (i, v) for i, v in demo.configstrings.items()
            if CS_MODELS <= i < CS_MODELS + 256
        )
        if models:
            print("  models: " + ", ".join(f"{i}={v}" for i, v in models[:8]))
        skins = sorted(
            (i, v) for i, v in demo.configstrings.items()
            if CS_PLAYERSKINS <= i < CS_PLAYERSKINS + 32
        )
        if skins:
            print("  playerskins: " + ", ".join(f"{i}={v}" for i, v in skins[:4]))
    if demo.events:
        print("events:")
        for e in demo.events:
            print(f"  {e.time_ms / 1000.0:8.1f}s frame={e.frame} kind={e.kind}: {e.text}")
    if demo.end:
        e = demo.end
        print(f"end:       {reason_name(e.reason)}, frames {e.first_frame}..{e.last_frame}, "
              f"{e.client_frames} client-frames, {e.duration_ms / 1000.0:.1f}s")

    chosen = [f for f in demo.frames if slot_filter is None or f.slot == slot_filter]
    if not chosen or samples <= 0:
        return
    if len(chosen) > samples:
        step = max(1, len(chosen) // samples)
        chosen = chosen[::step][:samples]
    print("samples:")
    for f in chosen:
        st = stats(f.ps)
        angles = viewangles(f.ps)
        pos = origin(f.ps)
        off = viewoffset(f.ps)
        eye = tuple(pos[i] + off[i] for i in range(3))
        line = (
            f"  {f.time_ms / 1000.0:8.1f}s frame={f.frame:6d} slot={f.slot} "
            f"eye={fmt_vec(eye)} angles={fmt_vec(angles)} "
            f"hp={st[STAT_HEALTH]} armor={st[STAT_ARMOR]} frags={st[STAT_FRAGS]}"
        )
        if f.es is not None:
            body = struct.unpack_from("<3f", f.es, ES_ORIGIN)
            model = struct.unpack_from("<i", f.es, ES_MODELINDEX)[0]
            body_frame = struct.unpack_from("<i", f.es, ES_FRAME)[0]
            line += f" body={fmt_vec(body)} model={model} bframe={body_frame}"
        print(line)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Inspect a SoF server-side .sbd demo")
    ap.add_argument("file")
    ap.add_argument("--slot", type=int, default=None, help="only sample this slot")
    ap.add_argument("--samples", type=int, default=8, help="sample rows to print")
    ap.add_argument("--quiet", action="store_true", help="summary only")
    args = ap.parse_args(argv)

    try:
        with open(args.file, "rb") as f:
            data = f.read()
        demo = parse(data)
    except (OSError, FormatError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    print(f"file:      {args.file} ({len(data)} bytes)")
    print_summary(demo, 0 if args.quiet else args.samples, args.slot)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
