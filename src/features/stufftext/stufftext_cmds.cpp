#include "stufftext_cmds.h"

#include "generated_engine_pointers.h"
#include "log.h"

#include <cstring>
#include <windows.h>

extern "C" void __cdecl stufftext_Command_f();
extern "C" void __cdecl stufftext_audit_v1_f();
extern "C" void __cdecl stufftext_audit_v2_f();
extern "C" void __cdecl stufftext_audit_v3_f();
extern "C" void __cdecl stufftext_audit_v4_f();
extern "C" void __cdecl stufftext_audit_v5_f();
extern "C" void __cdecl stufftext_audit_v6_f();
extern "C" void __cdecl stufftext_audit_state_f();
extern "C" void __cdecl stuffreconnect_Command_f();

namespace {

// Engine cmd_function_t list (hash_lookup/engine.h — shared SoF.exe layout).
constexpr unsigned kRvaCmdFunctions = 0x241840;
constexpr unsigned kCmdNextOfs = 0x00;
constexpr unsigned kCmdNameOfs = 0x04;
constexpr unsigned kCmdFnOfs = 0x08;

inline bool IsExec(const void* ptr) {
    if (!ptr)
        return false;
    MEMORY_BASIC_INFORMATION mbi = {0};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0)
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                       PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & exec) != 0;
}

HMODULE SofExe() {
    if (HMODULE h = GetModuleHandleA("SoF.exe"))
        return h;
    if (HMODULE h = GetModuleHandleA("SoF-spsv.exe"))
        return h;
    return GetModuleHandleA(nullptr);
}

void** CmdFunctionsHead() {
    HMODULE h = SofExe();
    if (!h)
        return nullptr;
    return reinterpret_cast<void**>(reinterpret_cast<char*>(h) + kRvaCmdFunctions);
}

void* FindCmdNode(const char* name) {
    void** head = CmdFunctionsHead();
    if (!head)
        return nullptr;
    for (void* node = *head; node; node = *reinterpret_cast<void**>(node)) {
        const char* n = *reinterpret_cast<const char**>(
            static_cast<char*>(node) + kCmdNameOfs);
        if (n && strcmp(n, name) == 0)
            return node;
    }
    return nullptr;
}

void InstallCommand(const char* name, void* fn) {
    void* node = FindCmdNode(name);
    if (node) {
        *reinterpret_cast<void**>(static_cast<char*>(node) + kCmdFnOfs) = fn;
        return;
    }
    SOF_EP_Cmd_AddCommand(const_cast<char*>(name), fn);
}

}  // namespace

void StuffText_RegisterConsoleCommands() {
    if (!detour_Cmd_AddCommand::oCmd_AddCommand ||
        !IsExec(reinterpret_cast<const void*>(detour_Cmd_AddCommand::oCmd_AddCommand))) {
        PrintOut(PRINT_BAD,
                 "[stufftext] Cmd_AddCommand unavailable - commands not registered\n");
        return;
    }

    InstallCommand("stufftext", reinterpret_cast<void*>(&stufftext_Command_f));
    InstallCommand("stufftext_audit_v1",
                   reinterpret_cast<void*>(&stufftext_audit_v1_f));
    InstallCommand("stufftext_audit_v2",
                   reinterpret_cast<void*>(&stufftext_audit_v2_f));
    InstallCommand("stufftext_audit_v3",
                   reinterpret_cast<void*>(&stufftext_audit_v3_f));
    InstallCommand("stufftext_audit_v4",
                   reinterpret_cast<void*>(&stufftext_audit_v4_f));
    InstallCommand("stufftext_audit_v5",
                   reinterpret_cast<void*>(&stufftext_audit_v5_f));
    InstallCommand("stufftext_audit_v6",
                   reinterpret_cast<void*>(&stufftext_audit_v6_f));
    InstallCommand("stufftext_audit_state",
                   reinterpret_cast<void*>(&stufftext_audit_state_f));
    InstallCommand("stufftext_reconnect",
                   reinterpret_cast<void*>(&stuffreconnect_Command_f));

    PrintOut(PRINT_LOG, "[stufftext] server commands registered\n");
}
