# stufftext

Sends `svc_stufftext` to clients from the **server console** (and via rcon).

```
stufftext <slot|all|name> <command...>
```

| target | meaning |
|---|---|
| `0` – `maxclients-1` | client slot (numeric) |
| `all` | every connected client |
| anything else | first client whose userinfo `name` contains it (case-insensitive) |

`command...` is `argv[2..]` joined with spaces; a trailing `\n` is appended
when missing, as `svc_stufftext` expects. Delivery is the stock sequence
from `p_client.cpp`: `WriteByte(svc_stufftext)` + `WriteString` +
`unicast(ent, true)`.

## Cvars

**Settings** (you set these):

| cvar | default | what it does |
|---|---|---|
| `_sofbuddy_stufftext` | 1 | Master (live). `0` refuses the command |

**Gauges** (read-only; the DLL writes them):

| cvar | unit | what it tells you |
|---|---|---|
| `_sofbuddy_stufftext_sent` | count | Clients successfully stuffed |
| `_sofbuddy_stufftext_errors` | count | Failed / rejected invocations |

## Chained reconnect — `stufftext_reconnect`

Forces one client through a clean reconnect with a welcome line in the
middle, while the server refuses new connects:

```
stufftext_reconnect <slot>    # 0-based client slot (strictly numeric, no "all")
stufftext_reconnect cancel    # abort a running chain, restore the flag
```

Chain of events:

1. Save the engine's `attractloop` flag, set it, and **read it back** — the
   chain refuses to start unless the flag verifiably reads set before
   anything is stuffed. Then neuter the connect-refusal check for the chain
   window (see below): the flag reads set, but nobody — target included — is
   refused while it runs.
2. Stuff `reconnect` to the target slot.
3. Once the target is observed server-side as `cs_connected`
   (reconnecting) — or after 10 s — stuff
   `echo Welcome to SoF Buddy - enjoy your stay!` to the target. The slot
   exists to unicast to only because the refusal is neutered (see below).
4. Restore the refusal byte, then `attractloop` to the saved value (restores
   run *before* any final send, so a failed send can never leave the server
   neutered or refusing connects; the restored flag value is read back and
   logged).
5. Stuff `reconnect` to the target when still present, plus every other slot
   seen in `cs_connected` that is still present (connected or spawned). A
   target that left for good is simply absent from the list — no loud
   failure for the normal path.

Why the refusal must be neutered: stock `SVC_DirectConnect` refuses *every*
remote connect while `attractloop` is set — including the target's own (only
a true loopback client bypasses; a UDP client, even on 127.0.0.1, is refused
with "Remote connect in attract loop. Ignored."). Unpatched, the target
parks in the getchallenge/connect retry loop, its slot goes zombie/free, and
no later unicast has anyone to reach. The 1-byte patch cannot discriminate
target from strangers, so during the window exclusion is traded for a
working resync: whoever arrives is tracked and re-stuffed in the final
round. The patch is applied at chain start (after the flag verifies set)
and restored at finish, abort, cancel and detach.

Only one chain runs at a time; a second invocation is rejected until the
first finishes, times out (≤ ~12 s total), or is cancelled. Every stuff in
the chain is accounted in `_sofbuddy_stufftext_sent` / `_errors`. Detach
(`StuffText_Shutdown`) restores byte and flag and parks the machine without
sending. The ticker is an `SV_Frame` Post hook: a single idle check per
frame when no chain is active.

Engine addresses (single place to update for a new server binary, see the
top of `stufftext_reconnect.cpp`; verified in IDA, and `SoF.exe` /
`SoF-spsv.exe` share `.text` byte-for-byte):

| Symbol | Address | Notes |
|---|---|---|
| `sv.attractloop` | engine base + `0x3A1F24` | BYTE (neighbours `sv.state`/`sv.time`, cf. clamp_monitor) |
| `svs.clients` | engine base + `0x396EEC` | `client_t*`, stride `0xD2AC`, state DWORD at `+0x0` (`0 free / 1 zombie / 2 connected / 3 spawned`) |
| refusal check | engine base + `0x5E808` | `74 53` (`je allow`) in `SVC_DirectConnect`; patched `74`→`EB` (`jmp allow`) for the window, restored after. Refused unless the bytes read exactly `74 53` |

## How it is wired

No game binary patching: on shim bootstrap (`GameDllLoaded`, once per
`gamex86.dll` load) `stufftext_OnGameDllLoaded` registers all server-console
commands via `StuffText_RegisterConsoleCommands()` (`stufftext_cmds.cpp`).
Map rotates reload the shim but the engine `cmd_functions` list persists, so
re-bootstrap walks that list and repoints existing nodes (or `Cmd_AddCommand`s on
first boot). No duplicate `already defined` spam. Addresses in `detours.yaml`, call-only
via `hooks/pointers.json`. Delivery resolves targets through the game DLL's
`g_edicts` / `maxclients` pair (same RVAs as `ctf_spawn`) and sends through
`game_import_t` slots 30/32/36 (`Buddy_StuffText` in `src/buddy_import.cpp`).

Engine `Cmd_AddCommand`/`Cmd_Argc`/`Cmd_Argv` addresses are the ones audited
in `src/features/cpu_optimizations/hash_lookup/README.md` (shared engine `.text`, identical in
both shipped server builds).

## Audit harness (whitehat)

Each hardening point has its own `Cmd_AddCommand` that audits a candidate
string and prints `ALLOW`/`BLOCK` + reason. Nothing is sent to clients.
Quote the candidate and escape control bytes (`\n`, `\r`, `\xHH`):

```
stufftext_audit_v1 "password foo\n"
stufftext_audit_state                # show attack/precache/quote flags
stufftext_audit_state attack 1       # simulate attackLoop bypass (V1/V2/V4)
stufftext_audit_state precache 1     # allow precache/cmd-state cmds (V4)
stufftext_audit_state quote_reset    # clear V3 persistent flag
```

| Command | Isolates |
|---|---|
| `stufftext_audit_v1` | Baseline transcription of the original guard |
| `stufftext_audit_v2` | Strict values: charset + argc, `$"'`\` rejected |
| `stufftext_audit_v3` | Quote tracking (stricter-than-stock; stock has no carry) |
| `stufftext_audit_v4` | State gate: precache/cmd-state need `precache 1`, no attack bypass |
| `stufftext_audit_v5` | `.check` parser only (u_char ctype, var len ≤ 32) |
| `stufftext_audit_v6` | Canonical framing: no `\r`/`;`, single trailing `\n` |

## Q2 cross-check (`../Quake-2`)

- `$` expansion runs *after* `Cbuf_Execute` splits (`qcommon/cmd.c`
  `Cmd_TokenizeString`), is skipped inside quotes, and unmatched quotes
  discard the line — so expanded `;`/`\n` cannot chain new Cbuf commands,
  only alter argv values/truncate/comment/quote-merge. V2 stays as
  defense-in-depth with corrected rationale.
- No cross-packet quote state: `Cbuf_Execute` resets `quotes` per line and
  unmatched quotes are discarded. V3 is stricter-than-stock logging.
- `precache` is a real client command (`client/cl_main.c`
  `CL_Precache_f`); mid-game stuff resets download state, and unknown
  commands forward to the server (`Cmd_ForwardToServer`). V4 gate stands.
- `set` requires 3/4 args (`qcommon/cvar.c` `Cvar_Set_f`); `\r` is
  whitespace to `COM_Parse`/`Cmd_TokenizeString` but not a `Cbuf_Execute`
  separator, so it smuggles argc, not commands. V6 `BLOCK_CR` stands.
- `MSG_ReadString` stops at first NUL (`qcommon/common.c`), same as the
  filter's C-string view: no NUL differential. V6 NUL block is
  defense-in-depth for host fuzzing only.
