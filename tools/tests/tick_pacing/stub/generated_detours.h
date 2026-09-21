#pragma once
namespace detour_Cbuf_AddText {
using tCbuf_AddText = void (*)(char*);
inline tCbuf_AddText oCbuf_AddText = nullptr;
}
namespace detour_Cbuf_Execute { using tCbuf_Execute = void (*)(void); }
namespace detour_Sys_Milliseconds {
using tSys_Milliseconds = int (*)();
inline tSys_Milliseconds oSys_Milliseconds = nullptr;
}
