// Host tests for src/features/stufftext/stufftext_audit.cpp.
// Pure validator checks with generic metachar filter vectors (no exploits).
#include <cstdio>
#include <cstring>

#include "stufftext_audit.h"

static int g_fails = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("FAIL %d: %s\n", __LINE__, #cond);                     \
            ++g_fails;                                                         \
        }                                                                      \
    } while (0)

static StuffAuditResult A1(const char* s) {
    return StuffAudit_V1(s, strlen(s), false);
}

int main() {
    // V1 baseline transcription.
    CHECK(A1("password foo\n").allow);
    CHECK(!A1("password foo;bar\n").allow);
    CHECK(!A1("password foo").allow);
    CHECK(!A1("password foo\nbar\n").allow);
    CHECK(A1("changing\n").allow);
    CHECK(A1("reconnect\n").allow);
    CHECK(A1("precache 1\n").allow);  // documents point 3: too broad
    CHECK(A1("set _sp_cl_sv_1 foo\n").allow);
    CHECK(A1("set _sp_cl_sv_12 foo\n").allow);
    CHECK(!A1("set _sp_cl_sv_x foo\n").allow);
    CHECK(!A1("evilcommand\n").allow);
    CHECK(StuffAudit_V1("anything\n", 9, true).allow);  // attackLoop bypass

    // Valid .check shape: "cmd .check <8alnum> <var> #<var>\n".
    CHECK(A1("cmd .check 12345678 cl_foo #cl_foo\n").allow);
    CHECK(!A1("cmd .check 1234 cl_foo #cl_foo\n").allow);
    CHECK(!A1("cmd .check 12345678 badprefix #badprefix\n").allow);

    // V2 strict: expansion/quote metachars + argc.
    CHECK(StuffAudit_V2_Strict("password foo\n", 13, false).allow);
    CHECK(!StuffAudit_V2_Strict("password foo$bar\n", 17, false).allow);
    CHECK(!StuffAudit_V2_Strict("password foo\"bar\n", 17, false).allow);
    CHECK(!StuffAudit_V2_Strict("password foo\\bar\n", 17, false).allow);
    CHECK(!StuffAudit_V2_Strict("set _sp_cl_sv_1 a b c d\n", 23, false).allow);

    // V3 quote-carry across packets.
    {
        bool open = false;
        CHECK(!StuffAudit_V3_QuoteCarry("password foo\"\n", 14, false, &open)
                   .allow);
        CHECK(open);
        CHECK(!StuffAudit_V3_QuoteCarry("password bar\n", 13, false, &open)
                   .allow);
        open = false;
        CHECK(StuffAudit_V3_QuoteCarry("password bar\n", 13, false, &open)
                  .allow);
    }

    // V4 state gate + no attackLoop bypass.
    CHECK(!StuffAudit_V4_StateGated("precache 1\n", 11, false, false).allow);
    CHECK(StuffAudit_V4_StateGated("precache 1\n", 11, false, true).allow);
    CHECK(!StuffAudit_V4_StateGated("cmd configstrings\n", 18, false, false)
               .allow);
    CHECK(!StuffAudit_V4_StateGated("evilcommand\n", 12, true, false).allow);
    CHECK(StuffAudit_V1("evilcommand\n", 12, true).allow);  // V1 would allow

    // V5 .check hardened parser.
    CHECK(StuffAudit_V5_Check("cmd .check 12345678 cl_foo #cl_foo\n",
                              strlen("cmd .check 12345678 cl_foo #cl_foo\n"))
              .allow);
    CHECK(!StuffAudit_V5_Check(
               "cmd .check 1234 cl_foo #cl_foo\n",
               strlen("cmd .check 1234 cl_foo #cl_foo\n"))
               .allow);
    {
        char longvar[128];
        snprintf(longvar, sizeof(longvar),
                 "cmd .check 12345678 cl_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa "
                 "#cl_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n");
        CHECK(!StuffAudit_V5_Check(longvar, strlen(longvar)).allow);
    }
    CHECK(!StuffAudit_V5_Check("password foo\n", 13).allow);

    // V6 canonical LF/CR handling.
    CHECK(StuffAudit_V6_Canonical("password foo\n", 13).allow);
    CHECK(!StuffAudit_V6_Canonical("password foo\r\n", 14).allow);
    CHECK(!StuffAudit_V6_Canonical("password foo\nbar\n", 17).allow);
    CHECK(!StuffAudit_V6_Canonical("password foo", 12).allow);
    CHECK(!StuffAudit_V6_Canonical("password foo;\n", 14).allow);

    // Unescape helper for console test input.
    {
        char dst[32];
        size_t n = StuffAudit_Unescape(dst, sizeof(dst), "a\\n", 3);
        CHECK(n == 2 && dst[0] == 'a' && dst[1] == '\n');
        n = StuffAudit_Unescape(dst, sizeof(dst), "a\\x3b", 5);
        CHECK(n == 2 && dst[0] == 'a' && dst[1] == ';');
        n = StuffAudit_Unescape(dst, sizeof(dst), "a\\r", 3);
        CHECK(n == 2 && dst[0] == 'a' && dst[1] == '\r');
    }

    if (g_fails == 0)
        std::printf("stufftext_audit: all checks passed\n");
    return g_fails;
}
