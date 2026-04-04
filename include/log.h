#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define PRINT_BAD 2
#define PRINT_LOG 3
#define PRINT_LOG_EMPTY 4
#define PRINT_DEV 5

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
