#pragma once

// Monotonic clocks for short-interval measurement inside the server thread.
// QueryPerformanceCounter is sub-ms on Wine 8+, but it is wall time: off-CPU
// gaps (scheduler, etc.) still show up in the delta.
// Thread-CPU mode prefers host CLOCK_THREAD_CPUTIME_ID under Wine on Linux
// (nanosec); Wine's GetThreadTimes maps /proc jiffies (~10 ms at HZ=100).

#include <windows.h>

#include <cstdint>

inline bool Buddy_IsWine() {
    static int cached = -1;
    if (cached >= 0)
        return cached != 0;
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    cached = (ntdll && GetProcAddress(ntdll, "wine_get_version")) ? 1 : 0;
    return cached != 0;
}

inline bool Buddy_WineHostIsLinux() {
    static int cached = -1;
    if (cached >= 0)
        return cached != 0;
    if (!Buddy_IsWine())
        return cached = 0;
    using wine_get_host_version_t = void(__cdecl*)(const char**, const char**);
    auto fn = reinterpret_cast<wine_get_host_version_t>(
        GetProcAddress(GetModuleHandleA("ntdll.dll"), "wine_get_host_version"));
    const char* sys = nullptr;
    if (fn)
        fn(&sys, nullptr);
    cached = (sys && sys[0] == 'L' && sys[1] == 'i' && sys[2] == 'n' && sys[3] == 'u' &&
              sys[4] == 'x')
                 ? 1
                 : 0;
    return cached != 0;
}

enum class BuddyClockMode { WallQpc = 0, ThreadCpu = 1 };

inline BuddyClockMode& BuddyClockModeSlot() {
    static BuddyClockMode mode = BuddyClockMode::WallQpc;
    return mode;
}

inline void BuddyClockSetMode(BuddyClockMode mode) {
    BuddyClockModeSlot() = mode;
}

inline BuddyClockMode BuddyClockGetMode() {
    return BuddyClockModeSlot();
}

inline bool BuddyClockInitQpc(LARGE_INTEGER& freq) {
    return QueryPerformanceFrequency(&freq) && freq.QuadPart > 0;
}

struct BuddyTimespec {
    long tv_sec;
    long tv_nsec;
};

#if defined(__x86_64__) && !defined(_MSC_VER)
inline bool BuddyLinuxClockGettimeThreadCpu(BuddyTimespec* ts) {
    if (!ts)
        return false;
    long ret = 0;
    constexpr long kNrClockGettime = 228;
    constexpr long kClockThreadCputimeId = 3;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "0"(kNrClockGettime), "D"(kClockThreadCputimeId), "S"(ts)
                     : "rcx", "r11", "memory");
    return ret == 0;
}
#elif defined(__i386__) && !defined(_MSC_VER)
inline bool BuddyLinuxClockGettimeThreadCpu(BuddyTimespec* ts) {
    if (!ts)
        return false;
    long ret = 0;
    constexpr long kNrClockGettime = 265;
    constexpr long kClockThreadCputimeId = 3;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "0"(kNrClockGettime), "b"(kClockThreadCputimeId), "c"(ts)
                     : "memory");
    return ret == 0;
}
#else
inline bool BuddyLinuxClockGettimeThreadCpu(BuddyTimespec*) {
    return false;
}
#endif

inline bool BuddyClockThreadCpuLinuxMs(double& out_ms) {
    BuddyTimespec ts = {};
    if (!BuddyLinuxClockGettimeThreadCpu(&ts))
        return false;
    out_ms = static_cast<double>(ts.tv_sec) * 1000.0 + static_cast<double>(ts.tv_nsec) / 1e6;
    return true;
}

inline double BuddyClockThreadCpuFiletimeMs() {
    FILETIME _cre, _exit, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &_cre, &_exit, &kernel, &user))
        return -1.0;
    auto ft100ns = [](const FILETIME& ft) -> std::uint64_t {
        ULARGE_INTEGER u;
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        return u.QuadPart;
    };
    return static_cast<double>(ft100ns(kernel) + ft100ns(user)) / 10000.0;
}

// Cumulative ms for the current thread; use deltas only.
inline double BuddyClockThreadCpuMs() {
    if (Buddy_IsWine() && Buddy_WineHostIsLinux()) {
        double ms = 0.0;
        if (BuddyClockThreadCpuLinuxMs(ms))
            return ms;
    }
    return BuddyClockThreadCpuFiletimeMs();
}

inline const char* BuddyClockThreadCpuBackendLabel() {
    if (BuddyClockGetMode() != BuddyClockMode::ThreadCpu)
        return "wall-qpc";
    if (Buddy_IsWine() && Buddy_WineHostIsLinux())
        return "thread-cpu (linux cputime)";
    if (Buddy_IsWine())
        return "thread-cpu (getthreadtimes)";
    return "thread-cpu";
}

inline double BuddyClockWallQpcMs(const LARGE_INTEGER& freq) {
    LARGE_INTEGER now = {};
    if (!QueryPerformanceCounter(&now))
        return -1.0;
    return static_cast<double>(now.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

inline double BuddyClockWallNowMs() {
    static LARGE_INTEGER qpc_freq = {};
    static bool qpc_ok = false;
    if (!qpc_ok)
        qpc_ok = BuddyClockInitQpc(qpc_freq);
    if (!qpc_ok)
        return 0.0;
    return BuddyClockWallQpcMs(qpc_freq);
}

// Thread CPU ms (lagometer buckets); ignores WallQpc mode.
inline double BuddyClockCpuNowMs() {
    const double cpu = BuddyClockThreadCpuMs();
    return cpu >= 0.0 ? cpu : 0.0;
}

inline double BuddyClockNowMs() {
    if (BuddyClockGetMode() == BuddyClockMode::ThreadCpu)
        return BuddyClockCpuNowMs();
    return BuddyClockWallNowMs();
}
