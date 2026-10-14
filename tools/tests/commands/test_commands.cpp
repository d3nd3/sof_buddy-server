#include "commands_logic.h"

#include <cstdio>
#include <cstring>

static int fails = 0;

#define CHECK(cond, msg)                     \
    do {                                     \
        if (!(cond)) {                       \
            std::printf("FAIL: %s\n", msg);  \
            ++fails;                         \
        }                                    \
    } while (0)

static void test_catalog() {
    CHECK(kCmdsEntryCount > 40, "catalog too small");
    // No duplicate names, no double quotes (breaks layout string token).
    for (int i = 0; i < kCmdsEntryCount; ++i) {
        CHECK(kCmdsEntries[i].name && kCmdsEntries[i].name[0], "empty name");
        CHECK(!std::strchr(kCmdsEntries[i].name, ' '), "name has space");
        CHECK(!std::strchr(kCmdsEntries[i].name, '"'), "name has quote");
        CHECK(kCmdsEntries[i].blurb && kCmdsEntries[i].blurb[0], "empty blurb");
        CHECK(!std::strchr(kCmdsEntries[i].blurb, '"'), "blurb has quote");
        if (kCmdsEntries[i].syntax)
            CHECK(!std::strchr(kCmdsEntries[i].syntax, '"'), "syntax has quote");
        for (int j = i + 1; j < kCmdsEntryCount; ++j)
            CHECK(std::strcmp(kCmdsEntries[i].name, kCmdsEntries[j].name) != 0,
                  "duplicate command name");
    }
    // Spot-check key commands exist.
    bool has_stufftext = false, has_show = false, has_list = false, has_track = false;
    bool has_mgpush = false, has_ttt = false, has_lagshow = false;
    bool has_mgwords = false;
    for (int i = 0; i < kCmdsEntryCount; ++i) {
        const char* n = kCmdsEntries[i].name;
        if (!std::strcmp(n, "stufftext"))
            has_stufftext = true;
        if (!std::strcmp(n, "cmds_show"))
            has_show = true;
        if (!std::strcmp(n, "cmds_list"))
            has_list = true;
        if (!std::strcmp(n, "sv_tracktime"))
            has_track = true;
        if (!std::strcmp(n, "mg_push"))
            has_mgpush = true;
        if (!std::strcmp(n, "ttt_start"))
            has_ttt = true;
        if (!std::strcmp(n, "lag_show"))
            has_lagshow = true;
        if (kCmdsEntries[i].side && !std::strcmp(kCmdsEntries[i].side, "C"))
            has_mgwords = true;
    }
    CHECK(has_stufftext, "missing stufftext");
    CHECK(has_show, "missing cmds_show");
    CHECK(has_list, "missing cmds_list");
    CHECK(has_track, "missing sv_tracktime");
    CHECK(has_mgpush, "missing mg_push");
    CHECK(has_ttt, "missing ttt_start");
    CHECK(has_lagshow, "missing lag_show");
    CHECK(!has_mgwords, "client words leaked into server catalog");
    CHECK(kCmdsCategoryCount >= 5, "category list too small");
    for (int i = 0; i < kCmdsEntryCount; ++i)
        CHECK(CmdsCategoryFor(kCmdsEntries[i].name) >= 0, "uncategorized command");
    for (int i = 0; i < kCmdsCategoryCount; ++i)
        CHECK(CmdsCategoryEntryCount(i) > 0, "empty category");
    for (int i = 0; i < kCmdsCategoryCount; ++i) {
        CHECK(CmdsCategoryName(i) && CmdsCategoryName(i)[0], "empty category name");
        CHECK(kCmdsCategories[i].desc && kCmdsCategories[i].desc[0], "empty category desc");
        CHECK(!std::strchr(kCmdsCategories[i].desc, '"'), "category desc has quote");
    }
}

static void test_pages() {
    const int n = CmdsCategoryPageCount(0);
    CHECK(n >= 1, "category has no pages");
    CHECK(CmdsClampCategoryPage(0, -1) == 0, "clamp low");
    CHECK(CmdsClampCategoryPage(0, 1 << 30) == n - 1, "clamp high");
    CHECK(CmdsParsePageArg("1", 0) == 0, "page 1");
    CHECK(CmdsParsePageArg("2", 0) == 1, "page 2");
    CHECK(CmdsParsePageArg("next", 0) == 1, "next");
    CHECK(CmdsParsePageArg("nonsense", 0) < 0, "next prefix rejected");
    CHECK(CmdsParsePageArg("prev", 1) == 0, "prev");
    CHECK(CmdsParsePageArg("previous", 1) < 0, "prev prefix rejected");
    CHECK(CmdsParsePageArg("bogus", 0) < 0, "bogus rejected");
    CHECK(CmdsParsePageArg("", 0) < 0, "empty rejected");
}

static void test_render_fits() {
    for (int category = 0; category < kCmdsCategoryCount; ++category) {
        const int n = CmdsCategoryPageCount(category);
        for (int p = 0; p < n; ++p) {
        MgCanvas c;
        CmdsRender(category, p, c);
        CHECK(c.len > 0 && c.len < kMgLayoutCap, "layout fits");
        char want[32];
        std::snprintf(want, sizeof(want), "page %d/%d", p + 1, n);
        CHECK(std::strstr(c.text, want) != nullptr, "page header");
        CHECK(std::strstr(c.text, "SERVER-SIDE COMMANDS") != nullptr, "title");
        }
    }
    MgCanvas c;
    CmdsRenderCategories(c, -1);
    CHECK(std::strstr(c.text, "select a category") != nullptr, "category header");
    CmdsRender(0, 0, c);
    CHECK(std::strstr(c.text, "stufftext") != nullptr, "first page has stufftext");
}

int main() {
    test_catalog();
    test_pages();
    test_render_fits();
    if (fails)
        std::printf("%d test(s) failed\n", fails);
    else
        std::printf("ok\n");
    return fails ? 1 : 0;
}
