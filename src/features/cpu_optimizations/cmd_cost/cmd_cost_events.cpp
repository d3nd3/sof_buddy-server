// sofbuddy_cmdcost_events — live _sp_sv_on_* and _sp_sc_on_change_* handlers

#include "buddy_import.h"
#include "func_analyze.h"
#include "generated_engine_pointers.h"
#include "log.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
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

constexpr unsigned kRvaCvarVars   = 0x24B1D8;
constexpr unsigned kCvarNameOfs = 0x00;
constexpr unsigned kCvarStrOfs  = 0x04;
constexpr unsigned kCvarNextOfs = 0x1C;
constexpr unsigned kCvarSize    = 0x20;

constexpr unsigned kMaxListWalk = 1u << 18;
constexpr unsigned kMaxCStr     = 4096;

struct Bind {
    bool ok = false;
    void** cvarHead = nullptr;
};

Bind g;

static const char* kSvEvents[] = {
    "_sp_sv_on_client_begin", "_sp_sv_on_client_die", "_sp_sv_on_client_disconnect",
    "_sp_sv_on_client_spawn", "_sp_sv_on_client_userinfo_change",
    "_sp_sv_on_ctf_flag_capture", "_sp_sv_on_map_begin", "_sp_sv_on_map_end",
    "_sp_sv_on_map_rotate", nullptr};

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
    HMODULE exe = ExeMod();
    if (!exe || !SpsvMod()) return false;
    if (!ModRva(exe, kRvaCvarVars, sizeof(void*))) return false;
    g.cvarHead = reinterpret_cast<void**>(reinterpret_cast<char*>(exe) + kRvaCvarVars);
    if (!SafePtr(g.cvarHead, sizeof(void*))) return false;
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

static bool Starts(const char* s, const char* p) {
    return s && p && !std::strncmp(s, p, std::strlen(p));
}

static std::string Trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

static void SplitCsv(const char* s, std::vector<std::string>& out) {
    if (!s || !s[0]) return;
    std::string cur;
    for (const char* p = s; *p; ++p) {
        if (*p == ',') {
            std::string t = Trim(cur);
            if (!t.empty()) out.push_back(t);
            cur.clear();
        } else
            cur += *p;
    }
    std::string t = Trim(cur);
    if (!t.empty()) out.push_back(t);
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

static FuncFiles BuildFuncIndex() {
    FuncFiles out;
    std::string base = LookupCvar("base");
    if (base.empty()) base = "base";
    std::string user = LookupCvar("user");
    if (user.empty()) user = "User";
    std::vector<std::string> baseDirs, userDirs;
    PushAddonRoots(baseDirs, base);
    PushAddonRoots(userDirs, user);
    for (const std::string& d : baseDirs) MergeDir(out, d.c_str(), FuncRoot::kBase);
    for (const std::string& d : userDirs) MergeDir(out, d.c_str(), FuncRoot::kUser);
    return out;
}

static std::map<std::string, std::vector<std::string>> CollectEvents() {
    std::map<std::string, std::vector<std::string>> ev;
    for (int i = 0; kSvEvents[i]; ++i) ev[kSvEvents[i]] = {};
    if (!g.cvarHead || !SafePtr(g.cvarHead, sizeof(void*))) return ev;
    void* head = *g.cvarHead;
    unsigned walked = 0;
    for (void* cv = head; cv; cv = *reinterpret_cast<void**>(static_cast<char*>(cv) + kCvarNextOfs)) {
        if (++walked > kMaxListWalk) break;
        if (!SafePtr(cv, kCvarSize)) break;
        const char* namep = *reinterpret_cast<const char**>(static_cast<char*>(cv) + kCvarNameOfs);
        std::string name;
        if (!CopyCStr(namep, name, 128) || name.empty() || name[0] == '~') continue;
        if (!Starts(name.c_str(), "_sp_sv_on_") && !Starts(name.c_str(), "_sp_sc_on_change_")) continue;
        const char* valp = *reinterpret_cast<const char**>(static_cast<char*>(cv) + kCvarStrOfs);
        std::string val;
        CopyCStr(valp, val);
        std::vector<std::string> list;
        SplitCsv(val.c_str(), list);
        ev[name] = list;
    }
    return ev;
}

#else
static FuncFiles BuildFuncIndex() { return {}; }
static std::map<std::string, std::vector<std::string>> CollectEvents() { return {}; }
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

static const char* EventLabel(const std::string& name) {
    if (Starts(name.c_str(), "_sp_sv_on_")) return name.c_str() + 10;
    if (Starts(name.c_str(), "_sp_sc_on_change_")) return name.c_str() + 17;
    return name.c_str();
}

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

static void SayFileTag(const FuncFileRef* ref) {
    if (!ref || ref->file.empty()) return;
    if (ref->root == FuncRoot::kUser)
        Say("  " P_BLUE "(" P_PURPLE "%s" P_BLUE " " P_CYAN "U" P_BLUE ")" P_WHITE, ref->file.c_str());
    else
        Say("  " P_BLUE "(" P_PURPLE "%s" P_BLUE " " P_YELLOW "B" P_BLUE ")" P_WHITE, ref->file.c_str());
}

static void PrintHandlers(const std::vector<std::string>& handlers, const FuncFiles& idx) {
    if (handlers.empty()) {
        Say(P_BLUE "    (none)" P_WHITE "\n");
        return;
    }
    for (const std::string& h : handlers) {
        Say("    " P_CYAN "%s()" P_WHITE, HandlerFuncName(h).c_str());
        SayFileTag(FuncLookup(idx, HandlerFuncName(h)));
        Say("\n");
    }
}

static void PrintEventBlock(const char* title, const std::map<std::string, std::vector<std::string>>& ev,
                            bool onChange, const FuncFiles& idx) {
    int slots = 0, handlers = 0;
    for (const auto& kv : ev) {
        if (onChange != Starts(kv.first.c_str(), "_sp_sc_on_change_")) continue;
        ++slots;
        handlers += static_cast<int>(kv.second.size());
    }
    Say("\n" P_YELLOW "%s" P_WHITE "  " P_BLUE "(%d slots, %d handlers)" P_WHITE "\n", title, slots,
        handlers);
    if (!slots) {
        Say(P_BLUE "  (none registered)\n");
        return;
    }
    for (const auto& kv : ev) {
        if (onChange != Starts(kv.first.c_str(), "_sp_sc_on_change_")) continue;
        Say("  " P_GREEN "%s" P_WHITE "\n", EventLabel(kv.first));
        PrintHandlers(kv.second, idx);
    }
}

static void RunEvents() {
    if (!Bind()) {
        Say(P_RED "cmdcost_events: could not read spsv.dll / engine state\n");
        return;
    }
    if (!CanPrint()) {
        PrintOut(PRINT_BAD, "[cmdcost_events] Com_Printf not available\n");
        return;
    }
    const FuncFiles idx = BuildFuncIndex();
    const auto ev = CollectEvents();
    Say(P_CYAN "\n=== sofplus events ===" P_WHITE "\n");
    PrintEventBlock("Server events", ev, false, idx);
    PrintEventBlock("On-change hooks", ev, true, idx);
    Say("\n" P_GREEN "done" P_WHITE "\n\n");
}

}  // namespace

#if defined(_WIN32)
#define EVENTS_CDECL __cdecl
#else
#define EVENTS_CDECL
#endif

extern "C" void EVENTS_CDECL cmdcost_events_f() { RunEvents(); }

void cmdcost_RegisterEventsCmd() {
    if (!detour_Cmd_AddCommand::oCmd_AddCommand) return;
    void* fn = reinterpret_cast<void*>(&cmdcost_events_f);
    SOF_EP_Cmd_AddCommand(const_cast<char*>("sofbuddy_cmdcost_events"), fn);
    SOF_EP_Cmd_AddCommand(const_cast<char*>("events"), fn);
}
