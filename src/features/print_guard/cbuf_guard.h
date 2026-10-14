#pragma once

// Safe Cbuf_Execute drain. Pass the detour trampoline from cmdpark (or nullptr
// when print_guard owns Cbuf_Execute directly); used for nested re-entry.
void Pg_SafeCbufExecute(void (*stock_cbuf)());

// Stock Cbuf_ExecuteText(EXEC_NOW) copies the whole queue to a 0x400 stack
// buffer with no length check, then Cmd_ExecuteString + Cbuf_Execute.
void Pg_SafeCbufExecuteText(char* text);
