#pragma once
// Host-test stub: registration is a no-op.
namespace detour_Cmd_AddCommand {
using tCmd_AddCommand = void (*)(char*, void*);
inline tCmd_AddCommand oCmd_AddCommand = nullptr;
}
namespace detour_Cmd_Argc {
using tCmd_Argc = int (*)();
inline tCmd_Argc oCmd_Argc = nullptr;
}
namespace detour_Cmd_Argv {
using tCmd_Argv = char* (*)(int);
inline tCmd_Argv oCmd_Argv = nullptr;
}
inline void SOF_EP_Cmd_AddCommand(char*, void*) {}
inline int SOF_EP_Cmd_Argc() { return 0; }
inline char* SOF_EP_Cmd_Argv(int) { return const_cast<char*>(""); }
