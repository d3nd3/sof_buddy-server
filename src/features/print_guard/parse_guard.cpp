// parse_guard: tighten COM_Parse's com_token bound.
//
// Engine layout (SoF.exe / SoF-spsv.exe, IDA-verified, base 0x20000000):
//   COM_Parse @ 0x20055470, com_token @ 0x20390B10 (256 bytes: the next
//   global starts at 0x20390C10).
//
// Stock bug: both store gates use `cmp edx, 0x100`, so a 256+ char token
// stores 256 chars (indices 0..255) and the epilogue
//   0x2005550E  mov byte_20390B10[edx], 0
// writes the NUL at [256] — one byte past the buffer, clobbering the low
// byte of the dword at 0x20390C10. (The user-visible symptom was a 256-char
// print where 255 + NUL was the most that could ever be correct.)
//
// Fix: retune both store-gate immediates 0x100 -> 0xFF. At most 255 chars
// are stored and the NUL lands at [edx] <= [255]. deliberately NOT touched:
// the exit gate @ 0x20055504 (`cmp edx,0x100` -> empty token on unquoted
// overflow) becomes dead code — edx can no longer reach 0x100 — which also
// means over-long *unquoted* tokens now truncate to 255 instead of coming
// back empty. That matches the quoted path and is the requested behaviour.
//
// Method follows hash_lookup/patch.cpp: verify every site against the exact
// bytes reverse-engineered from retail SoF.exe before touching anything
// (all-or-nothing). A mismatch disables the fix with a log line. The patch
// is idempotent, which matters because spsv reloads gamex86 between games.

#include "parse_guard.h"

#include "log.h"

#include <cstdint>
#include <cstring>
#include <windows.h>

namespace {

constexpr unsigned kAbsQuotedGate = 0x200554DA;    // cmp edx, 0x100 (quoted store)
constexpr unsigned kAbsUnquotedGate = 0x200554EB;  // cmp edx, 0x100 (unquoted store)
// 81 FA imm32 — verify-before-write pattern, imm32 read from retail binary.
constexpr std::uint8_t kCmpEdxPrefix[2] = {0x81, 0xFA};
constexpr std::uint32_t kStockCap = 0x100;
constexpr std::uint32_t kFixedCap = 0xFF;  // 255 chars, NUL at [255]

bool Readable(const void* p, unsigned len) {
    return !IsBadReadPtr(const_cast<void*>(p), len);
}

bool SiteIsStock(unsigned absAddr) {
    const auto* at = reinterpret_cast<const std::uint8_t*>(
        static_cast<std::uintptr_t>(absAddr));
    if (!Readable(at, 6))
        return false;
    std::uint32_t imm = 0;
    std::memcpy(&imm, at + 2, sizeof(imm));
    return std::memcmp(at, kCmpEdxPrefix, sizeof(kCmpEdxPrefix)) == 0 &&
           imm == kStockCap;
}

bool SiteIsFixed(unsigned absAddr) {
    const auto* at = reinterpret_cast<const std::uint8_t*>(
        static_cast<std::uintptr_t>(absAddr));
    if (!Readable(at, 6))
        return false;
    std::uint32_t imm = 0;
    std::memcpy(&imm, at + 2, sizeof(imm));
    return std::memcmp(at, kCmpEdxPrefix, sizeof(kCmpEdxPrefix)) == 0 &&
           imm == kFixedCap;
}

bool WriteImm(unsigned absAddr, std::uint32_t imm) {
    auto* at = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(absAddr)) + 2;
    DWORD old = 0;
    if (!VirtualProtect(at, sizeof(imm), PAGE_EXECUTE_READWRITE, &old))
        return false;
    std::memcpy(at, &imm, sizeof(imm));
    VirtualProtect(at, sizeof(imm), old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, sizeof(imm));
    return true;
}

}  // namespace

bool Pg_InstallParseGuard() {
    static bool installed = false;
    if (installed)
        return true;

    const unsigned sites[2] = {kAbsQuotedGate, kAbsUnquotedGate};
    bool allFixed = true;
    for (unsigned s : sites)
        allFixed = allFixed && SiteIsFixed(s);
    if (allFixed) {
        installed = true;
        return true;
    }

    // All-or-nothing: prove both sites are stock before writing either.
    for (unsigned s : sites) {
        if (!SiteIsStock(s)) {
            PrintOut(PRINT_BAD,
                     "[print_guard] COM_Parse @0x%X: unexpected bytes - "
                     "engine build not recognised, parse guard disabled\n",
                     s);
            return false;
        }
    }
    for (unsigned s : sites) {
        if (!WriteImm(s, kFixedCap)) {
            PrintOut(PRINT_BAD,
                     "[print_guard] COM_Parse @0x%X: VirtualProtect failed - "
                     "parse guard disabled\n",
                     s);
            return false;
        }
    }
    // Re-read to prove the new bound is really in place.
    for (unsigned s : sites) {
        if (!SiteIsFixed(s)) {
            PrintOut(PRINT_BAD,
                     "[print_guard] COM_Parse @0x%X: verify failed - "
                     "parse guard disabled\n",
                     s);
            return false;
        }
    }

    PrintOut(PRINT_LOG,
             "[print_guard] COM_Parse token cap 256 -> 255 (NUL at [255])\n");
    installed = true;
    return true;
}
