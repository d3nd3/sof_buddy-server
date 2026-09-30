// sofbuddy_cpuopt_status: one console command dumping every cpuopt gauge and
// master switch, so a single rcon line shows what the folder has done this
// boot. Read-only: cvar pointers are bound once at GameDllLoaded (bootstrap
// context — gi.cvar is unsafe from frame-path hooks on some Wine builds) and
// the command only reads cvar_t.string directly, never publishing anything.

#include "generated_detours.h"
#include "generated_engine_pointers.h"
#include "log.h"

#include "buddy_import.h"

#include <cstddef>
#include <cstring>
#include <windows.h>

#if defined(_WIN32)
#define CPUOPTSTATUS_CDECL __cdecl
#else
#define CPUOPTSTATUS_CDECL
#endif

namespace cpuoptstatus {
namespace {

// Verified cvar_t layout (IDA, Cvar_Set2 @ 0x20021d70 in SoF.exe):
// string +0x04, value +0x18. Engine cvar flags: 1 = ARCHIVE, 8 = NOSET.
constexpr unsigned kCvarStringOfs = 0x04;
constexpr unsigned kCvarValueOfs = 0x18;
constexpr int kCvarFlagArchive = 1;
constexpr int kCvarFlagNoSet = 8;

// A null name is a section header printed as-is (dflt holds the label).
struct Watched {
    const char* name;   // engine cvar, or nullptr for a [section] line
    const char* dflt;   // creation default; must match the owning feature
    int flags;
    const char* desc;   // one-liner shown after the value (nullptr on headers)
};

Watched g_watched[] = {
    {nullptr, "[cpuopt]", 0, nullptr},
    {"dedicated", "0", 0, "1 = dedicated server; spin/hold paths only live here"},
    {"_sofbuddy_cpuopt", "1", kCvarFlagArchive, "master switch; 0 disables modules below (qpc_timer is independent)"},
    {"_sofbuddy_strict_exit", "0", kCvarFlagArchive, "quit on a strict violation (default: log + file, keep running)"},
    {"_sofbuddy_cmdpark_strict", "1", kCvarFlagArchive, "parks all inter-tick inserts; execution post-tick only; clamp violations logged"},
    {nullptr, "[tick_pacing]", 0, nullptr},
    {"_sofbuddy_tickpace", "1", kCvarFlagArchive, "drain-boundary skip + settle + spin (not park backlog)"},
    {"_sofbuddy_tickpace_settle", "1", kCvarFlagArchive, "with tickpace on: credit untilAfterMsec on straddle"},
    {"_sofbuddy_tickpace_spin_ms", "2", kCvarFlagArchive, "with tickpace on: skip Sleep(1) when tick due within N ms"},
    {nullptr, "[qpc_timer]", 0, nullptr},
    {"_sofbuddy_qpc", "1", kCvarFlagArchive, "QPC Sys_Milliseconds timebase (off = 15ms-stepped clock)"},
    {"_sofbuddy_tickpace_saved", "0", kCvarFlagNoSet, "times a drain hid the tick boundary and Sleep(1) was skipped"},
    {"_sofbuddy_tickpace_late_avg", "0", kCvarFlagNoSet, "typical tick lateness, rolling mean (ms)"},
    {nullptr, "[cmdtext_parking]", 0, nullptr},
    {"_sofbuddy_cmdpark", "1", kCvarFlagArchive, "park inserts near the tick, drip one chunk per safe sub-tick"},
    {"_sofbuddy_cmdpark_reserve_ms", "0", kCvarFlagArchive, "strict-style protection within this many ms of the tick"},
    {"_sofbuddy_cmdpark_cbuf_last", "0", kCvarFlagNoSet, "last tick drain wall time (ms)"},
    {"_sofbuddy_cmdpark_cbuf_max", "0", kCvarFlagNoSet, "worst single drain (ms)"},
    {"_sofbuddy_cmdpark_defers", "0", kCvarFlagNoSet, "pre-frame queues moved aside instead of run"},
    {"_sofbuddy_cmdpark_cbuf_cursize", "0", kCvarFlagNoSet, "current cmd_text occupancy (bytes)"},
    {"_sofbuddy_cmdpark_cbuf_fill_max", "0", kCvarFlagNoSet, "peak cmd_text occupancy (bytes)"},
    {nullptr, "[clamp_monitor]", 0, nullptr},
    {"_sofbuddy_clamp_notify_ms", "0", kCvarFlagArchive, "log when one clamp deletes >= this (0 = off)"},
    {"_sofbuddy_clamp_broadcast_ms", "0", kCvarFlagArchive, "tell all clients when the avg reaches this (0 = off)"},
    {"_sofbuddy_clamp_log_file", "0", kCvarFlagArchive, "append every clamp to sof_buddy-highclamps.txt (0 = off)"},
    {"_sofbuddy_highclamps", "0", kCvarFlagNoSet, "overrun ticks whose excess wall time was thrown away (see clamp_lost_ms)"},
    {"_sofbuddy_clamp_avg", "0", kCvarFlagNoSet, "rolling mean deleted per highclamp (ms)"},
    {"_sofbuddy_clamp_last", "0", kCvarFlagNoSet, "last highclamp deleted this (ms)"},
    {"_sofbuddy_clamp_lost_ms", "0", kCvarFlagNoSet, "total wall time dropped by highclamps (ms)"},
    {"_sofbuddy_lowclamps", "0", kCvarFlagNoSet, "clock jumps forward; was more than a tick behind"},
    {"_sofbuddy_lowclamp_checks", "0", kCvarFlagNoSet, "tick decisions observed (must climb)"},
    {"_sofbuddy_lowclamp_gained_ms", "0", kCvarFlagNoSet, "total time invented by lowclamps (ms)"},
    {"_sofbuddy_lowclamp_worst", "0", kCvarFlagNoSet, "worst single lowclamp jump (ms)"},
    {nullptr, "[cmd_cost]", 0, nullptr},
    {"_sofbuddy_cmdcost", "1", kCvarFlagArchive, "live handler timing when 1 (wrap on demand)"},
    {"_sofbuddy_cmdcost_max", "0", kCvarFlagNoSet, "worst single sample (ms)"},
    {"_sofbuddy_cmdcost_name", "0", kCvarFlagNoSet, "which command gave it"},
    {"_sofbuddy_cmdcost_ema", "0", kCvarFlagNoSet, "highest per-name average (ms)"},
    {"_sofbuddy_cmdcost_ema_name", "0", kCvarFlagNoSet, "which name"},
    {"_sofbuddy_cmdcost_n", "0", kCvarFlagNoSet, "samples of the last published name"},
    {nullptr, "[cbuf_insert]", 0, nullptr},
    {"_sofbuddy_cbuf_insert", "1", kCvarFlagArchive, "in-place Cbuf_InsertText fast path"},
    {"_sofbuddy_cbuf_inserts", "0", kCvarFlagNoSet, "fast-path inserts served"},
    {"_sofbuddy_cbuf_insert_bytes", "0", kCvarFlagNoSet, "bytes shifted in place"},
    {"_sofbuddy_cbuf_insert_max", "0", kCvarFlagNoSet, "peak buffer cursize seen"},
    {"_sofbuddy_cbuf_insert_us", "0", kCvarFlagNoSet, "total time spent in the fast path (us)"},
    {"_sofbuddy_cbuf_insert_slow", "0", kCvarFlagNoSet, "fallbacks to the engine path"},
    {nullptr, "[recvbuf_increase]", 0, nullptr},
    {"_sofbuddy_recvbuf_kb", "3107", kCvarFlagArchive, "requested SO_RCVBUF per UDP socket (KiB, 0 = off)"},
    {"_sofbuddy_recvbuf_applied", "0", kCvarFlagNoSet, "actual SO_RCVBUF bytes after kernel clamp"},
    {"_sofbuddy_recvbuf_sockets", "0", kCvarFlagNoSet, "sockets raised"},
    {nullptr, "[load-time]", 0, nullptr},
    {"_sofbuddy_hashmap", "1", kCvarFlagArchive, "hash maps for cvar/command/alias lists"},
    {"_sofbuddy_zpool", "1", kCvarFlagArchive, "recycled zone allocator"},
};

void* g_bound[sizeof(g_watched) / sizeof(g_watched[0])] = {};

const char* ReadStr(void* cv) {
    if (!cv)
        return "(unbound)";
    char* s = *reinterpret_cast<char**>(static_cast<char*>(cv) + kCvarStringOfs);
    return (s && s[0]) ? s : "0";
}

float ReadVal(void* cv, float dflt) {
    if (!cv)
        return dflt;
    return *reinterpret_cast<volatile float*>(static_cast<char*>(cv) + kCvarValueOfs);
}

float Val(const char* name, float dflt) {
    for (std::size_t i = 0; i < sizeof(g_watched) / sizeof(g_watched[0]); ++i) {
        if (g_watched[i].name && std::strcmp(g_watched[i].name, name) == 0)
            return ReadVal(g_bound[i], dflt);
    }
    return dflt;
}

// Tunables whose value alone does not say whether they do anything: spin and
// the reserve window need dedicated (plus their master switch).
const char* LiveTag(const char* name, float value) {
    const float cpuopt = Val("_sofbuddy_cpuopt", 1.0f);
    const float ded = Val("dedicated", 0.0f);
    if (std::strcmp(name, "_sofbuddy_tickpace_spin_ms") == 0) {
        if (!(value > 0.0f))
            return "";
        if (cpuopt == 0.0f || Val("_sofbuddy_tickpace", 1.0f) == 0.0f)
            return "  (idle: tickpace off)";
        return ded == 0.0f ? "  (idle: dedicated-only)" : "  (active)";
    }
    if (std::strcmp(name, "_sofbuddy_cmdpark_reserve_ms") == 0) {
        if (!(value > 0.0f))
            return "  (window off)";
        if (cpuopt == 0.0f || Val("_sofbuddy_cmdpark", 1.0f) == 0.0f)
            return "  (idle: cmdpark off)";
        return ded == 0.0f ? "  (idle: dedicated-only)" : "  (active)";
    }
    return "";
}

}  // namespace

constexpr unsigned kRvaCmdFunctions = 0x241840;
constexpr unsigned kCmdNextOfs = 0x00;
constexpr unsigned kCmdNameOfs = 0x04;
constexpr unsigned kCmdFnOfs = 0x08;

void** CmdFunctionsHead() {
    HMODULE exe = GetModuleHandleA("SoF.exe");
    if (!exe) exe = GetModuleHandleA("SoF-spsv.exe");
    if (!exe) return nullptr;
    return reinterpret_cast<void**>(reinterpret_cast<char*>(exe) + kRvaCmdFunctions);
}

void* FindCmdNode(const char* name) {
    void** head = CmdFunctionsHead();
    if (!head) return nullptr;
    for (void* node = *head; node; node = *reinterpret_cast<void**>(static_cast<char*>(node) + kCmdNextOfs)) {
        const char* n = *reinterpret_cast<const char**>(static_cast<char*>(node) + kCmdNameOfs);
        if (n && std::strcmp(n, name) == 0) return node;
    }
    return nullptr;
}

void InstallCommand(const char* name, void* fn) {
    if (void* node = FindCmdNode(name))
        *reinterpret_cast<void**>(static_cast<char*>(node) + kCmdFnOfs) = fn;
    else
        SOF_EP_Cmd_AddCommand(const_cast<char*>(name), fn);
}

void PrintRow(std::size_t i) {
    PrintOut(PRINT_LOG, "  %s = %s%s -- %s\n", g_watched[i].name, ReadStr(g_bound[i]),
             LiveTag(g_watched[i].name, ReadVal(g_bound[i], 0.0f)),
             g_watched[i].desc ? g_watched[i].desc : "");
}

// Gauges are the NOSET outputs the DLL publishes; everything else is a
// setting (ARCHIVE tunables plus the engine's own `dedicated`).
bool IsGauge(std::size_t i) {
    return g_watched[i].flags == kCvarFlagNoSet;
}

}  // namespace cpuoptstatus

extern "C" void CPUOPTSTATUS_CDECL cpuoptstatus_status_f() {
    using namespace cpuoptstatus;
    constexpr std::size_t kRows = sizeof(g_watched) / sizeof(g_watched[0]);
    PrintOut(PRINT_LOG, "[cpuopt] settings:\n");
    for (std::size_t i = 0; i < kRows; ++i) {
        if (!g_watched[i].name || IsGauge(i))
            continue;
        PrintRow(i);
    }
    PrintOut(PRINT_LOG, "[cpuopt] gauges:\n");
    for (std::size_t i = 0; i < kRows; ++i) {
        if (!g_watched[i].name) {
            PrintOut(PRINT_LOG, "  %s\n", g_watched[i].dflt);
            continue;
        }
        if (!IsGauge(i))
            continue;
        PrintRow(i);
    }
}

void cpuoptstatus_OnGameDllLoaded(void* game_export) {
    using namespace cpuoptstatus;
    (void)game_export;
    for (std::size_t i = 0; i < sizeof(g_watched) / sizeof(g_watched[0]); ++i) {
        if (!g_watched[i].name || g_bound[i])
            continue;
        g_bound[i] = Buddy_GetEngineCvar(g_watched[i].name, g_watched[i].dflt,
                                         g_watched[i].flags, nullptr);
    }
    if (!detour_Cmd_AddCommand::oCmd_AddCommand)
        return;
    InstallCommand("sofbuddy_cpuopt_status",
                   reinterpret_cast<void*>(&cpuoptstatus_status_f));
}
