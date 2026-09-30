// sv_tracktime: per-slot client frametime accounting - measurement only.
//
// Every usercmd_t the client sends arrives as SV_ClientThink(cl, cmd) from
// clc_move, and cmd->msec is the client's own claim of how long its last frame
// took. The server trusts that byte for client-side prediction and movement
// timing, and nothing checks it against reality: a client can claim 1 ms per
// frame at 60 fps and every stat readout downstream will believe it.
//
// This feature measures the disagreement instead of acting on it. For each slot
// it keeps the ClientThink arrival rate (frames the server actually saw, against
// the wall clock) next to the client's own msec total, and reports whether the
// two agree. A client whose average frametime is real lands on `ok`; one that
// inflates its rate lands on MSEC-LOW, one that drags it down on MSEC-HIGH.
//
// Nothing here kicks, clamps or rewrites anything - the numbers go to the
// server console (`sv_tracktime`, via Com_Printf so no `developer 1` is
// needed) and to three NOSET gauges for scripts. The maths lives in
// sv_tracktime_logic.h (host-tested); this file is the engine binding.
//
// Offsets (all verified, same values minigames/stufftext already use):
//   svs.clients ......... SoF.exe+0x396EEC, client_t stride 0xD2AC
//   client_t.state ...... client+0x00 (cs_spawned == 3)
//   client_t.userinfo ... client+0xCC
//   usercmd_t.msec ...... cmd+0 (q_shared.h)

#include "cvar.h"
#include "sv_tracktime_logic.h"

#include "buddy_import.h"
#include "generated_engine_pointers.h"
#include "log.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <windows.h>

namespace {

constexpr unsigned kRvaSvsClients = 0x396EEC;
constexpr unsigned kClientStride = 0xD2AC;
constexpr unsigned kClientStateOfs = 0x00;
constexpr unsigned kClientUserinfoOfs = 0xCC;
constexpr int kCsSpawned = 3;

// Bytes of userinfo hashed to notice that a slot changed hands. "name" sits at
// the front of a fresh userinfo ("\\name\\..." or "\\ip\\..."), so 48 bytes is
// plenty to tell two connections apart without walking the whole string.
constexpr unsigned kUserinfoHashLen = 48;

constexpr int kMaxClients = tracktime::kMaxSlots;

// Gauges are republished at most this often; the hook runs per usercmd, and a
// cvar write per usercmd would be the most expensive thing in the feature.
constexpr DWORD kPublishIntervalMs = 500;

tracktime::Slot g_slots[kMaxClients + 1];
long long g_totalFrames = 0;
DWORD g_lastPublish = 0;

// ---------------------------------------------------------------------------
// Memory validation (copied from ctf_spawn / stufftext - never dereference a
// client pointer we have not proved points at committed, readable memory).
// ---------------------------------------------------------------------------
HMODULE ExeMod() {
    // Resolve the engine by name, not via GetModuleHandleA(nullptr), which
    // would hand back whatever executable hosts the process.
    if (HMODULE h = GetModuleHandleA("SoF.exe"))
        return h;
    if (HMODULE h = GetModuleHandleA("SoF-spsv.exe"))
        return h;
    return nullptr;
}

bool IsSafeMemoryBlock(const void* ptr, std::size_t size) {
    auto addr = reinterpret_cast<std::uintptr_t>(ptr);
    if (!ptr || size == 0 || addr < 0x10000 || addr > 0x7FFFFFFF)
        return false;
    if (addr + size < addr)
        return false;
    MEMORY_BASIC_INFORMATION mbi = {0};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT)
        return false;
    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        return false;
    const auto regionEnd = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return addr + size <= regionEnd;
}

bool IsExecutableCodeAddress(const void* ptr) {
    if (!ptr)
        return false;
    MEMORY_BASIC_INFORMATION mbi = {0};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT)
        return false;
    const DWORD execMask =
        PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & execMask) != 0;
}

/** svs.clients as a base pointer. Resolved once; the engine's client array is
 *  static for the life of the process. */
char* ClientsBase() {
    static char* cached = nullptr;
    if (cached)
        return cached;
    HMODULE h = ExeMod();
    if (!h || !IsSafeMemoryBlock(reinterpret_cast<char*>(h) + kRvaSvsClients, sizeof(void*)))
        return nullptr;
    void* clients = *reinterpret_cast<void**>(reinterpret_cast<char*>(h) + kRvaSvsClients);
    if (!IsSafeMemoryBlock(clients, sizeof(void*)))
        return nullptr;
    cached = static_cast<char*>(clients);
    return cached;
}

int MaxClients() {
    static void* cv = nullptr;
    if (!cv)
        cv = Buddy_GetEngineCvar("maxclients", "4", 0, nullptr);
    int n = static_cast<int>(Buddy_ReadCvarValue(cv, 4.0f));
    if (n < 1)
        return 1;
    if (n > kMaxClients)
        return kMaxClients;
    return n;
}

char* ClientPtr(int slot1) {
    if (slot1 < 1 || slot1 > kMaxClients)
        return nullptr;
    char* base = ClientsBase();
    if (!base)
        return nullptr;
    char* cl = base + static_cast<std::size_t>(slot1 - 1) * kClientStride;
    if (!IsSafeMemoryBlock(cl, kClientUserinfoOfs + 8))
        return nullptr;
    return cl;
}

bool SlotSpawned(int slot1) {
    char* cl = ClientPtr(slot1);
    if (!cl)
        return false;
    return *reinterpret_cast<const volatile int*>(cl + kClientStateOfs) == kCsSpawned;
}

/** 1-based slot for a client_t*, or 0 when the pointer is not in the array. */
int SlotFromClient(void* client) {
    char* base = ClientsBase();
    if (!client || !base)
        return 0;
    const auto off = static_cast<std::size_t>(static_cast<char*>(client) - base);
    if (off % kClientStride != 0)
        return 0;
    const std::size_t slot1 = off / kClientStride + 1;
    if (slot1 < 1 || slot1 > static_cast<std::size_t>(kMaxClients))
        return 0;
    return static_cast<int>(slot1);
}

/** Stable identity of whoever is in the slot. A reconnect (or a different
 *  player in the same slot) changes it, which is what starts a fresh
 *  measurement instead of averaging two sessions together. */
std::uint32_t UserinfoHash(char* cl) {
    const char* ui = cl + kClientUserinfoOfs;
    if (!IsSafeMemoryBlock(ui, kUserinfoHashLen))
        return 0;
    std::uint32_t h = 2166136261u;
    for (unsigned i = 0; i < kUserinfoHashLen && ui[i]; ++i) {
        h ^= static_cast<unsigned char>(ui[i]);
        h *= 16777619u;
    }
    return h;
}

const char* SlotName(int slot1) {
    char* cl = ClientPtr(slot1);
    if (!cl || !detour_Info_ValueForKey::oInfo_ValueForKey)
        return "";
    if (!IsExecutableCodeAddress(
            reinterpret_cast<const void*>(detour_Info_ValueForKey::oInfo_ValueForKey)))
        return "";
    const char* ui = cl + kClientUserinfoOfs;
    if (!IsSafeMemoryBlock(ui, 1))
        return "";
    const char* name = SOF_EP_Info_ValueForKey(const_cast<char*>(ui), "name");
    if (!name || !IsSafeMemoryBlock(name, 1))
        return "";
    return name;
}

// ---------------------------------------------------------------------------
// Clock. GetTickCount advances in ~15.6 ms steps, which is the same order as
// the 16 ms frame gap being measured, so a sub-ms monotonic counter is not
// optional here.
// ---------------------------------------------------------------------------
long long NowUs() {
    static double scale = 0.0;  // microseconds per counter tick
    if (scale == 0.0) {
        LARGE_INTEGER freq;
        if (QueryPerformanceFrequency(&freq) && freq.QuadPart > 0)
            scale = 1000000.0 / static_cast<double>(freq.QuadPart);
    }
    LARGE_INTEGER c;
    if (scale == 0.0 || !QueryPerformanceCounter(&c))
        return static_cast<long long>(GetTickCount()) * 1000;
    return static_cast<long long>(static_cast<double>(c.QuadPart) * scale);
}

// ---------------------------------------------------------------------------
// Measurement.
// ---------------------------------------------------------------------------
void Publish(bool force) {
    const DWORD now = GetTickCount();
    if (!force && g_lastPublish && now - g_lastPublish < kPublishIntervalMs)
        return;
    g_lastPublish = now;

    const float tol = SvTracktime_Tolerance();
    const int window = tracktime::ClampWindow(SvTracktime_Window());
    const int minSamples = SvTracktime_MinSamples();
    const int n = MaxClients();

    int suspect = 0;
    double worst = 0.0;
    for (int slot = 1; slot <= n; ++slot) {
        const tracktime::Report r = tracktime::WindowReport(g_slots[slot], window, tol, minSamples);
        if (r.verdict != tracktime::Verdict::MsecLow && r.verdict != tracktime::Verdict::MsecHigh)
            continue;
        ++suspect;
        double d = r.driftPct < 0 ? -r.driftPct : r.driftPct;
        if (d > worst)
            worst = d;
    }
    SvTracktime_SetOutputs(g_totalFrames, suspect, worst);
}

}  // namespace

// ---------------------------------------------------------------------------
// Hook: SV_ClientThink Pre (from clc_move, after MSG_ReadDeltaUserCmd).
// ---------------------------------------------------------------------------
void svtrack_ClientThinkPre(void*& client, void*& cmd) {
    if (!SvTracktime_Enabled() || !client || !cmd)
        return;
    const int slot1 = SlotFromClient(client);
    if (slot1 < 1)
        return;
    char* cl = ClientPtr(slot1);
    if (!cl)
        return;
    if (*reinterpret_cast<const volatile int*>(cl + kClientStateOfs) != kCsSpawned)
        return;
    if (!IsSafeMemoryBlock(cmd, 1))
        return;

    tracktime::Slot& s = g_slots[slot1];
    const std::uint32_t hash = UserinfoHash(cl);
    if (!s.seeded || hash != s.infoHash)
        tracktime::ResetFor(s, hash);

    tracktime::Note(s, NowUs(), *static_cast<unsigned char*>(cmd));
    ++g_totalFrames;
    Publish(false);
}

// ---------------------------------------------------------------------------
// Console commands (engine console + rcon).
// ---------------------------------------------------------------------------
namespace {

/** Command responses go to the server console unconditionally. PrintOut (and
 *  everything built on it) is gi.dprintf, which the engine only shows with
 *  `developer 1` - wrong for a command an admin runs to read a table.
 *  Com_Printf has no such gate (same Say() pattern as cmd_cost_events). */
void ConPrintf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';
    SOF_EP_Com_Printf("%s", buf);
}

void PrintReport(const char* label, const tracktime::Report& r) {
    ConPrintf(
             "[tracktime]   %-6s %7lld frames  sum_msec %lld  span %lld ms"
             "  avg %.2f ms  claim %.1f fps  real %.1f fps  drift %+.1f%%  %s\n",
             label, static_cast<long long>(r.frames), static_cast<long long>(r.sumMs),
             static_cast<long long>(r.spanMs), r.avgMs, r.claimFps, r.realFps, r.driftPct,
             tracktime::VerdictText(r.verdict));
}

void DumpAll() {
    const float tol = SvTracktime_Tolerance();
    const int window = tracktime::ClampWindow(SvTracktime_Window());
    const int minSamples = SvTracktime_MinSamples();
    const int n = MaxClients();

    ConPrintf("[tracktime] window %d samples, min %d, tolerance %.0f%%%s\n", window,
              minSamples, static_cast<double>(tol),
              SvTracktime_Enabled() ? "" : "  (tracking OFF)");
    ConPrintf("[tracktime]  sl  name              frames   sum_ms  avg_ms claim_fps"
              "  real_fps     drift  verdict\n");

    char row[160];
    int suspect = 0;
    int withData = 0;
    double worst = 0.0;
    long long windowFrames = 0;
    for (int slot1 = 1; slot1 <= n; ++slot1) {
        const tracktime::Slot& s = g_slots[slot1];
        const tracktime::Report r = tracktime::WindowReport(s, window, tol, minSamples);
        const bool spawned = SlotSpawned(slot1);
        if (spawned && s.seeded)
            ++withData;
        windowFrames += r.frames;
        if (r.verdict == tracktime::Verdict::MsecLow ||
            r.verdict == tracktime::Verdict::MsecHigh) {
            ++suspect;
            const double d = r.driftPct < 0 ? -r.driftPct : r.driftPct;
            if (d > worst)
                worst = d;
        }
        tracktime::FormatRow(row, sizeof(row), slot1 - 1, spawned ? SlotName(slot1) : "-", r);
        ConPrintf("[tracktime]%s\n", row);
    }

    ConPrintf("[tracktime] %d/%d slots with data, %lld frames in window, %lld since boot\n",
              withData, n, windowFrames, g_totalFrames);
    ConPrintf("[tracktime] suspect %d, worst drift %.1f%%\n", suspect, worst);
    ConPrintf("[tracktime] MSEC-LOW = claims more frames/s than it sends (faked fast),"
              " MSEC-HIGH = claims fewer (faked slow), no-data = too few samples\n");
    Publish(true);
}

void DumpSlot(int slot0) {
    const float tol = SvTracktime_Tolerance();
    const int minSamples = SvTracktime_MinSamples();
    const int window = tracktime::ClampWindow(SvTracktime_Window());
    const int slot1 = slot0 + 1;
    if (slot0 < 0 || slot1 > MaxClients()) {
        ConPrintf("[tracktime] slot %d out of range (0..%d)\n", slot0, MaxClients() - 1);
        return;
    }

    const tracktime::Slot& s = g_slots[slot1];
    const bool spawned = SlotSpawned(slot1);
    ConPrintf("[tracktime] slot %d \"%s\" (%s) window %d samples\n", slot0,
              spawned ? SlotName(slot1) : "-", spawned ? "spawned" : "not spawned", window);

    const tracktime::Report w = tracktime::WindowReport(s, window, tol, minSamples);
    const tracktime::Report t = tracktime::TotalReport(s, tol, minSamples);
    PrintReport("window", w);
    PrintReport("total", t);
    if (s.seeded) {
        const long long idleMs = (NowUs() - s.lastUs) / 1000;
        ConPrintf("[tracktime]   msec min %u max %u, zero %lld, stalls %lld, worst gap %u ms,"
                  " stalled %lld ms, idle %lld ms\n",
                  s.minMs, s.maxMs, static_cast<long long>(s.zeroMs),
                  static_cast<long long>(s.stalls), s.worstGapUs / 1000u,
                  static_cast<long long>(s.stalledMs), idleMs);
    } else {
        ConPrintf("[tracktime]   no samples yet\n");
    }
}

const char* ArgS(int i) {
    if (!detour_Cmd_Argc::oCmd_Argc || !detour_Cmd_Argv::oCmd_Argv)
        return "";
    if (SOF_EP_Cmd_Argc() <= i)
        return "";
    const char* s = SOF_EP_Cmd_Argv(i);
    if (!s || !IsSafeMemoryBlock(s, 1))
        return "";
    return s;
}

/** True when the token is a plain non-negative decimal (so `sv_tracktime abc`
 *  is a usage error rather than slot 0). */
bool IsNumber(const char* s) {
    if (!s || !s[0])
        return false;
    for (int i = 0; s[i]; ++i) {
        if (s[i] < '0' || s[i] > '9')
            return false;
    }
    return true;
}

}  // namespace

extern "C" void __cdecl sv_tracktime_f() {
    if (!SvTracktime_Enabled())
        ConPrintf("[tracktime] disabled (_sofbuddy_tracktime 0) - nothing new is\n"
                  "[tracktime]   being measured, figures below are the last ones seen\n");
    const char* arg = ArgS(1);
    if (arg[0]) {
        if (!IsNumber(arg)) {
            ConPrintf("[tracktime] usage: sv_tracktime [<slot>]\n");
            return;
        }
        DumpSlot(std::atoi(arg));
        return;
    }
    DumpAll();
}

extern "C" void __cdecl sv_tracktime_reset_f() {
    const char* a = ArgS(1);
    const int n = MaxClients();
    int cleared = 0;
    if (a[0] && std::strcmp(a, "all") != 0) {
        if (!IsNumber(a)) {
            ConPrintf("[tracktime] usage: sv_tracktime_reset [<slot>|all]\n");
            return;
        }
        const int slot1 = std::atoi(a) + 1;
        if (slot1 < 1 || slot1 > n) {
            ConPrintf("[tracktime] slot %s out of range (0..%d)\n", a, n - 1);
            return;
        }
        tracktime::Reset(g_slots[slot1]);
        cleared = 1;
    } else {
        for (int slot1 = 1; slot1 <= n; ++slot1)
            tracktime::Reset(g_slots[slot1]);
        cleared = n;
    }
    // g_totalFrames is deliberately left alone: it is a boot-anchored lifetime
    // counter (like the clamp/highclamp counters elsewhere), not per-window data.
    g_lastPublish = 0;
    Publish(true);
    ConPrintf("[tracktime] cleared %d slot(s)\n", cleared);
}

// ---------------------------------------------------------------------------
// Registration.
// ---------------------------------------------------------------------------
namespace {

constexpr unsigned kRvaCmdFunctions = 0x241840;
constexpr unsigned kCmdNextOfs = 0x00;
constexpr unsigned kCmdNameOfs = 0x04;
constexpr unsigned kCmdFuncOfs = 0x08;

void** CmdFunctionsHead() {
    HMODULE h = ExeMod();
    if (!h)
        return nullptr;
    return reinterpret_cast<void**>(reinterpret_cast<char*>(h) + kRvaCmdFunctions);
}

void* FindCmdNode(const char* name) {
    void** head = CmdFunctionsHead();
    if (!head)
        return nullptr;
    for (void* node = *head; node;
         node = *reinterpret_cast<void**>(static_cast<char*>(node) + kCmdNextOfs)) {
        const char* n = *reinterpret_cast<const char**>(static_cast<char*>(node) + kCmdNameOfs);
        if (n && std::strcmp(n, name) == 0)
            return node;
    }
    return nullptr;
}

void InstallCommand(const char* name, void* fn) {
    if (void* node = FindCmdNode(name))
        *reinterpret_cast<void**>(static_cast<char*>(node) + kCmdFuncOfs) = fn;
    else
        SOF_EP_Cmd_AddCommand(const_cast<char*>(name), fn);
}

}  // namespace

void svtrack_OnGameDllLoaded(void* game_export) {
    (void)game_export;
    SvTracktime_InitCvars();
    SvTracktime_SetOutputs(0, 0, 0.0);

    if (!detour_Cmd_AddCommand::oCmd_AddCommand ||
        !IsExecutableCodeAddress(
            reinterpret_cast<const void*>(detour_Cmd_AddCommand::oCmd_AddCommand))) {
        PrintOut(PRINT_BAD, "[tracktime] Cmd_AddCommand unavailable - commands not registered\n");
        return;
    }
    InstallCommand("sv_tracktime", reinterpret_cast<void*>(&sv_tracktime_f));
    InstallCommand("sv_tracktime_reset", reinterpret_cast<void*>(&sv_tracktime_reset_f));
    PrintOut(PRINT_LOG, "[tracktime] measuring clc_move rate vs reported msec"
                        " (sv_tracktime / sv_tracktime_reset)\n");
}
