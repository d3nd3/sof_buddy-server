// Static .func cost model: parse scripts, sum per-handler estimates (EMA or
// bench fallbacks), fold if/while using literal/cvar inference.

#include "func_analyze.h"
#include "cmd_cost.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <unordered_set>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace addoncost {
namespace {

struct Node {
    enum Kind { kCmd, kIf, kWhile } kind = kCmd;
    std::string text;
    std::vector<Node> body, elseBody;
};

struct Func {
    std::string name, file;
    std::vector<std::string> params;
    std::vector<Node> body;
};

struct Script {
    std::unordered_map<std::string, Func> funcs;
    std::unordered_map<std::string, std::vector<std::string>> events;
};

struct Val {
    bool known = false;
    int64_t num = 0;
    int len = 0;
};

struct State {
    std::unordered_map<std::string, Val> vars;

    Val& slot(const std::string& k) { return vars[k]; }
    const Val* get(const std::string& k) const {
        auto it = vars.find(k);
        return it == vars.end() ? nullptr : &it->second;
    }
};

struct Ctx {
    const Script* script = nullptr;
    const Opts* opt = nullptr;
    int unknown = 0;
    int foldedIfs = 0;
    std::set<std::string> stack;
    State env;
};

static std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' ||
                          std::isspace(static_cast<unsigned char>(s.back()))))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

static std::string StripComments(const std::string& in) {
    std::string out;
    bool inStr = false;
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '"') {
            inStr = !inStr;
            out += c;
            continue;
        }
        if (!inStr && c == '/' && i + 1 < in.size() && in[i + 1] == '/') {
            while (i < in.size() && in[i] != '\n') ++i;
            continue;
        }
        out += c;
    }
    return out;
}

static std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    for (size_t start = 0; start <= text.size();) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        lines.push_back(Trim(text.substr(start, end - start)));
        if (end == text.size()) break;
        start = end + 1;
    }
    return lines;
}

static bool Starts(const std::string& s, const char* p) {
    return s.size() >= std::strlen(p) && !std::strncmp(s.c_str(), p, std::strlen(p));
}

static std::vector<Node> ParseBlock(std::vector<std::string>& lines, size_t& i);
static Node ParseControl(std::vector<std::string>& lines, size_t& i, const std::string& header, Node::Kind kind);

static size_t NextFunctionLine(const std::vector<std::string>& lines, size_t from) {
    for (size_t j = from; j < lines.size(); ++j)
        if (Starts(lines[j], "function ")) return j;
    return lines.size();
}

static std::vector<std::string> ExplodeFunctionLines(const std::vector<std::string>& in) {
    std::vector<std::string> out;
    const char* kw = "function ";
    for (const std::string& line : in) {
        size_t pos = 0;
        while (pos < line.size()) {
            size_t f = line.find(kw, pos);
            if (f == std::string::npos) {
                std::string tail = Trim(line.substr(pos));
                if (!tail.empty()) out.push_back(tail);
                break;
            }
            if (f > pos) {
                std::string before = Trim(line.substr(pos, f - pos));
                if (!before.empty()) out.push_back(before);
            }
            out.push_back(Trim(line.substr(f)));
            break;
        }
    }
    return out;
}

static std::vector<Node> ParseBlock(std::vector<std::string>& lines, size_t& i) {
    std::vector<Node> nodes;
    unsigned guard = 0;
    while (i < lines.size()) {
        if (++guard > 100000u) break;
        std::string line = lines[i];
        if (!line.size()) {
            ++i;
            continue;
        }
        if (line == "}") break;
        if (Starts(line, "function ")) {
            ++i;
            if (i < lines.size() && lines[i] == "{") ++i;
            while (i < lines.size() && lines[i] != "}") ++i;
            if (i < lines.size()) ++i;
            continue;
        }
        if (Starts(line, "sp_sc_flow_if ") || Starts(line, "sp_sc_flow_while ")) {
            size_t bp = line.find('{');
            std::string hdr = bp == std::string::npos ? line : Trim(line.substr(0, bp));
            Node::Kind kind = Starts(line, "sp_sc_flow_if ") ? Node::kIf : Node::kWhile;
            if (bp != std::string::npos)
                lines[i] = line.substr(bp + 1);
            else
                ++i;
            nodes.push_back(ParseControl(lines, i, hdr, kind));
            continue;
        }
        if (line == "else" || Starts(line, "else ")) break;
        size_t bp = line.find('{');
        if (bp != std::string::npos) {
            std::string hdr = Trim(line.substr(0, bp));
            lines[i] = line.substr(bp + 1);
            Node::Kind k = Starts(hdr, "sp_sc_flow_if ") ? Node::kIf :
                           Starts(hdr, "sp_sc_flow_while ") ? Node::kWhile : Node::kCmd;
            if (k != Node::kCmd)
                nodes.push_back(ParseControl(lines, i, hdr, k));
            else
                nodes.push_back({Node::kCmd, hdr});
            continue;
        }
        nodes.push_back({Node::kCmd, line});
        ++i;
    }
    return nodes;
}

static Node ParseControl(std::vector<std::string>& lines, size_t& i, const std::string& header, Node::Kind kind) {
    Node n;
    n.kind = kind;
    n.text = header;
    if (i < lines.size() && lines[i] == "{") ++i;
    n.body = ParseBlock(lines, i);
    if (i < lines.size() && lines[i] == "}") ++i;
    if (kind == Node::kIf && i < lines.size()) {
        std::string line = lines[i];
        if (line == "else" || Starts(line, "else ")) {
            size_t bp = line.find('{');
            if (bp != std::string::npos)
                lines[i] = line.substr(bp + 1);
            else {
                ++i;
                if (i < lines.size() && lines[i] == "{") ++i;
            }
            n.elseBody = ParseBlock(lines, i);
            if (i < lines.size() && lines[i] == "}") ++i;
        }
    }
    return n;
}

static std::vector<std::string> ParseParams(const std::string& hdr) {
    std::vector<std::string> p;
    size_t l = hdr.find('('), r = hdr.find(')', l == std::string::npos ? 0 : l + 1);
    if (l == std::string::npos || r == std::string::npos) return p;
    std::string inside = hdr.substr(l + 1, r - l - 1);
    size_t i = 0;
    while (i < inside.size()) {
        while (i < inside.size() && std::isspace(static_cast<unsigned char>(inside[i]))) ++i;
        if (i >= inside.size()) break;
        if (inside[i] == '*') {
            p.push_back("*");
            ++i;
            continue;
        }
        size_t j = i;
        while (j < inside.size() && inside[j] != ',' && inside[j] != ' ') ++j;
        std::string tok = Trim(inside.substr(i, j - i));
        if (!tok.empty()) p.push_back(tok);
        i = inside.find(',', j);
        if (i == std::string::npos) break;
        ++i;
    }
    return p;
}

static Script ParseScript(const char* file, const std::string& text) {
    Script s;
    auto lines = ExplodeFunctionLines(SplitLines(StripComments(text)));
    for (size_t i = 0; i < lines.size();) {
        if (!Starts(lines[i], "function ")) {
            ++i;
            continue;
        }
        const size_t start = i;
        std::string hdr = lines[i];
        size_t bp = hdr.find('{');
        if (bp != std::string::npos) hdr = Trim(hdr.substr(0, bp));
        size_t p0 = hdr.find(' ');
        if (p0 == std::string::npos) {
            i = NextFunctionLine(lines, i + 1);
            continue;
        }
        size_t p1 = hdr.find('(', p0 + 1);
        std::string name = Trim(hdr.substr(p0 + 1, (p1 == std::string::npos ? hdr.size() : p1) - p0 - 1));
        if (name.empty()) {
            i = NextFunctionLine(lines, i + 1);
            continue;
        }
        if (bp != std::string::npos)
            lines[i] = lines[i].substr(bp + 1);
        else {
            ++i;
            if (i < lines.size() && lines[i] == "{") ++i;
        }
        Func f;
        f.name = name;
        f.file = file ? file : "";
        f.params = ParseParams(hdr);
        f.body = ParseBlock(lines, i);
        if (i < lines.size() && lines[i] == "}") {
            ++i;
        } else if (i >= lines.size() || !Starts(lines[i], "function ")) {
            i = NextFunctionLine(lines, i);
        }
        if (i <= start) i = start + 1;
        s.funcs[name] = std::move(f);
    }
    return s;
}

static void Token(const char* s, char* dst, int n = 32) {
    while (s && (*s == ' ' || *s == '\t')) ++s;
    int i = 0;
    if (s)
        while (s[i] && s[i] != ' ' && s[i] != '\t' && s[i] != ';' && s[i] != '\n' && i < n - 1)
            dst[i] = s[i], ++i;
    dst[i] = 0;
}

static const char* TokAt(const std::string& line, int idx) {
    static thread_local char buf[8][64];
    static thread_local int slot = 0;
    char* out = buf[slot = (slot + 1) & 7];
    const char* s = line.c_str();
    for (int i = 0; i < idx && s; ++i) {
        while (*s == ' ' || *s == '\t') ++s;
        if (!*s) return out[0] = 0, out;
        while (*s && *s != ' ' && *s != '\t') ++s;
    }
    Token(s, out, 64);
    return out;
}

static std::string NormKey(const char* raw) {
    if (!raw || !raw[0]) return {};
    if (raw[0] == '#') return NormKey(raw + 1);
    if (raw[0] == '$') return NormKey(raw + 1);
    return raw;
}

static int64_t ParseNumTok(const char* raw, const State& st, const Opts& opt) {
    if (!raw || !raw[0]) return 0;
    if (raw[0] == '#') {
        std::string k = NormKey(raw);
        if (k == "maxclients") return opt.maxclients;
        if (const Val* v = st.get(k)) return v->known ? v->num : 0;
        return 0;
    }
    if (raw[0] == '$') {
        std::string k = NormKey(raw);
        if (const Val* v = st.get(k)) return v->known ? v->num : 0;
        return 0;
    }
    char* end = nullptr;
    long n = std::strtol(raw, &end, 10);
    if (end != raw && *end == 0) return n;
    if (const Val* v = st.get(NormKey(raw))) return v->known ? v->num : 0;
    return 0;
}

static int EstStrLen(const char* raw, const State& st, const Opts& opt) {
    if (!raw || !raw[0]) return 0;
    if (raw[0] == '"') {
        size_t n = std::strlen(raw);
        return n >= 2 ? static_cast<int>(n - 2) : 0;
    }
    std::string k = NormKey(raw);
    if (const Val* v = st.get(k)) return v->known ? v->len : opt.defaultStrLen;
    if (k == "maxclients") return 2;
    return opt.defaultStrLen;
}

static bool IsNumFlow(const std::string& hdr) {
    return hdr.find(" number ") != std::string::npos || Starts(hdr, "sp_sc_flow_if number") ||
           Starts(hdr, "sp_sc_flow_while number");
}

static std::string FlowCondRest(const std::string& hdr) {
    size_t sp = hdr.find(' ');
    if (sp == std::string::npos) return {};
    std::string rest = Trim(hdr.substr(sp + 1));
    if (Starts(rest, "number ")) return Trim(rest.substr(7));
    if (Starts(rest, "string ")) return Trim(rest.substr(7));
    return rest;
}

static bool ParseFlowCmp(const std::string& hdr, std::string& lhs, std::string& op, std::string& rhs) {
    static const char* ops[] = {"==", "!=", "<=", ">=", "<", ">"};
    std::string rest = FlowCondRest(hdr);
    if (rest.empty()) return false;
    for (const char* o : ops) {
        size_t p = rest.find(std::string(" ") + o + " ");
        if (p == std::string::npos) continue;
        lhs = Trim(rest.substr(0, p));
        op = o;
        rhs = Trim(rest.substr(p + std::strlen(o) + 2));
        return true;
    }
    return false;
}

static int64_t FlowOperand(const std::string& tok, bool number, const State& st, const Opts& opt) {
    if (tok.empty()) return 0;
    if (Starts(tok, "val ")) return ParseNumTok(tok.c_str() + 4, st, opt);
    if (Starts(tok, "cvar ")) return ParseNumTok(tok.c_str() + 5, st, opt);
    if (number) return ParseNumTok(tok.c_str(), st, opt);
    return EstStrLen(tok.c_str(), st, opt);
}

// -1 unknown, 0 false, 1 true
static int EvalCmp(int64_t a, int64_t b, const std::string& op) {
    if (op == "==") return a == b ? 1 : 0;
    if (op == "!=") return a != b ? 1 : 0;
    if (op == "<") return a < b ? 1 : 0;
    if (op == "<=") return a <= b ? 1 : 0;
    if (op == ">") return a > b ? 1 : 0;
    if (op == ">=") return a >= b ? 1 : 0;
    return -1;
}

static bool OperandKnown(const std::string& tok, const State& st, const Opts& opt) {
    if (Starts(tok, "val ")) {
        const char* v = tok.c_str() + 4;
        if (v[0] == '#') {
            std::string k = NormKey(v);
            return k == "maxclients" || st.get(k);
        }
        return true;
    }
    if (Starts(tok, "cvar ")) return st.get(NormKey(tok.c_str() + 5)) != nullptr;
    return false;
}

static int EvalIf(const std::string& hdr, const State& st, const Opts& opt) {
    std::string lhs, op, rhs;
    if (!ParseFlowCmp(hdr, lhs, op, rhs)) return -1;
    bool num = IsNumFlow(hdr);
    if (!num) {
        if (Starts(lhs, "val \"") && Starts(rhs, "val \""))
            return EvalCmp(EstStrLen(lhs.c_str() + 4, st, opt), EstStrLen(rhs.c_str() + 4, st, opt), op);
        return -1;
    }
    if (!OperandKnown(lhs, st, opt) || !OperandKnown(rhs, st, opt)) return -1;
    return EvalCmp(FlowOperand(lhs, true, st, opt), FlowOperand(rhs, true, st, opt), op);
}

static std::string WhileVar(const std::string& hdr) {
    std::string lhs, op, rhs;
    if (!ParseFlowCmp(hdr, lhs, op, rhs)) return {};
    if (Starts(lhs, "cvar ")) return NormKey(lhs.c_str() + 5);
    if (Starts(lhs, "val ")) return {};
    return NormKey(lhs.c_str());
}

static int WhileIters(const std::string& hdr, const State& st, const Opts& opt) {
    std::string lhs, op, rhs;
    if (!ParseFlowCmp(hdr, lhs, op, rhs)) return opt.defaultWhile;
    int64_t start = 0;
    if (Starts(lhs, "cvar ")) {
        std::string k = NormKey(lhs.c_str() + 5);
        if (const Val* v = st.get(k)) start = v->known ? v->num : 0;
    }
    int64_t bound = FlowOperand(rhs, true, st, opt);
    if (bound <= 0) return opt.defaultWhile;
    int64_t n = 0;
    if (op == "<") n = bound - start;
    else if (op == "<=") n = bound - start + 1;
    else if (op == ">") n = start - bound;
    else if (op == ">=") n = start - bound + 1;
    else n = opt.defaultWhile;
    if (n < 0) n = 0;
    if (n > opt.maxwhile) n = opt.maxwhile;
    return static_cast<int>(n);
}

static void ApplyCmdState(const std::string& line, State& st, const Opts& opt) {
    char cmd[48];
    Token(line.c_str(), cmd, 48);
    if (!std::strcmp(cmd, "set") || !std::strcmp(cmd, "sset")) {
        std::string key = NormKey(TokAt(line, 1));
        const char* val = TokAt(line, 2);
        Val& v = st.slot(key);
        v.known = true;
        char* end = nullptr;
        long n = std::strtol(val, &end, 10);
        if (end != val && (*end == 0 || std::isspace(static_cast<unsigned char>(*end)))) {
            v.num = n;
            v.len = 0;
        } else {
            v.len = EstStrLen(val, st, opt);
            v.num = v.len;
        }
        return;
    }
    if (!std::strcmp(cmd, "zero")) {
        Val& v = st.slot(NormKey(TokAt(line, 1)));
        v.known = true;
        v.num = 0;
        v.len = 0;
        return;
    }
    if (!std::strcmp(cmd, "add")) {
        std::string key = NormKey(TokAt(line, 1));
        Val& v = st.slot(key);
        if (!v.known) v.known = true;
        v.num += ParseNumTok(TokAt(line, 2), st, opt);
        return;
    }
    if (!std::strcmp(cmd, "sp_sc_cvar_math_add")) {
        std::string key = NormKey(TokAt(line, 1));
        Val& v = st.slot(key);
        if (!v.known) v.known = true;
        v.num += ParseNumTok(TokAt(line, 2), st, opt);
    }
}

static void ApplyPrecedingState(const std::vector<Node>& nodes, size_t idx, State& st, const Opts& opt) {
    for (size_t j = 0; j < idx; ++j) {
        const Node& n = nodes[j];
        if (n.kind != Node::kCmd) continue;
        for (const std::string& line : n.text.find(';') == std::string::npos ?
                 std::vector<std::string>{n.text} : std::vector<std::string>{}) {
            ApplyCmdState(line, st, opt);
        }
        // semicolon split inline
        std::string cur;
        bool inStr = false;
        for (size_t k = 0; k <= n.text.size(); ++k) {
            char c = k < n.text.size() ? n.text[k] : ';';
            if (k < n.text.size() && c == '"') inStr = !inStr;
            if (!inStr && c == ';') {
                std::string t = Trim(cur);
                if (!t.empty()) ApplyCmdState(t, st, opt);
                cur.clear();
            } else if (k < n.text.size())
                cur += c;
        }
    }
}

static float FallbackMs(const char* cmd) {
    struct Row {
        const char* n;
        float ms;
    };
    static const Row k[] = {
        {"sp_sv_sound_client", 4.0f}, {"sp_sv_sound_broadcast", 4.0f},
        {"sp_sv_print_client", 4.0f}, {"sp_sv_print_broadcast", 4.0f},
        {"sp_sc_cvar_save", 3.2f}, {"sp_sv_players", 2.0f},
        {"sp_sc_func_list", 0.64f}, {"sp_sc_file_find", 0.64f},
        {"sp_sc_flow_while", 0.5f}, {"sp_sc_flow_if", 0.5f},
        {"sp_sc_exec_file", 0.5f}, {"sp_sc_exec_cvar", 0.5f},
        {"sp_sc_func_load_file", 0.25f}, {"sp_sc_timer", 0.25f},
        {"sp_sc_on_change", 0.25f},
        {"sp_sv_say_mute", 0.25f}, {"sp_sv_say_unmute", 0.25f},
        {"sp_sv_client_swap", 0.25f}, {"sp_sv_client_spec", 0.25f},
        {"sp_sv_client_red", 0.25f}, {"sp_sv_client_play", 0.25f},
        {"sp_sv_client_blue", 0.25f},
        {"sp_sc_func_load_cvar", 0.125f}, {"sp_sc_func_exec", 0.125f},
        {"sp_sc_cvar_list", 0.0112f}, {"sp_sv_info_client", 0.0144f},
        {"sp_sc_info_time", 0.008f}, {"sp_sv_client_check", 0.0064f},
        {"sp_sv_client_cvar_set", 0.00064f},
        {"sp_sc_cvar_math_add", 0.00032f}, {"sp_sc_cvar_math_sub", 0.00032f},
        {"sp_sc_cvar_copy", 0.00032f}, {"sp_sc_cvar_append", 0.00032f},
        {"sp_sc_cvar_sset", 0.00032f}, {"sp_sc_cvar_len", 0.00032f},
        {"sp_sc_cvar_hex", 0.00032f}, {"sp_sc_cvar_escape", 0.00032f},
        {"sp_sc_alias", 0.00032f}, {"sp_sc_func_alias", 0.00032f},
        {"set", 0.00032f}, {"sset", 0.00032f}, {"add", 0.00032f},
        {"zero", 0.00032f}, {"echo", 0.00032f},
        {nullptr, 0.0005f},
    };
    for (int i = 0; k[i].n; ++i)
        if (!std::strcmp(cmd, k[i].n)) return k[i].ms;
    if (cmd[0] == 's' && cmd[1] == 'p' && cmd[2] == '_') return 0.05f;
    return k[sizeof(k) / sizeof(k[0]) - 1].ms;
}

static int MaxStrLenInLine(const std::string& line, const State& st, const Opts& opt) {
    int mx = opt.defaultStrLen;
    const char* s = line.c_str();
    while (s && *s) {
        while (*s == ' ' || *s == '\t') ++s;
        if (!*s) break;
        const char* start = s;
        while (*s && *s != ' ' && *s != '\t') ++s;
        std::string tok(start, s - start);
        int l = EstStrLen(tok.c_str(), st, opt);
        if (l > mx) mx = l;
        if (mx > 256) return 256;
    }
    return mx;
}

static float CmdMs(const char* line, const State& st, const Opts& opt) {
    char tok[32];
    Token(line, tok);
    if (!tok[0]) return 0.f;
    float fb = FallbackMs(tok);
    float live = cmdcost::PredictMs(line);
    float base = live > 0.f ? (live > fb ? live : (live + fb) * 0.5f) : fb;

    const int slen = MaxStrLenInLine(line, st, opt);
    if (!std::strcmp(tok, "sp_sc_cvar_hex"))
        return base + static_cast<float>(slen) * 0.05f;
    if (!std::strcmp(tok, "sp_sc_cvar_unhex"))
        return base + static_cast<float>(slen) * 0.03f;
    if (!std::strcmp(tok, "sp_sc_cvar_save"))
        return slen > 128 ? 0.48f : (slen > 64 ? 0.64f : 0.96f);
    if (!std::strcmp(tok, "sp_sc_cvar_append") || !std::strcmp(tok, "sp_sc_cvar_append_newline"))
        return base + static_cast<float>(std::min(slen, 64)) * 0.001f;
    if (!std::strncmp(tok, "sp_sv_print", 11) || !std::strncmp(tok, "sp_sv_sound", 11))
        return std::max(base, fb);
    return base;
}

static Cost Mk(double v) { return {v, v, v}; }

static Cost Add(const Cost& a, const Cost& b) {
    return {a.min + b.min, a.avg + b.avg, a.max + b.max};
}

static Cost Scale(const Cost& c, int n) {
    if (n <= 0) return {};
    return {c.min * n, c.avg * n, c.max * n};
}

static std::vector<std::string> SplitCmds(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool inStr = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '"') {
            inStr = !inStr;
            cur += c;
            continue;
        }
        if (!inStr && c == ';') {
            std::string t = Trim(cur);
            if (!t.empty()) out.push_back(t);
            cur.clear();
            continue;
        }
        cur += c;
    }
    std::string t = Trim(cur);
    if (!t.empty()) out.push_back(t);
    return out.empty() ? std::vector<std::string>{line} : out;
}

static bool IsRegHelper(const char* fn) {
    return !std::strcmp(fn, "spf_sc_list_add_func") || !std::strcmp(fn, "spf_sc_list_add") ||
           !std::strcmp(fn, "spf_sc_list_onchange_add_func") || !std::strcmp(fn, "spf_sc_list_add_cmd");
}

static void TrackEvent(Script& s, const std::string& ev, const std::string& fn) {
    if (ev.empty() || fn.empty()) return;
    auto& v = s.events[ev];
    if (std::find(v.begin(), v.end(), fn) == v.end()) v.push_back(fn);
}

static std::string Unquote(const char* t) {
    if (!t || !t[0]) return {};
    if (t[0] == '"') {
        size_t n = std::strlen(t);
        if (n >= 2 && t[n - 1] == '"') return std::string(t + 1, n - 2);
    }
    return t;
}

static void ScanOnChange(const std::string& line, Script& s) {
    const char* name = TokAt(line, 1);
    const char* sp = line.c_str();
    sp = std::strchr(sp, ' ');
    if (!sp) return;
    sp = std::strchr(sp + 1, ' ');
    if (!sp) return;
    std::string key = std::string("_sp_sc_on_change_") + name;
    while (*++sp == ' ') {}
    std::string rest(sp);
    for (size_t start = 0; start < rest.size();) {
        size_t comma = rest.find(',', start);
        std::string f = Trim(rest.substr(start, comma == std::string::npos ? rest.size() : comma - start));
        if (!f.empty()) TrackEvent(s, key, f);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
}

static void ScanRegs(const std::string& line, Script& s) {
    char cmd[48];
    Token(line.c_str(), cmd, 48);
    if (!cmd[0]) return;
    if (!std::strncmp(cmd, "_sp_sv_on_", 10)) {
        TrackEvent(s, cmd, Unquote(TokAt(line, 1)));
        return;
    }
    if (!std::strcmp(cmd, "sp_sc_on_change")) {
        ScanOnChange(line, s);
        return;
    }
    if (std::strcmp(cmd, "sp_sc_func_exec")) return;
    if (IsRegHelper(TokAt(line, 1)))
        TrackEvent(s, Unquote(TokAt(line, 2)), Unquote(TokAt(line, 3)));
}

static void ScanRegsNode(const Node& n, Script& s) {
    if (n.kind == Node::kCmd) {
        ScanRegs(n.text, s);
        return;
    }
    for (const Node& c : n.body) ScanRegsNode(c, s);
    for (const Node& c : n.elseBody) ScanRegsNode(c, s);
}

static void BindParams(State& st, const std::vector<std::string>& params, const Opts& opt) {
    for (const std::string& p : params) {
        if (p == "*") continue;
        std::string k = NormKey(p.c_str());
        if (k.find("slot") != std::string::npos || k.find("par_slot") != std::string::npos) {
            Val& v = st.slot(k);
            v.known = true;
            v.num = opt.eventSlot;
        }
    }
}

static Cost AnalyzeFunc(const std::string& name, Ctx& ctx);

static Cost AnalyzeNodes(const std::vector<Node>& nodes, Ctx& ctx, State st) {
    Cost total;
    for (size_t ni = 0; ni < nodes.size(); ++ni) {
        const Node& n = nodes[ni];
        if (n.kind == Node::kCmd) {
            for (const std::string& line : SplitCmds(n.text)) {
                char cmd[48];
                Token(line.c_str(), cmd, 48);
                double ms = CmdMs(line.c_str(), st, *ctx.opt);
                if (ms <= 0 && cmd[0]) ++ctx.unknown;
                Cost c = Mk(ms);
                if (!std::strcmp(cmd, "sp_sc_func_exec")) {
                    const char* fn = TokAt(line, 1);
                    if (!IsRegHelper(fn)) {
                        Cost fc = AnalyzeFunc(Unquote(fn), ctx);
                        c = Add(c, fc);
                    }
                }
                total = Add(total, c);
                ApplyCmdState(line, st, *ctx.opt);
            }
            continue;
        }
        double oh = CmdMs(n.text.c_str(), st, *ctx.opt);
        if (n.kind == Node::kIf) {
            int cond = EvalIf(n.text, st, *ctx.opt);
            State stT = st, stF = st;
            Cost body = AnalyzeNodes(n.body, ctx, stT);
            Cost el = AnalyzeNodes(n.elseBody, ctx, stF);
            if (cond == 1) {
                ++ctx.foldedIfs;
                total = Add(total, Add(Mk(oh), body));
                st = stT;
            } else if (cond == 0) {
                ++ctx.foldedIfs;
                total = Add(total, Add(Mk(oh), el));
                st = stF;
            } else {
                Cost branch = {std::min(body.min, el.min), (body.avg + el.avg) * 0.5, std::max(body.max, el.max)};
                total = Add(total, Add(Mk(oh), branch));
            }
            continue;
        }
        State loopSt = st;
        ApplyPrecedingState(nodes, ni, loopSt, *ctx.opt);
        int hi = WhileIters(n.text, loopSt, *ctx.opt);
        int lo = 0;
        std::string wv = WhileVar(n.text);
        if (!wv.empty() && loopSt.get(wv) && loopSt.get(wv)->known) lo = hi;
        int avgN = hi > 0 ? (lo + hi) / 2 : 0;
        if (avgN < 1 && hi > 0) avgN = hi / 2;
        Cost once = Add(Mk(oh), AnalyzeNodes(n.body, ctx, loopSt));
        total = Add(total, {Scale(once, lo).min, Scale(once, avgN).avg, Scale(once, hi).max});
    }
    return total;
}

static Cost AnalyzeFunc(const std::string& name, Ctx& ctx) {
    if (!ctx.script || ctx.stack.count(name)) return {};
    auto it = ctx.script->funcs.find(name);
    if (it == ctx.script->funcs.end()) return {};
    ctx.stack.insert(name);
    State st = ctx.env;
    BindParams(st, it->second.params, *ctx.opt);
    Cost c = AnalyzeNodes(it->second.body, ctx, st);
    ctx.stack.erase(name);
    return c;
}

static void CollectCmdNode(const Node& n, std::unordered_set<std::string>& out) {
    if (n.kind == Node::kCmd) {
        for (const std::string& line : SplitCmds(n.text)) {
            char tok[32];
            Token(line.c_str(), tok);
            if (tok[0]) out.insert(tok);
        }
        return;
    }
    for (const Node& c : n.body) CollectCmdNode(c, out);
    for (const Node& c : n.elseBody) CollectCmdNode(c, out);
}

static void CollectKnownEvents(Script& s) {
    static const char* kEv[] = {
        "_sp_sv_on_client_begin", "_sp_sv_on_client_die", "_sp_sv_on_client_disconnect",
        "_sp_sv_on_client_spawn", "_sp_sv_on_client_userinfo_change",
        "_sp_sv_on_ctf_flag_capture", "_sp_sv_on_map_begin", "_sp_sv_on_map_end",
        "_sp_sv_on_map_rotate", nullptr};
    for (int i = 0; kEv[i]; ++i)
        if (!s.events.count(kEv[i])) s.events[kEv[i]] = {};
}

static Report BuildReport(Script& s, const Opts& opt) {
    CollectKnownEvents(s);
    for (auto& kv : s.funcs)
        for (const Node& n : kv.second.body) ScanRegsNode(n, s);
    Ctx ctx;
    ctx.script = &s;
    ctx.opt = &opt;
    Report r;
    r.files = 1;
    for (auto& kv : s.funcs) {
        ctx.stack.clear();
        FuncCost fc;
        fc.name = kv.first;
        fc.file = kv.second.file;
        ctx.env = State{};
        Cost c = AnalyzeNodes(kv.second.body, ctx, ctx.env);
        fc.cost = c;
        r.funcs.push_back(fc);
    }
    r.unknownCmds = ctx.unknown;
    r.foldedIfs = ctx.foldedIfs;
    for (auto& ev : s.events) {
        EventCost ec;
        ec.event = ev.first;
        ec.funcs = ev.second;
        Cost sum;
        for (const std::string& fn : ev.second) sum = Add(sum, AnalyzeFunc(fn, ctx));
        ec.cost = sum;
        if (!ev.second.empty() || ev.first[0] == '_') r.events.push_back(ec);
    }
    std::sort(r.funcs.begin(), r.funcs.end(),
              [](const FuncCost& a, const FuncCost& b) { return a.name < b.name; });
    std::sort(r.events.begin(), r.events.end(),
              [](const EventCost& a, const EventCost& b) { return a.event < b.event; });
    return r;
}

static void MergeFuncs(std::unordered_map<std::string, Func>& dst, const Script& src) {
    for (auto& kv : src.funcs)
        if (!dst.count(kv.first)) dst[kv.first] = kv.second;
}

static std::string ReadFuncFile(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    if (sz < 0) {
        std::fclose(f);
        return {};
    }
    std::fseek(f, 0, SEEK_SET);
    std::string buf(static_cast<size_t>(sz), '\0');
    if (sz > 0 && std::fread(buf.data(), 1, buf.size(), f) != buf.size()) {
        buf.clear();
    } else if (sz == 0) {
        buf.clear();
    }
    std::fclose(f);
    if (!buf.empty() && buf.back() != '\n') buf += '\n';
    return buf;
}

static bool IsFuncName(const char* name) {
    size_t n = std::strlen(name);
    if (n <= 5) return false;
#if defined(_WIN32)
    return !_stricmp(name + n - 5, ".func");
#else
    return !std::strcmp(name + n - 5, ".func");
#endif
}

// Invoke fn(path) for each .func in dir; one failure must not stop the walk.
template <typename Fn>
static void ForEachFuncFile(const char* dir, Fn fn) {
    if (!dir || !dir[0]) return;
#if defined(_WIN32)
    std::string pat = dir;
    if (!pat.empty() && pat.back() != '\\' && pat.back() != '/') pat += '\\';
    pat += "*.func";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!IsFuncName(fd.cFileName)) continue;
        std::string path = dir;
        if (path.back() != '\\' && path.back() != '/') path += '\\';
        path += fd.cFileName;
        fn(path);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir);
    if (!d) return;
    while (dirent* ent = readdir(d)) {
        if (!IsFuncName(ent->d_name)) continue;
        std::string path = std::string(dir) + "/" + ent->d_name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        fn(path);
    }
    closedir(d);
#endif
}

static Script LoadDir(const char* dir, int& files) {
    Script merged;
    files = 0;
    ForEachFuncFile(dir, [&](const std::string& path) {
        std::string text = ReadFuncFile(path);
        if (text.empty()) return;
        MergeFuncs(merged.funcs, ParseScript(path.c_str(), text));
        ++files;
    });
    return merged;
}

}  // namespace

Report AnalyzeSource(const char* file, const char* text, const Opts& opt) {
    Script s = ParseScript(file, text ? text : "");
    return BuildReport(s, opt);
}

Report AnalyzeDir(const char* dir, const Opts& opt) {
    int files = 0;
    Script merged = LoadDir(dir, files);
    Report r = BuildReport(merged, opt);
    r.files = files;
    return r;
}

static std::string BaseName(const char* path) {
    if (!path || !path[0]) return {};
    const char* base = path;
    for (const char* p = path; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    return base;
}

static std::string ExtractFunctionName(const std::string& line) {
    size_t fn = line.find("function ");
    if (fn == std::string::npos) return {};
    std::string hdr = Trim(line.substr(fn));
    size_t p0 = hdr.find(' ');
    if (p0 == std::string::npos) return {};
    size_t bp = hdr.find('{');
    if (bp != std::string::npos) hdr = Trim(hdr.substr(0, bp));
    size_t p1 = hdr.find('(', p0 + 1);
    return Trim(hdr.substr(p0 + 1, (p1 == std::string::npos ? hdr.size() : p1) - p0 - 1));
}

static std::string CommentAboveFunc(const std::vector<std::string>& lines, size_t fnLine) {
    for (size_t j = fnLine; j-- > 0;) {
        std::string line = Trim(lines[j]);
        if (line.empty()) continue;
        if (!Starts(line, "//")) break;
        std::string desc = Trim(line.substr(2));
        if (!desc.empty()) return desc;
    }
    return {};
}

static void IndexUserCmdDescFromText(const std::string& text,
                                     std::unordered_map<std::string, std::string>& out) {
    std::string t = text;
    if (!t.empty() && t.back() != '\n') t += '\n';
    const std::vector<std::string> lines = SplitLines(t);
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        if (!Starts(line, "function ")) continue;
        std::string name = ExtractFunctionName(line);
        if (name.empty() || name[0] != '.') continue;
        std::string desc = CommentAboveFunc(lines, i);
        if (!desc.empty() && !out.count(name)) out[name] = desc;
    }
}

// Line scan for events file tags — each top-level `function name(` line only.
static void IndexFuncNamesFromText(const char* file, const std::string& text,
                                   std::unordered_map<std::string, std::string>& out) {
    std::string t = text;
    if (!t.empty() && t.back() != '\n') t += '\n';
    const std::string base = BaseName(file);
    for (const std::string& line : ExplodeFunctionLines(SplitLines(StripComments(t)))) {
        if (!Starts(line, "function ")) continue;
        std::string name = ExtractFunctionName(line);
        if (!name.empty() && !out.count(name)) out[name] = base;
    }
}

std::unordered_map<std::string, std::string> FuncFileIndex(const char* dir) {
    std::unordered_map<std::string, std::string> out;
    ForEachFuncFile(dir, [&](const std::string& path) {
        std::string text = ReadFuncFile(path);
        if (text.empty()) return;
        IndexFuncNamesFromText(path.c_str(), text, out);
    });
    return out;
}

std::unordered_map<std::string, std::string> FuncFileIndexMerge(const char* const* dirs, size_t n) {
    std::unordered_map<std::string, std::string> out;
    for (size_t i = 0; i < n; ++i) {
        if (!dirs[i] || !dirs[i][0]) continue;
        for (auto& kv : FuncFileIndex(dirs[i])) out[kv.first] = kv.second;
    }
    return out;
}

std::unordered_map<std::string, std::string> FuncUserCmdDescIndex(const char* dir) {
    std::unordered_map<std::string, std::string> out;
    ForEachFuncFile(dir, [&](const std::string& path) {
        std::string text = ReadFuncFile(path);
        if (text.empty()) return;
        IndexUserCmdDescFromText(text, out);
    });
    return out;
}

std::unordered_map<std::string, std::string> FuncUserCmdDescIndexMerge(const char* const* dirs,
                                                                     size_t n) {
    std::unordered_map<std::string, std::string> out;
    for (size_t i = 0; i < n; ++i) {
        if (!dirs[i] || !dirs[i][0]) continue;
        for (auto& kv : FuncUserCmdDescIndex(dirs[i])) out[kv.first] = kv.second;
    }
    return out;
}

std::vector<std::string> CollectCommands(const char* dir, const Opts& opt) {
    int files = 0;
    Script s = LoadDir(dir, files);
    std::unordered_set<std::string> cmds;
    for (auto& kv : s.funcs)
        for (const Node& n : kv.second.body) CollectCmdNode(n, cmds);
    std::vector<std::string> out(cmds.begin(), cmds.end());
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace addoncost
