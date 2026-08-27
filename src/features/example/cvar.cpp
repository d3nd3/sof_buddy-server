#include "cvar.h"
#include "buddy_import.h"

namespace {

void* g_cvExampleEnabled = nullptr;

}  // namespace

void Example_InitCvars() {
    if (!g_cvExampleEnabled) {
        g_cvExampleEnabled = Buddy_GetEngineCvar("_sofbuddy_example_enabled", "1", 0, nullptr);
    }
}

bool Example_IsEnabled() {
    Example_InitCvars();
    if (!g_cvExampleEnabled) {
        return true;
    }
    return Buddy_ReadCvarValue(g_cvExampleEnabled, 1.0f) != 0.0f;
}