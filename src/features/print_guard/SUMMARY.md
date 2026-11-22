# print_guard

Bounds unchecked engine print and command paths: `SV_BroadcastPrintf`,
`PF_cprintf`/`PF_clprintf`, `SV_BroadcastCommand`, `SV_ClientPrint` and
`SV_ClientLocPrint`, `Cmd_MacroExpandString`, `Cbuf_Execute` and EXEC_NOW
lines, `Cmd_ExecuteString` raw lines, and the `COM_Parse` token cap. Prevents
long SoFPlus output from corrupting the stack and exposes broadcast/client
print-length cvars.
