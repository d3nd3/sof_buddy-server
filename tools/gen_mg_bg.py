#!/usr/bin/env python3
"""Build opaque sb/mg/bg.m32: honest 640x480 black RGBA (safe for stock loader)."""
import sys
from pathlib import Path

M32LIB = Path("/home/dinda/storage/Games/Soldier Of Fortune/Utility/MyPythonTools/png_to_m32_resizer")
sys.path.insert(0, str(M32LIB))
import m32lib  # noqa: E402

HEADER_SIZE = 968
NAME = "sb/mg/bg"
OUT_W, OUT_H = 640, 480
PIXEL = bytes((0, 0, 0, 255))


def build_mg_bg() -> bytes:
    header = m32lib.MipHeader(
        dictIn={
            "version": 4,
            "name": NAME,
            "width": [OUT_W] + [0] * (m32lib.MIPLEVELS - 1),
            "height": [OUT_H] + [0] * (m32lib.MIPLEVELS - 1),
            "offsets": [HEADER_SIZE] + [0] * (m32lib.MIPLEVELS - 1),
            "flags": 0,
            "contents": 0,
            "value": 0,
            "altname": "",
            "animname": "",
            "damagename": "",
            "scale_x": 1.0,
            "scale_y": 1.0,
            "mip_scale": 0,
            "dt_name": "",
            "dt_scale_x": 1.0,
            "dt_scale_y": 1.0,
            "dt_u": 0.0,
            "dt_v": 0.0,
            "dt_alpha": 1.0,
            "dt_src_blend_mode": 0,
            "dt_dst_blend_mode": 0,
            "flags2": 0,
            "damage_health": 0.0,
            "unused": [0] * 18,
        }
    )
    return bytes(header.file) + PIXEL * (OUT_W * OUT_H)


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    outs = [
        root / "src/features/minigames/assets/sb/mg/bg.m32",
        root / "src/features/minigames/tictactoe/assets/sb/mg/bg.m32",
    ]
    data = build_mg_bg()
    for path in outs:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        print(f"wrote {path} ({len(data)} bytes, {OUT_W}x{OUT_H} black)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
