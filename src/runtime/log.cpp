#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <windows.h>

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
	}
}
