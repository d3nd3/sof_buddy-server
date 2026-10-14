# print_guard

Bounds engine paths that copy command text or formatted strings into fixed
stack buffers with unchecked `vsprintf` / `strcpy`-style logic. Without it,
long SoFPlus script output (e.g. `sp_sv_print_broadcast #~out` after thousands
of `sp_sc_cvar_append`) corrupts the stack (`EIP 0x61616161`).

**On by default** when `print_guard` is enabled in `features.yaml`.

## SoFPlus print limits (cvars)

Two cvars for scripters — **how many characters you can print** with the usual
`#~cvar` pattern. Not cvar storage size, not engine buffer bytes.

```sofplus
// Max chars in the *printed text* (after #~cvar expand onto the command line).
//set _sofbuddy_printguard_broadcast_max "999"   // sp_sv_print_broadcast #~out
//set _sofbuddy_printguard_client_max "1000"    // sp_sv_print_client 0 #~out (slot 0-9)

sp_sc_cvar_len ~stored ~out
sp_sc_cvar_len ~limit _sofbuddy_printguard_broadcast_max
// ~stored may be 2048+; ~limit is 999 — that's all that prints in one broadcast.

sp_sc_flow_while number cvar ~i < val #~limit "sp_sc_cvar_append ~out a"
sp_sv_print_broadcast #~out    // prints 999 'a', not a crash
```

| Cvar | Default | SoFPlus command |
|------|---------|-----------------|
| `_sofbuddy_printguard_broadcast_max` | **999** | `sp_sv_print_broadcast #~cvar` |
| `_sofbuddy_printguard_client_max` | **1000** | `sp_sv_print_client <slot> #~cvar` (slot 0-9) |

Defaults match the **1023-char macro-expanded command line** minus the fixed
command prefix (`sp_sv_print_broadcast "` = 24 chars → 999; `sp_sv_print_client 0 "`
= 23 chars → 1000). Lower a cvar to truncate sooner; cannot raise past the
engine ceiling (broadcast 2047 / client 1023).

To print more than the limit, chunk (`sp_sc_cvar_substr`, loop, multiple calls).

On a dedicated server, [`reliable_defer`](../reliable_defer/README.md) also
limits how much reliable text can ship in one UDP payload (`msgMaxsize` 1384
when `maxclients > 1`). `print_guard` stops stack corruption; it does not raise
the network staging cap.

## What is patched (engine)

| Path | Engine address | Stack buffer | Guard |
|------|----------------|--------------|-------|
| `SV_BroadcastPrintf` | `0x200618d0` | `0x800` (2048 B) | `vsnprintf` → `"%s"` |
| `PF_cprintf` / `PF_clprintf` | `0x2005c2e0` / `0x2005c3b0` | `0x400` (1024 B) | same |
| `SV_BroadcastCommand` | `0x200619a0` | `0x400` | same |
| `SV_ClientPrint` / `SV_ClientLocPrint` | `0x200617f0` / `0x20061860` | `0x400` | same |
| `Cmd_MacroExpandString` | `0x20018d60` | stock bug on `#`/`$` expand | safe reimplementation |
| `Cbuf_Execute` drain | via `cmdtext_parking` | `0x400` per line | line cap 1023 chars |
| `Cbuf_ExecuteText(EXEC_NOW)` | via `cmdtext_parking` | stock copies whole queue | safe reimplementation |
| `Cmd_ExecuteString` | Pre hook | — | truncate raw line ≥ 1023 chars |
| `COM_Parse` token cap | `0x200554da` + `0x200554eb` | `com_token` 256 B @ `0x20390B10` | `cmp edx, 0x100` → `0xFF` |

Detours register at shim bootstrap (`RegisterDetour`); `cmdtext_parking` calls
`Pg_SafeCbufExecute` when both features are compiled in.

### `COM_Parse` token cap (`parse_guard.cpp`)

Stock stores up to 256 chars (indices 0..255) and then writes the NUL at
`[256]` — one byte past `com_token`, clobbering the low byte of the dword at
`0x20390C10`. The fix retunes both store-gate immediates to `0xFF` at
`GameDllLoaded`: at most 255 chars are kept and the NUL lands at `[255]`.
Verify-before-write (all-or-nothing, like `hash_lookup`); a byte mismatch
disables the fix with a log line. The unquoted exit gate at `0x20055504` is
left alone — it goes dead, so over-long unquoted tokens now truncate to 255
instead of coming back empty, matching the quoted path.

## Tests

```bash
tools/tests/print_guard/run.sh
```
