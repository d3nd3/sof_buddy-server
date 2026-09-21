#pragma once

// Pure, host-testable audit validators for the spcl SuffText_hook.
// No engine / windows / generated headers here on purpose: tools/tests/
// builds this for the host, and stufftext_variants.cpp binds it to
// Cmd_AddCommand handlers on the server.
//
// V1 is a faithful transcription of the original guard (prefix allowlist +
// ';' scan + single-trailing-'\n' check). V2..V6 each isolate ONE hardening
// point from the audit so hypotheses can be confirmed independently:
//
//   V1 baseline      - original logic as-is (lengths imply literal text,
//                      see .cpp; verify against rodata)
//   V2 strict-values - V1 + tokenize/argc + charset, reject $ " ' ` \
//                      (pre- vs post-expansion gap)
//   V3 quote-carry   - persistent open-quote state across packets
//   V4 state-gate    - precache / cmd-state cmds require inPrecahce flag,
//                      attackLoop bypass removed
//   V5 check-parser  - hardened .check parser (unsigned-char ctype,
//                      length cap, explicit gl_driver quirk)
//   V6 canonical     - strict \n / \r / interior-newline handling

#include <stddef.h>

struct StuffAuditResult {
    bool allow;          // true = cbuf_addtext, false = Com_Printf blocked
    const char* reason;  // static literal, never null
};

// Length-aware core (handles interior NUL for host fuzzing).
// Engine wrappers pass len = strlen(s).
StuffAuditResult StuffAudit_V1(const char* s, size_t len, bool attackLoop);
StuffAuditResult StuffAudit_V2_Strict(const char* s, size_t len, bool attackLoop);
StuffAuditResult StuffAudit_V4_StateGated(const char* s, size_t len, bool attackLoop,
                                          bool inPrecache);
StuffAuditResult StuffAudit_V5_Check(const char* s, size_t len);
StuffAuditResult StuffAudit_V6_Canonical(const char* s, size_t len);

// V3: quote tracker. Counts '"' (Q2 Cbuf has no backslash escapes).
// Stock Q2 has NO cross-packet quote state (Cbuf_Execute resets quotes per
// line, cmd.c:208; unmatched quotes are discarded in MacroExpand,
// cmd.c:602-606). *openQuote here is a STRICTER-than-stock harness flag for
// logging split-payload attempts, not engine behavior. Starts false.
StuffAuditResult StuffAudit_V3_QuoteCarry(const char* s, size_t len, bool attackLoop,
                                          bool* openQuote);

// Test-input unescaping for console use: "\\n"->LF, "\\r"->CR, "\\\\"->'\',
// "\\xHH"->byte. Returns output length (<= input length). Never expands $.
size_t StuffAudit_Unescape(char* dst, size_t dstCap, const char* src, size_t srcLen);

// Helpers exposed for unit tests.
bool StuffAudit_HasSingleTrailingLF(const char* s, size_t len);
bool StuffAudit_ContainsByte(const char* s, size_t len, char b);
unsigned StuffAudit_CountQuotes(const char* s, size_t len);
