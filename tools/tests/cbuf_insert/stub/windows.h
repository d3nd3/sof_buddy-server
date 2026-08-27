// Minimal host stub of the pieces of <windows.h> cbuf_insert uses.
#pragma once
#include <cstdint>
#include <cstring>

typedef unsigned long DWORD;
typedef unsigned int  UINT;
typedef void* HMODULE;
typedef int BOOL;
typedef std::size_t SIZE_T;

#define IMAGE_DOS_SIGNATURE 0x5A4D
#define IMAGE_NT_SIGNATURE  0x00004550
#define MEM_COMMIT    0x1000
#define PAGE_GUARD    0x100
#define PAGE_NOACCESS 0x01
#define PAGE_READWRITE 0x04

typedef struct { void* BaseAddress; void* AllocationBase; DWORD AllocationProtect;
                 SIZE_T RegionSize; DWORD State; DWORD Protect; DWORD Type; }
        MEMORY_BASIC_INFORMATION;
typedef struct { std::uint16_t e_magic; std::uint16_t pad[29]; std::int32_t e_lfanew; }
        IMAGE_DOS_HEADER;
typedef struct { std::uint32_t SizeOfImage; } IMAGE_OPTIONAL_HEADER_STUB;
typedef struct { std::uint32_t Signature; IMAGE_OPTIONAL_HEADER_STUB OptionalHeader; }
        IMAGE_NT_HEADERS;
typedef union { struct { std::uint32_t LowPart; std::int32_t HighPart; }; std::int64_t QuadPart; }
        LARGE_INTEGER;

HMODULE GetModuleHandleA(const char* name);
SIZE_T  VirtualQuery(const void* addr, MEMORY_BASIC_INFORMATION* mbi, SIZE_T len);
BOOL QueryPerformanceCounter(LARGE_INTEGER* out);
BOOL QueryPerformanceFrequency(LARGE_INTEGER* out);
