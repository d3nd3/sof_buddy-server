// Build on chinserverA (Linux): cc -O2 -o wine_clock_probe wine_clock_probe.c
// Run under Wine: wine wine_clock_probe.exe   OR native: ./wine_clock_probe
// Expect sub-ms deltas on "linux cputime" line when built for win64 with mingw under wine.

#include <stdio.h>
#include <windows.h>

typedef void(__cdecl* wine_get_host_version_t)(const char**, const char**);

static int is_wine(void) {
    HMODULE n = GetModuleHandleA("ntdll.dll");
    return n && GetProcAddress(n, "wine_get_version");
}

static int host_linux(void) {
    wine_get_host_version_t fn = (wine_get_host_version_t)GetProcAddress(
        GetModuleHandleA("ntdll.dll"), "wine_get_host_version");
    const char* sys = NULL;
    if (fn)
        fn(&sys, NULL);
    return sys && sys[0] == 'L';
}

#if defined(__x86_64__) && !defined(_MSC_VER)
static int thread_cputime_ms(double* out) {
    struct {
        long sec;
        long nsec;
    } ts = {0};
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "0"(228), "D"(3), "S"(&ts)
                     : "rcx", "r11", "memory");
    if (ret)
        return 0;
    *out = ts.sec * 1000.0 + ts.nsec / 1e6;
    return 1;
}
#else
static int thread_cputime_ms(double* out) {
    (void)out;
    return 0;
}
#endif

static double filetime_cpu_ms(void) {
    FILETIME a, b, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &a, &b, &k, &u))
        return -1;
    ULARGE_INTEGER ku, uu;
    ku.LowPart = k.dwLowDateTime;
    ku.HighPart = k.dwHighDateTime;
    uu.LowPart = u.dwLowDateTime;
    uu.HighPart = u.dwHighDateTime;
    return (ku.QuadPart + uu.QuadPart) / 10000.0;
}

int main(void) {
    printf("wine=%d host_linux=%d\n", is_wine(), host_linux());
    double t0 = 0, t1 = 0;
    if (thread_cputime_ms(&t0)) {
        volatile unsigned x = 0;
        for (int i = 0; i < 500000; ++i)
            x += i;
        thread_cputime_ms(&t1);
        printf("linux cputime delta=%.4f ms\n", t1 - t0);
    }
    double f0 = filetime_cpu_ms(), f1 = filetime_cpu_ms();
    printf("getthreadtimes delta=%.4f ms (expect ~0 or 10ms steps)\n", f1 - f0);
    return 0;
}
