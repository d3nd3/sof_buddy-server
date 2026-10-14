#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace cmdcost {
float PredictMs(const char*) { return 0.f; }
}  // namespace cmdcost

#include "../../../src/features/cpu_optimizations/cmd_cost/func_analyze.cpp"

static std::string ReadFile(const char* path) {
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int g_failures = 0;
#define CHECK(cond, ...)                                                                          \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            ++g_failures;                                                                         \
            std::printf("  FAIL %s:%d: ", __FILE__, __LINE__);                                    \
            std::printf(__VA_ARGS__);                                                             \
            std::printf("\n");                                                                    \
        }                                                                                         \
    } while (0)

int main() {
    const std::string whileCpu = ReadFile("../cmd_cost/fixtures/while_cpu.func");
    addoncost::Opts opt;
    opt.maxwhile = 100000;
    addoncost::Report r = addoncost::AnalyzeSource("while_cpu.func", whileCpu.c_str(), opt);
    CHECK(r.funcs.size() == 1, "expected 1 func, got %zu", r.funcs.size());
    const addoncost::Cost c = r.funcs[0].cost;
    CHECK(c.max > 1000.0, "while_cpu max %.1fms expected >1s", c.max);
    CHECK(c.min == 0.0, "while min should be 0 iterations");

    const char* evSrc =
        "function init() {\n"
        "  sp_sc_func_exec spf_sc_list_add_func _sp_sv_on_map_begin \"on_begin\"\n"
        "}\n"
        "function on_begin() { set x 1; echo hi }\n";
    addoncost::Report ev = addoncost::AnalyzeSource("ev.func", evSrc, opt);
    bool found = false;
    for (const addoncost::EventCost& e : ev.events) {
        if (e.event == "_sp_sv_on_map_begin") {
            found = true;
            CHECK(e.funcs.size() == 1 && e.funcs[0] == "on_begin", "handler list");
            CHECK(e.cost.max > 0.0003, "on_begin max %.6f", e.cost.max);
        }
    }
    CHECK(found, "map_begin event missing");

    const char* ifSrc =
        "function branchy() {\n"
        "  sp_sc_flow_if number val 1 == val 1 { set a 1 } else { set a 1; set b 1 }\n"
        "}\n";
    addoncost::Report br = addoncost::AnalyzeSource("if.func", ifSrc, opt);
    CHECK(br.foldedIfs >= 1, "expected folded literal if, got %d", br.foldedIfs);
    CHECK(br.funcs[0].cost.min == br.funcs[0].cost.max, "folded if min %.4f max %.4f",
          br.funcs[0].cost.min, br.funcs[0].cost.max);

    const char* whileKnown =
        "function reset_loop() {\n"
        "  set wcnt 0\n"
        "  sp_sc_flow_while number cvar wcnt < val 10 {\n"
        "    sp_sc_cvar_math_add wcnt 1\n"
        "  }\n"
        "}\n";
    addoncost::Report wl = addoncost::AnalyzeSource("wl.func", whileKnown, opt);
    CHECK(wl.funcs[0].cost.min > 0.001, "known-start while min %.4f", wl.funcs[0].cost.min);
    CHECK(wl.funcs[0].cost.min == wl.funcs[0].cost.max, "known-start while min %.4f max %.4f",
          wl.funcs[0].cost.min, wl.funcs[0].cost.max);

    auto desc = addoncost::FuncUserCmdDescIndex("fixtures");
    CHECK(desc[".stats"] == "Print stats", ".stats desc '%s'", desc[".stats"].c_str());
    CHECK(desc[".whisper"] == "Whisper to player", ".whisper desc");
    CHECK(!desc.count(".nodesc"), ".nodesc should have no description");

    if (g_failures == 0)
        std::printf("All cmd_cost_addons tests passed.\n");
    else
        std::printf("%d failure(s).\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
