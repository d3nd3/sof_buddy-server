#include "stufftext_audit.h"

#include <ctype.h>
#include <string.h>

// Inferred literals from decompile lengths. Prefix lengths are exact
// (strncmp bounds); full texts with '\n' are inferred — verify rodata.
static const char kChanging[] = "changing\n";
static const char kReconnect[] = "reconnect\n";
static const char kPredicting[] = "predicting 1\n";
static const char kClRun[] = "cl_run 1\n";
static const char kAllowPrefix[] = "allow_download 1\n";  // 17 bytes
static const char kAllowSuffix[] =
    "allow_download_stringpackage 1\n"
    "allow_download_sounds 1\n";
static const char kSpectator[] = "spectator ";          // 10
static const char kTeam[] = "team_red_blue ";          // 14
static const char kClMaxfps[] = "cl_maxfps ";          // 10
static const char kWeaponselect[] = "weaponselect ";   // 13
static const char kPassword[] = "password ";           // 9
static const char kSpecPass[] = "spectator_password ";  // 19
static const char kSetSv[] = "set _sp_cl_sv_";          // 14
static const char kSetSvVer[] = "set _sp_cl_sv_version ";  // 22
static const char kCmd[] = "cmd ";                      // 4
static const char kPrecache[] = "precache ";            // 9
static const char kConfigstrings[] = "configstrings ";  // 14
static const char kGhoulstrings[] = "ghoulstrings ";    // 13
static const char kBaselines[] = "baselines ";          // 10
static const char kBegin[] = "begin ";                  // 6
static const char kCheck[] = ".check ";                 // 7

namespace {

inline unsigned char U(char c) { return static_cast<unsigned char>(c); }

bool LenStartsWith(const char* s, size_t len, const char* pre) {
    size_t n = strlen(pre);
    return len >= n && memcmp(s, pre, n) == 0;
}

bool LenEquals(const char* s, size_t len, const char* lit) {
    size_t n = strlen(lit);
    return len == n && memcmp(s, lit, n) == 0;
}

StuffAuditResult R(bool allow, const char* reason) {
    StuffAuditResult r;
    r.allow = allow;
    r.reason = reason;
    return r;
}

// LABEL_69 equivalent: no ';', exactly one trailing '\n'.
StuffAuditResult CheckTail69(const char* s, size_t len) {
    if (StuffAudit_ContainsByte(s, len, ';'))
        return R(false, "BLOCK_SEMICOLON");
    const char* nl = static_cast<const char*>(memchr(s, '\n', len));
    if (!nl)
        return R(false, "BLOCK_NO_LF");
    if (nl != s + len - 1)
        return R(false, "BLOCK_LF_NOT_SINGLE_TRAILING");
    return R(true, "ALLOW_TAIL_OK");
}

// set _sp_cl_sv_<1-2 digits><space> shape check.
bool MatchSetSvNumber(const char* s, size_t len) {
    if (!LenStartsWith(s, len, kSetSv))
        return false;
    size_t p = strlen(kSetSv);  // 14
    if (len <= p || !isdigit(U(s[p])))
        return false;
    ++p;
    if (p < len && s[p] == ' ')
        return true;
    if (p < len && isdigit(U(s[p])) && p + 1 < len && s[p + 1] == ' ')
        return true;
    return false;
}

bool CheckPrefixAllowlist(const char* v9, size_t /*v9len*/) {
    // v9 is NUL-free, newline-terminated in original; here length-aware
    // prefix match replicates !strncmp semantics.
    const char* prefs[] = {"cl_",  "fov", "ghl_", "gl_",  "r_",
                           "rate", "scr_", "vid_", "_sp_cl_info_",
                           "_sp_sc_info_version"};
    // Compare up to NUL or implicit end; caller guarantees v9 points at
    // cvarname mirror of length v8 followed by '\n'.
    for (unsigned i = 0; i < sizeof(prefs) / sizeof(prefs[0]); ++i) {
        size_t n = strlen(prefs[i]);
        if (strncmp(v9, prefs[i], n) == 0)
            return true;
    }
    return false;
}

// Shared .check core. If hardened=true: unsigned-char ctype, cvarname cap 32.
// Returns ALLOW only for fully valid .check; else BLOCK with reason.
// checkStart points just after "cmd .check " (i.e. a1+11), checkLen = remaining.
StuffAuditResult ParseCheck(const char* cur, size_t rem, bool hardened) {
    // Challenge: exactly 8 [A-Za-z0-9].
    size_t ci = 0;
    while (ci < rem && cur[ci] != ' ') {
        if (!isalnum(U(cur[ci])))
            return R(false, "BLOCK_CHECK_BAD_CHALLENGE_CHAR");
        ++ci;
    }
    if (ci >= rem || cur[ci] != ' ')
        return R(false, "BLOCK_CHECK_CHALLENGE_TERM");
    if (ci != 8)
        return R(false, "BLOCK_CHECK_CHALLENGE_LEN");
    size_t p = ci + 1;  // start of cvarname
    size_t vi = p;
    while (vi < rem && cur[vi] != ' ') {
        char c = cur[vi];
        if (!(isalnum(U(c)) || c == '_'))
            return R(false, "BLOCK_CHECK_VAR_CHAR");
        ++vi;
    }
    if (vi >= rem || cur[vi] != ' ')
        return R(false, "BLOCK_CHECK_VAR_TERM");
    size_t v8 = vi - p;
    if (v8 == 0)
        return R(false, "BLOCK_CHECK_VAR_EMPTY");
    if (hardened && v8 > 32)
        return R(false, "BLOCK_CHECK_VAR_TOOLONG");
    if (vi + 1 >= rem || cur[vi + 1] != '#')
        return R(false, "BLOCK_CHECK_HASH");
    const char* v9 = cur + vi + 2;
    size_t v9rem = rem - (vi + 2);
    if (v9rem < v8 + 1)
        return R(false, "BLOCK_CHECK_MIRROR_SHORT");
    if (memcmp(cur + p, v9, v8) != 0)
        return R(false, "BLOCK_CHECK_MIRROR_MISMATCH");
    if (v9[v8] != '\n')
        return R(false, "BLOCK_CHECK_MIRROR_LF");
    if (v8 + 1 != v9rem)
        return R(false, "BLOCK_CHECK_TRAILING");
    // Prefix allowlist on the mirrored name. Temporarily NUL-terminate
    // logically by copying prefix into small buf for strncmp.
    char name[64];
    size_t cn = v8 < sizeof(name) - 1 ? v8 : sizeof(name) - 1;
    memcpy(name, v9, cn);
    name[cn] = '\0';
    if (!CheckPrefixAllowlist(name, cn))
        return R(false, "BLOCK_CHECK_PREFIX");
    return R(true, "ALLOW_CHECK_OK");
}

}  // namespace

bool StuffAudit_ContainsByte(const char* s, size_t len, char b) {
    return len > 0 && memchr(s, b, len) != nullptr;
}

unsigned StuffAudit_CountQuotes(const char* s, size_t len) {
    unsigned n = 0;
    for (size_t i = 0; i < len; ++i)
        if (s[i] == '"')
            ++n;
    return n;
}

bool StuffAudit_HasSingleTrailingLF(const char* s, size_t len) {
    if (len == 0 || s[len - 1] != '\n')
        return false;
    return memchr(s, '\n', len - 1) == nullptr;
}

StuffAuditResult StuffAudit_V1(const char* s, size_t len, bool attackLoop) {
    if (attackLoop)
        return R(true, "ALLOW_ATTACKLOOP_BYPASS");
    if (len == 0)
        return R(true, "ALLOW_EMPTY");
    if (LenEquals(s, len, kChanging) || LenEquals(s, len, kReconnect))
        return R(true, "ALLOW_STATE_WORD");
    if (LenStartsWith(s, len, kSpectator) || LenStartsWith(s, len, kTeam))
        return CheckTail69(s, len);
    if (LenEquals(s, len, kPredicting))
        return R(true, "ALLOW_PREDICTING");
    if (!LenStartsWith(s, len, kCmd)) {
        if (LenStartsWith(s, len, kPrecache)) {
            // precache <anything> -> tail check only (point 3: too broad;
            // Q2 CL_Precache_f takes optional spawncount, resets download
            // state even mid-game — hence V4 state-gate).
            return CheckTail69(s, len);
        }
        if (LenStartsWith(s, len, kAllowPrefix)) {
            size_t pre = strlen(kAllowPrefix);
            if (len == pre)
                return R(true, "ALLOW_ALLOWDL_SHORT");
            size_t suf = strlen(kAllowSuffix);
            if (len == pre + suf && memcmp(s + pre, kAllowSuffix, suf) == 0)
                return R(true, "ALLOW_ALLOWDL_FULL");
            return R(false, "BLOCK_ALLOWDL_SUFFIX");
        }
        if (LenEquals(s, len, kClRun))
            return R(true, "ALLOW_CLRUN");
        if (LenStartsWith(s, len, kClMaxfps) || LenStartsWith(s, len, kWeaponselect) ||
            LenStartsWith(s, len, kPassword) || LenStartsWith(s, len, kSpecPass) ||
            MatchSetSvNumber(s, len) || LenStartsWith(s, len, kSetSvVer)) {
            return CheckTail69(s, len);
        }
        return R(false, "BLOCK_PREFIX");
    }
    // cmd branch
    const char* sub = s + strlen(kCmd);
    size_t sublen = len >= strlen(kCmd) ? len - strlen(kCmd) : 0;
    if (LenStartsWith(sub, sublen, kConfigstrings) || LenStartsWith(sub, sublen, kGhoulstrings) ||
        LenStartsWith(sub, sublen, kBaselines) || LenStartsWith(sub, sublen, kBegin)) {
        return CheckTail69(s, len);
    }
    if (LenStartsWith(sub, sublen, kCheck)) {
        const char* cur = sub + strlen(kCheck);
        size_t rem = sublen >= strlen(kCheck) ? sublen - strlen(kCheck) : 0;
        StuffAuditResult cr = ParseCheck(cur, rem, false);
        if (cr.allow) {
            // gl_driver spoof quirk preserved verbatim in verdict (rewrite
            // happens client-side in original; audit only reports it).
            return R(true, "ALLOW_CHECK_OK");
        }
        return cr;
    }
    return R(false, "BLOCK_CMD_UNKNOWN");
}

StuffAuditResult StuffAudit_V6_Canonical(const char* s, size_t len) {
    if (len == 0)
        return R(false, "BLOCK_EMPTY");
    if (len > 1024)
        return R(false, "BLOCK_TOOLONG");
    // Engine uses NUL-terminated strings throughout (MSG_ReadString stops at
    // first 0, Cbuf uses strlen): any NUL inside len truncates identically
    // for filter and engine, so no smuggling differential. Still BLOCK as
    // defense-in-depth; engine wrappers pass len=strlen(s) so this only
    // fires in host fuzzing with explicit lengths.
    if (len > 0 && memchr(s, '\0', len) != nullptr)
        return R(false, "BLOCK_INTERIOR_NUL");
    if (StuffAudit_ContainsByte(s, len, '\r'))
        return R(false, "BLOCK_CR");
    if (StuffAudit_ContainsByte(s, len, ';'))
        return R(false, "BLOCK_SEMICOLON");
    if (!StuffAudit_HasSingleTrailingLF(s, len)) {
        if (memchr(s, '\n', len) == nullptr)
            return R(false, "BLOCK_NO_LF");
        return R(false, "BLOCK_LF_NOT_SINGLE_TRAILING");
    }
    return R(true, "ALLOW_CANONICAL_OK");
}

static bool HasExpSmuggleChars(const char* s, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        char c = s[i];
        if (c == '$' || c == '"' || c == '\'' || c == '`' || c == '\\')
            return true;
    }
    return false;
}

static int CountTokensNoQuotes(const char* s, size_t len) {
    // s without trailing LF; tokens split on ' ', no quote handling
    // (quotes already rejected). Collapses runs of spaces.
    int argc = 0;
    size_t i = 0;
    while (i < len) {
        while (i < len && s[i] == ' ')
            ++i;
        if (i >= len)
            break;
        ++argc;
        while (i < len && s[i] != ' ')
            ++i;
    }
    return argc;
}

static bool ValueCharsetOk(const char* s, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        char c = s[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '.' || c == '-' || c == ' ';
        if (!ok)
            return false;
    }
    return true;
}

StuffAuditResult StuffAudit_V2_Strict(const char* s, size_t len, bool attackLoop) {
    StuffAuditResult base = StuffAudit_V1(s, len, attackLoop);
    if (!base.allow)
        return base;  // already blocked; strict adds no new allows
    // Q2-corrected: $ expansion happens AFTER Cbuf split (cmd.c
    // Cmd_TokenizeString), so expanded ';'/'\n' cannot chain new Cbuf
    // commands — only alter argv values/truncate/comment/quote-merge.
    // Still reject metachars as defense-in-depth for value smuggling.
    // Charset gate runs on the LF-stripped body (trailing '\n' is framing).
    size_t body = (len > 0 && s[len - 1] == '\n') ? len - 1 : len;
    // Multi-line allow_download blob is byte-exact by V1; exempt it from the
    // single-line charset gate (its interior LFs are the framing, verified).
    if (LenStartsWith(s, len, kAllowPrefix))
        return R(true, "ALLOW_V2_ALLOWDL_OK");
    if (HasExpSmuggleChars(s, body))
        return R(false, "BLOCK_V2_EXPANSION_CHARS");
    if (!ValueCharsetOk(s, body))
        return R(false, "BLOCK_V2_CHARSET");
    int argc = CountTokensNoQuotes(s, body);
    if (LenEquals(s, len, kChanging) || LenEquals(s, len, kReconnect) || LenEquals(s, len, kPredicting) ||
        LenEquals(s, len, kClRun)) {
        return R(true, "ALLOW_V2_EXACT_OK");
    }
    if (LenStartsWith(s, len, kCmd) || LenStartsWith(s, len, kPrecache)) {
        if (argc < 2 || argc > 3)
            return R(false, "BLOCK_V2_ARGC");
        return R(true, "ALLOW_V2_CMD_OK");
    }
    // Single-value and set commands: 2 tokens (cmd value) or 3 (set k v).
    if (argc < 2 || argc > 3)
        return R(false, "BLOCK_V2_ARGC");
    return R(true, "ALLOW_V2_STRICT_OK");
}

StuffAuditResult StuffAudit_V3_QuoteCarry(const char* s, size_t len, bool attackLoop, bool* openQuote) {
    // Q2-corrected: stock Cbuf_Execute resets quotes=0 per line (cmd.c:208)
    // and MacroExpand discards unmatched-quote lines (cmd.c:602-606), so
    // there is NO cross-packet quote state in the engine. This harness is
    // STRICTER-than-stock on purpose: odd quotes BLOCK here (engine would
    // also discard), and an incoming open flag BLOCKs to surface split-
    // payload attempts in logs. Do not read CARRY_IN as engine behavior.
    bool incoming = openQuote && *openQuote;
    if (incoming)
        return R(false, "BLOCK_QUOTE_CARRY_IN");
    unsigned q = StuffAudit_CountQuotes(s, len);
    if (q % 2 == 1) {
        if (openQuote)
            *openQuote = true;
        return R(false, "BLOCK_QUOTE_LEAVES_OPEN");
    }
    return StuffAudit_V1(s, len, attackLoop);
}

StuffAuditResult StuffAudit_V4_StateGated(const char* s, size_t len, bool attackLoop, bool inPrecache) {
    // attackLoop bypass removed: validate regardless, report it.
    if (attackLoop) {
        StuffAuditResult inner = StuffAudit_V4_StateGated(s, len, false, inPrecache);
        if (inner.allow)
            return R(true, "ALLOW_V4_WOULD_ALLOW_DESPITE_ATTACKLOOP");
        return R(false, "BLOCK_V4_ATTACKLOOP_NOBYPASS");
    }
    bool isPrecache = LenStartsWith(s, len, kPrecache);
    bool isStateCmd = false;
    if (LenStartsWith(s, len, kCmd)) {
        const char* sub = s + strlen(kCmd);
        size_t sublen = len >= strlen(kCmd) ? len - strlen(kCmd) : 0;
        isStateCmd = LenStartsWith(sub, sublen, kConfigstrings) || LenStartsWith(sub, sublen, kGhoulstrings) ||
                     LenStartsWith(sub, sublen, kBaselines) || LenStartsWith(sub, sublen, kBegin);
    }
    if ((isPrecache || isStateCmd) && !inPrecache)
        return R(false, "BLOCK_V4_WRONG_STATE");
    return StuffAudit_V1(s, len, false);
}

StuffAuditResult StuffAudit_V5_Check(const char* s, size_t len) {
    if (!LenStartsWith(s, len, kCmd))
        return R(false, "BLOCK_V5_NOT_CMD");
    const char* sub = s + strlen(kCmd);
    size_t sublen = len >= strlen(kCmd) ? len - strlen(kCmd) : 0;
    if (!LenStartsWith(sub, sublen, kCheck))
        return R(false, "BLOCK_V5_NOT_CHECK");
    const char* cur = sub + strlen(kCheck);
    size_t rem = sublen >= strlen(kCheck) ? sublen - strlen(kCheck) : 0;
    return ParseCheck(cur, rem, true);
}

size_t StuffAudit_Unescape(char* dst, size_t dstCap, const char* src, size_t srcLen) {
    size_t o = 0;
    for (size_t i = 0; i < srcLen && o + 1 < dstCap;) {
        if (src[i] == '\\' && i + 1 < srcLen) {
            char e = src[i + 1];
            if (e == 'n') {
                dst[o++] = '\n';
                i += 2;
                continue;
            }
            if (e == 'r') {
                dst[o++] = '\r';
                i += 2;
                continue;
            }
            if (e == '\\') {
                dst[o++] = '\\';
                i += 2;
                continue;
            }
            if ((e == 'x' || e == 'X') && i + 3 < srcLen) {
                auto hex = [](char c) -> int {
                    if (c >= '0' && c <= '9')
                        return c - '0';
                    if (c >= 'a' && c <= 'f')
                        return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F')
                        return c - 'A' + 10;
                    return -1;
                };
                int hi = hex(src[i + 2]);
                int lo = hex(src[i + 3]);
                if (hi >= 0 && lo >= 0) {
                    dst[o++] = static_cast<char>((hi << 4) | lo);
                    i += 4;
                    continue;
                }
            }
        }
        dst[o++] = src[i++];
    }
    if (dstCap > 0)
        dst[o < dstCap ? o : dstCap - 1] = '\0';
    return o;
}
