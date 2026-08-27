#!/usr/bin/env python3
"""Generate quads.pdf (stdlib only). Run from repo root: python3 scripts/gen_quads_pdf.py"""
import zlib
from io import BytesIO
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "quads.pdf"

W, H = 612, 792
LM, TM = 72, 72
BM = 72
LINE_H = 12
MAX_W = 92

# ASCII-only (PDF string literals + Helvetica)
PARAS = r"""
SoF Client: Quad & Debris Cvars
Reverse-engineering notes (Linux 1.06a client, IDA). April 2026.

ABSTRACT
This report summarizes how client cvars control effect quads, debris, and FX simulation, and how they relate to CPU cost. Function names and behavior are taken from the sof-bin Linux client (patch 1.06a) in Hex-Rays decompilation.

1. Executive summary
fx_maxdebrisonscreen limits how many debris pieces are submitted to the renderer each frame.
fx_sim_time sets how many FX simulation sub-steps run per frame while catching up effect time.
cl_max_debris and cl_max_quads cap ring-buffer scans and allocation pools.
cl_freezequads (with freezeworld) blocks new quad/debris allocation.

Renderer cost for debris is dominated by pushing entities into the refresh list (V_AddEntity) and downstream mesh/Ghoul work - not the small memcpy inside V_AddEntity itself. Simulation cost scales with fx_sim_time because RunQuadThinkFuncs runs once per sub-step.

2. Frame pipeline (where things run)
CL_AddEntities ends by calling FX_AddEffects. Inside FX_AddEffects, order matters:

- FX_AddQuads runs first: mark/decal quads, debris ring scan, then the active_quads list. Debris uses V_AddEntity when within the on-screen cap.

- Then fxFrameTime = 1.0 / fx_sim_time.

- A loop runs while client time is ahead of fxCurTime: CEffectSystem::update, RunQuadThinkFuncs, FX_UpdateWeaponIdles, then fxCurTime += fxFrameTime (clamped).

FX_AddQuads therefore sees pre-catch-up state for that call; fxCurTime advances afterward.

3. Cvar reference

3.1 fx_maxdebrisonscreen
Registered in FX_Init, default 16.
Used in FX_AddQuads only, when iterating the debrisQuads ring (size from cl_max_debris). For each slot with positive alpha: if fewer than N debris have already been submitted this frame, the client calls V_AddEntity; otherwise it clears that quad's alpha (drops it from display).

Cost: At 0, no debris V_AddEntity calls - only cheap comparisons and alpha clears. At 1, one debris piece per frame enters the full refresh-entity path (downstream Ghoul/render work). Higher values multiply that draw-side cost up to the cap each frame.

Does not change how many FX simulation sub-steps run; see fx_sim_time.

3.2 fx_sim_time
Registered in FX_Init, default 60.
Meaning: Treated as an effective FX tick rate (Hz). The client sets fxFrameTime = 1.0 / fx_sim_time.
Used in FX_AddEffects: the while loop advances fxCurTime in steps of fxFrameTime until it catches client time (with a snap if FX lags more than ~0.2 s).

Cost: Higher fx_sim_time means smaller step, so more loop iterations per wall-clock frame for the same time debt - more calls per frame to CEffectSystem::update and RunQuadThinkFuncs. Lower values coarsen simulation but reduce CPU.

3.3 cl_max_debris
Registered in FX_Init as cl_max_debris, default 128.
Used in FX_AddQuads and RunQuadThinkFuncs: both iterate the debris ring modulo this count (hard-capped at 1024 in code).

Cost: Wider ring means more slots scanned every frame in FX_AddQuads and every FX sub-step in RunQuadThinkFuncs (for callbacks on live debris). Does not by itself cap on-screen debris; fx_maxdebrisonscreen does that at submit time.

3.4 cl_max_quads
Registered in FX_Init, default 256 (symbol cl_maxQuads in decompilation).
Used in FX_AddQuads and RunQuadThinkFuncs for the markQuads array (decals/mark quads), capped at 1024.

Cost: Higher values increase per-frame work for V_AddQuad on marks and for mark think/update passes. Independent of debris caps.

Note: A separate cvar named cl_quads was not found in FX_Init in this binary; mods may differ.

3.5 cl_freezequads (and freezeworld)
Registered in FX_Init as cl_freezequads.
Used in FX_AllocNewQuad, FX_AllocNewDebrisQuad, and RunQuadThinkFuncs (early-out when frozen). When freeze conditions hold, new general quads and new debris quads fail to allocate; running thinks are skipped.

Cost: Reduces new effect allocation and simulation work when enabled; orthogonal to draw caps.

4. V_AddEntity (renderer handoff)
V_AddEntity copies 96 bytes from the source blob into r_entities, appends a pointer in r_entities_list, increments r_numentities (max 512 entries in the disassembly guard).

Callers include CL_AddPacketEntities (network entities), FX_AddQuads (debris), CL_AddViewWeapon, and particle updates.

The expensive part is not the copy: V_RenderView later passes entity counts and lists into the renderer (function-pointer table re[...]), which drives culling, lighting, and GhoulInst::Render-style paths for mesh entities.

5. Ghoul debris simulation
Debris often carries an IGhoulInst on the quad (e.g. offset 0x188 in HandleDebris / FX_QuadGhoulUpdate paths). FX_QuadGhoulUpdate performs movement, collision (FX_AttemptMove), matrix/color updates via vtable calls - substantial CPU while the quad is alive, mostly independent of whether that frame's V_AddEntity submitted it (visibility cap).

6. How costs combine (matrix)
fx_maxdebrisonscreen - Debris draw: V_AddEntity count + downstream render
fx_sim_time - FX sim: iterations of RunQuadThinkFuncs / CEffectSystem::update per frame
cl_max_debris - Scan + think work on debris ring (width of iteration)
cl_max_quads - Mark/decal V_AddQuad + mark thinks
cl_freezequads - Stops new quads/debris; skips thinks when frozen

Typical tuning: lower fx_maxdebrisonscreen to cut draw entities; lower fx_sim_time to cut simulation frequency; lower cl_max_debris to shrink ring work if many slots are touched.

7. Disclaimer
Behavior is inferred from one Linux client build. Other platforms, patches, or mods may differ. Verify in your own binary if precision matters for competitive or compatibility decisions.
""".strip().split("\n\n")


def wrap_para(p):
    lines = []
    for raw in p.split("\n"):
        raw = raw.rstrip()
        if not raw:
            lines.append("")
            continue
        words = raw.split()
        cur = []
        n = 0
        for w in words:
            L = len(w) + (1 if cur else 0)
            if n + L > MAX_W and cur:
                lines.append(" ".join(cur))
                cur = [w]
                n = len(w)
            else:
                cur.append(w)
                n += L
        if cur:
            lines.append(" ".join(cur))
    return lines


def escape_pdf(s: str) -> bytes:
    out = bytearray()
    for ch in s:
        o = ord(ch)
        if ch == "\\":
            out.extend(b"\\\\")
        elif ch == "(":
            out.extend(b"\\(")
        elif ch == ")":
            out.extend(b"\\)")
        elif 32 <= o <= 126:
            out.append(o)
        else:
            for b in ch.encode("utf-8"):
                out.extend(b"\\%03o" % b)
    return bytes(out)


def chunk_lines(lines, max_lines=52):
    pages = []
    buf = []
    for ln in lines:
        buf.append(ln)
        if len(buf) >= max_lines:
            pages.append(buf)
            buf = []
    if buf:
        pages.append(buf)
    return pages


def build_pdf(pages):
    n = len(pages)
    # Object ids: 1=font, 2..n+1=contents, n+2..2n+1=pages, 2n+2=Pages, 2n+3=Catalog
    oid_font = 1
    oid_first_content = 2
    oid_first_page = 2 + n
    oid_pages = 2 + 2 * n
    oid_catalog = 3 + 2 * n

    objs = []
    objs.append(b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")

    for lines in pages:
        stream_parts = [b"BT", b"/F1 10 Tf", ("%f %f Td" % (LM, H - TM)).encode()]
        first = True
        for line in lines:
            if line == "":
                stream_parts.append(b"0 -7 Td")
                first = True
                continue
            esc = escape_pdf(line)
            if first and len(stream_parts) == 3:
                stream_parts.append(b"(" + esc + b") Tj")
                first = False
            else:
                stream_parts.append(("%f -%d Td (" % (0, LINE_H)).encode() + esc + b") Tj")
        stream_parts.append(b"ET")
        data = b"\n".join(stream_parts)
        compressed = zlib.compress(data)
        body = b"<< /Length %d /Filter /FlateDecode >>\nstream\n" % len(compressed) + compressed + b"\nendstream"
        objs.append(body)

    page_oids = list(range(oid_first_page, oid_first_page + n))
    for i in range(n):
        cid = oid_first_content + i
        page = b"<< /Type /Page /Parent %d 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 %d 0 R >> >> /Contents %d 0 R >>" % (
            oid_pages,
            oid_font,
            cid,
        )
        objs.append(page)

    kids = b"[" + b" ".join(b"%d 0 R" % p for p in page_oids) + b"]"
    objs.append(b"<< /Type /Pages /Kids " + kids + b" /Count %d >>" % n)
    objs.append(b"<< /Type /Catalog /Pages %d 0 R >>" % oid_pages)

    buf = BytesIO()
    buf.write(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    offsets = [0]
    for i, ob in enumerate(objs, start=1):
        offsets.append(buf.tell())
        buf.write(b"%d 0 obj\n" % i)
        buf.write(ob)
        buf.write(b"\nendobj\n")

    xref_pos = buf.tell()
    buf.write(b"xref\n0 %d\n" % (len(objs) + 1))
    buf.write(b"0000000000 65535 f \n")
    for off in offsets[1:]:
        buf.write(b"%010d 00000 n \n" % off)
    buf.write(
        b"trailer\n<< /Size %d /Root %d 0 R >>\nstartxref\n%d\n%%%%EOF\n"
        % (len(objs) + 1, oid_catalog, xref_pos)
    )
    return buf.getvalue()


def main():
    flat = []
    for block in PARAS:
        flat.extend(wrap_para(block))
        flat.append("")
    pages = chunk_lines(flat, max_lines=52)
    OUT.write_bytes(build_pdf(pages))
    print("Wrote", OUT, OUT.stat().st_size, "bytes,", len(pages), "pages")


if __name__ == "__main__":
    main()
