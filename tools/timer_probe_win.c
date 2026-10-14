// PE timer probe (run: wine timer_probe_win.exe). Matches buddy_clock / qpc_timer APIs.
// Build: x86_64-w64-mingw32-gcc -O2 -o timer_probe_win.exe timer_probe_win.c

#include <stdio.h>
#include <windows.h>

typedef void(__cdecl* wine_host_t)(const char**, const char**);

static int wine_linux(void) {
    wine_host_t fn = (wine_host_t)GetProcAddress(GetModuleHandleA("ntdll.dll"), "wine_get_host_version");
    const char* sys = NULL;
    if (fn)
        fn(&sys, NULL);
    return sys && sys[0] == 'L';
}

#if defined(__x86_64__) && !defined(_MSC_VER)
static long linux_cputime_syscall(struct { long sec; long nsec; }* ts) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "0"(228), "D"(3), "S"(ts) : "rcx", "r11", "memory");
    return ret;
}
#elif defined(__i386__) && !defined(_MSC_VER)
static long linux_cputime_syscall(struct { long sec; long nsec; }* ts) {
    long ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "0"(265), "b"(3), "c"(ts) : "memory");
    return ret;
}
#endif

static int linux_cputime_ms(double* out) {
#if (defined(__x86_64__) || defined(__i386__)) && !defined(_MSC_VER)
    struct {
        long sec;
        long nsec;
    } ts = {0};
    long ret = linux_cputime_syscall(&ts);
    if (ret)
        return 0;
    *out = ts.sec * 1000.0 + ts.nsec / 1e6;
    return 1;
#else
    (void)out;
    return 0;
#endif
}

static double qpc_ms(void) {
    static LARGE_INTEGER f;
    static int ok = 0;
    LARGE_INTEGER c;
    if (!ok)
        ok = QueryPerformanceFrequency(&f) && f.QuadPart > 0;
    if (!ok || !QueryPerformanceCounter(&c))
        return -1;
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static double ft_ms(const FILETIME* ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft->dwLowDateTime;
    u.HighPart = ft->dwHighDateTime;
    return (double)u.QuadPart / 10000.0;
}

static double gtt_ms(void) {
    FILETIME cre, ex, kern, user;
    if (!GetThreadTimes(GetCurrentThread(), &cre, &ex, &kern, &user))
        return -1;
    return ft_ms(&kern) + ft_ms(&user);
}

static void busy(void) {
    volatile unsigned x = 0;
    for (int i = 0; i < 8000000; ++i)
        x += i;
}

static void hist_gtt_steps(void) {
    double prev = gtt_ms();
    int steps[64];
    memset(steps, 0, sizeof(steps));
    double maxd = 0;
    for (int i = 0; i < 200; ++i) {
        busy();
        double cur = gtt_ms();
        double d = cur - prev;
        prev = cur;
        if (d > maxd)
            maxd = d;
        int bucket = (int)(d + 0.5);
        if (bucket >= 0 && bucket < 64)
            steps[bucket]++;
    }
    printf("GetThreadTimes delta histogram (1 ms buckets, 200 busy loops, max=%.1f):\n", maxd);
    int any = 0;
    for (int b = 0; b < 64; ++b) {
        if (steps[b]) {
            printf("  %d ms: %d\n", b, steps[b]);
            any = 1;
        }
    }
    if (!any)
        printf("  (all deltas 0)\n");
}

int main(void) {
    int wine = GetProcAddress(GetModuleHandleA("ntdll.dll"), "wine_get_version") != NULL;
    printf("wine=%d host_linux=%d\n", wine, wine_linux());

    double q0 = qpc_ms(), q1;
    busy();
    q1 = qpc_ms();
    printf("QPC busy delta=%.4f ms\n", q1 - q0);

    q0 = qpc_ms();
    Sleep(16);
    q1 = qpc_ms();
    printf("QPC Sleep(16) delta=%.4f ms\n", q1 - q0);

    DWORD t0 = timeGetTime();
    Sleep(16);
    DWORD t1 = timeGetTime();
    printf("timeGetTime Sleep(16) delta=%lu ms\n", (unsigned long)(t1 - t0));

    struct {
        long sec;
        long nsec;
    } ts = {0};
#if (defined(__x86_64__) || defined(__i386__)) && !defined(_MSC_VER)
    long sret = linux_cputime_syscall(&ts);
    printf("cputime syscall ret=%ld t=%.6f ms\n", sret, ts.sec * 1000.0 + ts.nsec / 1e6);
#endif
    if (linux_cputime_ms(&q0)) {
        busy();
        if (linux_cputime_ms(&q1))
            printf("linux CLOCK_THREAD_CPUTIME syscall busy delta=%.4f ms\n", q1 - q0);
        else
            printf("linux cputime syscall failed after busy\n");
    } else
        printf("linux cputime syscall: unavailable (arch/build)\n");

    hist_gtt_steps();
    return 0;
}
