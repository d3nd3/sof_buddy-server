#include "log.h"
#include "buddy_import.h"

#include <cstdarg>
#include <cstdio>
#include <windows.h>

static void LogFallback(const char* buf)
{
	OutputDebugStringA(buf);
	fputs(buf, stderr);
	fflush(stderr);
}

void LogFallbackImpl(int mode, const char* msg, ...)
{
	char buf[2048];
	va_list ap;
	va_start(ap, msg);
	vsnprintf(buf, sizeof(buf), msg, ap);
	va_end(ap);
	if (mode == PRINT_BAD || mode == PRINT_LOG || mode == PRINT_DEV)
		LogFallback(buf);
}

void PrintOutImpl(int mode, const char* msg, ...)
{
	char buf[2048];
	va_list ap;
	va_start(ap, msg);
	vsnprintf(buf, sizeof(buf), msg, ap);
	va_end(ap);
	if (mode != PRINT_BAD && mode != PRINT_LOG && mode != PRINT_DEV)
		return;
	if (Buddy_GetGameImport())
		Buddy_DebugPrintf("%s", buf);
	else
		LogFallback(buf);
}
