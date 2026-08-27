#include "buddy_import.h"
#include "log.h"

#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <windows.h>

namespace {

// game_import_t is only valid for the duration of the engine's GetGameAPI
// call - it's a transient struct (often stack-allocated on the engine side).
// Every stock game DLL knows this and immediately snapshots it: the SDK
// source (reference/engine/gamecpp/g_main.cpp) does `gi = *import;` into its
// own persistent global `game_import_t gi;`, and the compiled retail
// GetGameAPI does the equivalent `qmemcpy(&GameImport_t, a1, 0x18Cu)`
// (verified in IDA). Caching the raw `import` pointer instead of copying it
// (as this file used to) meant early calls (made during/soon after bootstrap,
// before that stack region gets reused) worked, but any later call - like a
// broadcast fired from a hook thousands of ticks after boot - read reused,
// unrelated stack memory as if it were the import table. That's what caused
// a crash (garbage function pointer) in exactly that scenario.
constexpr std::size_t kGameImportSize = 0x18C;
unsigned char g_giCopy[kGameImportSize] = {};
void* g_gi = nullptr;

// Defense-in-depth for slot-table reads: a garbage function pointer (bad
// slot offset, uninitialized/corrupted game_import_t) would otherwise crash
// by jumping into unmapped or non-executable memory with no diagnostic. This
// converts that into a safe, logged no-op instead.
bool IsExecutableCodeAddress(const void* ptr) {
    if (!ptr)
        return false;
    MEMORY_BASIC_INFORMATION mbi = {0};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0)
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    const DWORD execMask = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & execMask) != 0;
}

// Resolves and validates a game_import_t function-pointer slot in one place,
// so every gi.* wrapper below gets the same "garbage pointer fails safely,
// logged, instead of crashing" behavior that Buddy_BroadcastPrintf originally
// had inline (see IsExecutableCodeAddress above / the crash root-cause note
// in clamp_monitor's README).
template <typename FnT>
FnT ResolveGiSlot(unsigned slot, const char* who) {
    if (!g_gi)
        return nullptr;
    FnT f = *reinterpret_cast<FnT*>(static_cast<char*>(g_gi) + slot * 4u);
    if (!f)
        return nullptr;
    if (!IsExecutableCodeAddress(reinterpret_cast<const void*>(f))) {
        PrintOut(PRINT_BAD, "[buddy_import] %s: slot value %p is not executable memory - aborting call\n",
                 who, reinterpret_cast<const void*>(f));
        return nullptr;
    }
    return f;
}

using cvar_fn = void* (*)(const char*, const char*, int, void*);

// game_import_t::cvar — 32-bit, one slot per pointer.
// Offset confirmed three ways: SDK reference/engine/gamecpp/game.h field order,
// IDA on retail gamex86.dll (GameImport_t 0x5015C9F0, cvar slot 0x5015CB4C),
// and IDA on Linux game.so (Cvar_Get at 0x15C).
constexpr std::uint32_t kGiSlotCvar = 87;
constexpr unsigned kGiOffCvar = kGiSlotCvar * 4u;
constexpr unsigned kCvarValueOfs = 0x18;

cvar_fn GiCvarFn() {
    if (!g_gi)
        return nullptr;
    return *reinterpret_cast<cvar_fn*>(static_cast<char*>(g_gi) + kGiOffCvar);
}

}  // namespace

void Buddy_BindGameImport(void* import) {
    if (!import)
        return;
    std::memcpy(g_giCopy, import, kGameImportSize);
    g_gi = g_giCopy;
}

void* Buddy_GetGameImport(void) {
    return g_gi;
}

void* Buddy_GetEngineCvar(const char* var_name, const char* value, int flags, void* command) {
    if (!var_name || !value || !g_gi)
        return nullptr;
    cvar_fn f = GiCvarFn();
    if (!f)
        return nullptr;
    return f(var_name, value, flags, command);
}

// gi.cvar_setvalue — slot 89 (0x164), see docs/game_import_t.md.
using setvalue_fn = void (*)(const char*, float);

void Buddy_SetEngineCvarValue(const char* var_name, float value) {
    if (!var_name || !g_gi)
        return;
    auto f = *reinterpret_cast<setvalue_fn*>(static_cast<char*>(g_gi) + 89u * 4u);
    if (f)
        f(var_name, value);
}

// gi.bprintf — slot 12 (0x30), variadic broadcast.
using bprintf_fn = void (*)(int, const char*, ...);

void Buddy_BroadcastPrintf(int print_level, const char* fmt, ...) {
    if (!fmt)
        return;
    auto f = ResolveGiSlot<bprintf_fn>(12u, "bprintf");
    if (!f)
        return;

    /* A va_list can't be forwarded as a single vararg to another "..."
     * function (that passes the va_list's own pointer as the callee's first
     * format argument, not "the rest of our args" - garbage or a crash).
     * Format once ourselves, then hand the engine a fixed "%s" so it does no
     * further format-string interpretation of caller-controlled content. */
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    f(print_level, "%s", buf);
}

// gi.dprintf — slot 13 (0x34): void (*dprintf)(char *fmt, ...). Server
// console/log only; no client ever receives this.
using dprintf_fn = void (*)(const char*, ...);

void Buddy_DebugPrintf(const char* fmt, ...) {
    if (!fmt)
        return;
    auto f = ResolveGiSlot<dprintf_fn>(13u, "dprintf");
    if (!f)
        return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    f("%s", buf);
}

// gi.cprintf — slot 14 (0x38): void (*cprintf)(edict_t *ent, int printlevel, char *fmt, ...).
// Prints to one client (ent == NULL is the server console, same as dprintf).
using cprintf_fn = void (*)(void*, int, const char*, ...);

void Buddy_ClientPrintf(void* ent, int print_level, const char* fmt, ...) {
    if (!fmt)
        return;
    auto f = ResolveGiSlot<cprintf_fn>(14u, "cprintf");
    if (!f)
        return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    f(ent, print_level, "%s", buf);
}

// gi.clprintf — slot 15 (0x3C): void (*clprintf)(edict_t *ent, edict_t *from, int color, char *fmt, ...).
// Name/color-tagged line attributed to `from`, delivered to `ent`.
using clprintf_fn = void (*)(void*, void*, int, const char*, ...);

void Buddy_NamePrintf(void* ent, void* from, int color, const char* fmt, ...) {
    if (!fmt)
        return;
    auto f = ResolveGiSlot<clprintf_fn>(15u, "clprintf");
    if (!f)
        return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    f(ent, from, color, "%s", buf);
}

// gi.welcomeprint — slot 16 (0x40): void (*welcomeprint)(edict_t *ent). No format args.
using welcomeprint_fn = void (*)(void*);

void Buddy_WelcomePrintf(void* ent) {
    auto f = ResolveGiSlot<welcomeprint_fn>(16u, "welcomeprint");
    if (!f)
        return;
    f(ent);
}

// gi.centerprintf — slot 17 (0x44): void (*centerprintf)(edict_t *ent, char *fmt, ...).
using centerprintf_fn = void (*)(void*, const char*, ...);

void Buddy_CenterPrintf(void* ent, const char* fmt, ...) {
    if (!fmt)
        return;
    auto f = ResolveGiSlot<centerprintf_fn>(17u, "centerprintf");
    if (!f)
        return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    f(ent, "%s", buf);
}

// gi.cinprintf — slot 18 (0x48): void (*cinprintf)(edict_t *ent, int x, int y, int textspeed, char *text).
// Not a printf - `text` is passed through as-is, no format interpretation on either side.
using cinprintf_fn = void (*)(void*, int, int, int, const char*);

void Buddy_CinPrintf(void* ent, int x, int y, int textspeed, const char* text) {
    if (!text)
        return;
    auto f = ResolveGiSlot<cinprintf_fn>(18u, "cinprintf");
    if (!f)
        return;
    f(ent, x, y, textspeed, text);
}

// gi.bcaption — slot 19 (0x4C): void (*bcaption)(int printlevel, unsigned short ID).
// Broadcasts a StringPackage-registered caption string (by ID, see gi.SP_Register /
// gi.SP_Print) to all clients; not free-form text.
using bcaption_fn = void (*)(int, unsigned short);

void Buddy_BroadcastCaption(int print_level, unsigned short string_id) {
    auto f = ResolveGiSlot<bcaption_fn>(19u, "bcaption");
    if (!f)
        return;
    f(print_level, string_id);
}

// gi.captionprintf — slot 20 (0x50): void (*captionprintf)(edict_t *ent, unsigned short ID).
using captionprintf_fn = void (*)(void*, unsigned short);

void Buddy_CaptionPrintf(void* ent, unsigned short string_id) {
    auto f = ResolveGiSlot<captionprintf_fn>(20u, "captionprintf");
    if (!f)
        return;
    f(ent, string_id);
}

float Buddy_ReadCvarValue(void* cv, float default_val) {
    if (!cv)
        return default_val;
    return *reinterpret_cast<float*>(static_cast<char*>(cv) + kCvarValueOfs);
}
