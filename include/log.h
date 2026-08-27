#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define PRINT_BAD 2
#define PRINT_LOG 3
#define PRINT_LOG_EMPTY 4
#define PRINT_DEV 5

void PrintOutImpl(int mode, const char* msg, ...);

/** Like PrintOutImpl, but ALSO prints to the server console via gi.dprintf, so
 *  the line lands somewhere an operator can actually see it - the console and,
 *  with `logfile 1`, User/sof.log. PrintOut alone goes to stderr and the
 *  debugger, and a typical launch script discards stderr.
 *
 *  Bootstrap context only (e.g. GameDllLoaded): it calls into game_import_t. */
void PrintOutConsoleImpl(int mode, const char* msg, ...);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#define PrintOut(mode, msg, ...) PrintOutImpl(mode, msg, ##__VA_ARGS__)
#define PrintOutConsole(mode, msg, ...) PrintOutConsoleImpl(mode, msg, ##__VA_ARGS__)
#endif

#ifdef __cplusplus
void* GetModuleBase(const char* moduleName);
#endif
