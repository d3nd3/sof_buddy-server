// sofbuddy_cmdcost_usercmds / sofbuddy_cmdcost_timers — live sofplus introspection

#include "buddy_import.h"
#include "func_analyze.h"
#include "generated_engine_pointers.h"
#include "log.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#define GetModuleHandleA(x) ((HMODULE)nullptr)
#endif

namespace {

#define P_WHITE  "\001"
#define P_RED    "\002"
#define P_GREEN  "\003"
#define P_YELLOW "\004"
#define P_BLUE   "\005"
#define P_PURPLE "\006"
#define P_CYAN   "\007"

enum class FuncRoot { kBase, kUser };

struct FuncFileRef {
    std::string file;
    FuncRoot root = FuncRoot::kBase;
};

using FuncFiles = std::unordered_map<std::string, FuncFileRef>;
using UserCmdDescs = std::unordered_map<std::string, std::string>;

constexpr unsigned kRvaSofplusTimer = 0x4F150;
constexpr unsigned kRvaCurtimeSlot  = 0x2F9F4;
constexpr unsigned kRvaFuncRoot     = 0x2F918;

constexpr unsigned kRvaCvarVars   = 0x24B1D8;
constexpr unsigned kCvarNameOfs = 0x00;
constexpr unsigned kCvarStrOfs  = 0x04;
constexpr unsigned kCvarNextOfs = 0x1C;
constexpr unsigned kCvarSize    = 0x20;

constexpr unsigned kTimerSize = 12;

constexpr unsigned kFnBodyOfs = 0x04;
constexpr unsigned kFnNextOfs = 0x20;
constexpr unsigned kFnNameOfs = 0x24;
constexpr unsigned kFnNameLen = 0x28;
constexpr unsigned kFnNodeMin = 0x2C;

// sp_sc_func_list linked-list node (malloc 0x30 in calls_sp_sc_func_exec)
constexpr unsigned kFnListNameOfs   = 0x00;
constexpr unsigned kFnListParamsOfs = 0x08;
constexpr unsigned kFnListParentOfs = 0x0C;
constexpr unsigned kFnListNextOfs   = 0x18;
constexpr unsigned kFnListNodeMin = 0x1C;

constexpr unsigned kMaxListWalk = 1u << 18;
constexpr unsigned kMaxCStr     = 4096;

struct Bind {
    bool ok = false;
    void** timerHead = nullptr;
    int* curtime = nullptr;
    void** funcRoot = nullptr;
    void** cvarHead = nullptr;
};

Bind g;

#if defined(_WIN32)

static bool SafePtr(const void* p, size_t n) {
    if (!p || !n) return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    auto a = reinterpret_cast<uintptr_t>(p);
    auto b = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return a + n <= b;
}

static bool ModRva(HMODULE h, unsigned rva, unsigned sz) {
    if (!h || !sz) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(h);
    if (!SafePtr(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const char*>(dos) + dos->e_lfanew);
    if (!SafePtr(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE) return false;
    return rva + sz <= nt->OptionalHeader.SizeOfImage;
}

static HMODULE ExeMod() {
    if (HMODULE h = GetModuleHandleA("SoF.exe")) return h;
    if (HMODULE h = GetModuleHandleA("SoF-spsv.exe")) return h;
    return GetModuleHandleA(nullptr);
}

static HMODULE SpsvMod() {
    if (HMODULE h = GetModuleHandleA("spsv.dll")) return h;
    if (HMODULE h = GetModuleHandleA("spsv-16.dll")) return h;
    return nullptr;
}

static bool Bind() {
    if (g.ok) return true;
    HMODULE spsv = SpsvMod();
    HMODULE exe = ExeMod();
    if (!spsv || !exe) return false;
    if (!ModRva(spsv, kRvaSofplusTimer, sizeof(void*)) ||
        !ModRva(spsv, kRvaCurtimeSlot, sizeof(void*)) ||
        !ModRva(spsv, kRvaFuncRoot, sizeof(void*)) ||
        !ModRva(exe, kRvaCvarVars, sizeof(void*)))
        return false;
    auto* sb = reinterpret_cast<char*>(spsv);
    auto* eb = reinterpret_cast<char*>(exe);
    g.timerHead = reinterpret_cast<void**>(sb + kRvaSofplusTimer);
    int** ct = reinterpret_cast<int**>(sb + kRvaCurtimeSlot);
    if (SafePtr(ct, sizeof(int*)) && *ct) g.curtime = *ct;
    g.funcRoot = reinterpret_cast<void**>(sb + kRvaFuncRoot);
    g.cvarHead = reinterpret_cast<void**>(eb + kRvaCvarVars);
    if (!SafePtr(g.timerHead, sizeof(void*)) || !SafePtr(g.funcRoot, sizeof(void*)) ||
        !SafePtr(g.cvarHead, sizeof(void*)))
        return false;
    g.ok = true;
    return true;
}

static bool CopyCStr(const char* p, std::string& out, size_t cap = kMaxCStr) {
    out.clear();
    if (!p || !SafePtr(p, 1)) return false;
    for (size_t i = 0; i < cap; ++i) {
        if (!SafePtr(p + i, 1)) break;
        char c = p[i];
        if (!c) return true;
        out += c;
    }
    return !out.empty();
}

#else
static bool Bind() { return false; }
static bool CopyCStr(const char*, std::string&, size_t = kMaxCStr) { return false; }
#endif

static bool CanPrint() {
    return detour_Com_Printf::oCom_Printf != nullptr;
}

#if defined(_WIN32)

static std::string LookupCvar(const char* want) {
    if (!want || !want[0] || !g.cvarHead || !SafePtr(g.cvarHead, sizeof(void*))) return {};
    void* head = *g.cvarHead;
    unsigned walked = 0;
    for (void* cv = head; cv; cv = *reinterpret_cast<void**>(static_cast<char*>(cv) + kCvarNextOfs)) {
        if (++walked > kMaxListWalk) break;
        if (!SafePtr(cv, kCvarSize)) break;
        const char* namep = *reinterpret_cast<const char**>(static_cast<char*>(cv) + kCvarNameOfs);
        std::string name;
        if (!CopyCStr(namep, name, 128) || _stricmp(name.c_str(), want) != 0) continue;
        const char* valp = *reinterpret_cast<const char**>(static_cast<char*>(cv) + kCvarStrOfs);
        std::string val;
        CopyCStr(valp, val, 256);
        return val;
    }
    return {};
}

static std::string JoinAddons(const char* root) {
    if (!root || !root[0]) return {};
    std::string dir = root;
    if (dir.back() != '/' && dir.back() != '\\') dir += '/';
    dir += "sofplus\\addons";
    return dir;
}

static void PushUnique(std::vector<std::string>& v, const std::string& s) {
    if (!s.empty() && std::find(v.begin(), v.end(), s) == v.end()) v.push_back(s);
}

static void PushAddonRoots(std::vector<std::string>& out, const std::string& root) {
    if (root.empty()) return;
    PushUnique(out, JoinAddons(root.c_str()));
    if (root.find('/') == std::string::npos && root.find('\\') == std::string::npos) {
        std::string alt = root;
        char& c = alt[0];
        if (c >= 'A' && c <= 'Z')
            c = char(c + ('a' - 'A'));
        else if (c >= 'a' && c <= 'z')
            c = char(c + ('A' - 'a'));
        if (alt != root) PushUnique(out, JoinAddons(alt.c_str()));
    }
}

static void MergeDir(FuncFiles& out, const char* dir, FuncRoot root) {
    for (auto& kv : addoncost::FuncFileIndex(dir))
        out[kv.first] = {kv.second, root};
}

static void CollectAddonDirs(std::vector<std::string>& baseDirs, std::vector<std::string>& userDirs) {
    std::string base = LookupCvar("base");
    if (base.empty()) base = "base";
    std::string user = LookupCvar("user");
    if (user.empty()) user = "User";
    PushAddonRoots(baseDirs, base);
    PushAddonRoots(userDirs, user);
}

static FuncFiles BuildFuncIndex() {
    FuncFiles out;
    std::vector<std::string> baseDirs, userDirs;
    CollectAddonDirs(baseDirs, userDirs);
    for (const std::string& d : baseDirs) MergeDir(out, d.c_str(), FuncRoot::kBase);
    for (const std::string& d : userDirs) MergeDir(out, d.c_str(), FuncRoot::kUser);
    return out;
}

static UserCmdDescs BuildUserCmdDescIndex() {
    std::vector<std::string> baseDirs, userDirs;
    CollectAddonDirs(baseDirs, userDirs);
    std::vector<const char*> ptrs;
    for (const std::string& d : baseDirs) ptrs.push_back(d.c_str());
    for (const std::string& d : userDirs) ptrs.push_back(d.c_str());
    return addoncost::FuncUserCmdDescIndexMerge(ptrs.data(), ptrs.size());
}

#else
static FuncFiles BuildFuncIndex() { return {}; }
static UserCmdDescs BuildUserCmdDescIndex() { return {}; }
#endif

static void Say(const char* fmt, ...) {
    if (!CanPrint()) return;
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    SOF_EP_Com_Printf("%s", buf);
}

static bool Starts(const char* s, const char* p) {
    return s && p && !std::strncmp(s, p, std::strlen(p));
}

static std::string Trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

static void SayFileTag(const FuncFileRef* ref) {
    if (!ref || ref->file.empty()) return;
    if (ref->root == FuncRoot::kUser)
        Say("  " P_BLUE "(" P_PURPLE "%s" P_BLUE " " P_CYAN "U" P_BLUE ")" P_WHITE, ref->file.c_str());
    else
        Say("  " P_BLUE "(" P_PURPLE "%s" P_BLUE " " P_YELLOW "B" P_BLUE ")" P_WHITE, ref->file.c_str());
}

struct UserCmdRow {
    std::string name;
    std::string params;
};

#if defined(_WIN32)

static std::string FormatParams(char* node) {
    std::vector<std::string> args;
    const char** block = *reinterpret_cast<const char***>(node + kFnListParamsOfs);
    if (!block || !SafePtr(block, sizeof(char*))) return {};
    for (int off = 0; off < 512; off += 4) {
        if (!SafePtr(block + off, sizeof(char*))) break;
        const char* p = *reinterpret_cast<const char**>(reinterpret_cast<char*>(block) + off);
        if (!p || !SafePtr(p, 1)) break;
        std::string one;
        if (!CopyCStr(p, one, 64)) break;
        args.push_back(Trim(one));
    }
    if (!args.empty()) args.erase(args.begin());
    for (auto it = args.begin(); it != args.end(); ++it) {
        if (*it == "*") {
            args.erase(it);
            break;
        }
    }
    std::string out;
    for (const std::string& a : args) {
        if (!out.empty()) out += ", ";
        out += a;
    }
    return out;
}

static std::vector<UserCmdRow> CollectUserCmds() {
    std::vector<UserCmdRow> rows;
    if (!g.funcRoot || !SafePtr(g.funcRoot, sizeof(void*))) return rows;
    char* head = *reinterpret_cast<char**>(g.funcRoot);
    if (!head || !SafePtr(head, kFnListNodeMin)) return rows;
    unsigned walked = 0;
    for (char* node = head; node; node = *reinterpret_cast<char**>(node + kFnListNextOfs)) {
        if (++walked > kMaxListWalk) break;
        if (!SafePtr(node, kFnListNodeMin)) break;
        if (*reinterpret_cast<void**>(node + kFnListParentOfs)) continue;
        const char* namep = *reinterpret_cast<const char**>(node + kFnListNameOfs);
        std::string name;
        if (!CopyCStr(namep, name, 128) || name.empty() || name[0] != '.') continue;
        UserCmdRow r;
        r.name = name;
        r.params = FormatParams(node);
        rows.push_back(r);
    }
    std::sort(rows.begin(), rows.end(),
              [](const UserCmdRow& a, const UserCmdRow& b) { return a.name < b.name; });
    return rows;
}

static const char* FuncBody(const char* name) {
    if (!g.funcRoot || !name || !name[0]) return nullptr;
    if (!SafePtr(g.funcRoot, sizeof(void*))) return nullptr;
    char* outer = *reinterpret_cast<char**>(g.funcRoot);
    if (!outer || !SafePtr(outer, 0x18)) return nullptr;
    char* inner = *reinterpret_cast<char**>(outer + 0x10);
    if (!inner || !SafePtr(inner, 0x18)) return nullptr;
    char* buckets = *reinterpret_cast<char**>(inner);
    int mask = *reinterpret_cast<int*>(inner + 4) - 1;
    if (!buckets || mask < 0 || mask > 65535 || !SafePtr(buckets, 12)) return nullptr;
    char* sent = *reinterpret_cast<char**>(inner + 0x14);
    const size_t nlen = std::strlen(name);
    const int bucketN = mask + 1;
    for (int b = 0; b < bucketN; ++b) {
        if (!SafePtr(buckets + 12 * b, sizeof(char*))) break;
        char* n = *reinterpret_cast<char**>(buckets + 12 * b);
        for (int guard = 0; n && n != sent && guard < 4096; ++guard) {
            if (!SafePtr(n, kFnNodeMin)) break;
            const int nlen2 = *reinterpret_cast<int*>(n + kFnNameLen);
            const char* np = *reinterpret_cast<const char**>(n + kFnNameOfs);
            if (nlen2 >= 0 && nlen2 <= 128 && nlen2 == static_cast<int>(nlen) && np &&
                SafePtr(np, nlen2 + 1) && !std::memcmp(np, name, nlen)) {
                const char* body = *reinterpret_cast<const char**>(n + kFnBodyOfs);
                if (body && SafePtr(body, 1)) return body;
                return nullptr;
            }
            n = *reinterpret_cast<char**>(n + kFnNextOfs);
        }
    }
    return nullptr;
}

#else
static std::vector<UserCmdRow> CollectUserCmds() { return {}; }
static const char* FuncBody(const char*) { return nullptr; }
#endif

static bool ScanTimerPeriod(const std::string& text, int& periodMs) {
    bool found = false;
    size_t pos = 0;
    while ((pos = text.find("sp_sc_timer", pos)) != std::string::npos) {
        pos += 11;
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) ++pos;
        int v = (pos < text.size()) ? std::atoi(text.c_str() + pos) : 0;
        if (v > 0) {
            periodMs = v;
            found = true;
        }
    }
    return found;
}

static bool ExpandExec(const std::string& line, std::string& out) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i < line.size() && line[i] == '!') ++i;
    if (Starts(line.c_str() + i, "sp_sc_func_exec")) {
        i += 15;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        size_t j = i;
        while (j < line.size() && line[j] != ' ' && line[j] != '\t' && line[j] != '\n' && line[j] != '\r')
            ++j;
        if (j == i) return false;
        std::string body;
        if (!CopyCStr(FuncBody(line.substr(i, j - i).c_str()), body)) return false;
        out += body;
        if (!out.empty() && out.back() != '\n') out += '\n';
        return true;
    }
    std::string body;
    if (!CopyCStr(FuncBody(line.c_str() + i), body)) return false;
    out += body;
    if (!out.empty() && out.back() != '\n') out += '\n';
    return true;
}

static bool IsRecursiveTimer(const std::string& cmd, int& periodMs) {
    if (cmd.empty()) return false;
    periodMs = 0;
    if (ScanTimerPeriod(cmd, periodMs)) return true;
    if (cmd.find("sp_sc_func_exec") == std::string::npos) return false;
    std::string expanded;
    for (size_t start = 0; start < cmd.size();) {
        size_t end = cmd.find('\n', start);
        if (end == std::string::npos) end = cmd.size();
        std::string line = Trim(cmd.substr(start, end - start));
        if (!line.empty()) ExpandExec(line, expanded);
        start = end < cmd.size() ? end + 1 : end;
    }
    return !expanded.empty() && ScanTimerPeriod(expanded, periodMs);
}

struct TimerRow {
    int dueMs = 0;
    int fireAt = 0;
    std::string cmd;
    bool recursive = false;
    int periodMs = 0;
};

#if defined(_WIN32)

static std::vector<TimerRow> CollectTimers() {
    std::vector<TimerRow> rows;
    if (!g.timerHead || !SafePtr(g.timerHead, sizeof(void*))) return rows;
    void* head = *g.timerHead;
    const int now = (g.curtime && SafePtr(g.curtime, sizeof(int))) ? *g.curtime : 0;
    unsigned walked = 0;
    for (void* t = head; t; t = *reinterpret_cast<void**>(static_cast<char*>(t) + 8)) {
        if (++walked > kMaxListWalk) break;
        if (!SafePtr(t, kTimerSize)) break;
        TimerRow r;
        r.fireAt = *reinterpret_cast<int*>(t);
        r.dueMs = r.fireAt - now;
        const char* cmd = *reinterpret_cast<const char**>(static_cast<char*>(t) + 4);
        CopyCStr(cmd, r.cmd);
        while (!r.cmd.empty() && (r.cmd.back() == '\n' || r.cmd.back() == '\r')) r.cmd.pop_back();
        r.recursive = IsRecursiveTimer(r.cmd, r.periodMs);
        rows.push_back(r);
    }
    std::sort(rows.begin(), rows.end(),
              [](const TimerRow& a, const TimerRow& b) { return a.fireAt < b.fireAt; });
    return rows;
}

#else
static std::vector<TimerRow> CollectTimers() { return {}; }
#endif

static std::string HandlerFuncName(const std::string& h) {
    std::string t = Trim(h);
    size_t pos = t.find("sp_sc_func_exec");
    if (pos != std::string::npos) {
        size_t i = pos + 15;
        while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) ++i;
        size_t j = i;
        while (j < t.size() && t[j] != ' ' && t[j] != '\t' && t[j] != '"' && t[j] != '\n' && t[j] != '\r')
            ++j;
        if (j > i) return t.substr(i, j - i);
    }
    size_t i = 0;
    if (i < t.size() && t[i] == '!') ++i;
    while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) ++i;
    if (t.size() >= i + 2 && t[i] == '"' && t.back() == '"') return Trim(t.substr(i + 1, t.size() - i - 2));
    return t.substr(i);
}

static const FuncFileRef* FuncLookup(const FuncFiles& idx, const std::string& name) {
    auto it = idx.find(name);
    return (it != idx.end() && !it->second.file.empty()) ? &it->second : nullptr;
}

static void PrintUserCmds(const std::vector<UserCmdRow>& cmds, const FuncFiles& idx,
                          const UserCmdDescs& descs) {
    Say(P_CYAN "\n=== sofplus user commands ===" P_WHITE "  " P_BLUE "(%u registered)" P_WHITE "\n",
        static_cast<unsigned>(cmds.size()));
    if (cmds.empty()) {
        Say(P_BLUE "  (none)\n");
        return;
    }
    for (const UserCmdRow& c : cmds) {
        Say("  " P_CYAN "%s()" P_WHITE, c.name.c_str());
        if (!c.params.empty()) Say("  " P_BLUE "%s" P_WHITE, c.params.c_str());
        SayFileTag(FuncLookup(idx, c.name));
        Say("\n");
        auto dit = descs.find(c.name);
        if (dit != descs.end() && !dit->second.empty())
            Say("      " P_GREEN "%s" P_WHITE "\n", dit->second.c_str());
    }
}

static void PrintTimers(const std::vector<TimerRow>& timers, const FuncFiles& idx) {
    int recursive = 0;
    for (const TimerRow& t : timers)
        if (t.recursive) ++recursive;
    if (recursive)
        Say(P_CYAN "\n=== sofplus timers ===" P_WHITE "  " P_BLUE "(%u queued, " P_PURPLE "%d self-rescheduling" P_BLUE ")" P_WHITE "\n",
            static_cast<unsigned>(timers.size()), recursive);
    else
        Say(P_CYAN "\n=== sofplus timers ===" P_WHITE "  " P_BLUE "(%u queued)" P_WHITE "\n",
            static_cast<unsigned>(timers.size()));
    if (timers.empty()) {
        Say(P_BLUE "  (none)\n");
        return;
    }
    int n = 0;
    for (const TimerRow& t : timers) {
        ++n;
        const int clen = static_cast<int>(std::min(t.cmd.size(), size_t{200}));
        Say("  " P_CYAN "#%d" P_WHITE "  due " P_YELLOW "%d" P_WHITE " ms", n, t.dueMs);
        if (t.recursive)
            Say("  " P_PURPLE "every %d ms" P_WHITE, t.periodMs);
        Say("\n");
        if (clen > 0) {
            Say("      " P_WHITE "%.*s", clen, t.cmd.c_str());
            SayFileTag(FuncLookup(idx, HandlerFuncName(t.cmd)));
            Say("\n");
        }
    }
}

static bool Ready(const char* tag) {
    if (!Bind()) {
        Say(P_RED "%s: could not read spsv.dll / engine state\n", tag);
        return false;
    }
    if (!CanPrint()) {
        PrintOut(PRINT_BAD, "[%s] Com_Printf not available\n", tag);
        return false;
    }
    return true;
}

static void RunUserCmds() {
    if (!Ready("cmdcost_usercmds")) return;
    PrintUserCmds(CollectUserCmds(), BuildFuncIndex(), BuildUserCmdDescIndex());
    Say("\n" P_GREEN "done" P_WHITE "\n\n");
}

static void RunTimers() {
    if (!Ready("cmdcost_timers")) return;
    const FuncFiles idx = BuildFuncIndex();
    PrintTimers(CollectTimers(), idx);
    Say("\n" P_GREEN "done" P_WHITE "\n\n");
}

}  // namespace

#if defined(_WIN32)
#define SOFPLUS_CDECL __cdecl
#else
#define SOFPLUS_CDECL
#endif

extern "C" void SOFPLUS_CDECL cmdcost_usercmds_f() { RunUserCmds(); }
extern "C" void SOFPLUS_CDECL cmdcost_timers_f() { RunTimers(); }

void cmdcost_RegisterSofplusCmds() {
    if (!detour_Cmd_AddCommand::oCmd_AddCommand) return;
    void* usercmds = reinterpret_cast<void*>(&cmdcost_usercmds_f);
    void* timers = reinterpret_cast<void*>(&cmdcost_timers_f);
    SOF_EP_Cmd_AddCommand(const_cast<char*>("sofbuddy_cmdcost_usercmds"), usercmds);
    SOF_EP_Cmd_AddCommand(const_cast<char*>("usercmds"), usercmds);
    SOF_EP_Cmd_AddCommand(const_cast<char*>("sofbuddy_cmdcost_timers"), timers);
    SOF_EP_Cmd_AddCommand(const_cast<char*>("timers"), timers);
}
