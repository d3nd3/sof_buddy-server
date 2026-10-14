#pragma once

// gamepark_logic: pure, host-testable core for the gamepark transfer.
//
// When a signed-in server admin (privilege 0) or map admin (privilege 1)
// changes the map with a sofplus `.rcon` command, the gamepark claim follows
// them: they become the signed-in gamepark holder and any previous holder's
// rcon session is logged out. The engine side (gamepark.cpp) only feeds this
// machine observations (pending intent, map-change detection, cvar reads)
// and queues the lines built here through Cbuf.
//
// Why deferred: the ClientCommand hook runs BEFORE sofplus, so it cannot know
// whether the `.rcon map` will actually succeed (rcon auth, grants, valid map
// are all sofplus-side). Recording intent and acting only when the requested
// map really loads keeps causality exact and never disturbs failed attempts.
//
// Line protocol (mirrors spf_sv_gamepark.func claim flow):
//   sp_sv_info_client <slot>          refresh _sp_sv_info_client_* for sender
//   sp_sc_info_time                   refresh _sp_sc_info_time_min
//   set _spf_sv_gamepark_claimed #_sp_sc_info_time_min
//   add _spf_sv_gamepark_claimed #_spf_sv_gamepark_duration
//   set _spf_sv_gamepark_claimed_ip <sender ip>
//   zero _spf_sv_gamepark_claimed_inactive
//   sp_sc_cvar_sset hostname #_spf_sv_gamepark_hostname #_sp_sv_info_client_name
//                                     (only when hostnameBase is non-empty)
//   zero _spf_sv_rcon_username_<i> / _ip_<i> / _commands_<i>
//                                     (previous-holder slots only)

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

namespace gamepark {

// Engine map-change commands accepted as `.rcon` subcommands. `restart`
// reloads in place without a map load and is deliberately excluded.
inline bool IsMapChangeSubcommand(const char* word) {
    if (!word || !word[0])
        return false;
    std::string s;
    for (const char* p = word; *p; ++p)
        s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*p))));
    return s == "map" || s == "gamemap";
}

// Case-insensitive map-name equality (engine mapnames compare folded).
inline bool MapsEqualFold(const std::string& a, const std::string& b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

// Keep [0-9.] (bare dotted quad, e.g. 1.2.3.4 — the inet_ntoa form without
// port that sp_sv_info_client stores). Anything else is dropped; empty result
// means "no usable address".
inline std::string SanitizeIp(const std::string& s) {
    std::string out;
    for (char c : s) {
        if ((c >= '0' && c <= '9') || c == '.')
            out.push_back(c);
        if (out.size() >= 32)
            break;
    }
    return out;
}

// Formats raw address bytes exactly like inet_ntoa (network order, no port):
// the string form sp_sv_info_client / rcon sessions / the claim ip use.
inline std::string FormatBareIp(unsigned b0, unsigned b1, unsigned b2, unsigned b3) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", b0 & 0xFFu, b1 & 0xFFu, b2 & 0xFFu,
                  b3 & 0xFFu);
    return std::string(buf);
}

// Map argument as queued: alnum plus _ . - (engine mapnames). Truncated.
inline std::string SanitizeMapArg(const std::string& s) {
    std::string out;
    for (char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '.' || c == '-')
            out.push_back(c);
        if (out.size() >= 64)
            break;
    }
    return out;
}

struct RconSlot {
    int slot0 = -1;
    std::string ip;  // raw _spf_sv_rcon_ip_<slot> value
};

struct TransferInput {
    int senderSlot0 = -1;
    std::string senderIp;      // sanitized userinfo ip of the issuer
    std::string oldClaimedIp;  // _spf_sv_gamepark_claimed_ip at transfer time
    std::string hostnameBase;  // _spf_sv_gamepark_hostname (empty = skip rename)
    std::vector<RconSlot> rconSlots;  // spawned slots' current rcon ips
};

// Builds the ordered Cbuf lines for the transfer. Previous-holder logout is
// targeted: only slots whose rcon ip equals the old claim ip (and which are
// not the sender) are cleared, so unrelated rcon sessions are untouched.
inline std::vector<std::string> BuildTransferLines(const TransferInput& in) {
    std::vector<std::string> lines;
    if (in.senderSlot0 < 0 || in.senderIp.empty())
        return lines;
    char buf[128];
    std::snprintf(buf, sizeof(buf), "sp_sv_info_client %d", in.senderSlot0);
    lines.push_back(buf);
    lines.push_back("sp_sc_info_time");
    lines.push_back("set _spf_sv_gamepark_claimed #_sp_sc_info_time_min");
    lines.push_back("add _spf_sv_gamepark_claimed #_spf_sv_gamepark_duration");
    lines.push_back("set _spf_sv_gamepark_claimed_ip " + in.senderIp);
    lines.push_back("zero _spf_sv_gamepark_claimed_inactive");
    if (!in.hostnameBase.empty())
        lines.push_back(
            "sp_sc_cvar_sset hostname #_spf_sv_gamepark_hostname #_sp_sv_info_client_name");
    if (!in.oldClaimedIp.empty()) {
        for (const RconSlot& rs : in.rconSlots) {
            if (rs.slot0 < 0 || rs.slot0 == in.senderSlot0)
                continue;
            if (rs.ip != in.oldClaimedIp)
                continue;
            std::snprintf(buf, sizeof(buf), "zero _spf_sv_rcon_username_%d", rs.slot0);
            lines.push_back(buf);
            std::snprintf(buf, sizeof(buf), "zero _spf_sv_rcon_ip_%d", rs.slot0);
            lines.push_back(buf);
            std::snprintf(buf, sizeof(buf), "zero _spf_sv_rcon_commands_%d", rs.slot0);
            lines.push_back(buf);
        }
    }
    return lines;
}

}  // namespace gamepark
