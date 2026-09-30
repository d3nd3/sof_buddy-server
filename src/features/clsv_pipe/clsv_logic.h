#pragma once

// clsv_logic: pure, host-testable pipeline for the _sp_cl_sv_* transport.
//
// SoFplus servers can set client cvars _sp_cl_sv_0 .. _sp_cl_sv_99 with
//   sp_sv_client_cvar_set SLOT NUMBER STRING
// STRING passes through COM_Parse, so one token is capped at 255 bytes
// (256 with the NUL). The client runs a small .func listener that registers
// sp_sc_on_change handlers on those cvars and executes each arrival.
//
// This header owns everything that does NOT need the engine:
//   - validation + escaping of parser-produced chunks,
//   - parsing a func_parser/rfm_parser .cfg into (name, value) pairs,
//   - sequencing those pairs into INFO/DATA/CMD/DONE sends with lane
//     rotation, paced per_tick/interval batches,
//   - the per-slot job state machine.
//
// The engine binding (edict scan, Buddy_StuffText sends, SV_Frame ticker,
// ClientCommand ACK snoop, console commands) lives in clsv.cpp and only
// feeds this machine config + observations.
//
// Wire format (all sends are stufftext `set _sp_cl_sv_N "text"`; the client
// only ever runs `sp_sc_exec_cvar <lane>` per arrival plus a counter, so the
// listener needs NO text parsing, NO split, NO names):
//   INFO  cvar 98 : "<gen>"                        (job tag, resets counter)
//   DATA  lane    : "sset <name> <escaped-value>"  (define, still %-encoded)
//   CMD   lane    : "sp_sc_cvar_unescape <n> <n>"   (decode in place)
//   CMD   lane    : "sp_sc_func_load_cvar <entry>"  (activate function)
//   DONE  cvar 99 : "<gen>"                        (triggers ack + echo)
//
// Why sset + full escaping: the DATA text is the VALUE of an outer
// `set _sp_cl_sv_L "..."`, so it must contain no raw `"` (nesting would
// truncate the token). sset takes the whole value as ONE token, so spaces
// must go (%20) or the outer set would see argc>3 and reject the line.
// `;` must go (%3B) or it would split the inner command at exec time.
// `#`/`$` must go (%23/%24) or the client's token-level substitution would
// rewrite content at define time instead of at func runtime. The decoder
// (sp_sc_cvar_unescape, "%hh 00..ff") is generic, so all of these round-trip
// with the single unescape CMD the server appends per chunk. Tab would split
// tokens too, hence %09. The input is assumed to be parser output, i.e.
// `%`, `"`, newline are ALREADY %-encoded; raw `"`, CR, LF are rejected.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace clsv {

constexpr int kMinLanes = 1;
constexpr int kMaxLanes = 90;  // data cvars _sp_cl_sv_0 .. _sp_cl_sv_89
constexpr int kDefaultLanes = 4;
constexpr int kInfoCvar = 98;  // reserved: job tag channel
constexpr int kDoneCvar = 99;  // reserved: completion channel

constexpr int kMaxSlots = 64;

// Loader caps (v1: single INFO-less protocol, names live server-side only,
// so these bound memory, not the wire).
constexpr std::size_t kMaxChunks = 1024;  // DATA pairs per content
constexpr std::size_t kMaxEntries = 64;   // func entry points per content
constexpr std::size_t kMaxNameLen = 48;   // cvar name length
constexpr std::size_t kMaxLineLen = 4096;  // sanity cap per loader line

// Wire caps. One stufftext line is `set _sp_cl_sv_NN "text"`; COM_Parse
// caps every token at 255 (+NUL). Keep the whole line <= 250 so the inner
// text token always clears the limit with margin.
constexpr std::size_t kWireCap = 250;
// Max %-escaped payload bytes per DATA chunk (default; live-tunable).
constexpr int kMaxEscMin = 32;
constexpr int kMaxEscCap = 240;
constexpr int kMaxEscDefault = 200;

constexpr int kPerTickMin = 1;
constexpr int kPerTickCap = 12;  // hard cap; >6/packet overflows MP (README)
constexpr int kPerTickDefault = 2;
constexpr int kIntervalMinFrames = 1;
constexpr int kIntervalCapFrames = 600;
constexpr int kIntervalDefaultFrames = 1;

struct ClsvConfig {
    int lanes = kDefaultLanes;
    int perTick = kPerTickDefault;
    int intervalFrames = kIntervalDefaultFrames;
    int maxEsc = kMaxEscDefault;
};

inline ClsvConfig SanitizeConfig(ClsvConfig c) {
    if (c.lanes < kMinLanes) c.lanes = kMinLanes;
    if (c.lanes > kMaxLanes) c.lanes = kMaxLanes;
    if (c.perTick < kPerTickMin) c.perTick = kPerTickMin;
    if (c.perTick > kPerTickCap) c.perTick = kPerTickCap;
    if (c.intervalFrames < kIntervalMinFrames) c.intervalFrames = kIntervalMinFrames;
    if (c.intervalFrames > kIntervalCapFrames) c.intervalFrames = kIntervalCapFrames;
    if (c.maxEsc < kMaxEscMin) c.maxEsc = kMaxEscMin;
    if (c.maxEsc > kMaxEscCap) c.maxEsc = kMaxEscCap;
    return c;
}

// Escape one parser-produced value for the single-token sset form.
// Returns false (with err) when the input holds bytes that can never
// survive the nesting: raw `"`, CR or LF (feed the file through
// func_parser/rfm_parser first so those arrive %-encoded).
inline bool EscapeForSset(const std::string& in, std::string& out, std::string& err) {
    out.clear();
    out.reserve(in.size() + 16);
    static const char* kHex = "0123456789ABCDEF";
    for (std::size_t i = 0; i < in.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c == 0) {
            err = "value holds a NUL byte (MSG_WriteString would truncate it)";
            return false;
        }
        if (c == '"' || c == '\n' || c == '\r') {
            err = "value holds a raw quote/CR/LF byte; re-run the file through";
            err += " func_parser/rfm_parser so it arrives %-encoded";
            return false;
        }
        unsigned char code = 0;
        bool esc = true;
        switch (c) {
            case ' ': code = 0x20; break;
            case '\t': code = 0x09; break;
            case ';': code = 0x3B; break;
            case '#': code = 0x23; break;
            case '$': code = 0x24; break;
            default: esc = false; break;
        }
        if (!esc) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[(code >> 4) & 0xF]);
            out.push_back(kHex[code & 0xF]);
        }
    }
    return true;
}

inline bool IsCvarNameChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '~' || c == '.' || c == '-';
}

struct ClsvContent {
    std::string name;                       // file basename, logs only
    std::vector<std::string> names;         // DATA cvar names, in send order
    std::vector<std::string> values;        // parser-escaped values, parallel
    std::vector<std::string> entries;       // sp_sc_func_load_cvar targets
};

// Parse one loader file (func_parser/rfm_parser .cfg output) into content.
// blank lines, `//` comments and `sp_sc_cvar_unescape` lines are ignored
// (this pipeline emits its own unescape CMDs). Anything else is an error
// with a line number so bad input fails loudly instead of half-loading.
inline bool ParseLoader(const std::string& text, const std::string& name, int maxEsc,
                        ClsvContent& out, std::string& err) {
    out = ClsvContent();
    out.name = name;
    std::size_t pos = 0;
    unsigned lineNo = 0;
    auto fail = [&](const std::string& what) {
        err = "line " + std::to_string(lineNo) + ": " + what;
        return false;
    };
    while (pos <= text.size()) {
        std::size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::size_t s = line.find_first_not_of(" \t");
        if (s == std::string::npos) continue;  // blank
        if (line.compare(s, 2, "//") == 0) continue;
        if (line.size() > kMaxLineLen) return fail("line too long");
        std::string body = line.substr(s);
        if (body.compare(0, 4, "set ") == 0) {
            std::size_t q1 = body.find('"', 4);
            std::size_t q2 = body.rfind('"');
            if (q1 == std::string::npos || q2 == std::string::npos || q2 <= q1)
                return fail("malformed set line (need set NAME \"VALUE\")");
            std::string cname = body.substr(4, q1 - 4);
            while (!cname.empty() && (cname.back() == ' ' || cname.back() == '\t'))
                cname.pop_back();
            if (cname.empty() || cname.size() > kMaxNameLen) return fail("bad cvar name");
            for (char c : cname)
                if (!IsCvarNameChar(c)) return fail("bad cvar name char");
            std::string value = body.substr(q1 + 1, q2 - q1 - 1);
            if (value.find('"') != std::string::npos)
                return fail("raw quote inside VALUE; re-run through the parser");
            if (static_cast<int>(value.size()) > maxEsc * 4)
                return fail("VALUE absurdly long, refusing");
            std::string esc;
            if (!EscapeForSset(value, esc, err)) return fail(err);
            if (static_cast<int>(esc.size()) > maxEsc)
                return fail("escaped VALUE (" + std::to_string(esc.size()) +
                             "B) exceeds chunk cap " + std::to_string(maxEsc));
            if (out.names.size() >= kMaxChunks) return fail("too many chunks");
            out.names.push_back(cname);
            out.values.push_back(value);
            continue;
        }
        if (body.compare(0, 21, "sp_sc_func_load_cvar ") == 0) {
            std::size_t p = 21;
            while (p < body.size()) {
                while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) ++p;
                if (p >= body.size()) break;
                std::size_t q = p;
                while (q < body.size() && body[q] != ' ' && body[q] != '\t') ++q;
                std::string entry = body.substr(p, q - p);
                if (entry.empty() || entry.size() > kMaxNameLen) return fail("bad entry name");
                for (char c : entry)
                    if (!IsCvarNameChar(c)) return fail("bad entry name char");
                if (out.entries.size() >= kMaxEntries) return fail("too many entries");
                out.entries.push_back(entry);
                p = q;
            }
            if (out.entries.empty()) return fail("empty load line");
            continue;
        }
        if (body.compare(0, 20, "sp_sc_cvar_unescape ") == 0) continue;  // we emit our own
        return fail("unsupported line (only `set`, `sp_sc_func_load_cvar`, "
                    "`sp_sc_cvar_unescape`, comments)");
    }
    if (out.names.empty()) {
        err = "no `set` chunks found; need func_parser/rfm_parser .cfg input";
        return false;
    }
    return true;
}

struct ClsvSend {
    int cvar = 0;          // 0..89 data lane, 98 INFO, 99 DONE
    std::string text;      // inner text (the VALUE of the outer set)
};

// One full job: INFO, DATA ring, per-chunk CMD unescapes, entry LOADs, DONE.
inline bool BuildPlan(const ClsvContent& content, const ClsvConfig& cfgIn, unsigned gen,
                      std::vector<ClsvSend>& plan, std::string& err) {
    plan.clear();
    const ClsvConfig cfg = SanitizeConfig(cfgIn);
    if (content.names.empty()) {
        err = "empty content";
        return false;
    }
    auto pushChecked = [&](int cvar, const std::string& text) -> bool {
        // spcl gate (see ClsvWirePassesSpclGate): data lanes are numeric
        // 0..89, control lives on 98/99. Anything else never reaches Cbuf.
        const bool laneOk = (cvar >= 0 && cvar <= kMaxLanes - 1) || cvar == kInfoCvar ||
                            cvar == kDoneCvar;
        if (!laneOk) {
            err = "cvar index outside the spcl whitelist shape";
            return false;
        }
        // The gate scans the whole line for `;` and requires a single
        // trailing `\n`, so the inner text must hold neither (our escaping
        // guarantees it; this is the backstop).
        if (text.find(';') != std::string::npos || text.find('\n') != std::string::npos ||
            text.find('\r') != std::string::npos) {
            err = "send text holds a raw ;/CR/LF byte the spcl gate would block";
            return false;
        }
        // Outer wire: set _sp_cl_sv_NN "text"  (5 + 1..2 + 2 + text + 1)
        const std::size_t wire = text.size() + 16;
        if (wire > kWireCap) {
            err = "wire line (" + std::to_string(wire) +
                  "B) exceeds cap; lower _sofbuddy_clsv_chunk or re-parse smaller";
            return false;
        }
        ClsvSend s;
        s.cvar = cvar;
        s.text = text;
        plan.push_back(s);
        return true;
    };
    if (!pushChecked(kInfoCvar, std::to_string(gen))) return false;
    // One continuous lane rotation across DATA + CMD phases: with
    // lanes >= perTick no tick ever touches the same cvar twice, which
    // keeps the client's per-arrival handler order unambiguous.
    int lane = 0;
    for (std::size_t i = 0; i < content.names.size(); ++i) {
        std::string esc, escErr;
        if (!EscapeForSset(content.values[i], esc, escErr)) {
            err = "chunk " + std::to_string(i) + ": " + escErr;
            return false;
        }
        if (static_cast<int>(esc.size()) > cfg.maxEsc) {
            err = "chunk " + std::to_string(i) + " escaped to " + std::to_string(esc.size()) +
                  "B > cap " + std::to_string(cfg.maxEsc);
            return false;
        }
        if (!pushChecked(lane, "sset " + content.names[i] + " " + esc)) return false;
        lane = (lane + 1) % cfg.lanes;
    }
    for (std::size_t i = 0; i < content.names.size(); ++i) {
        const std::string cmd = "sp_sc_cvar_unescape " + content.names[i] + " " + content.names[i];
        if (!pushChecked(lane, cmd)) return false;
        lane = (lane + 1) % cfg.lanes;
    }
    for (const std::string& e : content.entries) {
        if (!pushChecked(lane, "sp_sc_func_load_cvar " + e)) return false;
        lane = (lane + 1) % cfg.lanes;
    }
    if (!pushChecked(kDoneCvar, std::to_string(gen))) return false;
    return true;
}

// Outer stufftext line for one send. Trailing \n is part of svc_stufftext
// framing (see stufftext/BuildStuffText).
inline std::string FormatWire(const ClsvSend& send) {
    return "set _sp_cl_sv_" + std::to_string(send.cvar) + " \"" + send.text + "\"\n";
}

// Faithful model of the spcl.dll stufftext gate (StuffText_hook) for our
// sends: `set _sp_cl_sv_` + 1-2 digits + space, no `;` anywhere, exactly
// one `\n` as the final byte (followed by NUL). Anything else prints
// "SP Blocked server command" client-side and never reaches Cbuf.
inline bool ClsvWirePassesSpclGate(const std::string& wire) {
    constexpr char kPrefix[] = "set _sp_cl_sv_";
    constexpr std::size_t kPre = sizeof(kPrefix) - 1;  // 14
    if (wire.size() < kPre + 2)
        return false;
    if (wire.compare(0, kPre, kPrefix) != 0)
        return false;
    auto isdig = [](char c) { return c >= '0' && c <= '9'; };
    std::size_t i = kPre;
    if (!isdig(wire[i]))
        return false;
    ++i;
    if (i < wire.size() && wire[i] == ' ') {
        ++i;
    } else if (i + 1 < wire.size() && isdig(wire[i]) && wire[i + 1] == ' ') {
        i += 2;
    } else {
        return false;  // 3+ digits or missing space: not our cvar shape
    }
    (void)i;
    bool seenNl = false;
    for (std::size_t k = 0; k < wire.size(); ++k) {
        if (wire[k] == ';')
            return false;
        if (wire[k] == '\n') {
            if (seenNl || k + 1 != wire.size())
                return false;
            seenNl = true;
        }
    }
    return seenNl;
}

enum ClsvPhase : std::uint8_t { CLSV_IDLE = 0, CLSV_SENDING = 1, CLSV_LINGER = 2 };

struct ClsvJob {
    ClsvPhase phase = CLSV_IDLE;
    unsigned gen = 0;
    std::size_t pos = 0;                 // next unsent index into plan
    unsigned long long lastTick = 0;     // SV_Frame counter of last batch
    bool firstBatch = true;
    std::uint32_t doneMs = 0;            // GetTickCount when DONE went out
    bool acked = false;
    int ackGot = -1;
    std::vector<ClsvSend> plan;
};

inline void ClsvJobReset(ClsvJob& job) {
    job.phase = CLSV_IDLE;
    job.gen = 0;
    job.pos = 0;
    job.lastTick = 0;
    job.firstBatch = true;
    job.doneMs = 0;
    job.acked = false;
    job.ackGot = -1;
    job.plan.clear();
}

inline void ClsvJobBegin(ClsvJob& job, unsigned gen, const std::vector<ClsvSend>& plan,
                         unsigned long long nowTick) {
    ClsvJobReset(job);
    job.phase = CLSV_SENDING;
    job.gen = gen;
    job.plan = plan;
    job.lastTick = nowTick;
    job.firstBatch = true;  // first tick sends immediately (see ClsvBatchCount)
}

// How many sends to emit this tick (0 when the interval has not elapsed or
// the job is not in SENDING). Pure: the caller stuffs plan[pos..pos+n).
inline std::size_t ClsvBatchCount(const ClsvJob& job, const ClsvConfig& cfgIn,
                                  unsigned long long nowTick) {
    const ClsvConfig cfg = SanitizeConfig(cfgIn);
    if (job.phase != CLSV_SENDING) return 0;
    if (job.pos >= job.plan.size()) return 0;
    if (!job.firstBatch) {
        const unsigned long long elapsed = nowTick - job.lastTick;
        if (elapsed < static_cast<unsigned long long>(cfg.intervalFrames)) return 0;
    }
    std::size_t left = job.plan.size() - job.pos;
    std::size_t n = static_cast<std::size_t>(cfg.perTick);
    return n < left ? n : left;
}

inline bool ClsvElapsed(std::uint32_t nowMs, std::uint32_t startMs, std::uint32_t spanMs) {
    return static_cast<std::uint32_t>(nowMs - startMs) >= spanMs;
}

}  // namespace clsv
