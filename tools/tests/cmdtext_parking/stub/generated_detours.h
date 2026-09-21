#pragma once
namespace detour_Cbuf_AddText {
using tCbuf_AddText = void (*)(char*);
inline tCbuf_AddText oCbuf_AddText = nullptr;
}
namespace detour_Cbuf_Execute {
using tCbuf_Execute = void (*)(void);
inline tCbuf_Execute oCbuf_Execute = nullptr;
}
namespace detour_Cbuf_ExecuteText {
using tCbuf_ExecuteText = void (*)(int, char*);
}
