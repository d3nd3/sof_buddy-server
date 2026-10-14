// Platform glue: locating the engine image. Kept apart from zpool.cpp so the
// pool logic stays free of <windows.h> and can be exercised host-side.

#include "zpool.h"

#include <windows.h>

namespace zpool {
namespace {

HMODULE EngineModule() {
    // The dedicated server ships under several names, and in every case the
    // engine *is* the process image - so the unnamed handle is the reliable
    // answer; the named lookups just make the common cases obvious.
    if (HMODULE h = GetModuleHandleA("SoF.exe"))
        return h;
    if (HMODULE h = GetModuleHandleA("SoF-spsv.exe"))
        return h;
    return GetModuleHandleA(nullptr);
}

}  // namespace

bool Bind() {
    HMODULE mod = EngineModule();
    if (!mod)
        return false;
    return BindAt(reinterpret_cast<unsigned char*>(mod));
}

}  // namespace zpool
