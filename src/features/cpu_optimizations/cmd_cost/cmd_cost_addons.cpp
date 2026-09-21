// sofbuddy_cmdcost_addons — static cost model for sofplus/addons/*.func

#include "buddy_import.h"
#include "cmd_cost.h"
#include "func_analyze.h"
#include "generated_engine_pointers.h"
#include "log.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr unsigned kCvarStr = 0x04;

static const char* CvarString(void* cv) {
    if (!cv) return "";
    const char* s = *reinterpret_cast<const char**>(static_cast<char*>(cv) + kCvarStr);
    return (s && s[0]) ? s : "";
}

static std::string AddonsDir() {
    const char* user = CvarString(Buddy_GetEngineCvar("user", "User", 0, nullptr));
    if (!user[0]) user = "User";
    std::string dir = user;
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';
#if defined(_WIN32)
    dir += "sofplus\\addons";
#else
    dir += "sofplus/addons";
#endif
    return dir;
}

static int ArgI(int i, int def) {
    if (!detour_Cmd_Argc::oCmd_Argc || !detour_Cmd_Argv::oCmd_Argv) return def;
    if (SOF_EP_Cmd_Argc() <= i) return def;
    const char* s = SOF_EP_Cmd_Argv(i);
    return (s && s[0]) ? std::atoi(s) : def;
}

static const char* ArgS(int i) {
    if (!detour_Cmd_Argc::oCmd_Argc || !detour_Cmd_Argv::oCmd_Argv) return "";
    if (SOF_EP_Cmd_Argc() <= i) return "";
    const char* s = SOF_EP_Cmd_Argv(i);
    return s ? s : "";
}

static void PrimeAddons(const std::string& dir, const addoncost::Opts& opt) {
    const std::vector<std::string> cmds = addoncost::CollectCommands(dir.c_str(), opt);
    int n = 0;
    for (const std::string& c : cmds) {
        if (c.empty() || c[0] == '.') continue;
        const bool sp = c.size() >= 3 && c[0] == 's' && c[1] == 'p' && c[2] == '_';
        if (!sp && c != "echo" && c != "set") continue;
        cmdcost::Spin(c.c_str(), 0, opt.eventSlot);
        ++n;
    }
    PrintOut(PRINT_LOG, "[cmdcost_addons] primed %d command names (live EMA for analyze)\n", n);
}

static void PrintReport(const addoncost::Report& r, float tickMs) {
    PrintOut(PRINT_LOG, "[cmdcost_addons] files=%d funcs=%zu events=%zu unknown_cmds=%d folded_ifs=%d\n",
             r.files, r.funcs.size(), r.events.size(), r.unknownCmds, r.foldedIfs);
    for (const addoncost::FuncCost& f : r.funcs) {
        if (f.cost.max < 0.0001) continue;
        PrintOut(PRINT_LOG, "[cmdcost_addons] func %-40s min=%.3f avg=%.3f max=%.3f ms  (%s)\n",
                 f.name.c_str(), f.cost.min, f.cost.avg, f.cost.max, f.file.c_str());
    }
    double worst = 0;
    const char* worstEv = "-";
    for (const addoncost::EventCost& e : r.events) {
        if (e.funcs.empty() && e.cost.max < 0.0001) continue;
        std::string list;
        for (size_t i = 0; i < e.funcs.size(); ++i) {
            if (i) list += ",";
            list += e.funcs[i];
        }
        PrintOut(PRINT_LOG, "[cmdcost_addons] event %-32s min=%.3f avg=%.3f max=%.3f ms  [%s]\n",
                 e.event.c_str(), e.cost.min, e.cost.avg, e.cost.max, list.c_str());
        if (e.cost.max > worst) {
            worst = e.cost.max;
            worstEv = e.event.c_str();
        }
    }
    if (worst > 0)
        PrintOut(PRINT_LOG, "[cmdcost_addons] tick=%.0fms worst_event=%s max=%.3fms (%.0f%%)\n",
                 tickMs, worstEv, worst, 100.0 * worst / tickMs);
    PrintOut(PRINT_LOG, "[cmdcost_addons] done (live cmdcost EMA when sampled; else bench fallbacks)\n");
}

static void RunAnalyze() {
    const std::string dir = AddonsDir();
    bool prime = !_stricmp(ArgS(1), "prime");
    const int base = prime ? 2 : 1;
    addoncost::Opts opt;
    opt.eventSlot = ArgI(base, 0);
    opt.maxclients = ArgI(base + 1, 12);
    opt.maxwhile = ArgI(base + 2, 100000);
    opt.defaultWhile = ArgI(base + 3, 64);
    const float tickMs = static_cast<float>(ArgI(base + 4, 100));
    PrintOut(PRINT_LOG, "[cmdcost_addons] scan %s slot=%d maxclients=%d\n", dir.c_str(),
             opt.eventSlot, opt.maxclients);
    if (prime) PrimeAddons(dir, opt);
    addoncost::Report r = addoncost::AnalyzeDir(dir.c_str(), opt);
    if (!r.files) {
        PrintOut(PRINT_BAD, "[cmdcost_addons] no .func files in %s\n", dir.c_str());
        return;
    }
    PrintReport(r, tickMs > 0.f ? tickMs : 100.f);
}

}  // namespace

#if defined(_WIN32)
#define ADDONS_CDECL __cdecl
#else
#define ADDONS_CDECL
#endif

extern "C" void ADDONS_CDECL cmdcost_addons_f() { RunAnalyze(); }

void cmdcost_RegisterAddonsCmd() {
    if (!detour_Cmd_AddCommand::oCmd_AddCommand) return;
    SOF_EP_Cmd_AddCommand(const_cast<char*>("sofbuddy_cmdcost_addons"),
                          reinterpret_cast<void*>(&cmdcost_addons_f));
}
