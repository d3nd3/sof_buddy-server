// Native Linux timer grain (host under Wine). Build: cc -O2 -o timer_probe_linux timer_probe_linux.c
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>

static double timespec_ms(const struct timespec* ts) {
    return ts->tv_sec * 1000.0 + ts->tv_nsec / 1e6;
}

static int clock_ms(clockid_t id, double* out) {
    struct timespec ts;
    if (clock_gettime(id, &ts) != 0)
        return 0;
    *out = timespec_ms(&ts);
    return 1;
}

static void busy(unsigned n) {
    volatile unsigned x = 0;
    for (unsigned i = 0; i < n; ++i)
        x += i;
}

int main(void) {
    long hz = sysconf(_SC_CLK_TCK);
    printf("CLK_TCK=%ld  (jiffy_ms=%.3f)\n", hz, hz > 0 ? 1000.0 / hz : 0.0);

    double m0 = 0, m1 = 0, c0 = 0, c1 = 0;
    if (clock_ms(CLOCK_MONOTONIC, &m0) && clock_ms(CLOCK_THREAD_CPUTIME_ID, &c0)) {
        busy(8000000);
        clock_ms(CLOCK_MONOTONIC, &m1);
        clock_ms(CLOCK_THREAD_CPUTIME_ID, &c1);
        printf("CLOCK_MONOTONIC busy delta=%.4f ms (wall, expect sub-ms+)\n", m1 - m0);
        printf("CLOCK_THREAD_CPUTIME_ID busy delta=%.4f ms (cpu, expect sub-ms+)\n", c1 - c0);
    }

    if (clock_ms(CLOCK_MONOTONIC, &m0)) {
        struct timespec req = {0, 16000000L};
        nanosleep(&req, NULL);
        clock_ms(CLOCK_MONOTONIC, &m1);
        printf("CLOCK_MONOTONIC sleep 16ms delta=%.4f ms\n", m1 - m0);
    }

    return 0;
}
