// Sleep-skip gate for WinMain's dedicated-server Sleep(1).
//
// IDA, SoF.exe base 0x20000000 (SoF-spsv.exe shares .text):
//   0x2006639D  push 1                  ; dwMilliseconds
//   0x2006639F  call ds:Sleep           ; IAT slot @0x2011114C (FF 15 disp32)
//   0x200663A5  ...                     ; return address
// Reached only when the `dedicated` cvar exists and is nonzero; the client
// loop (quit path) never sleeps here.
//
// Mechanism: patch the IAT slot to our gate. The gate skips the sleep only
// for that one return address, only for 1ms sleeps, and only while a tick is
// due within the spin window on a running dedicated map. Everything else
// falls through to the real Sleep. `_sofbuddy_tickpace_spin_ms 0` is
// bit-identical stock for the spin path (default is 2).
//
// Reload safety: spsv FreeLibrary/reloads this DLL between game restarts, so
// a previous instance may have left our gate address in the slot. Install
// writes the gate unconditionally (after proving the slot readable and noting
// what it held), and remove writes the real Sleep back. We are the only
// writer of this slot - spsv hooks WinMain's *other* call (its
// Sys_Millisec_Hook patches the Sys_Milliseconds call at 0x20066412).

#include "sleep_gate.h"
#include "cvar.h"
#include "engine.h"
#include "../cpuopt.h"
#include "../cmdtext_parking/cmdpark.h"

#include "buddy_import.h"
#include "log.h"

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#include <cstdint>

namespace tickpace {
namespace {

// IAT slot of Sleep in the engine image, and the WinMain return address.
constexpr unsigned kRvaSleepIAT = 0x11114C;      // ds:Sleep @0x2011114C
constexpr unsigned kRvaWinMainSleepRet = 0x663A5;  // past `call ds:Sleep`

typedef void(WINAPI* SleepFn)(DWORD);

SleepFn g_origSleep = nullptr;
void** g_slot = nullptr;
bool g_installed = false;

HMODULE SleepGateExeMod() {
    if (HMODULE h = GetModuleHandleA("SoF.exe"))
        return h;
    if (HMODULE h = GetModuleHandleA("SoF-spsv.exe"))
        return h;
    return GetModuleHandleA(nullptr);
}

bool WritableDword(void* p) {
    if (!p)
        return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT)
        return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))
        return false;
    const auto addr = reinterpret_cast<std::uintptr_t>(p);
    return addr + sizeof(void*) <=
           reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

bool WriteSlot(void** slot, void* value) {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
        return false;
    *slot = value;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

inline void* CallerReturnAddress() {
#if defined(_MSC_VER)
    return _ReturnAddress();
#else
    return __builtin_return_address(0);
#endif
}

void WINAPI SleepGate(DWORD ms) {
    void* ret = CallerReturnAddress();
    if (ret != nullptr && ret == SleepGate_WinMainSleepRet() &&
        SleepGate_ShouldSkip(ms, ret)) {
        return;
    }
    SleepFn orig = g_origSleep;
    if (orig)
        orig(ms);
}

}  // namespace

void* SleepGate_WinMainSleepRet() {
    HMODULE exe = SleepGateExeMod();
    if (!exe)
        return nullptr;
    return reinterpret_cast<char*>(exe) + kRvaWinMainSleepRet;
}

bool SleepGate_ShouldSkip(DWORD ms, void* retaddr) {
    if (ms != 1)
        return false;
    if (retaddr == nullptr || retaddr != SleepGate_WinMainSleepRet())
        return false;
    if (!EngineReady())
        return false;
    if (!IsDedicated() || !ServerRunning())
        return false;

    const Config cfg = ReadConfig();

    // `_sofbuddy_tickpace` master (also off when `_sofbuddy_cpuopt 0`): drain-
    // boundary one-shot from SvFramePre (skip Sleep only, no msec credit).
    if (cfg.enabled && ConsumeSkipNextSleep())
        return true;

    // `cmdtext_parking`: armed (strict or reserve_ms) and side store non-empty.
    if (cmdpark::HoldArmed() && cmdpark::ParkBytes() > 0)
        return true;

    // Spin window: `_sofbuddy_tickpace` master && `_sofbuddy_tickpace_spin_ms` > 0.
    if (!cfg.enabled)
        return false;
    const float window = SpinWindowMs();
    if (!(window > 0.0f))
        return false;
    const std::int64_t realtime =
        static_cast<std::int64_t>(*Engine().svsRealtime) -
        static_cast<std::int64_t>(CurrentSettleMs());
    const std::int32_t until = static_cast<std::int32_t>(
        static_cast<std::int64_t>(*Engine().svTime) - realtime);
    return static_cast<double>(until) <= static_cast<double>(window);
}

void SleepGate_Install() {
    if (g_installed)
        return;
    HMODULE exe = SleepGateExeMod();
    if (!exe) {
        PrintOut(PRINT_BAD, "[tick_pacing] sleep gate: engine module not found - off\n");
        return;
    }
    void** slot = reinterpret_cast<void**>(reinterpret_cast<char*>(exe) + kRvaSleepIAT);
    if (!WritableDword(slot)) {
        PrintOut(PRINT_BAD, "[tick_pacing] sleep gate: IAT slot not writable - off\n");
        return;
    }
    HMODULE kernel = GetModuleHandleA("kernel32");
    void* realSleep = kernel ? reinterpret_cast<void*>(GetProcAddress(kernel, "Sleep")) : nullptr;
    if (!realSleep) {
        PrintOut(PRINT_BAD, "[tick_pacing] sleep gate: kernel32 Sleep not found - off\n");
        return;
    }
    g_origSleep = reinterpret_cast<SleepFn>(realSleep);
    g_slot = slot;
    if (!WriteSlot(slot, reinterpret_cast<void*>(&SleepGate))) {
        PrintOut(PRINT_BAD, "[tick_pacing] sleep gate: patch failed - off\n");
        g_origSleep = nullptr;
        g_slot = nullptr;
        return;
    }
    g_installed = true;
}

void SleepGate_Remove() {
    if (!g_installed || !g_slot)
        return;
    HMODULE kernel = GetModuleHandleA("kernel32");
    void* realSleep = kernel ? reinterpret_cast<void*>(GetProcAddress(kernel, "Sleep")) : nullptr;
    if (realSleep)
        WriteSlot(g_slot, realSleep);
    g_installed = false;
    g_origSleep = nullptr;
    g_slot = nullptr;
}

}  // namespace tickpace
