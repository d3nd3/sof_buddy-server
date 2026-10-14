#pragma once

// profiles_logic: pure, host-testable identity core for the profiles feature.
//
// Ports git-projects/sof-profiles (profiles.func + userinfo_rcon.py) to C++
// without the SoFplus script VM, the ext_trigger/file-watch round-trip, or
// the rcon dumpuser snapshot pipeline:
//
//   .func before: userinfo change -> ext_trigger file -> export-fire ->
//     userinfo_rcon.py -> rcon dumpuser -> snapshot_<slot>.cfg -> 200ms
//     timer -> exec -> parse -> maybe stufftext restore
//   native after: ClientUserinfoChanged(userinfo*) -> parse -> restore
//     via Buddy_StuffText in the same call. No files, no timers, no Python.
//
// This header owns everything that does NOT need the engine:
//   - guid validation (24-digit canonical; legacy 62-digit accepted on read
//     but deprecated — see GuidDeprecated) + minting (always 24-digit)
//   - team_red_blue parse/build (canonical <guid><bit>-blue|-red: the bit
//     terminates the leading digit run so AssignTeam's atoi() reads it;
//     plain all-digit for legacy 62-digit guids; bare 0/1 = collapsed)
//   - nickname -> nickname_clean (colors stripped, lowercased, a-z0-9)
//   - player registry (guid <-> nickname + privilege) with save/load incl. legacy
//     profiles.func registry.cfg format
//   - per-slot state: one guid, cleared on disconnect
//
// Engine binding (edict scan, ge Client* hooks, Buddy_StuffText sends,
// console commands, file persistence paths) lives in profiles.cpp and only
// feeds this machine config + observations.

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace profiles {

constexpr int kIdentityDigits = 24;
constexpr int kLegacyIdentityDigits = 62;
constexpr int kMaxSlots = 64;
constexpr int kMaxNicknameLen = 64;
constexpr int kMaxRegistryEntries = 4096;
constexpr int kServerAdmin = 0;
constexpr int kMapAdmin = 1;
constexpr int kNormalUser = 2;
constexpr int kUnspecifiedPrivilege = -1;

inline bool ValidPrivilege(int privilege) {
    return privilege >= kServerAdmin && privilege <= kNormalUser;
}

inline bool IsDigitsOnly(const std::string& s) {
    if (s.empty())
        return false;
    for (char c : s) {
        if (c < '0' || c > '9')
            return false;
    }
    return true;
}

inline bool IsSignedDecimal(const std::string& s) {
    if (s.empty())
        return false;
    std::size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    if (i == s.size())
        return false;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9')
            return false;
    }
    return true;
}

// guid must be all digits, 24 chars (current) — legacy 62 accepted on read
// and in the registry so old entries keep working.
inline bool GuidValid(const std::string& guid) {
    if (guid.size() != static_cast<std::size_t>(kIdentityDigits) &&
        guid.size() != static_cast<std::size_t>(kLegacyIdentityDigits))
        return false;
    return IsDigitsOnly(guid);
}

// Deprecated identity length. 62-digit guids still validate, parse, push,
// and load (internal compat), but nothing mints or should carry them: the
// team_red_blue interaction is canonical 24-digit, which is what
// profile_register produces and what sign-in output shows.
inline bool GuidDeprecated(const std::string& guid) {
    return guid.size() == static_cast<std::size_t>(kLegacyIdentityDigits) &&
           IsDigitsOnly(guid);
}

// nickname -> nickname_clean (registry reverse-lookup key).
// .func: sp_sc_cvar_no_color then sp_sc_cvar_replace "%00-%2f:" "%3a-%60:"
// "%7b-%ff:" — i.e. keep 0-9 + a-z, drop everything else. Uppercase falls
// in the dropped range there, so README documents the key as a-z0-9.
// Native: strip SoF color escapes (%NN and ^X), lowercase, keep a-z0-9.
inline std::string NicknameClean(const std::string& nickname) {
    std::string noColor;
    noColor.reserve(nickname.size());
    for (std::size_t i = 0; i < nickname.size(); ++i) {
        char c = nickname[i];
        // SoF color escape: '%' + two chars (e.g. %02, %04 in .func msgs).
        if (c == '%' && i + 2 < nickname.size()) {
            // Only skip when two chars follow; single trailing '%' is dropped
            // as non a-z0-9 below anyway.
            i += 2;
            continue;
        }
        if (c == '%' && i + 1 < nickname.size()) {
            // Lone "%X" at end of string: drop both, same as above.
            ++i;
            continue;
        }
        // Quake-style color escape: '^' + one char.
        if (c == '^' && i + 1 < nickname.size()) {
            ++i;
            continue;
        }
        noColor.push_back(c);
    }
    std::string out;
    out.reserve(noColor.size());
    for (char c : noColor) {
        unsigned char u = static_cast<unsigned char>(c);
        char l = static_cast<char>(std::tolower(u));
        if ((l >= 'a' && l <= 'z') || (l >= '0' && l <= '9'))
            out.push_back(l);
    }
    return out;
}

inline std::string DisplayNickname(const std::string& nickname) {
    std::string out;
    out.reserve(nickname.size());
    for (std::size_t i = 0; i < nickname.size(); ++i) {
        char c = nickname[i];
        if (c == '%' && i + 2 < nickname.size()) {
            i += 2;
            continue;
        }
        if (c == '^' && i + 1 < nickname.size()) {
            ++i;
            continue;
        }
        if (static_cast<unsigned char>(c) < 32)
            continue;
        out.push_back(c == '"' || c == '\\' ? ' ' : c);
    }
    return out;
}

struct TrbParsed {
    std::string raw;       // trimmed userinfo value
    std::string identity;  // digits only, no team bit
    int team = -1;         // 0 = blue, 1 = red, -1 = unknown
    bool valid = false;    // true when identity is a known-length guid
};

inline std::string ToLowerStr(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Split team_red_blue into (identity, team, valid).
// Canonical: <guid><bit>-blue|-red (the bit terminates the leading digit
// run, so AssignTeam's atoi() reads it). Plain all-digit <guid><bit> for
// legacy 62-digit guids. Collapsed bare 0/1 (team menu, swap) carry no
// identity: valid=false.
inline TrbParsed ParseTeamRedBlue(const std::string& value) {
    TrbParsed out;
    if (value.empty())
        return out;
    // Trim surrounding whitespace (userinfo values never carry it, but
    // snapshot files and console args might).
    std::size_t b = 0;
    while (b < value.size() && std::isspace(static_cast<unsigned char>(value[b])))
        ++b;
    std::size_t e = value.size();
    while (e > b && std::isspace(static_cast<unsigned char>(value[e - 1])))
        --e;
    std::string v = value.substr(b, e - b);
    out.raw = v;
    if (v.empty())
        return out;

    // Suffixed <digits><bit>-blue|-red (canonical emit): the bit terminates
    // the leading digit run so AssignTeam's atoi() reads it; the color word
    // is cross-checked. Team mirrors what the game sees for ANY digit run
    // (even short ones); identity is only valid at guid length with a
    // matching color (mismatch keeps the team but drops the identity).
    {
        std::size_t dash = v.rfind('-');
        if (dash != std::string::npos && dash > 0 && dash + 1 < v.size()) {
            std::string head = v.substr(0, dash);
            std::string color = ToLowerStr(v.substr(dash + 1));
            if ((color == "blue" || color == "red") && IsDigitsOnly(head) &&
                (head.back() == '0' || head.back() == '1')) {
                int team = head.back() - '0';
                out.team = team;
                bool longEnough =
                    (head.size() == static_cast<std::size_t>(kIdentityDigits) + 1 ||
                     head.size() == static_cast<std::size_t>(kLegacyIdentityDigits) + 1);
                std::string ident = head.substr(0, head.size() - 1);
                bool colorOk =
                    (color == "blue" && team == 0) || (color == "red" && team == 1);
                if (longEnough && colorOk && GuidValid(ident)) {
                    out.identity = ident;
                    out.valid = true;
                }
                return out;
            }
        }
    }
    // Legacy / collapsed all-digit forms.
    bool allDigits = IsDigitsOnly(v);
    if (allDigits) {
        int team = (v.back() - '0') & 1;
        out.team = team;
        if (v.size() >= 2 && (v.back() == '0' || v.back() == '1')) {
            std::string ident = v.substr(0, v.size() - 1);
            if (GuidValid(ident)) {
                out.identity = ident;
                out.valid = true;
            }
        }
        return out;
    }
    return out;
}

// Shape dmctf_c::AssignTeam may act on: suffixed or plain all-digit guid forms.
// Empty/missing key is allowed (first connect). Collapsed bare 0/1 is not.
inline bool TeamRedBlueAllowsAssignTeam(const std::string& value) {
    if (value.empty())
        return true;
    return ParseTeamRedBlue(value).valid;
}

// Team-menu collapse only. AssignTeam must still run for every other value,
// including a 32-bit wrap of <guid><bit> (SoF atol has no overflow check).
// That call sets resp.team from the low bit and edict+0x380. skinteam_quiet
// leaves +0x380 for ClientThink; skipping it leaves the player on no team.
inline bool TeamRedBlueIsBareBit(const std::string& value) {
    return value == "0" || value == "1";
}

// SoF _atol: ebx = ebx*10 + digit, 32-bit wrap, no saturation.
inline std::string AtolWrapDecimal(const std::string& digits) {
    std::uint32_t n = 0;
    for (char c : digits) {
        if (c < '0' || c > '9')
            break;
        n = n * 10u + static_cast<std::uint32_t>(c - '0');
    }
    return std::to_string(static_cast<std::int32_t>(n));
}

// value is the decimal SoF stored for <guid><bit>. teamOut is that bit.
inline bool WrapMatchesGuid(const std::string& value, const std::string& guid, int* teamOut) {
    if (!GuidValid(guid) || !IsSignedDecimal(value))
        return false;
    for (int bit = 0; bit <= 1; ++bit) {
        if (value == AtolWrapDecimal(guid + static_cast<char>('0' + bit))) {
            if (teamOut)
                *teamOut = bit;
            return true;
        }
    }
    return false;
}

// What a CVAR_INT cvar actually stores: atol (stops at the first non-digit,
// so -blue/-red and any other character are dropped), then float32
// (nearest-even), then trunc toward zero, then "%d". Above 2^24 the ulp
// can collapse both team bits onto one decimal — 303041696 is norway,
// 1676235264 is dende. Done in integers: an i686 x87 float cast stays
// 80-bit and returns the pre-float atol (1676235220) instead.
inline std::int32_t CvarIntStored(std::int32_t wrapped) {
    std::uint32_t mag = static_cast<std::uint32_t>(wrapped);
    if (wrapped < 0)
        mag = ~mag + 1u;
    if (mag >= (1u << 24)) {
        unsigned exp = 0;
        for (std::uint32_t t = mag; t > 1u; t >>= 1)
            ++exp;
        unsigned shift = exp - 23u;
        std::uint32_t ulp = 1u << shift;
        std::uint32_t rem = mag & (ulp - 1u);
        mag &= ~(ulp - 1u);
        if (rem > (ulp >> 1) || (rem == (ulp >> 1) && (mag & ulp) != 0u))
            mag += ulp;
    }
    if (wrapped >= 0)
        return static_cast<std::int32_t>(mag);
    return mag == 0x80000000u ? static_cast<std::int32_t>(0x80000000u)
                              : -static_cast<std::int32_t>(mag);
}

inline std::uint32_t AtolWrapU32(const std::string& digits) {
    std::uint32_t n = 0;
    for (char c : digits) {
        if (c < '0' || c > '9')
            break;
        n = n * 10u + static_cast<std::uint32_t>(c - '0');
    }
    return n;
}

inline std::string CvarIntDecimal(const std::string& digits) {
    return std::to_string(CvarIntStored(static_cast<std::int32_t>(AtolWrapU32(digits))));
}

// Accidental truncation of a full <guid><bit> push. teamOut is -1 when both
// bits store the same decimal.
inline bool IntCvarTruncation(const std::string& value, const std::string& guid, int* teamOut) {
    if (!GuidValid(guid) || !IsSignedDecimal(value))
        return false;
    std::string s0 = CvarIntDecimal(guid + '0');
    std::string s1 = CvarIntDecimal(guid + '1');
    bool m0 = value == s0;
    bool m1 = value == s1;
    if (!m0 && !m1)
        return false;
    if (teamOut)
        *teamOut = (m0 && m1) ? -1 : (m0 ? 0 : 1);
    return true;
}

// Slot guid is empty (fresh connect). value is the decimal a CVAR_INT client
// stored for some <guid><bit>. One roster guid stores it → that guid.
// teamOut is -1 when both bits store the same decimal. Two matches → false.
inline bool MatchRosterTruncation(const std::string& value,
                                  const std::map<std::string, std::string>& roster,
                                  std::string* guidOut, int* teamOut) {
    if (!IsSignedDecimal(value))
        return false;
    std::string found;
    int bit = -1;
    for (const auto& kv : roster) {
        int b = -1;
        if (!IntCvarTruncation(value, kv.first, &b))
            continue;
        if (!found.empty())
            return false;
        found = kv.first;
        bit = b;
    }
    if (found.empty())
        return false;
    if (guidOut)
        *guidOut = found;
    if (teamOut)
        *teamOut = bit;
    return true;
}

// Float32 keeps every integer below 2^24. That is 24 bits: 23 for the guid,
// and the low bit is the team (AssignTeam reads atoi & 1).
inline std::uint32_t GuidFold23(const std::string& guid) {
    std::uint32_t n = AtolWrapU32(guid);
    n ^= n >> 16;
    return n & 0x7FFFFFu;
}

inline std::uint32_t IntCvarPack(const std::string& guid, int team) {
    return (GuidFold23(guid) << 1) | static_cast<std::uint32_t>(team & 1);
}

inline std::string IntCvarPackDecimal(const std::string& guid, int team) {
    return std::to_string(IntCvarPack(guid, team));
}

// Short login number: decimal 23-bit fold of the guid (no team bit). Fewer
// characters to store/type than the 24-digit guid; accepted by .login.
inline std::string GuidFoldDecimal(const std::string& guid) {
    if (!GuidValid(guid))
        return "";
    return std::to_string(GuidFold23(guid));
}

// Well-formed login number: bare decimal below 2^23. "0"/"1" are excluded:
// they read as team bits, so a fold-0/1 guid logs in with its full guid.
inline bool IsLoginNumber(const std::string& s) {
    if (s.empty() || s.size() > 7 || s == "0" || s == "1")
        return false;
    std::uint32_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9')
            return false;
        v = v * 10u + static_cast<std::uint32_t>(c - '0');
    }
    return v < (1u << 23);
}

// Resolve a login number to the unique roster guid with that fold. False
// for malformed input, and when zero or several roster entries share the
// fold (then the full guid is required).
inline bool LookupGuidByFold(const std::map<std::string, std::string>& roster,
                             const std::string& foldDec, std::string* guidOut) {
    if (!IsLoginNumber(foldDec))
        return false;
    std::uint32_t want = 0;
    for (char c : foldDec)
        want = want * 10u + static_cast<std::uint32_t>(c - '0');
    std::string found;
    int count = 0;
    for (const auto& kv : roster) {
        if (!GuidValid(kv.first))
            continue;
        if (GuidFold23(kv.first) == want) {
            found = kv.first;
            if (++count > 1)
                return false;
        }
    }
    if (count != 1 || !guidOut)
        return false;
    *guidOut = found;
    return true;
}

// Split a packed CVAR_INT value into the 23-bit guid fold and the team bit.
inline bool IntCvarSplit(const std::string& value, std::uint32_t* foldOut, int* teamOut) {
    if (!IsDigitsOnly(value))
        return false;
    std::uint32_t n = AtolWrapU32(value);
    if (n >= (1u << 24))
        return false;
    if (foldOut)
        *foldOut = n >> 1;
    if (teamOut)
        *teamOut = static_cast<int>(n & 1u);
    return true;
}

// value is the packed decimal for this guid. teamOut is bit 0.
inline bool IntCvarMatchesGuid(const std::string& value, const std::string& guid, int* teamOut) {
    std::uint32_t fold = 0;
    int team = -1;
    if (!GuidValid(guid) || !IntCvarSplit(value, &fold, &team))
        return false;
    if (fold != GuidFold23(guid))
        return false;
    if (teamOut)
        *teamOut = team;
    return true;
}

// True when the carrier names a different profile than the slot is bound
// to: a valid text guid that is not the slot guid, or a pack whose fold is
// not the slot guid's fold. Stock AssignTeam must not switch teams on such
// a carrier (a menu re-sending a remembered value, a pasted carrier); the
// restore path re-pushes the slot's own pack instead, so the carrier
// converges without a flip-flop. Bare bits and garbage pass through to the
// existing handling, as do unbound slots (guests need the stock assign).
inline bool ForeignCarrier(const std::string& trb, const std::string& slotGuid, bool registered) {
    if (!registered || !GuidValid(slotGuid) || trb.size() < 2)
        return false;
    TrbParsed parsed = ParseTeamRedBlue(trb);
    if (parsed.valid)
        return parsed.identity != slotGuid;
    std::uint32_t fold = 0;
    int team = -1;
    if (IntCvarSplit(trb, &fold, &team))
        return fold != GuidFold23(slotGuid);
    return false;
}

// Team bit to embed when re-pushing after collapse. Prefers the userinfo bit
// (client's team pick, including collapsed 0/1) over the live server team.
inline int ResolveRestoreTeam(int liveTeam, int parsedTeam, int slotTeam) {
    if (parsedTeam == 0 || parsedTeam == 1)
        return parsedTeam;
    if (liveTeam == 0 || liveTeam == 1)
        return liveTeam;
    if (slotTeam == 0 || slotTeam == 1)
        return slotTeam;
    return -1;
}

// team_red_blue value the client carries in its userinfo:
// <guid><bit>-blue|-red. The team bit MUST terminate the leading digit
// run: dmctf_c::AssignTeam (gamex86) reads the key with atoi() then
// (v & 1) + 1, and atoi's parity is set by the last digit of the leading
// run — so the prefix text blue-<guid>-0 form atoi()s to 0 (blue for every
// carrier, red players force-teamed blue), while a bit-terminated run
// reads correctly (overflow wraps but the low bit stays the trailing
// bit). The color word after the dash is human-readable and
// cross-checked on parse (mismatch = invalid). The canonical carrier is
// the short 24-digit form; legacy 62-digit guids (deprecated, see
// GuidDeprecated) stay plain all-digit for compat: with suffix they'd
// exceed the engine's 64-char userinfo value limit (62+1+1+4=68), and the
// identity itself must round-trip unchanged.
inline std::string BuildTeamRedBlue(const std::string& guid, int team) {
    int bit = (team & 1) != 0 ? 1 : 0;
    const char* color = bit == 0 ? "blue" : "red";
    if (guid.size() != static_cast<std::size_t>(kIdentityDigits)) {
        std::string out;
        out.reserve(guid.size() + 1);
        out += guid;
        out += static_cast<char>('0' + bit);
        return out;
    }
    std::string out;
    out.reserve(guid.size() + 6);
    out += guid;
    out += static_cast<char>('0' + bit);
    out += '-';
    out += color;
    return out;
}

// Carrier the server pushes: always the packed decimal (23-bit guid fold
// with the team in the low bit). It is short, survives every client cvar
// type exactly (below 2^24, so float-stored clients keep it bit-exact),
// and AssignTeam reads the team from its low bit. The long
// <guid><bit>-blue|-red text form is legacy on the wire: still accepted on
// read (parse/wrap/truncation paths), never pushed.
inline std::string TeamRedBlueForClient(const std::string& guid, int team) {
    return IntCvarPackDecimal(guid, team);
}

// True when a packed carrier disagreeing with the live team must be
// re-synced to live (stale pack, or a ulp-collapsed decimal whose low bit is
// meaningless). A bare 0/1 pick is exempt by construction (it never matches
// a guid pack): stock AssignTeam is skipped for bare bits, so live is still
// the pre-switch team there and must lose to the pick.
inline bool PackNeedsLiveResync(const std::string& value, const std::string& guid, int liveTeam) {
    if (liveTeam != 0 && liveTeam != 1)
        return false;
    int packBit = -1;
    if (!IntCvarMatchesGuid(value, guid, &packBit))
        return false;
    return packBit != liveTeam;
}

// Team to stuff for an integer client. The packed decimal's low bit is the
// team. A truncated full-guid push is not: both bits can land on one value,
// so keep the slot team.
inline int IntCvarTeam(const std::string& value, const std::string& guid, int slotTeam, int liveTeam) {
    int bit = -1;
    if (IntCvarMatchesGuid(value, guid, &bit))
        return bit;
    if (TeamRedBlueIsBareBit(value))
        return value[0] - '0';
    if (slotTeam == 0 || slotTeam == 1)
        return slotTeam;
    if (liveTeam == 0 || liveTeam == 1)
        return liveTeam;
    return -1;
}

// A successful stufftext write is not proof that stock userinfo handling kept
// the carrier; AssignTeam can rewrite it to bare 0/1.
inline bool RestoreTargetPresent(const std::string& current, const std::string& target) {
    return !target.empty() && current == target;
}

inline bool SuppressCarrierAssign(bool carrier, int liveTeam, int desiredTeam) {
    return carrier && liveTeam >= 0 && liveTeam == desiredTeam;
}

// Mint a fresh identity: kIdentityDigits decimal digits. `nextDigit` is any
// functor returning an unsigned (mod 10 is applied, so raw random bytes are
// fine). Pure/host-testable — the engine side supplies CSPRNG bytes
// (RtlGenRandom) and retries on registry collision.
template <typename NextDigit>
inline std::string MintIdentity(NextDigit nextDigit) {
    std::string out;
    out.reserve(static_cast<std::size_t>(kIdentityDigits));
    for (int i = 0; i < kIdentityDigits; ++i)
        out.push_back(static_cast<char>('0' + (nextDigit() % 10u)));
    return out;
}

// Profile registry: guid -> nickname + privilege, plus clean-nickname ->
// guid reverse map (~reg_<guid> / ~guid_by_<clean>).
class Registry {
public:
    bool Add(const std::string& guid, const std::string& nickname, std::string* err = nullptr,
             int privilege = kUnspecifiedPrivilege, bool hideTips = false,
             bool swapScore = false) {
        if (!GuidValid(guid)) {
            if (err)
                *err = "guid must be 24 digits (legacy 62 ok)";
            return false;
        }
        if (privilege != kUnspecifiedPrivilege && !ValidPrivilege(privilege)) {
            if (err)
                *err = "privilege must be 0, 1, or 2";
            return false;
        }
        if (nickname.empty()) {
            if (err)
                *err = "nickname is required";
            return false;
        }
        std::string clean = NicknameClean(nickname);
        if (clean.empty()) {
            if (err)
                *err = "nickname must contain letters or digits";
            return false;
        }
        if (guidToNick_.size() >= static_cast<std::size_t>(kMaxRegistryEntries) &&
            guidToNick_.find(guid) == guidToNick_.end()) {
            if (err)
                *err = "registry full";
            return false;
        }
        const bool existed = guidToNick_.find(guid) != guidToNick_.end();
        // One clean key maps to one guid: replacing a nickname that collides
        // with another entry's clean key steals the key (same as .func
        // ~guid_by_ overwrite). The old guid entry keeps its own ~reg_ slot
        // until explicitly deleted.
        guidToNick_[guid] = nickname;
        cleanToGuid_[clean] = guid;
        if (ValidPrivilege(privilege))
            guidToPrivilege_[guid] = privilege;
        else if (!existed)
            guidToPrivilege_[guid] = kNormalUser;
        if (!existed) {
            guidToHideTips_[guid] = hideTips;
            guidToSwapScore_[guid] = swapScore;
        }
        return true;
    }

    bool LookupPrivilege(const std::string& guid, int* privilegeOut) const {
        if (!privilegeOut)
            return false;
        auto it = guidToNick_.find(guid);
        if (it == guidToNick_.end())
            return false;
        auto privilege = guidToPrivilege_.find(guid);
        *privilegeOut = privilege == guidToPrivilege_.end()
            ? kNormalUser : privilege->second;
        return true;
    }

    bool LookupHideTips(const std::string& guid, bool* hideOut) const {
        if (!hideOut || guidToNick_.find(guid) == guidToNick_.end())
            return false;
        auto it = guidToHideTips_.find(guid);
        *hideOut = it != guidToHideTips_.end() && it->second;
        return true;
    }

    bool SetHideTips(const std::string& guid, bool hide, std::string* err = nullptr) {
        if (guidToNick_.find(guid) == guidToNick_.end()) {
            if (err)
                *err = "guid not in roster";
            return false;
        }
        guidToHideTips_[guid] = hide;
        return true;
    }

    bool LookupSwapScore(const std::string& guid, bool* swapOut) const {
        if (!swapOut || guidToNick_.find(guid) == guidToNick_.end())
            return false;
        auto it = guidToSwapScore_.find(guid);
        *swapOut = it != guidToSwapScore_.end() && it->second;
        return true;
    }

    bool SetSwapScore(const std::string& guid, bool swap, std::string* err = nullptr) {
        if (guidToNick_.find(guid) == guidToNick_.end()) {
            if (err)
                *err = "guid not in roster";
            return false;
        }
        guidToSwapScore_[guid] = swap;
        return true;
    }

    bool SetPrivilege(const std::string& guid, int privilege, std::string* err = nullptr) {
        if (!ValidPrivilege(privilege)) {
            if (err)
                *err = "privilege must be 0, 1, or 2";
            return false;
        }
        if (guidToNick_.find(guid) == guidToNick_.end()) {
            if (err)
                *err = "guid not in roster";
            return false;
        }
        guidToPrivilege_[guid] = privilege;
        return true;
    }

    // Delete by guid OR nickname (other arg empty), mirroring
    // profile_del <guid> <nickname>.
    bool Remove(const std::string& guid, const std::string& nickname, std::string* err = nullptr) {
        if (!guid.empty()) {
            auto it = guidToNick_.find(guid);
            if (it == guidToNick_.end()) {
                // .func clears the slot unconditionally and saves; report ok
                // with nothing removed.
                return true;
            }
            std::string clean = NicknameClean(it->second);
            guidToNick_.erase(it);
            guidToPrivilege_.erase(guid);
            guidToHideTips_.erase(guid);
            guidToSwapScore_.erase(guid);
            auto jt = cleanToGuid_.find(clean);
            if (jt != cleanToGuid_.end() && jt->second == guid)
                cleanToGuid_.erase(jt);
            return true;
        }
        if (!nickname.empty()) {
            std::string clean = NicknameClean(nickname);
            auto jt = cleanToGuid_.find(clean);
            if (jt == cleanToGuid_.end()) {
                if (err)
                    *err = "label not in registry";
                return false;
            }
            std::string g = jt->second;
            cleanToGuid_.erase(jt);
            guidToNick_.erase(g);
            guidToPrivilege_.erase(g);
            guidToHideTips_.erase(g);
            guidToSwapScore_.erase(g);
            return true;
        }
        if (err)
            *err = "usage: profile_del <guid24> <nickname>";
        return false;
    }

    bool Lookup(const std::string& guid, std::string* nicknameOut) const {
        auto it = guidToNick_.find(guid);
        if (it == guidToNick_.end())
            return false;
        if (nicknameOut)
            *nicknameOut = it->second;
        return true;
    }

    bool FindByNickname(const std::string& nickname, std::string* guidOut) const {
        std::string clean = NicknameClean(nickname);
        auto it = cleanToGuid_.find(clean);
        if (it == cleanToGuid_.end())
            return false;
        if (guidOut)
            *guidOut = it->second;
        return true;
    }

    bool FindByClean(const std::string& clean, std::string* guidOut) const {
        auto it = cleanToGuid_.find(clean);
        if (it == cleanToGuid_.end())
            return false;
        if (guidOut)
            *guidOut = it->second;
        return true;
    }

    std::size_t Size() const { return guidToNick_.size(); }
    void Clear() {
        guidToNick_.clear();
        cleanToGuid_.clear();
        guidToPrivilege_.clear();
        guidToHideTips_.clear();
        guidToSwapScore_.clear();
    }

    const std::map<std::string, std::string>& GuidToNick() const { return guidToNick_; }

    // Serialize to the native file format:
    // "<guid> <privilege> <nickname> hide_tips=<0|1> swap_score=<0|1>" per line.
    std::string Save() const {
        std::string out = "# sof_buddy profiles registry (guid privilege nickname)\n";
        for (const auto& kv : guidToNick_) {
            out += kv.first;
            out += ' ';
            auto privilege = guidToPrivilege_.find(kv.first);
            out += privilege == guidToPrivilege_.end()
                ? std::to_string(kNormalUser) : std::to_string(privilege->second);
            out += ' ';
            out += kv.second;
            auto hide = guidToHideTips_.find(kv.first);
            out += hide != guidToHideTips_.end() && hide->second
                ? " hide_tips=1" : " hide_tips=0";
            auto swap = guidToSwapScore_.find(kv.first);
            out += swap != guidToSwapScore_.end() && swap->second
                ? " swap_score=1\n" : " swap_score=0\n";
        }
        return out;
    }

    // Load native format + legacy profiles.func registry.cfg lines:
    //   set "~reg_<guid>" "<nickname>"  (also sset/set with single quotes)
    //   set "~guid_by_<clean>" "<guid>" (rebuilt from nicknames; accepted)
    // Unknown lines are ignored. Returns entries loaded.
    std::size_t Load(const std::string& text) {
        std::size_t n = 0;
        std::size_t pos = 0;
        while (pos < text.size()) {
            std::size_t nl = text.find('\n', pos);
            std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            if (nl == std::string::npos)
                pos = text.size();
            else
                pos = nl + 1;
            // Trim \r + whitespace.
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                line.pop_back();
            std::size_t s = 0;
            while (s < line.size() && std::isspace(static_cast<unsigned char>(line[s])))
                ++s;
            if (s >= line.size() || line[s] == '#' || line[s] == '/' )
                continue;
            std::string t = line.substr(s);
            // Legacy: ("s"set ...) "~reg_<guid>" "<nick>" / '~reg_...' '...'.
            std::size_t regPos = t.find("~reg_");
            if (regPos != std::string::npos) {
                std::size_t g0 = regPos + 5;
                std::size_t g1 = g0;
                while (g1 < t.size() && std::isdigit(static_cast<unsigned char>(t[g1])))
                    ++g1;
                std::string guid = t.substr(g0, g1 - g0);
                // Nickname = last quoted token on the line.
                std::string nick;
                std::size_t q1 = t.rfind('"');
                if (q1 != std::string::npos) {
                    std::size_t q0 = t.rfind('"', q1 == 0 ? 0 : q1 - 1);
                    // Need the pair before the trailing quote: find opening.
                    // rfind above gives closing; walk back for its opener that
                    // is not the same char (handles "" empty).
                    if (q0 != std::string::npos && q0 < q1)
                        nick = t.substr(q0 + 1, q1 - q0 - 1);
                    else {
                        // Single-quote style: 'nick'.
                        q1 = t.rfind('\'');
                        if (q1 != std::string::npos) {
                            std::size_t q0b = t.rfind('\'', q1 == 0 ? 0 : q1 - 1);
                            if (q0b != std::string::npos && q0b < q1)
                                nick = t.substr(q0b + 1, q1 - q0b - 1);
                        }
                    }
                } else {
                    std::size_t sq1 = t.rfind('\'');
                    if (sq1 != std::string::npos) {
                        std::size_t sq0 = t.rfind('\'', sq1 == 0 ? 0 : sq1 - 1);
                        if (sq0 != std::string::npos && sq0 < sq1)
                            nick = t.substr(sq0 + 1, sq1 - sq0 - 1);
                    }
                }
                if (GuidValid(guid) && !nick.empty() && Add(guid, nick))
                    ++n;
                continue;
            }
            if (t.find("~guid_by_") != std::string::npos)
                continue;  // rebuilt from nicknames; no need to import
            // set "guid" "nick" wrapper around a native line.
            if ((t.compare(0, 4, "set ") == 0 || t.compare(0, 5, "sset ") == 0)) {
                // Extract quoted tokens.
                std::vector<std::string> toks;
                for (std::size_t i = 0; i < t.size();) {
                    if (t[i] == '"' || t[i] == '\'') {
                        char q = t[i++];
                        std::size_t j = t.find(q, i);
                        if (j == std::string::npos)
                            break;
                        toks.push_back(t.substr(i, j - i));
                        i = j + 1;
                    } else {
                        ++i;
                    }
                }
                if (toks.size() >= 2 && GuidValid(toks[toks.size() - 2]) && Add(toks[toks.size() - 2], toks.back()))
                    ++n;
                continue;
            }
            // Native: <guid> <privilege|-> <nickname...>. Older two-field
            // lines are ordinary profiles.
            std::size_t sp = t.find_first_of(" \t");
            if (sp == std::string::npos)
                continue;
            std::string guid = t.substr(0, sp);
            std::size_t ns = t.find_first_not_of(" \t", sp);
            std::string rest = ns == std::string::npos ? "" : t.substr(ns);
            int privilege = kNormalUser;
            bool hideTips = false;
            bool swapScore = false;
            // Strip trailing " key=0|1" option suffixes in any order.
            // Old files carry only hide_tips (or neither); new files add swap_score.
            for (;;) {
                bool stripped = false;
                const std::string hideMarker = " hide_tips=";
                std::size_t hs = rest.rfind(hideMarker);
                if (hs != std::string::npos && hs + hideMarker.size() + 1 == rest.size() &&
                    (rest[hs + hideMarker.size()] == '0' ||
                     rest[hs + hideMarker.size()] == '1')) {
                    hideTips = rest[hs + hideMarker.size()] == '1';
                    rest.resize(hs);
                    while (!rest.empty() &&
                           std::isspace(static_cast<unsigned char>(rest.back())))
                        rest.pop_back();
                    stripped = true;
                }
                const std::string swapMarker = " swap_score=";
                std::size_t ss = rest.rfind(swapMarker);
                if (ss != std::string::npos && ss + swapMarker.size() + 1 == rest.size() &&
                    (rest[ss + swapMarker.size()] == '0' ||
                     rest[ss + swapMarker.size()] == '1')) {
                    swapScore = rest[ss + swapMarker.size()] == '1';
                    rest.resize(ss);
                    while (!rest.empty() &&
                           std::isspace(static_cast<unsigned char>(rest.back())))
                        rest.pop_back();
                    stripped = true;
                }
                if (!stripped)
                    break;
            }
            std::string nick = rest;
            std::size_t ls = rest.find_first_of(" \t");
            if (ls != std::string::npos) {
                const std::string value = rest.substr(0, ls);
                if (value == "0" || value == "1" || value == "2") {
                    privilege = value[0] - '0';
                    std::size_t name = rest.find_first_not_of(" \t", ls);
                    nick = name == std::string::npos ? "" : rest.substr(name);
                } else if (value == "-" || value == "-1") {
                    std::size_t name = rest.find_first_not_of(" \t", ls);
                    nick = name == std::string::npos ? "" : rest.substr(name);
                }
            }
            // Strip surrounding quotes if the file was hand-edited quoted.
            if (nick.size() >= 2 &&
                ((nick.front() == '"' && nick.back() == '"') ||
                 (nick.front() == '\'' && nick.back() == '\'')))
                nick = nick.substr(1, nick.size() - 2);
            if (GuidValid(guid) && !nick.empty() &&
                Add(guid, nick, nullptr, privilege, hideTips, swapScore))
                ++n;
        }
        return n;
    }

private:
    std::map<std::string, std::string> guidToNick_;
    std::map<std::string, std::string> cleanToGuid_;
    std::map<std::string, int> guidToPrivilege_;
    std::map<std::string, bool> guidToHideTips_;
    std::map<std::string, bool> guidToSwapScore_;
};

// Per-slot runtime (memory only, cleared on disconnect / map change).
// guid is written from the first valid team_red_blue guid, overwritten
// when a new roster identity binds, and cleared on disconnect. A bare 0/1
// team_red_blue does not assign this field. The re-push builds the full
// team_red_blue string from it.
struct SlotState {
    std::string guid;
    int team = -1;               // 0 blue / 1 red / -1 unknown
    std::string nickname;        // registry nickname for this slot
    bool registered = false;     // roster player on this slot
    int privilege = kUnspecifiedPrivilege; // 0 server, 1 map, 2 normal
    std::string restored;        // last pushed team_red_blue (dedup)
    bool intCvar = false;        // client stores team_red_blue as CVAR_INT
    bool carrierPending = false; // profile push awaiting its userinfo echo
    bool offered = false;        // setup-required notice sent once
    bool begun = false;          // ClientBegin ran (fully connected)
    bool welcomePending = false; // signed in while connecting: welcome on Begin
};

}  // namespace profiles
