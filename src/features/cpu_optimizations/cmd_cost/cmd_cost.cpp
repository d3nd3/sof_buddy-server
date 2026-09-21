// Per-handler wall time. IDA (SoF.exe): Cmd_ExecuteString @0x200194F0 walks
// cmd_functions and `call dword ptr [esi+8]` @0x2001955E. We swap that slot
// so timing excludes tokenize + list scan. Cheap handlers are measured by
// spinning the original until ~2ms of QPC elapses, then storing per-call ms.

#include "cvar.h"
#include "cmd_cost.h"
#include "generated_cmd_catalog.h"
#include "generated_engine_pointers.h"
#include "log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <windows.h>

#if !defined(_WIN32)
#include <strings.h>
#define _stricmp strcasecmp
#define GetModuleHandleA(x) ((HMODULE)nullptr)
#endif

namespace cmdcost {

constexpr int kNeed = 4;
constexpr int kMaxWrap = 384;
constexpr unsigned kRvaCmdFunctions = 0x241840;
constexpr unsigned kRvaCmdArgv      = 0x23F2E0;
constexpr unsigned kRvaTokenize     = 0x18FC0;  // TokenizeString @0x20018FC0
constexpr unsigned kRvaCbufAddText  = 0x18180;  // Cbuf_AddText @0x20018180
constexpr unsigned kCmdNextOfs = 0x00;
constexpr unsigned kCmdNameOfs = 0x04;
constexpr unsigned kCmdFuncOfs = 0x08;
constexpr double kSpinTargetMs = 32.0;  // > Wine QPC quantum (~15.6ms)
constexpr int kSpinMaxN = 1000000;

using CmdFn = void (*)();

struct Clock {
    double scale = 0;
    double quantumMs = 16.0;  // measured floor for meaningful batch totals
    bool ready = false;
    void Init() {
        LARGE_INTEGER f;
        if (QueryPerformanceFrequency(&f) && f.QuadPart > 0) {
            scale = 1000.0 / static_cast<double>(f.QuadPart);
            ready = true;
        }
        if (!ready) return;
        // Estimate counter quantum (Wine often ~15.6ms).
        double worst = 0;
        for (int i = 0; i < 64; ++i) {
            const double a = Now();
            double b = a;
            for (int spin = 0; spin < 1000000 && b <= a; ++spin) b = Now();
            const double d = b - a;
            if (d > worst) worst = d;
        }
        if (worst > 0.1) quantumMs = worst;
    }
    double Now() const {
        LARGE_INTEGER c;
        if (!QueryPerformanceCounter(&c)) return 0;
        return static_cast<double>(c.QuadPart) * scale;
    }
};

struct Slot {
    int n = 0;
    double sum = 0, mx = 0, ema = 0;
};

struct Wrap {
    char name[32] = {};
    CmdFn orig = nullptr;
    void* node = nullptr;
};

struct State {
    Clock clock;
    bool ready = false;
    int depth = 0;
    double start[8] = {};
    char tok[8][32] = {};
    Slot slot[kCmdCount + 1] = {};
    double maxMs = 0;
    int maxIdx = -1;
    int notes = 0;
    int looks = 0;
    Wrap wrap[kMaxWrap] = {};
    int wraps = 0;
    bool patched = false;
    void** cmdHead = nullptr;
    char** cmdArgv = nullptr;
    void (*tokenize)(char*, int) = nullptr;
    void (*addText)(char*) = nullptr;
};

State g;

extern "C" void TimedCmd();

int Find(const char* tok) {
    int lo = 0, hi = kCmdCount;
    while (lo < hi) {
        const int mid = (lo + hi) >> 1;
        const int c = std::strcmp(tok, CmdName(mid));
        if (c == 0) return mid;
        if (c < 0) hi = mid;
        else lo = mid + 1;
    }
    return kCmdCount;
}

void CopyTok(char* dst, const char* s) {
    while (s && (*s == ' ' || *s == '\t')) ++s;
    int i = 0;
    if (s)
        while (s[i] && s[i] != ' ' && s[i] != '\t' && s[i] != ';' && s[i] != '\n' && i < 31)
            dst[i] = s[i], ++i;
    if (!i) { dst[0] = '?'; dst[1] = 0; return; }
    dst[i] = 0;
}

const char* SkipTok(const char* s) {
    while (s && (*s == ' ' || *s == '\t')) ++s;
    while (s && *s && *s != ' ' && *s != '\t' && *s != ';' && *s != '\n') ++s;
    return s;
}

const char* Name(int i) { return i == kCmdCount ? "?" : CmdName(i); }

int HottestEma() {
    int h = kCmdCount;
    for (int i = 0; i < kCmdCount; ++i)
        if (g.slot[i].ema > g.slot[h].ema) h = i;
    return h;
}

void Note(const char* tok, double ms) {
    if (ms < 0) ms = 0;
    const int i = Find(tok);
    Slot& s = g.slot[i];
    s.n++;
    s.sum += ms;
    if (ms > s.mx) s.mx = ms;
    s.ema = s.n == 1 ? ms : s.ema + 0.125 * (ms - s.ema);
    if (ms > g.maxMs) { g.maxMs = ms; g.maxIdx = i; }
    ++g.notes;
    if (ms >= g.maxMs || (g.notes & 63) == 0) {
        const int h = HottestEma();
        Publish(static_cast<float>(g.maxMs), Name(g.maxIdx),
                static_cast<float>(g.slot[h].ema), Name(h), s.n);
    }
}

float PredictMs(const char* text) {
    if (!g.ready || !text) return 0;
    char tok[32];
    CopyTok(tok, text);
    const int i = Find(tok);
    return g.slot[i].n >= kNeed ? static_cast<float>(g.slot[i].ema) : 0.0f;
}

CmdFn FindOrig(const char* name) {
    if (!name) return nullptr;
    for (int i = 0; i < g.wraps; ++i)
        if (!_stricmp(g.wrap[i].name, name))
            return g.wrap[i].orig;
    return nullptr;
}

void InstallOrig(const char* name, void (*fn)()) {
    if (!name || !fn) return;
    for (int i = 0; i < g.wraps; ++i) {
        if (!_stricmp(g.wrap[i].name, name)) {
            g.wrap[i].orig = fn;
            return;
        }
    }
    if (g.wraps >= kMaxWrap) return;
    Wrap& w = g.wrap[g.wraps++];
    CopyTok(w.name, name);
    w.orig = fn;
    w.node = nullptr;
}

bool SpinSkip(const char* n) {
    // Engine session-breakers only. Sofplus mutate/list cmds use SpinCap (N=1..4).
    static const char* k[] = {
        "map", "gamemap", "connect", "exec", "wait", "screenshot", "tgashot",
        "winstart", "cmdlist", "alias", "console", "sv_maplist", "sv_maplistfile",
        "status", "serverstatus", nullptr
    };
    for (int i = 0; k[i]; ++i)
        if (!std::strcmp(n, k[i])) return true;
    return n[0] == '.';  // player .COMMAND — client-side vote path
}

// Caps: flooders hard-limited; mutate/exec soft-capped; 0 = full adaptive.
int SpinCap(const char* n) {
    if (!std::strcmp(n, "sp_sv_print_client") || !std::strcmp(n, "sp_sv_print_broadcast") ||
        !std::strcmp(n, "sp_sv_sound_client") || !std::strcmp(n, "sp_sv_sound_broadcast"))
        return 2;
    if (!std::strcmp(n, "sp_sv_players")) return 8;
    if (!std::strcmp(n, "sp_sv_client_cvar_set") || !std::strcmp(n, "sp_sv_client_check"))
        return 256;
    if (!std::strcmp(n, "sp_sv_info_client") || !std::strcmp(n, "sp_sv_info_client_mod"))
        return 10000;
    if (!std::strcmp(n, "sp_sc_timer") || !std::strcmp(n, "sp_sc_on_change")) return 64;
    if (!std::strcmp(n, "sp_sv_client_blue") || !std::strcmp(n, "sp_sv_client_red") ||
        !std::strcmp(n, "sp_sv_client_spec") || !std::strcmp(n, "sp_sv_client_play") ||
        !std::strcmp(n, "sp_sv_client_swap") || !std::strcmp(n, "sp_sv_say_mute") ||
        !std::strcmp(n, "sp_sv_say_unmute"))
        return 64;
    if (!std::strcmp(n, "sp_sc_cvar_save") || !std::strcmp(n, "sp_sc_file_find"))
        return 256;
    if (!std::strcmp(n, "sp_sc_exec_cvar") || !std::strcmp(n, "sp_sc_exec_file") ||
        !std::strcmp(n, "sp_sc_flow_while") || !std::strcmp(n, "sp_sc_flow_if"))
        return 32;
    if (!std::strcmp(n, "sp_sc_func_exec") || !std::strcmp(n, "sp_sc_func_load_file") ||
        !std::strcmp(n, "sp_sc_func_load_cvar"))
        return 128;
    return 0;  // full adaptive until QPC resolves
}

void Dump() {
    PrintOut(PRINT_LOG, "[cmdcost] begin origs=%d patched=%d\n", g.wraps, g.patched ? 1 : 0);
    int rows = 0;
    for (int i = 0; i <= kCmdCount; ++i) {
        const Slot& s = g.slot[i];
        if (!s.n) continue;
        const double avg = s.sum / static_cast<double>(s.n);
        PrintOut(PRINT_LOG, "[cmdcost] %s %d %.6f %.6f %.6f %.3f\n",
                 Name(i), s.n, s.mx, s.ema, avg, s.ema * 1000.0);
        ++rows;
    }
    PrintOut(PRINT_LOG, "[cmdcost] end rows=%d max=%.6f %s\n",
             rows, g.maxMs, g.maxIdx < 0 ? "-" : Name(g.maxIdx));
}

void Reset() {
    for (int i = 0; i <= kCmdCount; ++i)
        g.slot[i] = Slot{};
    g.maxMs = 0;
    g.maxIdx = -1;
    g.notes = 0;
    Publish(0, "-", 0, "-", 0);
}

void BindEngine() {
    if (g.cmdHead) return;
    HMODULE exe = GetModuleHandleA("SoF.exe");
    if (!exe) exe = GetModuleHandleA("SoF-spsv.exe");
    if (!exe) return;
    auto* base = reinterpret_cast<char*>(exe);
    g.cmdHead = reinterpret_cast<void**>(base + kRvaCmdFunctions);
    g.cmdArgv = reinterpret_cast<char**>(base + kRvaCmdArgv);
    g.tokenize = reinterpret_cast<void (*)(char*, int)>(base + kRvaTokenize);
    g.addText = reinterpret_cast<void (*)(char*)>(base + kRvaCbufAddText);
}

void TouchNode(void* node, bool patch) {
    if (!node) return;
    auto* fnp = reinterpret_cast<CmdFn*>(static_cast<char*>(node) + kCmdFuncOfs);
    CmdFn fn = *fnp;
    if (fn == &TimedCmd) {
        for (int i = 0; i < g.wraps; ++i) {
            if (g.wrap[i].node == node) {
                fn = g.wrap[i].orig;
                break;
            }
        }
    }
    if (!fn) return;
    const char* name = *reinterpret_cast<char**>(static_cast<char*>(node) + kCmdNameOfs);
    if (!name || !name[0]) return;
    if (!std::strncmp(name, "sofbuddy_cmdcost", 16)) return;
    for (int i = 0; i < g.wraps; ++i) {
        if (g.wrap[i].node == node) {
            g.wrap[i].orig = fn;
            if (patch) *fnp = &TimedCmd;
            return;
        }
        if (!_stricmp(g.wrap[i].name, name)) {
            g.wrap[i].node = node;
            g.wrap[i].orig = fn;
            if (patch) *fnp = &TimedCmd;
            return;
        }
    }
    if (g.wraps >= kMaxWrap) return;
    Wrap& w = g.wrap[g.wraps++];
    CopyTok(w.name, name);
    w.orig = fn;
    w.node = node;
    if (patch) *fnp = &TimedCmd;
}

void SyncOrigs() {
    BindEngine();
    if (!g.cmdHead || !*g.cmdHead) return;
    for (void* n = *g.cmdHead; n; n = *reinterpret_cast<void**>(static_cast<char*>(n) + kCmdNextOfs))
        TouchNode(n, false);
}

void PatchAll() {
    for (int i = 0; i < g.wraps; ++i) {
        if (!g.wrap[i].node || !g.wrap[i].orig) continue;
        *reinterpret_cast<CmdFn*>(static_cast<char*>(g.wrap[i].node) + kCmdFuncOfs) = &TimedCmd;
    }
    g.patched = g.wraps > 0;
}

void WrapAll() {
    SyncOrigs();
    PatchAll();
}

void UnwrapAll() {
    for (int i = 0; i < g.wraps; ++i) {
        if (!g.wrap[i].node || !g.wrap[i].orig) continue;
        *reinterpret_cast<CmdFn*>(static_cast<char*>(g.wrap[i].node) + kCmdFuncOfs) = g.wrap[i].orig;
    }
    g.patched = false;
}

void UpdateWrapState() {
    if (!g.ready) return;
    if (Enabled()) {
        if (!g.patched) WrapAll();
        else if ((++g.looks & 255) == 0) WrapAll();
    } else if (g.patched) {
        UnwrapAll();
    }
}

char g_spinLine[192];
int g_spinStrLen = 0;   // _cmdcost_blob length
int g_spinPiece = 1;    // append piece length (1..64)

void BuildBlob(int len) {
    SyncOrigs();
    if (!g.tokenize) return;
    if (len < 0) len = 0;
    // Softplus/engine cvars are short — overflowing the string buffer crashes.
    if (len > 256) len = 256;
    char line[96];
    CmdFn setfn = FindOrig("set");
    CmdFn append = FindOrig("sp_sc_cvar_append");
    CmdFn hex = FindOrig("sp_sc_cvar_hex");
    if (setfn) {
        std::snprintf(line, sizeof(line), "set _cmdcost_blob \"\"");
        g.tokenize(line, 1);
        setfn();
        std::snprintf(line, sizeof(line), "set _cmdcost_dst \"\"");
        g.tokenize(line, 1);
        setfn();
        std::snprintf(line, sizeof(line), "set _cmdcost_hex \"\"");
        g.tokenize(line, 1);
        setfn();
    }
    if (!append) {
        g_spinStrLen = 0;
        PrintOut(PRINT_LOG, "[cmdcost] blob: no append\n");
        return;
    }
    constexpr int kChunkLen = 32;
    for (int left = len; left > 0; ) {
        const int n = left < kChunkLen ? left : kChunkLen;
        char piece[40];
        for (int i = 0; i < n; ++i) piece[i] = 'A';
        piece[n] = 0;
        std::snprintf(line, sizeof(line), "sp_sc_cvar_append _cmdcost_blob %s", piece);
        g.tokenize(line, 1);
        append();
        left -= n;
    }
    if (hex && len > 0 && len <= 128) {
        std::snprintf(line, sizeof(line), "sp_sc_cvar_hex _cmdcost_hex _cmdcost_blob");
        g.tokenize(line, 1);
        hex();
    }
    g_spinStrLen = len;
    g_spinPiece = 1;
    PrintOut(PRINT_LOG, "[cmdcost] blob len=%d\n", len);
}

void PrepDstFromBlob() {
    CmdFn copy = FindOrig("sp_sc_cvar_copy");
    if (!copy || !g.tokenize) return;
    char line[64];
    std::snprintf(line, sizeof(line), "sp_sc_cvar_copy _cmdcost_dst _cmdcost_blob");
    g.tokenize(line, 1);
    copy();
}

const char* SpinLine(const char* n, int slot) {
    // String ops read _cmdcost_blob (fill via sofbuddy_cmdcost_blob <len>).
    if (!std::strcmp(n, "sp_sc_cvar_math_add")) return "sp_sc_cvar_math_add _cmdcost_t 1";
    if (!std::strcmp(n, "sp_sc_cvar_math_sub")) return "sp_sc_cvar_math_sub _cmdcost_t 1";
    if (!std::strcmp(n, "sp_sc_cvar_math_mul")) return "sp_sc_cvar_math_mul _cmdcost_t 1";
    if (!std::strcmp(n, "sp_sc_cvar_math_div")) return "sp_sc_cvar_math_div _cmdcost_t 2";
    if (!std::strcmp(n, "sp_sc_cvar_math_mod")) return "sp_sc_cvar_math_mod _cmdcost_t 3";
    if (!std::strcmp(n, "sp_sc_cvar_math_abs")) return "sp_sc_cvar_math_abs _cmdcost_t";
    if (!std::strcmp(n, "sp_sc_cvar_math_ceil")) return "sp_sc_cvar_math_ceil _cmdcost_t";
    if (!std::strcmp(n, "sp_sc_cvar_math_floor")) return "sp_sc_cvar_math_floor _cmdcost_t";
    if (!std::strcmp(n, "sp_sc_cvar_math_sqrt")) return "sp_sc_cvar_math_sqrt _cmdcost_t";
    if (!std::strcmp(n, "sp_sc_cvar_sset")) return "sp_sc_cvar_sset _cmdcost_t 1";
    if (!std::strcmp(n, "sp_sc_cvar_len")) return "sp_sc_cvar_len _cmdcost_d _cmdcost_blob";
    if (!std::strcmp(n, "sp_sc_cvar_copy")) return "sp_sc_cvar_copy _cmdcost_dst _cmdcost_blob";
    if (!std::strcmp(n, "sp_sc_cvar_copy_latched")) return "sp_sc_cvar_copy_latched _cmdcost_d hostname";
    // append piece length from g_spinPiece (dest pre-sized via PrepDstFromBlob).
    if (!std::strcmp(n, "sp_sc_cvar_append")) {
        int p = g_spinPiece;
        if (p < 1) p = 1;
        if (p > 64) p = 64;
        char piece[68];
        for (int i = 0; i < p; ++i) piece[i] = 'B';
        piece[p] = 0;
        std::snprintf(g_spinLine, sizeof(g_spinLine), "sp_sc_cvar_append _cmdcost_dst %s", piece);
        return g_spinLine;
    }
    if (!std::strcmp(n, "sp_sc_cvar_append_newline")) return "sp_sc_cvar_append_newline _cmdcost_dst";
    if (!std::strcmp(n, "sp_sc_cvar_escape")) return "sp_sc_cvar_escape _cmdcost_dst _cmdcost_blob";
    if (!std::strcmp(n, "sp_sc_cvar_unescape")) return "sp_sc_cvar_unescape _cmdcost_dst _cmdcost_blob";
    if (!std::strcmp(n, "sp_sc_cvar_hex")) return "sp_sc_cvar_hex _cmdcost_dst _cmdcost_blob";
    if (!std::strcmp(n, "sp_sc_cvar_unhex")) return "sp_sc_cvar_unhex _cmdcost_dst _cmdcost_hex";
    if (!std::strcmp(n, "sp_sc_cvar_find")) return "sp_sc_cvar_find _cmdcost_f _sofbuddy_";
    if (!std::strcmp(n, "sp_sc_cvar_nbsp")) return "sp_sc_cvar_nbsp _cmdcost_dst _cmdcost_blob";
    if (!std::strcmp(n, "sp_sc_cvar_no_color")) return "sp_sc_cvar_no_color _cmdcost_dst _cmdcost_blob";
    if (!std::strcmp(n, "sp_sc_cvar_replace")) return "sp_sc_cvar_replace _cmdcost_dst _cmdcost_blob A X";
    if (!std::strcmp(n, "sp_sc_cvar_split")) return "sp_sc_cvar_split _cmdcost_s _ _cmdcost_blob";
    if (!std::strcmp(n, "sp_sc_cvar_substr")) {
        std::snprintf(g_spinLine, sizeof(g_spinLine),
                      "sp_sc_cvar_substr _cmdcost_dst _cmdcost_blob 0 %d",
                      g_spinStrLen > 0 ? g_spinStrLen : 3);
        return g_spinLine;
    }
    if (!std::strcmp(n, "sp_sc_cvar_random_float")) return "sp_sc_cvar_random_float _cmdcost_t 0 1";
    if (!std::strcmp(n, "sp_sc_cvar_random_int")) return "sp_sc_cvar_random_int _cmdcost_t 0 10";
    if (!std::strcmp(n, "sp_sc_cvar_big_text")) return "sp_sc_cvar_big_text _cmdcost_dst 1";
    if (!std::strcmp(n, "sp_sc_file_find")) return "sp_sc_file_find _cmdcost_f *.cfg";
    if (!std::strcmp(n, "sp_sc_cvar_list")) return "sp_sc_cvar_list _sofbuddy_";
    if (!std::strcmp(n, "sp_sc_func_list")) return "sp_sc_func_list";
    if (!std::strcmp(n, "sp_sc_alias")) return "sp_sc_alias _cmdcost_a \"echo cmdcost_a\"";
    if (!std::strcmp(n, "sp_sc_func_alias")) return "sp_sc_func_alias _cmdcost_fn \"echo cmdcost_fn\"";
    if (!std::strcmp(n, "sp_sc_func_exec")) return "sp_sc_func_exec _cmdcost_fn";
    if (!std::strcmp(n, "sp_sc_func_load_file")) return "sp_sc_func_load_file cmdcost_fn.func";
    if (!std::strcmp(n, "sp_sc_func_load_cvar")) return "sp_sc_func_load_cvar _cmdcost_fnbody";
    if (!std::strcmp(n, "sp_sc_exec_file")) return "sp_sc_exec_file cmdcost_nop.cfg";
    if (!std::strcmp(n, "sp_sc_exec_cvar")) return "sp_sc_exec_cvar _cmdcost_exec";
    if (!std::strcmp(n, "sp_sc_cvar_save")) return "sp_sc_cvar_save _cmdcost_blob cmdcost_save.cfg";
    if (!std::strcmp(n, "sp_sc_timer")) return "sp_sc_timer _cmdcost_tm 999 \"echo cmdcost_tm\"";
    if (!std::strcmp(n, "sp_sc_on_change")) return "sp_sc_on_change _cmdcost_t \"echo cmdcost_oc\"";
    if (!std::strcmp(n, "sp_sc_flow_if"))
        return "sp_sc_flow_if number cvar _cmdcost_t = val 0 \"echo if_t\" \"echo if_f\"";
    if (!std::strcmp(n, "sp_sc_flow_while"))
        return "sp_sc_flow_while number cvar _cmdcost_w < val 2 \"sp_sc_cvar_math_add _cmdcost_w 1\"";
    if (!std::strcmp(n, "sp_sv_info_client")) {
        std::snprintf(g_spinLine, sizeof(g_spinLine), "sp_sv_info_client %d", slot);
        return g_spinLine;
    }
    if (!std::strcmp(n, "sp_sv_info_client_mod")) {
        std::snprintf(g_spinLine, sizeof(g_spinLine), "sp_sv_info_client_mod %d 0", slot);
        return g_spinLine;
    }
    if (!std::strcmp(n, "sp_sv_print_client")) {
        std::snprintf(g_spinLine, sizeof(g_spinLine), "sp_sv_print_client %d cmdcost_bench", slot);
        return g_spinLine;
    }
    if (!std::strcmp(n, "sp_sv_client_cvar_set")) {
        std::snprintf(g_spinLine, sizeof(g_spinLine), "sp_sv_client_cvar_set %d 99 cmdcost", slot);
        return g_spinLine;
    }
    if (!std::strcmp(n, "sp_sv_client_check")) {
        std::snprintf(g_spinLine, sizeof(g_spinLine), "sp_sv_client_check %d cmdcost name", slot);
        return g_spinLine;
    }
    if (!std::strcmp(n, "sp_sv_print_broadcast")) return "sp_sv_print_broadcast cmdcost_bench";
    if (!std::strcmp(n, "sp_sv_sound_broadcast")) return "sp_sv_sound_broadcast misc/menu1.wav";
    if (!std::strcmp(n, "sp_sv_sound_client")) {
        std::snprintf(g_spinLine, sizeof(g_spinLine), "sp_sv_sound_client %d misc/menu1.wav", slot);
        return g_spinLine;
    }
    if (!std::strcmp(n, "sp_sv_say_mute") || !std::strcmp(n, "sp_sv_say_unmute") ||
        !std::strcmp(n, "sp_sv_client_blue") || !std::strcmp(n, "sp_sv_client_red") ||
        !std::strcmp(n, "sp_sv_client_spec") || !std::strcmp(n, "sp_sv_client_play") ||
        !std::strcmp(n, "sp_sv_client_swap")) {
        std::snprintf(g_spinLine, sizeof(g_spinLine), "%s %d", n, slot);
        return g_spinLine;
    }
    if (!std::strcmp(n, "sp_sv_players")) return "sp_sv_players";
    if (!std::strcmp(n, "sp_sv_info_frames")) return "sp_sv_info_frames";
    return n;
}

void Spin(const char* name, int n, int slot) {
    char nameBuf[32];
    CopyTok(nameBuf, name ? name : "");
    name = nameBuf;
    SyncOrigs();
    CmdFn orig = FindOrig(name);
    if (!orig) {
        PrintOut(PRINT_LOG, "[cmdcost] spin %s: unknown\n", name[0] ? name : "?");
        return;
    }
    if (SpinSkip(name)) {
        PrintOut(PRINT_LOG, "[cmdcost] spin %s: skipped\n", name);
        return;
    }
    char buf[192];
    std::snprintf(buf, sizeof(buf), "%s", SpinLine(name, slot));
    const int cap = SpinCap(name);
    const bool flood = !std::strncmp(name, "sp_sv_print", 11) ||
                       !std::strncmp(name, "sp_sv_sound", 11) ||
                       !std::strcmp(name, "sp_sv_players");
    const bool reap = !std::strcmp(name, "sp_sc_cvar_append") ||
                      !std::strcmp(name, "sp_sc_cvar_append_newline");
    // Batch-time the whole run — per-call QPC is useless under Wine (~16ms quantum).
    const double need = g.clock.quantumMs > kSpinTargetMs ? g.clock.quantumMs : kSpinTargetMs;
    auto batch = [&](int runs) -> double {
        if (!reap && g.tokenize) g.tokenize(buf, 1);
        const double t0 = g.clock.Now();
        for (int i = 0; i < runs; ++i) {
            if (reap) {
                PrepDstFromBlob();
                if (g.tokenize) g.tokenize(buf, 1);
            }
            orig();
        }
        return g.clock.Now() - t0;
    };
    int runs = 1;
    double total = 0;
    if (flood && cap > 0) {
        runs = cap;
        total = batch(runs);
    } else {
        runs = n > 0 ? n : 1;
        if (runs > 1000 && n > 0) runs = 1000;
        for (;;) {
            total = batch(runs);
            if (total >= need) break;
            if (cap > 0 && runs >= cap) break;
            if (runs >= kSpinMaxN) break;
            const int next = runs < 10 ? 10 : runs * 10;
            runs = (cap > 0 && next > cap) ? cap : next;
            if (runs == 0) break;
        }
    }
    // Flooders capped below one QPC tick: report upper bound, not fake 0.
    bool upper = false;
    if (total <= 0 && runs > 0) {
        total = g.clock.quantumMs;
        upper = true;
    }
    const double per = total / static_cast<double>(runs > 0 ? runs : 1);
    Note(name, per);
    PrintOut(PRINT_LOG, "[cmdcost] spin %s n=%d total=%.6fms per=%.6fms (%.3fus)%s\n",
             name, runs, upper ? 0.0 : total, per, per * 1000.0,
             upper ? " [qpc-upper]" : "");
}

void QueueLine(const char* line) {
    if (!g.addText || !line) return;
    char buf[192];
    std::snprintf(buf, sizeof(buf), "%s\n", line);
    g.addText(buf);
}

void QueueSlots(int slot, int reps, bool rough) {
    SyncOrigs();
    if (!g.addText) {
        PrintOut(PRINT_LOG, "[cmdcost] slots: Cbuf_AddText not bound\n");
        return;
    }
    if (slot < 0 || slot > 31) slot = 0;
    if (reps < 1) reps = 8;
    if (reps > 64) reps = 64;
    PrintOut(PRINT_LOG, "[cmdcost] slots slot=%d reps=%d rough=%d (need a connected client)\n",
             slot, reps, rough ? 1 : 0);

    // Fine-grained: spin handlers with argv already tokenized for this slot.
    static const char* kSpin[] = {
        "sp_sv_info_client", "sp_sv_info_client_mod", "sp_sv_print_client",
        "sp_sv_client_cvar_set", "sp_sv_players", "sp_sv_info_frames", nullptr
    };
    for (int i = 0; kSpin[i]; ++i)
        if (FindOrig(kSpin[i])) Spin(kSpin[i], 0, slot);

    // Few live runs for TimedCmd (avoid SZ_GetSpace flood from print_*).
    QueueLine("sp_sv_players");
    QueueLine("sp_sv_info_frames");
    if (reps > 8) reps = 8;
    for (int i = 0; i < reps; ++i) {
        char b[96];
        std::snprintf(b, sizeof(b), "sp_sv_info_client %d", slot);
        QueueLine(b);
        std::snprintf(b, sizeof(b), "sp_sv_info_client_mod %d 0", slot);
        QueueLine(b);
        std::snprintf(b, sizeof(b), "sp_sv_client_check %d cmdcost name", slot);
        QueueLine(b);
        std::snprintf(b, sizeof(b), "sp_sv_client_cvar_set %d 99 cmdcost", slot);
        QueueLine(b);
    }
    {
        char b[96];
        std::snprintf(b, sizeof(b), "sp_sv_print_client %d cmdcost_bench", slot);
        QueueLine(b);
    }
    if (rough) {
        char b[64];
        std::snprintf(b, sizeof(b), "sp_sv_client_blue %d", slot);
        QueueLine(b);
        std::snprintf(b, sizeof(b), "sp_sv_client_red %d", slot);
        QueueLine(b);
        std::snprintf(b, sizeof(b), "sp_sv_client_spec %d", slot);
        QueueLine(b);
        std::snprintf(b, sizeof(b), "sp_sv_client_play %d", slot);
        QueueLine(b);
        std::snprintf(b, sizeof(b), "sp_sv_say_mute %d", slot);
        QueueLine(b);
        std::snprintf(b, sizeof(b), "sp_sv_say_unmute %d", slot);
        QueueLine(b);
    }
}

void SpinCheap() {
    // Spin every safe sofplus catalog cmd (+ echo/set baseline). No handler patch.
    SyncOrigs();
    char pre[32];
    std::snprintf(pre, sizeof(pre), "set _cmdcost_t 0");
    if (g.tokenize) g.tokenize(pre, 1);
    std::snprintf(pre, sizeof(pre), "set _cmdcost_w 0");
    if (g.tokenize) g.tokenize(pre, 1);
    for (int i = 0; i < kCmdCount; ++i) {
        const char* n = CmdName(i);
        if (SpinSkip(n) || !FindOrig(n)) continue;
        const bool sofplus = std::strncmp(n, "sp_", 3) == 0;
        const bool baseline = !std::strcmp(n, "echo") || !std::strcmp(n, "set");
        if (!sofplus && !baseline) continue;
        Spin(n, 0, 0);
    }
}

}  // namespace cmdcost

extern "C" void TimedCmd() {
    using namespace cmdcost;
    const char* name = (g.cmdArgv && g.cmdArgv[0]) ? g.cmdArgv[0] : "?";
    CmdFn orig = FindOrig(name);
    if (!orig) {
        SyncOrigs();
        orig = FindOrig(name);
    }
    if (!orig) {
        static char last[32];
        if (std::strncmp(last, name, 31) != 0) {
            CopyTok(last, name);
            PrintOut(PRINT_BAD, "[cmdcost] TimedCmd miss: %s\n", name);
        }
        return;
    }
    if (!Enabled()) { orig(); return; }
    const double t0 = g.clock.Now();
    orig();
    Note(name, g.clock.Now() - t0);
}

static int ArgI(int i, int def) {
    if (!detour_Cmd_Argc::oCmd_Argc || !detour_Cmd_Argv::oCmd_Argv) return def;
    if (SOF_EP_Cmd_Argc() <= i) return def;
    const char* s = SOF_EP_Cmd_Argv(i);
    if (!s || !s[0]) return def;
    return std::atoi(s);
}

static const char* ArgS(int i) {
    if (!detour_Cmd_Argc::oCmd_Argc || !detour_Cmd_Argv::oCmd_Argv) return "";
    if (SOF_EP_Cmd_Argc() <= i) return "";
    const char* s = SOF_EP_Cmd_Argv(i);
    return s ? s : "";
}

static bool HasArg(const char* w) {
    if (!detour_Cmd_Argc::oCmd_Argc || !detour_Cmd_Argv::oCmd_Argv) return false;
    const int n = SOF_EP_Cmd_Argc();
    for (int i = 1; i < n; ++i)
        if (!_stricmp(SOF_EP_Cmd_Argv(i), w)) return true;
    return false;
}

#if defined(_WIN32)
#define CMDCOST_CDECL __cdecl
#else
#define CMDCOST_CDECL
#endif

extern "C" void CMDCOST_CDECL cmdcost_dump_f() { cmdcost::Dump(); }
extern "C" void CMDCOST_CDECL cmdcost_reset_f() { cmdcost::Reset(); }
extern "C" void CMDCOST_CDECL cmdcost_wrap_f() {
    if (cmdcost::Enabled())
        cmdcost::WrapAll();
    else
        cmdcost::SyncOrigs();
}
extern "C" void CMDCOST_CDECL cmdcost_spinall_f() { cmdcost::SpinCheap(); }

extern "C" void CMDCOST_CDECL cmdcost_spin_f() {
    using namespace cmdcost;
    const char* name = ArgS(1);
    int n = ArgI(2, 0);
    int slot = ArgI(3, 0);
    if (!name[0]) {
        PrintOut(PRINT_LOG, "[cmdcost] usage: sofbuddy_cmdcost_spin <name> [n] [slot]\n");
        return;
    }
    Spin(name, n, slot);
}

extern "C" void CMDCOST_CDECL cmdcost_blob_f() {
    using namespace cmdcost;
    const int len = ArgI(1, 0);
    const int piece = ArgI(2, 1);
    BuildBlob(len);
    g_spinPiece = piece < 1 ? 1 : (piece > 64 ? 64 : piece);
    PrintOut(PRINT_LOG, "[cmdcost] piece=%d\n", g_spinPiece);
}

extern "C" void CMDCOST_CDECL cmdcost_slots_f() {
    using namespace cmdcost;
    int slot = 0, reps = 8;
    bool rough = HasArg("rough");
    if (ArgS(1)[0] && _stricmp(ArgS(1), "rough")) slot = ArgI(1, 0);
    if (ArgS(2)[0] && _stricmp(ArgS(2), "rough")) reps = ArgI(2, 8);
    QueueSlots(slot, reps, rough);
}

namespace {

void** CmdFunctionsHead() {
    HMODULE exe = GetModuleHandleA("SoF.exe");
    if (!exe) exe = GetModuleHandleA("SoF-spsv.exe");
    if (!exe) return nullptr;
    return reinterpret_cast<void**>(reinterpret_cast<char*>(exe) + cmdcost::kRvaCmdFunctions);
}

void* FindCmdNode(const char* name) {
    void** head = CmdFunctionsHead();
    if (!head) return nullptr;
    for (void* node = *head; node;
         node = *reinterpret_cast<void**>(static_cast<char*>(node) + cmdcost::kCmdNextOfs)) {
        const char* n = *reinterpret_cast<const char**>(static_cast<char*>(node) + cmdcost::kCmdNameOfs);
        if (n && !std::strcmp(n, name)) return node;
    }
    return nullptr;
}

void InstallCommand(const char* name, void* fn) {
    if (void* node = FindCmdNode(name))
        *reinterpret_cast<void**>(static_cast<char*>(node) + cmdcost::kCmdFuncOfs) = fn;
    else
        SOF_EP_Cmd_AddCommand(const_cast<char*>(name), fn);
}

}  // namespace

extern void cmdcost_RegisterAddonsCmd();
extern void cmdcost_RegisterEventsCmd();
extern void cmdcost_RegisterSofplusCmds();

static void RegisterCmds() {
    if (!detour_Cmd_AddCommand::oCmd_AddCommand) return;
    InstallCommand("sofbuddy_cmdcost_dump", reinterpret_cast<void*>(&cmdcost_dump_f));
    InstallCommand("sofbuddy_cmdcost_reset", reinterpret_cast<void*>(&cmdcost_reset_f));
    InstallCommand("sofbuddy_cmdcost_wrap", reinterpret_cast<void*>(&cmdcost_wrap_f));
    InstallCommand("sofbuddy_cmdcost_spin", reinterpret_cast<void*>(&cmdcost_spin_f));
    InstallCommand("sofbuddy_cmdcost_spinall", reinterpret_cast<void*>(&cmdcost_spinall_f));
    InstallCommand("sofbuddy_cmdcost_blob", reinterpret_cast<void*>(&cmdcost_blob_f));
    InstallCommand("sofbuddy_cmdcost_slots", reinterpret_cast<void*>(&cmdcost_slots_f));
    cmdcost_RegisterAddonsCmd();
    cmdcost_RegisterEventsCmd();
    cmdcost_RegisterSofplusCmds();
}

void cmdcost_CmdPre(char*& text) {
    using namespace cmdcost;
    UpdateWrapState();
    if (!g.ready || g.depth >= 8) return;
    if (g.patched) return;  // handler wrap owns timing
    if (!Enabled()) return;
    char tok[32];
    CopyTok(tok, text);
    g.start[g.depth] = g.clock.Now();
    std::memcpy(g.tok[g.depth], tok, sizeof(tok));
    ++g.depth;
}

void cmdcost_CmdPost(char* text) {
    using namespace cmdcost;
    (void)text;
    if (!g.ready || g.depth <= 0) return;
    --g.depth;
    if (g.patched) return;
    if (Enabled())
        Note(g.tok[g.depth], g.clock.Now() - g.start[g.depth]);
}

void cmdcost_OnGameDllLoaded(void* game_export) {
    using namespace cmdcost;
    (void)game_export;
    InitCvars();
    g.clock.Init();
    g.ready = g.clock.ready;
    if (!g.ready) {
        PrintOut(PRINT_BAD, "[cmd_cost] no QPC\n");
        return;
    }
    RegisterCmds();
}
