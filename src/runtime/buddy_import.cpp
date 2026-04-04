#include "buddy_import.h"

#include <cstdint>

namespace {

void* g_gi = nullptr;
void* g_respawn_cvar = nullptr;

using cvar_fn = void* (*)(const char*, const char*, int, void*);

// game_import_t::cvar — 32-bit, one slot per pointer. Set 1 if your game DLL was built with
// (!_FINAL_ && _RAVEN_) so GetLabel exists before the cvar block (see game.h).
#if defined(SOF_GI_HAS_GETLABEL) && SOF_GI_HAS_GETLABEL
constexpr std::uint32_t kGiSlotCvar = 88;
#else
constexpr std::uint32_t kGiSlotCvar = 87;
#endif

constexpr unsigned kGiOffCvar = kGiSlotCvar * 4u;
constexpr unsigned kCvarValueOfs = 0x18;

cvar_fn GiCvarFn() {
    if (!g_gi)
        return nullptr;
    return *reinterpret_cast<cvar_fn*>(static_cast<char*>(g_gi) + kGiOffCvar);
}

void EnsureRespawnCvar() {
    if (g_respawn_cvar || !g_gi)
        return;
    cvar_fn f = GiCvarFn();
    if (!f)
        return;
    g_respawn_cvar = f("_sofbuddy_custom_respawn", "0", 0, nullptr);
}

}  // namespace

void Buddy_BindGameImport(void* import) {
    g_gi = import;
    EnsureRespawnCvar();
}

int Buddy_CustomRespawnEnabled() {
    EnsureRespawnCvar();
    if (!g_respawn_cvar)
        return 0;
    float v = *reinterpret_cast<float*>(static_cast<char*>(g_respawn_cvar) + kCvarValueOfs);
    return v != 0.f ? 1 : 0;
}
