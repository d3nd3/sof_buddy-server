#pragma once
#include <cstdint>
#include <cstring>
typedef unsigned long DWORD;
typedef void* HMODULE;
typedef int BOOL;
typedef std::size_t SIZE_T;
typedef union { struct { std::uint32_t LowPart; std::int32_t HighPart; }; std::int64_t QuadPart; }
        LARGE_INTEGER;
BOOL QueryPerformanceCounter(LARGE_INTEGER* out);
BOOL QueryPerformanceFrequency(LARGE_INTEGER* out);
inline HMODULE GetModuleHandleA(const char*) { return nullptr; }
inline void ExitProcess(unsigned) {}
