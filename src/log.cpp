#include "log.h"
#include "buddy_import.h"

#include <cstdarg>
#include <cstdio>
#include <windows.h>

/* The shim's own log file.
 *
 * stderr is not good enough on its own: a typical launch script redirects it
 * to /dev/null (this one did), and then every diagnostic this shim produces -
 * which detours installed, which refused, why - is silently thrown away. That
 * cost real debugging time, so the shim now always writes its own file next to
 * the working directory regardless of how the server was started. */
static FILE *ShimLog(void)
{
	static FILE *f = NULL;
	static bool tried = false;
	if (!tried) {
		tried = true;
		f = fopen("sofbuddy-shim.log", "a");
		if (f) {
			SYSTEMTIME st;
			GetLocalTime(&st);
			fprintf(f, "\n=== sof_buddy shim log opened %04u-%02u-%02u %02u:%02u:%02u ===\n",
			        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
			fflush(f);
		}
	}
	return f;
}

void PrintOutImpl(int mode, const char* msg, ...)
{
	char buf[2048];
	va_list ap;
	va_start(ap, msg);
	vsnprintf(buf, sizeof(buf), msg, ap);
	va_end(ap);
	if (mode == PRINT_BAD || mode == PRINT_LOG || mode == PRINT_DEV) {
		OutputDebugStringA(buf);
		fputs(buf, stderr);
		fflush(stderr);
		if (FILE *f = ShimLog()) {
			fputs(buf, f);
			fflush(f);
		}
	}
}

void PrintOutConsoleImpl(int mode, const char* msg, ...)
{
	char buf[2048];
	va_list ap;
	va_start(ap, msg);
	vsnprintf(buf, sizeof(buf), msg, ap);
	va_end(ap);

	PrintOutImpl(mode, "%s", buf);
	/* No-ops until the game import is bound, which is fine: everything that
	 * uses this runs at or after GameDllLoaded. */
	Buddy_DebugPrintf("%s", buf);
}
