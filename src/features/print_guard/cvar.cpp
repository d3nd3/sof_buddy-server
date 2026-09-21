#include "cvar.h"
#include "print_guard_logic.h"

#include "buddy_import.h"

namespace {

constexpr int kCvarFlagArchive = 1;

void* g_cvBroadcast = nullptr;
void* g_cvClient = nullptr;

int ClampChars(int n, int hardChars) {
    if (n < 1)
        return 1;
    if (n > hardChars)
        return hardChars;
    return n;
}

int ReadChars(void* cv, int def, int hardChars) {
    return ClampChars(static_cast<int>(Buddy_ReadCvarValue(cv, static_cast<float>(def))),
                      hardChars);
}

}  // namespace

void Pg_InitCvars() {
    if (!g_cvBroadcast)
        g_cvBroadcast = Buddy_GetEngineCvar("_sofbuddy_printguard_broadcast_max", "999",
                                            kCvarFlagArchive, nullptr);
    if (!g_cvClient)
        g_cvClient = Buddy_GetEngineCvar("_sofbuddy_printguard_client_max", "1000",
                                         kCvarFlagArchive, nullptr);
}

int Pg_BroadcastMax() {
    return ReadChars(g_cvBroadcast, kPgSofplusBroadcastChars, kPgBroadcastChars);
}

int Pg_ClientMax() {
    return ReadChars(g_cvClient, kPgSofplusClientChars, kPgClientChars);
}

int Pg_MacroMax() {
    return kPgClientChars;
}

int Pg_CbufMax() {
    return kPgClientChars;
}
