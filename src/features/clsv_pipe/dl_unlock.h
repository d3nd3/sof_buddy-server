#pragma once

// dl_unlock: pure, host-testable allowlist matcher for the download unlock.
//
// spsv.dll's my_download blocks every server download whose path starts
// with "sofplus/" (strnicmp, 8). The unlock chains the engine `download`
// command slot and bypasses spsv for exactly ONE path: the pipe.func
// addon the client needs to bootstrap the clsv_pipe listener.
//
// The match is exact (case-insensitive, `/` and `\` both accepted as
// separators) so no traversal or prefix trick can widen it: anything else
// falls through to spsv's filter untouched.

namespace dlunlock {

constexpr char kPipePath[] = "sofplus/addons/pipe.func";
// Must stay a short single relative path: engine configstrings are 64 B,
// and the matcher's exactness is the whole security argument.
static_assert(sizeof(kPipePath) <= 64, "allowlisted download path must fit MAX_QPATH");
static_assert(sizeof(kPipePath) > 8, "allowlisted path must extend past the sofplus/ block");

inline char FoldSep(char c) {
    if (c >= 'A' && c <= 'Z')
        return static_cast<char>(c + ('a' - 'A'));
    if (c == '\\')
        return '/';
    return c;
}

// True only for exactly kPipePath (case/separator-insensitive). NUL,
// empty, prefix-only, overlong and `..` inputs all fail: the comparison
// walks both strings to their terminators.
inline bool AllowPath(const char* req) {
    if (!req)
        return false;
    const char* want = kPipePath;
    while (*want) {
        if (FoldSep(*req) != static_cast<char>(*want))
            return false;
        ++req;
        ++want;
    }
    return *req == '\0';
}

}  // namespace dlunlock

// Engine glue (dl_unlock.cpp). Tick is cheap (one slot-pointer read when
// healthy) and self-heals if anything re-patches under us.
void DlUnlock_Tick();
void DlUnlock_Shutdown();
void DlUnlock_Status();
