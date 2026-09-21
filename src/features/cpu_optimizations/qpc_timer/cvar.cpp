#include "cvar.h"

#include "buddy_import.h"

#include <windows.h>

namespace qpctimer {
namespace {

constexpr int kCvarFlagArchive = 1;

void* g_cvQpc = nullptr;

}  // namespace

void InitCvars() {
    if (!g_cvQpc)
        g_cvQpc = Buddy_GetEngineCvar("_sofbuddy_qpc", "1", kCvarFlagArchive, nullptr);
}

bool UseQpcClock() {
    return Buddy_ReadCvarValue(g_cvQpc, 1.0f) != 0.0f;
}

}  // namespace qpctimer
