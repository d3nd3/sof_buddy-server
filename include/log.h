#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define PRINT_BAD 2
#define PRINT_LOG 3
#define PRINT_LOG_EMPTY 4
#define PRINT_DEV 5

/** stderr + OutputDebugString only; never calls gi.dprintf. */
void LogFallbackImpl(int mode, const char* msg, ...);

/** Server console via gi.dprintf when import is bound. Tick-path output may
 *  tee to User/sof.log with logfile on; rcon-handled commands reply over UDP
 *  only. stderr + debugger before bind. */
void PrintOutImpl(int mode, const char* msg, ...);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#define PrintOut(mode, msg, ...) PrintOutImpl(mode, msg, ##__VA_ARGS__)
#endif

#ifdef __cplusplus
void* GetModuleBase(const char* moduleName);
#endif
