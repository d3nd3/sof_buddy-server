// stats (inside profiles): admin-vouched per-player match statistics.
//
// Trust model: the .upload_match request IS the data-quality signal. A
// signed-in server admin (privilege 0) or map admin (privilege 1) may trigger
// it; the resulting end-of-match frags snapshot is treated as clean. Guests
// and normal users are blocked before sofplus ever sees the word, so the
// sofplus .upload_match / .cancel_upload scheduling + discord screenshot
// still runs exactly once, only for admins.
//
// Ordering: minigames' outer spsv detour (HkSpsvClientCommand -> MgTake->
// ClientCommand) runs before spsv's own ClientCommand_HOOK, so
// Stats_HandleClientCommand() wins. Admin chat falls through (returns false)
// to the original sofplus dot-function; rejects consume (return true).
// The mg_stats detail page is open to every signed-in tier. The restricted
// mg_upload menu row (menuOnly + Level01Only, action-only, no page) fires
// Stats_TriggerUpload on one +use tap, which queues the sofplus schedule
// itself — taps are not client commands, so fallthrough cannot apply there.
//
// Storage (no SQLite — see README): two flat files under
// <user>/sofplus/data/stats/, same pattern as profiles registry.cfg:
//   totals.cfg  - aggregated per-guid (rewritten on flush, capped 4096)
//   matches.log - append-only per-match (header + N player rows)
// Synchronous IO happens only on flush (<=1/map, a few KB). Hot path cost is
// zero: no per-frame/per-kill hooks, just one bool + one float read per
// SV_Frame Post while idle, and a <=64-slot snapshot on flush.

#include "stats.h"
#include "stats_logic.h"
#include "../cvar.h"
#include "../../minigames/minigames_api.h"

#include "buddy_import.h"
#include "generated_detours.h"
#include "generated_engine_pointers.h"
#include "log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <windows.h>

namespace {

// ---------------------------------------------------------------------------
// Constants (same verified offsets minigames/profiles already use).
// ---------------------------------------------------------------------------
constexpr char kStatsGameId[] = "mg_stats";
constexpr unsigned kEdictClientOfs = 0x74;
constexpr unsigned kClientStatsOfs = 164;
constexpr int kStatFrags = 10;
constexpr unsigned kClTeamOfs = 0x324;      // gclient resp.team area: 1 blue, 2 red
constexpr unsigned kClUserinfoOfs = 0xCC;
constexpr unsigned kRvaLevelIntermissiontime = 0x15D1C8;
constexpr unsigned kCvarStringOfs = 0x04;
constexpr unsigned kCvarValueOfs = 0x18;
constexpr int kCvarFlagArchive = 1;
constexpr int kCvarFlagNoSet = 8;

// ---------------------------------------------------------------------------
// Safety helpers (same pattern as profiles.cpp / minigames.cpp).
// ---------------------------------------------------------------------------
inline bool IsValidUserPointer(const void* ptr) {
    auto addr = reinterpret_cast<uintptr_t>(ptr);
    return (addr >= 0x10000 && addr <= 0x7FFFFFFF);
}

inline bool Readable(const void* p, unsigned len) {
    return !IsBadReadPtr(const_cast<void*>(p), len);
}

HMODULE GameMod() {
    if (HMODULE shim = GetModuleHandleA("gamex86.dll")) {
        using handle_fn = HMODULE (*)();
        auto fn = reinterpret_cast<handle_fn>(GetProcAddress(shim, "Buddy_GetGameDllHandle"));
        if (fn) {
            if (HMODULE stock = fn())
                return stock;
        }
    }
    if (HMODULE h = GetModuleHandleA("oldgamex86.dll"))
        return h;
    return GetModuleHandleA("OLDGAMEX86.DLL");
}

bool IsValidModuleRva(HMODULE h, unsigned rva, unsigned size) {
    if (!h || size == 0 || !Readable(h, sizeof(IMAGE_DOS_HEADER)))
        return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(h);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
        return false;
    const char* base = reinterpret_cast<const char*>(h);
    if (!Readable(base + dos->e_lfanew, sizeof(IMAGE_NT_HEADERS)))
        return false;
    const auto* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE)
        return false;
    const unsigned img = nt->OptionalHeader.SizeOfImage;
    return rva < img && rva + size <= img;
}

bool LevelIntermission() {
    HMODULE h = GameMod();
    if (!h || !IsValidModuleRva(h, kRvaLevelIntermissiontime, sizeof(float)))
        return false;
    const float* t =
        reinterpret_cast<const float*>(reinterpret_cast<char*>(h) + kRvaLevelIntermissiontime);
    if (!Readable(t, sizeof(float)))
        return false;
    return *t != 0.0f;
}

bool InfoKeyReady() {
    return detour_Info_ValueForKey::oInfo_ValueForKey &&
           !IsBadReadPtr(reinterpret_cast<void*>(detour_Info_ValueForKey::oInfo_ValueForKey), 1);
}

bool IsExecutableCodeAddress(const void* ptr) {
    auto addr = reinterpret_cast<uintptr_t>(ptr);
    if (addr < 0x10000 || addr > 0x7FFFFFFF)
        return false;
    MEMORY_BASIC_INFORMATION mbi = {0};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0)
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    const DWORD execMask =
        PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & execMask) != 0;
}

// Numeric read of a sofplus engine cvar. Missing → 0 (Cvar_Get creates it
// empty — the same trade-off profiles already makes).
double SofplusCvarNumber(const char* name) {
    if (!name || !name[0])
        return 0.0;
    void* cv = Buddy_GetEngineCvar(name, "0", 0, nullptr);
    if (!cv || !Readable(static_cast<char*>(cv) + kCvarStringOfs, sizeof(char*)))
        return 0.0;
    char* s = *reinterpret_cast<char**>(static_cast<char*>(cv) + kCvarStringOfs);
    if (!s || !IsValidUserPointer(s) || !Readable(s, 1))
        return 0.0;
    return std::atof(s);
}

void QueueCbufLines(const std::vector<std::string>& lines);

// ---------------------------------------------------------------------------
// Cvars (_sofbuddy_stats master + NOSET gauges).
// ---------------------------------------------------------------------------
struct OutputCvar {
    void* cv = nullptr;
    char* original = nullptr;
    char buf[2][32] = {};
    int live = 0;

    void Bind(const char* name) {
        if (cv)
            return;
        cv = Buddy_GetEngineCvar(name, "0", kCvarFlagNoSet, nullptr);
        if (!cv)
            return;
        original = *reinterpret_cast<char**>(static_cast<char*>(cv) + kCvarStringOfs);
    }

    void Publish(float value, const char* text) {
        if (!cv)
            return;
        char* base = static_cast<char*>(cv);
        *reinterpret_cast<volatile float*>(base + kCvarValueOfs) = value;
        char* spare = buf[1 - live];
        std::size_t i = 0;
        for (; text[i] && i + 1 < sizeof(buf[0]); ++i)
            spare[i] = text[i];
        spare[i] = '\0';
        *reinterpret_cast<char* volatile*>(base + kCvarStringOfs) = spare;
        live = 1 - live;
    }
};

void FormatI64(char* out, std::size_t n, long long v) {
    if (n == 0)
        return;
    char digits[24];
    std::size_t d = 0;
    unsigned long long mag =
        v < 0 ? 0ull - static_cast<unsigned long long>(v) : static_cast<unsigned long long>(v);
    do {
        digits[d++] = static_cast<char>('0' + (mag % 10ull));
        mag /= 10ull;
    } while (mag != 0ull && d < sizeof(digits));
    std::size_t i = 0;
    if (v < 0 && i + 1 < n)
        out[i++] = '-';
    while (d > 0 && i + 1 < n)
        out[i++] = digits[--d];
    out[i] = '\0';
}

OutputCvar g_outMatches;  // _sofbuddy_stats_matches
OutputCvar g_outPlayers;  // _sofbuddy_stats_players
OutputCvar g_outErrors;   // _sofbuddy_stats_errors
OutputCvar g_outPending;  // _sofbuddy_stats_pending
void* g_cvEnabled = nullptr;  // _sofbuddy_stats

void Stats_InitCvars() {
    g_outMatches.Bind("_sofbuddy_stats_matches");
    g_outPlayers.Bind("_sofbuddy_stats_players");
    g_outErrors.Bind("_sofbuddy_stats_errors");
    g_outPending.Bind("_sofbuddy_stats_pending");
    if (!g_cvEnabled)
        g_cvEnabled = Buddy_GetEngineCvar("_sofbuddy_stats", "1", kCvarFlagArchive, nullptr);
}

// ---------------------------------------------------------------------------
// State.
// ---------------------------------------------------------------------------
stats::Totals g_totals;
long long g_matches = 0;  // recorded uploads this boot (+ loaded seq base)
long long g_seqBase = 0;  // matches.log headers counted on load (restart-proof seq)
long long g_errors = 0;

struct Pending {
    bool active = false;
    int uploaderSlot0 = -1;
    char uploaderGuid[64] = {};
    char uploaderNick[64] = {};
    char map[64] = {};
};
Pending g_pending;

void RefreshOutputs() {
    char text[32];
    FormatI64(text, sizeof(text), g_matches + g_seqBase);
    g_outMatches.Publish(static_cast<float>(g_matches + g_seqBase), text);
    FormatI64(text, sizeof(text), static_cast<long long>(g_totals.Size()));
    g_outPlayers.Publish(static_cast<float>(g_totals.Size()), text);
    FormatI64(text, sizeof(text), g_errors);
    g_outErrors.Publish(static_cast<float>(g_errors), text);
    FormatI64(text, sizeof(text), g_pending.active ? 1 : 0);
    g_outPending.Publish(g_pending.active ? 1.0f : 0.0f, text);
}

void NoteError() {
    ++g_errors;
    RefreshOutputs();
}

// Queue console lines as one Cbuf blob (chunked under the 0x1000 insert cap).
// Direct original (bypasses cmdpark parking): rare, small, must run now.
void QueueCbufLines(const std::vector<std::string>& lines) {
    auto add = detour_Cbuf_AddText::oCbuf_AddText;
    if (!add || !IsExecutableCodeAddress(reinterpret_cast<const void*>(add))) {
        Buddy_DebugPrintf("[stats] Cbuf unavailable, lines dropped\n");
        NoteError();
        return;
    }
    std::string chunk;
    auto flush = [&]() {
        if (chunk.empty())
            return;
        char tmp[1024];
        std::snprintf(tmp, sizeof(tmp), "%s", chunk.c_str());
        chunk.clear();
        add(tmp);
    };
    for (const std::string& ln : lines) {
        if (chunk.size() + ln.size() + 1 >= 900)
            flush();
        chunk += ln;
        chunk += "\n";
    }
    flush();
}

// ---------------------------------------------------------------------------
// Files: <user>/sofplus/data/stats/totals.cfg + matches.log
// ---------------------------------------------------------------------------
void StatsFsPath(const char* leaf, char* out, std::size_t cap) {
    if (!out || cap == 0)
        return;
    out[0] = '\0';
    if (!leaf || !leaf[0])
        return;
    const char* user = Buddy_FS_Userdir();
    if (!user || !user[0])
        user = "User";
    std::snprintf(out, cap, "%s/sofplus/data/stats/%s", user, leaf);
}

bool Stats_LoadTotals() {
    void* buf = nullptr;
    int len = Buddy_FS_LoadFile("sofplus/data/stats/totals.cfg", &buf, false);
    std::string text;
    if (len >= 0 && buf) {
        text.assign(static_cast<const char*>(buf), static_cast<std::size_t>(len));
        Buddy_FS_FreeFile(buf);
    } else {
        char path[512];
        StatsFsPath("totals.cfg", path, sizeof(path));
        FILE* f = path[0] ? std::fopen(path, "rb") : nullptr;
        if (!f)
            return false;
        char chunk[4096];
        std::size_t n = 0;
        while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0)
            text.append(chunk, n);
        std::fclose(f);
    }
    g_totals.Clear();
    g_totals.Load(text);
    // Restart-proof sequence: count existing match headers so the next
    // matches.log header continues the numbering instead of restarting at 1.
    g_seqBase = 0;
    void* mbuf = nullptr;
    int mlen = Buddy_FS_LoadFile("sofplus/data/stats/matches.log", &mbuf, false);
    std::string mtext;
    if (mlen >= 0 && mbuf) {
        mtext.assign(static_cast<const char*>(mbuf), static_cast<std::size_t>(mlen));
        Buddy_FS_FreeFile(mbuf);
    } else {
        char mpath[512];
        StatsFsPath("matches.log", mpath, sizeof(mpath));
        FILE* mf = mpath[0] ? std::fopen(mpath, "rb") : nullptr;
        if (mf) {
            char chunk[4096];
            std::size_t n = 0;
            while ((n = std::fread(chunk, 1, sizeof(chunk), mf)) > 0)
                mtext.append(chunk, n);
            std::fclose(mf);
        }
    }
    if (!mtext.empty()) {
        std::size_t pos = 0;
        while (pos < mtext.size()) {
            std::size_t nl = mtext.find('\n', pos);
            std::string line = mtext.substr(
                pos, nl == std::string::npos ? std::string::npos : nl - pos);
            pos = (nl == std::string::npos) ? mtext.size() : nl + 1;
            std::size_t s = 0;
            while (s < line.size() && (line[s] == ' ' || line[s] == '\t'))
                ++s;
            if (line.compare(s, 6, "match ") == 0)
                ++g_seqBase;
        }
    }
    g_matches = 0;
    RefreshOutputs();
    return true;
}

bool Stats_SaveTotals() {
    char path[512];
    StatsFsPath("totals.cfg", path, sizeof(path));
    if (!path[0])
        return false;
    Buddy_FS_CreatePath(path);
    FILE* f = std::fopen(path, "wb");
    if (!f) {
        Buddy_DebugPrintf("stats: cannot open '%s' for save\n", path);
        return false;
    }
    std::string text = g_totals.Save();
    bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    std::fclose(f);
    if (!ok)
        Buddy_DebugPrintf("stats: save to '%s' failed\n", path);
    return ok;
}

bool Stats_AppendMatch(const stats::Match& m) {
    char path[512];
    StatsFsPath("matches.log", path, sizeof(path));
    if (!path[0])
        return false;
    Buddy_FS_CreatePath(path);
    FILE* f = std::fopen(path, "ab");
    if (!f) {
        Buddy_DebugPrintf("stats: cannot open '%s' for append\n", path);
        return false;
    }
    std::string out = stats::FormatMatchHeader(m) + "\n";
    for (const auto& p : m.players)
        out += stats::FormatMatchPlayer(p) + "\n";
    bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    if (!ok)
        Buddy_DebugPrintf("stats: append to '%s' failed\n", path);
    return ok;
}

// ---------------------------------------------------------------------------
// Snapshot helpers.
// ---------------------------------------------------------------------------
void CurrentMap(char* out, int cap) {
    if (!out || cap <= 0)
        return;
    out[0] = '\0';
    void* cv = Buddy_GetEngineCvar("mapname", "", 0, nullptr);
    const char* s = "";
    if (cv && Readable(static_cast<char*>(cv) + kCvarStringOfs, sizeof(char*))) {
        char* v = *reinterpret_cast<char**>(static_cast<char*>(cv) + kCvarStringOfs);
        if (v && IsValidUserPointer(v) && Readable(v, 1))
            s = v;
    }
    int i = 0;
    for (; s[i] && i < cap - 1; ++i) {
        char c = s[i];
        out[i] = (c == '"' || c == '\\' || static_cast<unsigned char>(c) < 32) ? '_' : c;
    }
    out[i] = '\0';
    if (!out[0])
        std::snprintf(out, static_cast<std::size_t>(cap), "unknown");
}

void CurrentUtc(char* out, int cap) {
    if (!out || cap <= 0)
        return;
    SYSTEMTIME st = {};
    GetSystemTime(&st);
    std::snprintf(out, static_cast<std::size_t>(cap), "%04u-%02u-%02uT%02u:%02u:%02uZ",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

int SlotFrags(void* ent) {
    if (!ent || !Readable(static_cast<char*>(ent) + kEdictClientOfs, sizeof(void*)))
        return 0;
    void* client = *reinterpret_cast<void**>(static_cast<char*>(ent) + kEdictClientOfs);
    if (!client || !Readable(static_cast<char*>(client) + kClientStatsOfs + 32, 32))
        return 0;
    const short* stats =
        reinterpret_cast<const short*>(static_cast<char*>(client) + kClientStatsOfs);
    if (!Readable(stats + kStatFrags, sizeof(short)))
        return 0;
    return static_cast<int>(stats[kStatFrags]);
}

int SlotTeam(void* ent) {
    if (!ent || !Readable(static_cast<char*>(ent) + kEdictClientOfs, sizeof(void*)))
        return -1;
    void* client = *reinterpret_cast<void**>(static_cast<char*>(ent) + kEdictClientOfs);
    if (!client || !Readable(static_cast<char*>(client) + kClTeamOfs, sizeof(int)))
        return -1;
    int t = *reinterpret_cast<int*>(static_cast<char*>(client) + kClTeamOfs);
    if (t == 1)
        return 0;
    if (t == 2)
        return 1;
    return -1;
}

void SlotLiveName(void* ent, char* out, int cap) {
    if (!out || cap <= 0)
        return;
    out[0] = '\0';
    if (!ent || !Readable(static_cast<char*>(ent) + kEdictClientOfs, sizeof(void*)))
        return;
    void* client = *reinterpret_cast<void**>(static_cast<char*>(ent) + kEdictClientOfs);
    if (!client || !Readable(static_cast<char*>(client) + kClUserinfoOfs, 64))
        return;
    const char* userinfo = static_cast<const char*>(client) + kClUserinfoOfs;
    if (!Readable(userinfo, 1) || !InfoKeyReady())
        return;
    const char* v = SOF_EP_Info_ValueForKey(userinfo, "name");
    if (!v || !IsValidUserPointer(v) || !Readable(v, 1))
        return;
    std::size_t len = 0;
    while (len < 32 && v[len])
        ++len;
    if (len == 0)
        return;
    std::size_t n = len >= static_cast<std::size_t>(cap) ? static_cast<std::size_t>(cap - 1) : len;
    for (std::size_t i = 0; i < n; ++i) {
        char c = v[i];
        out[i] = (static_cast<unsigned char>(c) < 32 || c == '"' || c == '\\') ? ' ' : c;
    }
    out[n] = '\0';
}

bool SnapshotPlayers(std::vector<stats::MatchPlayer>* out) {
    if (!out)
        return false;
    out->clear();
    const int n = MgMaxClients();
    for (int slot1 = 1; slot1 <= n && slot1 <= kMgMaxSlots; ++slot1) {
        const int slot0 = slot1 - 1;
        if (!Profiles_SlotActive(slot0))
            continue;  // stats require a signed-in profile; guests are skipped
        char guid[64] = {}, nick[64] = {};
        if (!Profiles_SlotGuid(slot0, guid, sizeof(guid)))
            continue;
        Profiles_SlotNickname(slot0, nick, sizeof(nick));
        void* ent = MgEdictForSlot(slot1);
        if (!ent)
            continue;
        stats::MatchPlayer p;
        p.guid = guid;
        p.nick = nick[0] ? nick : "player";
        char live[40] = {};
        SlotLiveName(ent, live, sizeof(live));
        p.live = live;
        p.team = SlotTeam(ent);
        p.frags = SlotFrags(ent);
        out->push_back(p);
    }
    return !out->empty();
}

// Arms the native pending (the trust signal). Shared by the chat filter
// (which then lets sofplus schedule) and the menu single-tap trigger
// (which queues the sofplus schedule itself).
void ArmNativePending(int slot0, const char* map, const char* via) {
    char guid[64] = {}, nick[64] = {};
    Profiles_SlotGuid(slot0, guid, sizeof(guid));
    Profiles_SlotNickname(slot0, nick, sizeof(nick));
    std::snprintf(g_pending.uploaderGuid, sizeof(g_pending.uploaderGuid), "%s", guid);
    std::snprintf(g_pending.uploaderNick, sizeof(g_pending.uploaderNick), "%s",
                  nick[0] ? nick : "admin");
    std::snprintf(g_pending.map, sizeof(g_pending.map), "%s", map ? map : "unknown");
    g_pending.uploaderSlot0 = slot0;
    g_pending.active = true;
    RefreshOutputs();
    Buddy_DebugPrintf("[stats] %s vouched .upload_match on %s (slot %d, via %s)\n",
                      g_pending.uploaderNick, g_pending.map, slot0, via ? via : "?");
}

void FlushPending(const char* reason) {
    if (!g_pending.active)
        return;
    std::vector<stats::MatchPlayer> players;
    if (!SnapshotPlayers(&players)) {
        Buddy_DebugPrintf("[stats] flush (%s): no signed-in players, dropping pending\n",
                          reason ? reason : "?");
        g_pending = Pending();
        RefreshOutputs();
        return;
    }
    char utc[32] = {}, map[64] = {};
    CurrentUtc(utc, sizeof(utc));
    CurrentMap(map, sizeof(map));
    stats::Match m;
    m.seq = g_seqBase + g_matches + 1;
    m.utc = utc;
    m.map = map;
    m.uploaderGuid = g_pending.uploaderGuid;
    m.uploaderNick = g_pending.uploaderNick;
    m.players = players;
    if (!g_totals.AddMatch(m)) {
        Buddy_DebugPrintf("[stats] flush (%s): AddMatch failed\n", reason ? reason : "?");
        NoteError();
        g_pending = Pending();
        RefreshOutputs();
        return;
    }
    bool okSave = Stats_SaveTotals();
    bool okAppend = Stats_AppendMatch(m);
    if (!okSave || !okAppend)
        NoteError();
    else
        ++g_matches;
    Buddy_DebugPrintf("[stats] recorded match %lld (%s): map %s uploader %s %u players\n",
                      m.seq, reason ? reason : "?", m.map.c_str(), m.uploaderNick.c_str(),
                      static_cast<unsigned>(m.players.size()));
    g_pending = Pending();
    RefreshOutputs();
}

// ---------------------------------------------------------------------------
// Word matching (dot-optional, case-insensitive — same as MgClientCmdMatches).
// ---------------------------------------------------------------------------
bool SameNoCase(const char* a, const char* b) {
    if (!a || !b)
        return false;
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? static_cast<char>(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? static_cast<char>(*b + 32) : *b;
        if (ca != cb)
            return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

bool WordMatches(const char* word, const char* name) {
    if (!word || !word[0] || !name || !name[0])
        return false;
    if (SameNoCase(word, name))
        return true;
    if (word[0] == '.' && SameNoCase(word + 1, name))
        return true;
    return false;
}

// ---------------------------------------------------------------------------
// Minigames detail page (mg_stats, menuOnly — every signed-in tier).
// ---------------------------------------------------------------------------
bool g_registered = false;
bool g_armed[kMgMaxSlots + 1] = {};

void PaintStats(int slot1, bool push) {
    if (slot1 < 1 || slot1 > kMgMaxSlots || !g_armed[slot1])
        return;
    MgCanvas c;
    MgCanvasClear(c);
    MgCanvasPCenter(c, 320, 36, kMgPGreen, "PLAYER STATS");
    char line[128];
    std::string summary =
        stats::FormatTotalsSummary(g_matches + g_seqBase, g_totals.Size());
    std::snprintf(line, sizeof(line), "%s", summary.c_str());
    MgCanvasPCenter(c, 320, 54, kMgPWhite, line);
    if (g_pending.active)
        std::snprintf(line, sizeof(line), "upload pending (map %s)", g_pending.map);
    else
        std::snprintf(line, sizeof(line), "no upload pending");
    MgCanvasPCenter(c, 320, 72, kMgPYellow, line);
    // Top rows by frags (up to 5) — live view of lifetime totals.
    std::vector<std::pair<long long, stats::TotalsEntry>> rows;
    for (const auto& kv : g_totals.Entries())
        rows.push_back({kv.second.frags, kv.second});
    for (std::size_t i = 0; i < rows.size(); ++i)
        for (std::size_t j = i + 1; j < rows.size(); ++j)
            if (rows[j].first > rows[i].first)
                std::swap(rows[i], rows[j]);
    int y = 96;
    const std::size_t show = rows.size() < 5 ? rows.size() : 5;
    for (std::size_t i = 0; i < show; ++i) {
        std::string lifetime = stats::FormatLifetimeStats(rows[i].second.frags,
                                                          rows[i].second.matches);
        std::snprintf(line, sizeof(line), "%.14s - %s", rows[i].second.nick.c_str(),
                      lifetime.c_str());
        if (!MgCanvasPText(c, 24, y, kMgPWhite, line))
            break;
        y += 20;
    }
    if (!Profiles_SlotHideTips(slot1 - 1)) {
        if (Profiles_IsLevel01Admin(slot1 - 1))
            MgCanvasPCenter(c, 320, 284, kMgPGreen,
                            "tap upload match in the list or type .upload_match");
        else
            MgCanvasPCenter(c, 320, 284, kMgPGreen,
                            "ask a server or map admin for .upload_match");
    }
    MgPutLayoutCache(slot1, kStatsGameId, c);
    if (push && MgMinigameTabOpen(slot1))
        MgPushLayout(slot1, kStatsGameId, c);
}

void SetArmed(int slot1, bool on) {
    if (slot1 < 1 || slot1 > kMgMaxSlots)
        return;
    g_armed[slot1] = on;
    if (on) {
        MgTakeDisplay(slot1, kStatsGameId);
        PaintStats(slot1, true);
        return;
    }
    if (MgDisplayOwnedBy(slot1, kStatsGameId))
        MgClearLayout(slot1, kStatsGameId);
}

void OnStatsClientCmd(int slot1) {
    // Detail page for every signed-in tier. The platform already enforces
    // NeedsProfile before calling; the upload action lives on the separate
    // restricted mg_upload row, not here.
    if (!MgEnabled() || slot1 < 1 || slot1 > kMgMaxSlots)
        return;
    const bool on = !g_armed[slot1] || !MgDisplayOwnedBy(slot1, kStatsGameId);
    SetArmed(slot1, on);
}

// Single-tap upload trigger for the mg_upload menu row (privilege 0/1).
// Menu taps are not client commands, so sofplus never sees them: the
// schedule is queued explicitly, mirroring the .func mid-map branch. Chat
// keeps its own fallthrough path. Navigation is closed at intermission, so
// this is schedule-only; at map end the console remains the path.
void Stats_TriggerUpload(int slot1) {
    const int slot0 = slot1 - 1;
    if (slot1 < 1 || slot1 > kMgMaxSlots)
        return;
    void* ent = MgEdictForSlot(slot1);
    if (!Stats_IsEnabled() || !Profiles_IsEnabled()) {
        if (ent)
            Buddy_ClientPrintf(ent, 2, "Stats are switched off.\n");
        return;
    }
    if (!Profiles_IsLevel01Admin(slot0)) {
        if (ent)
            Buddy_ClientPrintf(ent, 2, "Server or map admins only.\n");
        return;
    }
    if (LevelIntermission()) {
        if (ent)
            Buddy_ClientPrintf(ent, 2, "Map has ended - type .upload_match in the console.\n");
        return;
    }
    if (g_pending.active || SofplusCvarNumber("_spf_upload_pending") != 0.0) {
        if (ent)
            Buddy_ClientPrintf(ent, 2,
                               "An upload for this map is already scheduled; only one upload "
                               "per map is allowed.\n");
        return;
    }
    char map[64] = {};
    CurrentMap(map, sizeof(map));
    ArmNativePending(slot0, map, "menu");
    char nick[64] = {};
    Profiles_SlotNickname(slot0, nick, sizeof(nick));
    QueueCbufLines(stats::BuildUploadScheduleLines(nick, slot0));
    if (ent)
        Buddy_ClientPrintf(ent, 2, "Upload scheduled - scoreboard goes to Discord at map end.\n");
}

void OnUploadClientCmd(int slot1) {
    // Action-only row: fire the trigger, take no display. Navigation then
    // rolls back to the minigames list (InvokeGameNoArgs ran the handler).
    if (!MgEnabled() || slot1 < 1 || slot1 > kMgMaxSlots)
        return;
    Stats_TriggerUpload(slot1);
}

void OnStatsSessionEnd(int slot1) {
    if (slot1 >= 1 && slot1 <= kMgMaxSlots)
        g_armed[slot1] = false;
}

constexpr char kUploadGameId[] = "mg_upload";

void StatsTryRegister() {
    if (!MgEnabled() || g_registered)
        return;
    static const MgGameOps kOps = {kStatsGameId, OnStatsClientCmd, nullptr, OnStatsSessionEnd,
                                   "per-player match stats", "stats", true};
    if (!MgRegisterGame(&kOps)) {
        PrintOut(PRINT_BAD, "[stats] MgRegisterGame failed\n");
        return;
    }
    // Restricted action row: one +use tap triggers the upload (no page).
    // A separate word from the chat ".upload_match" so Dispatch never shadows
    // the sofplus dot-function the chat filter falls through to.
    static const MgGameOps kUploadOps = {kUploadGameId, OnUploadClientCmd, nullptr, nullptr,
                                         "record this match (admin)", "upload match", true};
    if (!MgRegisterGame(&kUploadOps)) {
        PrintOut(PRINT_BAD, "[stats] MgRegisterGame upload failed\n");
        return;
    }
    g_registered = true;
}

// ---------------------------------------------------------------------------
// Console command registration (repoint-safe, same as profiles).
// ---------------------------------------------------------------------------
void** CmdFunctionsHead();
void* FindCmdNode(const char* name);
void InstallCommand(const char* name, void* fn);

HMODULE ExeModForCmd() {
    if (HMODULE h = GetModuleHandleA("SoF.exe"))
        return h;
    if (HMODULE h = GetModuleHandleA("SoF-spsv.exe"))
        return h;
    return GetModuleHandleA(nullptr);
}

void** CmdFunctionsHead() {
    HMODULE h = ExeModForCmd();
    if (!h)
        return nullptr;
    constexpr unsigned kRvaCmdFunctions = 0x241840;
    return reinterpret_cast<void**>(reinterpret_cast<char*>(h) + kRvaCmdFunctions);
}

void* FindCmdNode(const char* name) {
    void** head = CmdFunctionsHead();
    if (!head)
        return nullptr;
    for (void* node = *head; node; node = *reinterpret_cast<void**>(node)) {
        const char* n = *reinterpret_cast<const char**>(static_cast<char*>(node) + 0x04);
        if (n && std::strcmp(n, name) == 0)
            return node;
    }
    return nullptr;
}

void InstallCommand(const char* name, void* fn) {
    void* node = FindCmdNode(name);
    if (node) {
        *reinterpret_cast<void**>(static_cast<char*>(node) + 0x08) = fn;
        return;
    }
    SOF_EP_Cmd_AddCommand(const_cast<char*>(name), fn);
}

bool EngineCmdReady() {
    return detour_Cmd_Argc::oCmd_Argc && detour_Cmd_Argv::oCmd_Argv &&
           !IsBadReadPtr(reinterpret_cast<void*>(detour_Cmd_Argc::oCmd_Argc), 1) &&
           !IsBadReadPtr(reinterpret_cast<void*>(detour_Cmd_Argv::oCmd_Argv), 1);
}

void ConsolePrint(const char* fmt, ...) {
    if (!fmt)
        return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Buddy_ClientPrintf(nullptr, 2, "%s", buf);
}

extern "C" void __cdecl stats_Status_f();
extern "C" void __cdecl stats_List_f();

extern "C" void __cdecl stats_Status_f() {
    if (!Stats_IsEnabled()) {
        Buddy_DebugPrintf("stats is disabled (_sofbuddy_stats 0)\n");
        return;
    }
    ConsolePrint("[stats] %lld matches, %u players, pending=%d\n", g_matches + g_seqBase,
                 static_cast<unsigned>(g_totals.Size()), g_pending.active ? 1 : 0);
    if (g_pending.active)
        ConsolePrint("[stats] pending: map %s uploader %s\n", g_pending.map,
                     g_pending.uploaderNick);
}

extern "C" void __cdecl stats_List_f() {
    if (!Stats_IsEnabled())
        return;
    int limit = 10;
    if (EngineCmdReady() && SOF_EP_Cmd_Argc() >= 2)
        limit = std::atoi(SOF_EP_Cmd_Argv(1));
    if (limit <= 0)
        limit = 10;
    if (limit > 32)
        limit = 32;
    std::vector<stats::TotalsEntry> rows;
    for (const auto& kv : g_totals.Entries())
        rows.push_back(kv.second);
    for (std::size_t i = 0; i < rows.size(); ++i)
        for (std::size_t j = i + 1; j < rows.size(); ++j)
            if (rows[j].frags > rows[i].frags)
                std::swap(rows[i], rows[j]);
    ConsolePrint("[stats] top %d/%u players:\n", limit, static_cast<unsigned>(rows.size()));
    for (int i = 0; i < limit && i < static_cast<int>(rows.size()); ++i)
        ConsolePrint("  %s  %lld frags  %lld matches  last %s %s\n", rows[i].nick.c_str(),
                     rows[i].frags, rows[i].matches, rows[i].lastMap.c_str(),
                     rows[i].lastSeen.c_str());
}

}  // namespace

// ---------------------------------------------------------------------------
// Public seam (stats.h).
// ---------------------------------------------------------------------------
bool Stats_IsEnabled() {
    if (!g_cvEnabled)
        return true;
    return Buddy_ReadCvarValue(g_cvEnabled, 1.0f) != 0.0f;
}

bool Stats_LifetimeForSlot(int slot0, long long* matches, long long* frags) {
    if (matches)
        *matches = 0;
    if (frags)
        *frags = 0;
    if (!Stats_IsEnabled() || slot0 < 0 || slot0 >= kMgMaxSlots)
        return false;
    char guid[64] = {};
    if (!Profiles_SlotGuid(slot0, guid, sizeof(guid)))
        return false;
    stats::TotalsEntry e;
    if (!g_totals.Lookup(guid, &e))
        return false;
    if (matches)
        *matches = e.matches;
    if (frags)
        *frags = e.frags;
    return true;
}

void Stats_Summary(long long* matches, long long* players) {
    if (matches)
        *matches = Stats_IsEnabled() ? (g_matches + g_seqBase) : 0;
    if (players)
        *players =
            Stats_IsEnabled() ? static_cast<long long>(g_totals.Size()) : 0;
}

void stats_EnsureRegistered() { StatsTryRegister(); }

void stats_MaintainForSlot(int slot1) {
    if (slot1 < 1 || slot1 > kMgMaxSlots || !g_armed[slot1])
        return;
    if (!Profiles_SlotActive(slot1 - 1)) {
        SetArmed(slot1, false);
        return;
    }
    if (!MgEnabled() || !MgMinigameTabOpen(slot1))
        return;
    if (MgDisplayTakenByOther(slot1, kStatsGameId))
        return;
    if (!MgDisplayOwnedBy(slot1, kStatsGameId))
        MgTakeDisplay(slot1, kStatsGameId);
    if (!MgRunningSession(kStatsGameId))
        return;
    PaintStats(slot1, false);
}

void stats_OnGameDllLoaded(void* gameExport) {
    (void)gameExport;
    Stats_InitCvars();
    Stats_LoadTotals();
    RefreshOutputs();
    if (!detour_Cmd_AddCommand::oCmd_AddCommand ||
        IsBadReadPtr(reinterpret_cast<void*>(detour_Cmd_AddCommand::oCmd_AddCommand), 1)) {
        PrintOut(PRINT_BAD, "[stats] Cmd_AddCommand unavailable - commands not registered\n");
        return;
    }
    InstallCommand("stats_status", reinterpret_cast<void*>(&stats_Status_f));
    InstallCommand("stats_list", reinterpret_cast<void*>(&stats_List_f));
    StatsTryRegister();
    PrintOut(PRINT_LOG, "[stats] ready (%u players, %lld matches)\n",
             static_cast<unsigned>(g_totals.Size()), g_matches + g_seqBase);
}

void stats_SvFramePost(int msec) {
    (void)msec;
    if (!Stats_IsEnabled() || !g_pending.active)
        return;
    // Stale map (admin forced a map change mid-pending): drop, don't record.
    char cur[64] = {};
    CurrentMap(cur, sizeof(cur));
    if (std::strcmp(cur, g_pending.map) != 0) {
        Buddy_DebugPrintf("[stats] map changed (%s -> %s), dropping pending upload\n",
                          g_pending.map, cur);
        g_pending = Pending();
        RefreshOutputs();
        return;
    }
    if (LevelIntermission())
        FlushPending("map_end");
}

bool Stats_HandleClientCommand(void* ent, int slot1, const char* cmd, int splitN,
                               const char* const* words) {
    // Resolve the effective word: say-box ".upload_match" arrives as one
    // argument (words[0]); direct console arrives as cmd.
    const char* word = nullptr;
    if (splitN >= 1 && words && words[0] && words[0][0])
        word = words[0];
    else if (cmd && cmd[0])
        word = cmd;
    else
        return false;
    const bool isUpload = WordMatches(word, "upload_match");
    const bool isCancel = !isUpload && WordMatches(word, "cancel_upload");
    if (!isUpload && !isCancel)
        return false;
    if (!Stats_IsEnabled() || !Profiles_IsEnabled())
        return false;  // stock sofplus behaviour when switched off
    if (slot1 < 1 || slot1 > kMgMaxSlots || !ent) {
        if (ent)
            Buddy_ClientPrintf(ent, 2, "Cannot resolve your slot.\n");
        return true;
    }
    const int slot0 = slot1 - 1;
    if (!Profiles_IsLevel01Admin(slot0)) {
        // Private error (cprintf to caller only) so the attempt never reaches
        // chat — same pattern as .login credential handling.
        Buddy_ClientPrintf(ent, 2, "Only signed-in server or map admins can use .%s\n",
                           isUpload ? "upload_match" : "cancel_upload");
        if (!Profiles_SlotActive(slot0))
            Buddy_ClientPrintf(ent, 2, "Sign in first: .login <number>\n");
        return true;  // consume: sofplus must not see it
    }
    if (isCancel) {
        if (g_pending.active) {
            Buddy_DebugPrintf("[stats] %s cancelled pending upload (map %s)\n",
                              g_pending.uploaderNick, g_pending.map);
            g_pending = Pending();
            RefreshOutputs();
        }
        return false;  // let sofplus clear its own pending + message
    }
    // .upload_match from an admin: the trust signal.
    char map[64] = {};
    CurrentMap(map, sizeof(map));
    if (g_pending.active) {
        Buddy_ClientPrintf(ent, 2,
                           "An upload for this map is already scheduled; only one upload per "
                           "map is allowed.\n");
        return true;  // consume duplicate; sofplus would say the same twice
    }
    ArmNativePending(slot0, map, "chat");
    if (LevelIntermission()) {
        // Already at map end (mirrors the .func immediate branch): the
        // scoreboard is final now, so record synchronously instead of waiting
        // for the next SV_Frame.
        FlushPending("intermission");
    }
    return false;  // let sofplus run its .upload_match (schedule + discord)
}
