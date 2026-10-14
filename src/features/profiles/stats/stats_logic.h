#pragma once

// stats_logic: pure, host-testable aggregation for the admin-gated match stats.
//
// Design constraints (see stats/README.md):
//   - 32-bit Windows DLL under Wine, single game thread, 100ms tick budget.
//   - No extra deps (no SQLite): <4096 players, rare writes (<=1/map).
//   - Hot path cost must be zero: tracking is in-memory pending flag +
//     a frags snapshot at flush time. No per-frame/per-kill hooks.
//   - Persistence is two flat files under <user>/sofplus/data/stats/:
//       totals.cfg  - aggregated per-guid (rewritten on flush, like registry.cfg)
//       matches.log - append-only per-match (one header + N player lines)
//   - Synchronous file IO happens only on flush (admin .upload_match at
//     map end), a few KB, far below the tick budget. No threads, no locks:
//     everything runs on the game thread.
//
// This header owns everything that does NOT need the engine:
//   - totals aggregation (guid -> matches/frags/last map+time)
//   - totals.cfg save/load (native format + tolerant parse)
//   - matches.log line formatting (header + player rows)
//   - nickname sanitising for file output (no quotes/backslashes/controls)
//
// Engine binding (edict scan, frags read, file paths, pending/flush,
// ClientCommand filter, minigames page) lives in stats.cpp and only feeds
// this machine observations.

#include <cctype>
#include <cstddef>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace stats {

constexpr int kMaxTotalsEntries = 4096;
constexpr int kMaxNicknameLen = 64;
constexpr int kMaxMapLen = 64;

// One aggregated row: lifetime totals for a single guid.
struct TotalsEntry {
    std::string guid;
    std::string nick;       // last registry nickname seen (display, sanitised on save)
    long long matches = 0;  // matches this guid participated in (recorded uploads)
    long long frags = 0;    // sum of end-of-match frags (resp.score via ps.stats)
    std::string lastMap;    // map of most recent recorded match
    std::string lastSeen;   // UTC "YYYY-MM-DDTHH:MM:SSZ" of most recent recorded match
};

// One player row inside a single recorded match.
struct MatchPlayer {
    std::string guid;
    std::string nick;      // registry nickname at flush time
    std::string live;      // live userinfo name at flush time (may differ)
    int team = -1;         // 0 blue / 1 red / -1 unknown/spectator
    int frags = 0;         // ps.stats[STAT_FRAGS] at flush time
};

// One recorded match (admin vouched via .upload_match).
struct Match {
    long long seq = 0;  // 1-based sequence (totals matches counter at flush)
    std::string utc;    // UTC timestamp "YYYY-MM-DDTHH:MM:SSZ"
    std::string map;    // mapname at flush
    std::string uploaderGuid;
    std::string uploaderNick;
    std::vector<MatchPlayer> players;
};

inline void CleanField(char* dst, int cap, const char* src) {
    if (!dst || cap <= 0)
        return;
    int i = 0;
    for (; src && src[i] && i < cap - 1; ++i) {
        const unsigned char c = static_cast<unsigned char>(src[i]);
        dst[i] = (c < 32 || c == '"' || c == '\\') ? ' ' : static_cast<char>(c);
    }
    dst[i] = '\0';
}

inline std::string CleanStr(const std::string& s, std::size_t cap = 64) {
    std::string out;
    out.reserve(s.size() < cap ? s.size() : cap);
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        out.push_back((u < 32 || c == '"' || c == '\\') ? ' ' : c);
        if (out.size() + 1 >= cap)
            break;
    }
    // Trim trailing spaces (padding from replacements).
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

inline std::string CleanMap(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '-' || c == '.')
            out.push_back(c);
        else if (u < 32 || c == '"' || c == '\\')
            out.push_back('_');
        else
            out.push_back('_');
        if (out.size() >= static_cast<std::size_t>(kMaxMapLen))
            break;
    }
    return out.empty() ? std::string("unknown") : out;
}

// totals.cfg line: "<guid> <matches> <frags> <nick> last_map=<map> last_seen=<utc>"
// Nick may contain spaces; the two trailing key=value suffixes parse in any
// order. Older two-field "<guid> <nick>" lines load as matches=0/frags=0.
inline std::string FormatTotalsLine(const TotalsEntry& e) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s %lld %lld %s last_map=%s last_seen=%s", e.guid.c_str(),
                  e.matches, e.frags, CleanStr(e.nick, kMaxNicknameLen).c_str(),
                  CleanMap(e.lastMap).c_str(),
                  e.lastSeen.empty() ? "never" : CleanStr(e.lastSeen, 32).c_str());
    return std::string(buf);
}

// matches.log header: "match <seq> <utc> <map> <uploader_guid> <uploader_nick>"
inline std::string FormatMatchHeader(const Match& m) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), "match %lld %s %s %s %s", m.seq, m.utc.c_str(),
                  CleanMap(m.map).c_str(), m.uploaderGuid.c_str(),
                  CleanStr(m.uploaderNick, kMaxNicknameLen).c_str());
    return std::string(buf);
}

// HUD row vocabulary (shared by the .mg landing and the mg_stats detail
// page so the two read as one system; ASCII only — the HUD font has no
// middle-dot/glyph extras):
//   summary : "12 matches recorded | 34 players tracked"
//   lifetime: "145 frags | 3 matches"              (mg_stats detail rows)
//   landing : "12 frags | 3 matches | 145 total"   (landing rows, live first)
inline std::string FormatTotalsSummary(long long matches, long long players) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%lld matches recorded | %lld players tracked", matches,
                  players);
    return std::string(buf);
}

inline std::string FormatLifetimeStats(long long frags, long long matches) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%lld frags | %lld matches", frags, matches);
    return std::string(buf);
}

inline std::string FormatLandingStats(int liveFrags, long long frags, long long matches) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%d frags | %lld matches | %lld total", liveFrags,
                  matches, frags);
    return std::string(buf);
}

// Menu-tap upload schedule (mirrors the .upload_match .func mid-map branch,
// which the tap cannot reach via fallthrough — taps are not client commands).
// Sets the sofplus pending + slot, then announces, so the map-end hook fires
// the screenshot + discord exactly as if chat had scheduled it. Nick is
// sanitised: it is embedded in a quoted broadcast line.
inline std::vector<std::string> BuildUploadScheduleLines(const std::string& nick, int slot0) {
    std::vector<std::string> lines;
    if (slot0 < 0)
        return lines;
    char buf[288];
    lines.push_back("set _spf_upload_pending 1");
    std::snprintf(buf, sizeof(buf), "set _spf_upload_slot %d", slot0);
    lines.push_back(buf);
    std::string clean = CleanStr(nick, 24);
    if (clean.empty())
        clean = "admin";
    std::snprintf(buf, sizeof(buf),
                  "sp_sv_print_broadcast \"%s issued %%03.upload_match %%04command; "
                  "scoreboard uploads to Discord historic-public-matches when this ends. "
                  "Issue %%03.cancel_upload %%04to stop this.\"",
                  clean.c_str());
    lines.push_back(buf);
    return lines;
}

// matches.log player row: "player <guid> <frags> <team> <nick> | <live>"
// Team renders as blue/red/spec. The "| live" suffix is omitted when live
// is empty or equals nick.
inline std::string FormatMatchPlayer(const MatchPlayer& p) {
    const char* team = p.team == 0 ? "blue" : p.team == 1 ? "red" : "spec";
    std::string nick = CleanStr(p.nick, kMaxNicknameLen);
    std::string live = CleanStr(p.live, 32);
    char buf[512];
    if (!live.empty() && live != nick)
        std::snprintf(buf, sizeof(buf), "player %s %d %s %s | %s", p.guid.c_str(), p.frags,
                      team, nick.c_str(), live.c_str());
    else
        std::snprintf(buf, sizeof(buf), "player %s %d %s %s", p.guid.c_str(), p.frags, team,
                      nick.c_str());
    return std::string(buf);
}

// Per-guid lifetime totals. Keyed by full guid string (24-digit canonical,
// legacy 62-digit accepted — validation lives in profiles_logic.h on the
// engine side; here any non-empty key is accepted so unit tests stay
// dependency-free).
class Totals {
public:
    bool AddMatch(const Match& m) {
        if (m.players.empty())
            return false;
        for (const MatchPlayer& p : m.players) {
            if (p.guid.empty())
                continue;
            if (entries_.size() >= static_cast<std::size_t>(kMaxTotalsEntries) &&
                entries_.find(p.guid) == entries_.end())
                continue;  // full: update existing only, same policy as registry
            TotalsEntry& e = entries_[p.guid];
            e.guid = p.guid;
            if (!p.nick.empty())
                e.nick = p.nick;
            e.matches += 1;
            e.frags += p.frags;
            e.lastMap = m.map;
            e.lastSeen = m.utc;
        }
        return true;
    }

    bool Lookup(const std::string& guid, TotalsEntry* out) const {
        auto it = entries_.find(guid);
        if (it == entries_.end())
            return false;
        if (out)
            *out = it->second;
        return true;
    }

    std::size_t Size() const { return entries_.size(); }
    void Clear() { entries_.clear(); }
    const std::map<std::string, TotalsEntry>& Entries() const { return entries_; }

    std::string Save() const {
        std::string out = "# sof_buddy stats totals (guid matches frags nickname last_map= last_seen=)\n";
        for (const auto& kv : entries_)
            out += FormatTotalsLine(kv.second) + "\n";
        return out;
    }

    // Tolerant parse: native lines + legacy "<guid> <nick>" lines.
    // Unknown/malformed lines are ignored. Returns entries loaded.
    std::size_t Load(const std::string& text) {
        std::size_t n = 0;
        std::size_t pos = 0;
        while (pos < text.size()) {
            std::size_t nl = text.find('\n', pos);
            std::string line =
                text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            pos = (nl == std::string::npos) ? text.size() : nl + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                line.pop_back();
            std::size_t s = 0;
            while (s < line.size() && std::isspace(static_cast<unsigned char>(line[s])))
                ++s;
            if (s >= line.size() || line[s] == '#' || line[s] == '/')
                continue;
            std::string t = line.substr(s);
            std::size_t sp = t.find_first_of(" \t");
            if (sp == std::string::npos)
                continue;
            std::string guid = t.substr(0, sp);
            if (guid.empty())
                continue;
            std::size_t ns = t.find_first_not_of(" \t", sp);
            std::string rest = (ns == std::string::npos) ? "" : t.substr(ns);
            TotalsEntry e;
            e.guid = guid;
            // Strip trailing key=value suffixes in any order.
            for (;;) {
                bool stripped = false;
                const std::string mapMarker = " last_map=";
                std::size_t ms = rest.rfind(mapMarker);
                if (ms != std::string::npos && ms + mapMarker.size() < rest.size() &&
                    rest.find(' ', ms + 1) == std::string::npos) {
                    e.lastMap = rest.substr(ms + mapMarker.size());
                    rest.resize(ms);
                    while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.back())))
                        rest.pop_back();
                    stripped = true;
                }
                const std::string seenMarker = " last_seen=";
                std::size_t ss = rest.rfind(seenMarker);
                if (ss != std::string::npos && ss + seenMarker.size() < rest.size() &&
                    rest.find(' ', ss + 1) == std::string::npos) {
                    e.lastSeen = rest.substr(ss + seenMarker.size());
                    rest.resize(ss);
                    while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.back())))
                        rest.pop_back();
                    stripped = true;
                }
                if (!stripped)
                    break;
            }
            // Rest is either "<matches> <frags> <nick...>" or legacy "<nick...>".
            long long matches = 0, frags = 0;
            std::string nick = rest;
            std::size_t p1 = rest.find_first_of(" \t");
            if (p1 != std::string::npos) {
                std::string v1 = rest.substr(0, p1);
                std::size_t p1e = rest.find_first_not_of(" \t", p1);
                std::string r1 = (p1e == std::string::npos) ? "" : rest.substr(p1e);
                std::size_t p2 = r1.find_first_of(" \t");
                if (p2 != std::string::npos) {
                    std::string v2 = r1.substr(0, p2);
                    bool d1 = !v1.empty(), d2 = !v2.empty();
                    for (char c : v1)
                        if (c < '0' || c > '9')
                            d1 = false;
                    for (char c : v2)
                        if (!((c >= '0' && c <= '9') || c == '-'))
                            d2 = false;
                    if (d1 && d2) {
                        matches = std::atoll(v1.c_str());
                        frags = std::atoll(v2.c_str());
                        std::size_t ne = r1.find_first_not_of(" \t", p2);
                        nick = (ne == std::string::npos) ? "" : r1.substr(ne);
                    }
                }
            }
            if (nick.size() >= 2 &&
                ((nick.front() == '"' && nick.back() == '"') ||
                 (nick.front() == '\'' && nick.back() == '\'')))
                nick = nick.substr(1, nick.size() - 2);
            if (nick.empty())
                continue;
            if (entries_.size() >= static_cast<std::size_t>(kMaxTotalsEntries) &&
                entries_.find(guid) == entries_.end())
                continue;
            e.nick = nick;
            e.matches = matches < 0 ? 0 : matches;
            e.frags = frags;
            entries_[guid] = e;
            ++n;
        }
        return n;
    }

private:
    std::map<std::string, TotalsEntry> entries_;  // sorted by guid (stable file order)
};

}  // namespace stats
