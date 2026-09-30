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
//   - guid validation (24-digit, legacy 62-digit accepted on read) + minting
//   - team_red_blue parse/build (canonical <guid><bit>-blue|-red: the bit
//     terminates the leading digit run so AssignTeam's atoi() reads it;
//     plain all-digit for legacy 62-digit guids; bare 0/1 = collapsed)
//   - nickname -> nickname_clean (colors stripped, lowercased, a-z0-9)
//   - player registry (guid <-> nickname) with save/load incl. legacy
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

inline bool IsDigitsOnly(const std::string& s) {
    if (s.empty())
        return false;
    for (char c : s) {
        if (c < '0' || c > '9')
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
    if (!GuidValid(guid) || !IsDigitsOnly(value))
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
    if (!GuidValid(guid) || !IsDigitsOnly(value))
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
    if (!IsDigitsOnly(value))
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
// cross-checked on parse (mismatch = invalid). Legacy 62-digit guids
// stay plain all-digit: with suffix they'd exceed the engine's 64-char
// userinfo value limit (62+1+1+4=68).
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

// CVAR_INT clients cannot keep the guid text. Push the 23-bit fold with
// the team in the low bit.
inline std::string TeamRedBlueForClient(const std::string& guid, int team, bool intCvar) {
    if (intCvar)
        return IntCvarPackDecimal(guid, team);
    return BuildTeamRedBlue(guid, team);
}

// Team to stuff for an integer client. The packed decimal's low bit is the
// team. A truncated full-guid push is not: both bits can land on one value,
// so keep the slot team.
inline int IntCvarTeam(const std::string& value, const std::string& guid, int slotTeam, int liveTeam) {
    int bit = -1;
    if (IntCvarMatchesGuid(value, guid, &bit))
        return bit;
    if (slotTeam == 0 || slotTeam == 1)
        return slotTeam;
    if (liveTeam == 0 || liveTeam == 1)
        return liveTeam;
    return -1;
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

// Admin registry: guid -> nickname, plus clean-nickname -> guid reverse map
// (~reg_<guid> / ~guid_by_<clean> in profiles.func).
class Registry {
public:
    bool Add(const std::string& guid, const std::string& nickname, std::string* err = nullptr) {
        if (!GuidValid(guid)) {
            if (err)
                *err = "guid must be 24 digits (legacy 62 ok)";
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
        // One clean key maps to one guid: replacing a nickname that collides
        // with another entry's clean key steals the key (same as .func
        // ~guid_by_ overwrite). The old guid entry keeps its own ~reg_ slot
        // until explicitly deleted.
        guidToNick_[guid] = nickname;
        cleanToGuid_[clean] = guid;
        return true;
    }

    // Delete by guid OR nickname (other arg empty), mirroring
    // profile_del <guid> <nickname> (legacy prof_admin_del).
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
    }

    const std::map<std::string, std::string>& GuidToNick() const { return guidToNick_; }

    // Serialize to the native file format: one "<guid> <nickname>" per line.
    // Nicknames with spaces survive (everything after the first blank).
    std::string Save() const {
        std::string out = "# sof_buddy profiles registry (guid nickname)\n";
        for (const auto& kv : guidToNick_) {
            out += kv.first;
            out += ' ';
            out += kv.second;
            out += '\n';
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
            // Native: <guid> <nickname...>.
            std::size_t sp = t.find_first_of(" \t");
            if (sp == std::string::npos)
                continue;
            std::string guid = t.substr(0, sp);
            std::size_t ns = t.find_first_not_of(" \t", sp);
            std::string nick = ns == std::string::npos ? "" : t.substr(ns);
            // Strip surrounding quotes if the file was hand-edited quoted.
            if (nick.size() >= 2 &&
                ((nick.front() == '"' && nick.back() == '"') ||
                 (nick.front() == '\'' && nick.back() == '\'')))
                nick = nick.substr(1, nick.size() - 2);
            if (GuidValid(guid) && !nick.empty() && Add(guid, nick))
                ++n;
        }
        return n;
    }

private:
    std::map<std::string, std::string> guidToNick_;
    std::map<std::string, std::string> cleanToGuid_;
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
    std::string restored;        // last pushed team_red_blue (dedup)
    bool intCvar = false;        // client stores team_red_blue as CVAR_INT
    bool offered = false;        // setup-required notice sent once
};

}  // namespace profiles
