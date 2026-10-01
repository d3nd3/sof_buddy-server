#include "cvarview_logic.h"

#include <cstdio>
#include <cstring>

static int fails = 0;

#define CHECK(cond, msg)                    \
    do {                                    \
        if (!(cond)) {                      \
            std::printf("FAIL: %s\n", msg); \
            ++fails;                        \
        }                                   \
    } while (0)

static void fill(CvarLine& row, const char* name, const char* value) {
    CvarCopy(row.name, kCvarNameCap, name);
    CvarCopy(row.value, kCvarValueCap, value);
    CvarCopy(row.deflt, kCvarValueCap, "0");
    row.desc = "log when one clamp drops at least this";
}

static void test_pages() {
    CvarLine rows[kCvarPerPage + 3] = {};
    for (int i = 0; i < kCvarPerPage; ++i)
        fill(rows[i], "_sofbuddy_clamp_notify_ms", "1");
    fill(rows[kCvarPerPage], "_sofbuddy_clamp_window", "2");
    fill(rows[kCvarPerPage + 1], "_sofbuddy_lowclamp_checks", "4");
    fill(rows[kCvarPerPage + 2], "_sofbuddy_reldef", "1");
    CvarPage pages[8];
    const int n = CvarBuildPages(rows, kCvarPerPage + 3, pages, 8);
    CHECK(n == 3, "clamp splits, then reldef");
    CHECK(pages[0].count == kCvarPerPage, "first clamp page full");
    CHECK(std::strcmp(pages[0].group, "clamp") == 0, "clamp group");
    CHECK(std::strcmp(pages[1].group, "clamp") == 0, "lowclamp joins clamp");
    CHECK(pages[1].count == 2, "rest of clamp");
    CHECK(std::strcmp(pages[2].group, "reldef") == 0, "reldef page");
    const CvarPick clamp2 = CvarPickGroup(pages, n, "CLAMP", 1);
    CHECK(clamp2.found && clamp2.index == 1 && clamp2.local == 1 && clamp2.localCount == 2,
          "clamp page 2");
    const CvarPick rel = CvarPickGroup(pages, n, "reldef", 5);
    CHECK(rel.found && rel.index == 2 && rel.local == 0, "reldef clamps to its only page");
    CHECK(!CvarPickGroup(pages, n, "nope", 0).found, "unknown category");
    CvarCat cats[4];
    const int nc = CvarBuildCats(pages, n, cats, 4);
    CHECK(nc == 2, "two categories");
    CHECK(cats[0].pages == 2 && std::strcmp(cats[0].group, "clamp") == 0, "clamp spans pages");
    CHECK(cats[1].pages == 1, "reldef one page");
    int page = 0;
    CHECK(CvarParsePage("2", &page) && page == 1, "page arg");
    CHECK(!CvarParsePage("0", &page), "page is 1-based");
}

static void test_catalog() {
    for (int i = 1; i < kCvarInfoCount; ++i)
        CHECK(std::strcmp(kCvarInfo[i - 1].name, kCvarInfo[i].name) < 0, "catalog sorted");
    const CvarInfo* info = CvarLookup("_sofbuddy_clamp_notify_ms");
    CHECK(info && std::strcmp(info->def, "0") == 0 && info->desc && info->desc[0], "notify default");
    CHECK(CvarLookup("_sofbuddy_nope") == nullptr, "missing cvar");
    CvarLine row = {};
    CvarCopy(row.name, kCvarNameCap, "_sofbuddy_reldef_reserve");
    CvarAnnotate(row);
    CHECK(std::strcmp(row.deflt, "256") == 0, "annotated default");
    CHECK(std::strstr(row.desc, "reliable") != nullptr, "annotated description");
}

static void test_render_fits() {
    CvarLine rows[kCvarPerPage] = {};
    for (int i = 0; i < kCvarPerPage; ++i)
        fill(rows[i], "_sofbuddy_clamp_broadcast_interval_padding", "262144");
    CvarPage pages[2];
    CvarBuildPages(rows, kCvarPerPage, pages, 2);
    MgCanvas c;
    CvarRender(c, 0, 1, pages[0], rows);
    CHECK(c.len > 0 && c.len < kMgLayoutCap, "layout fits");
    CHECK(std::strstr(c.text, "[clamp]") != nullptr, "category header");
    CHECK(std::strstr(c.text, "1 / 1") == nullptr, "single page has no number");
    CHECK(std::strstr(c.text, "def 0") != nullptr, "default shown");
    CHECK(std::strstr(c.text, "log when one clamp") != nullptr, "description shown");
    CHECK(std::strstr(c.text, "clamp_broadcast_interval_padding") != nullptr, "tail shown");
    CHECK(std::strstr(c.text, "_sofbuddy_") == nullptr, "prefix dropped");
    CHECK(std::strstr(c.text, ".mg_cvars clamp") != nullptr, "category command");
    CHECK(std::strstr(c.text, "<page>") == nullptr, "single page omits page arg");
    CvarRender(c, 1, 2, pages[0], rows);
    CHECK(std::strstr(c.text, "[clamp]  2 / 2") != nullptr, "category page number");
    CHECK(std::strstr(c.text, ".mg_cvars clamp <page>") != nullptr, "page command");

    CvarCat many[kCvarCatsPerPage + 1] = {};
    for (int i = 0; i < kCvarCatsPerPage + 1; ++i) {
        std::snprintf(many[i].group, sizeof(many[i].group), "g%d", i);
        many[i].pages = i == 0 ? 2 : 1;
    }
    CvarRenderCats(c, many, kCvarCatsPerPage + 1, 0);
    CHECK(c.len > 0 && c.len < kMgLayoutCap, "category layout fits");
    CHECK(std::strstr(c.text, "[categories]  1 / 2") != nullptr, "category pages");
    CHECK(std::strstr(c.text, "g0  2 pages") != nullptr, "multi-page category");
    CHECK(std::strstr(c.text, ".mg_cvars <category>") != nullptr, "category hint");
    CHECK(std::strstr(c.text, ".mg_cvars <page>") != nullptr, "index page hint");
    CvarRenderCats(c, many, kCvarCatsPerPage + 1, 1);
    CHECK(std::strstr(c.text, "g8") != nullptr, "second category page");
    CHECK(std::strstr(c.text, "g0") == nullptr, "first page not on second");
}

int main() {
    test_pages();
    test_catalog();
    test_render_fits();
    if (fails)
        std::printf("%d test(s) failed\n", fails);
    else
        std::printf("ok\n");
    return fails ? 1 : 0;
}
