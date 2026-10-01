#pragma once

// Page layout for the sof_buddy cvar browser. Pure string work.

#include "cvarview_catalog.h"
#include "../minigames_api.h"

#include <cstdio>
#include <cstring>

constexpr int kCvarPerPage = 4;
constexpr int kCvarCatsPerPage = 8;
constexpr int kCvarNameCap = 64;
constexpr int kCvarValueCap = 16;
constexpr int kCvarMaxPages = 64;

struct CvarLine {
    char name[kCvarNameCap];
    char value[kCvarValueCap];
    char deflt[kCvarValueCap];
    const char* desc = nullptr;
};

struct CvarPage {
    int begin = 0;
    int count = 0;
    char group[16] = {};
};

struct CvarPick {
    int index = 0;
    int local = 0;
    int localCount = 0;
    bool found = false;
};

struct CvarCat {
    char group[16] = {};
    int pages = 0;
};

inline void CvarCopy(char* dst, int cap, const char* src) {
    if (!dst || cap <= 0)
        return;
    dst[0] = '\0';
    if (!src)
        return;
    int i = 0;
    for (; src[i] && i < cap - 1; ++i)
        dst[i] = src[i];
    dst[i] = '\0';
}

inline const char* CvarCanon(const char* group) {
    if (!group)
        return "";
    if (!std::strcmp(group, "lowclamp") || !std::strcmp(group, "lowclamps") ||
        !std::strcmp(group, "highclamps"))
        return "clamp";
    if (!std::strcmp(group, "pipe"))
        return "clsv";
    return group;
}

inline void CvarGroup(const char* name, char* out, int cap) {
    const char* p = name ? name : "";
    if (std::strncmp(p, "_sofbuddy_", 10) == 0)
        p += 10;
    else if (std::strncmp(p, "_sb_internal_", 13) == 0)
        p += 13;
    char raw[16];
    int i = 0;
    for (; p[i] && p[i] != '_' && i < static_cast<int>(sizeof(raw)) - 1; ++i)
        raw[i] = p[i];
    raw[i] = '\0';
    CvarCopy(out, cap, raw[0] ? CvarCanon(raw) : "sofbuddy");
}

inline int CvarBuildPages(const CvarLine* rows, int n, CvarPage* pages, int cap) {
    if (!rows || !pages || cap <= 0 || n <= 0)
        return 0;
    int pi = 0;
    int i = 0;
    while (i < n && pi < cap) {
        CvarGroup(rows[i].name, pages[pi].group, static_cast<int>(sizeof(pages[pi].group)));
        int j = i + 1;
        while (j < n && j - i < kCvarPerPage) {
            char g[16];
            CvarGroup(rows[j].name, g, static_cast<int>(sizeof(g)));
            if (std::strcmp(g, pages[pi].group) != 0)
                break;
            ++j;
        }
        pages[pi].begin = i;
        pages[pi].count = j - i;
        ++pi;
        i = j;
    }
    return pi;
}

inline int CvarBuildCats(const CvarPage* pages, int n, CvarCat* cats, int cap) {
    if (!pages || !cats || cap <= 0 || n <= 0)
        return 0;
    int c = 0;
    for (int i = 0; i < n; ++i) {
        if (c > 0 && std::strcmp(cats[c - 1].group, pages[i].group) == 0) {
            ++cats[c - 1].pages;
            continue;
        }
        if (c >= cap)
            break;
        CvarCopy(cats[c].group, static_cast<int>(sizeof(cats[c].group)), pages[i].group);
        cats[c].pages = 1;
        ++c;
    }
    return c;
}

inline int CvarCatPageCount(int n) {
    if (n <= 0)
        return 1;
    return (n + kCvarCatsPerPage - 1) / kCvarCatsPerPage;
}

inline bool CvarWord(const char* a, const char* b) {
    if (!a || !b)
        return false;
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z')
            ca = static_cast<char>(ca + 32);
        if (cb >= 'A' && cb <= 'Z')
            cb = static_cast<char>(cb + 32);
        if (ca != cb)
            return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

// 1-based digits. False when missing or not a page number.
inline bool CvarParsePage(const char* s, int* page) {
    if (!s || !s[0] || !page)
        return false;
    int n = 0;
    for (const char* p = s; *p; ++p) {
        if (*p < '0' || *p > '9')
            return false;
        n = n * 10 + (*p - '0');
    }
    if (n < 1)
        return false;
    *page = n - 1;
    return true;
}

// `page` is 0-based within the category. Empty group selects the first.
inline CvarPick CvarPickGroup(const CvarPage* pages, int n, const char* group, int page) {
    CvarPick pick;
    if (!pages || n <= 0)
        return pick;
    if (!group || !group[0])
        group = pages[0].group;
    int first = -1;
    int count = 0;
    for (int i = 0; i < n; ++i) {
        if (!CvarWord(pages[i].group, group))
            continue;
        if (first < 0)
            first = i;
        ++count;
    }
    if (first < 0)
        return pick;
    if (page < 0)
        page = 0;
    if (page >= count)
        page = count - 1;
    pick.index = first + page;
    pick.local = page;
    pick.localCount = count;
    pick.found = true;
    return pick;
}

inline const CvarInfo* CvarLookup(const char* name) {
    if (!name || !name[0])
        return nullptr;
    int lo = 0;
    int hi = kCvarInfoCount;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        const int cmp = std::strcmp(name, kCvarInfo[mid].name);
        if (cmp == 0)
            return &kCvarInfo[mid];
        if (cmp < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return nullptr;
}

inline void CvarAnnotate(CvarLine& row) {
    const CvarInfo* info = CvarLookup(row.name);
    if (!info) {
        CvarCopy(row.deflt, kCvarValueCap, "?");
        row.desc = "unlisted";
        return;
    }
    CvarCopy(row.deflt, kCvarValueCap, info->def ? info->def : "?");
    row.desc = info->desc ? info->desc : "";
}

inline const char* CvarTail(const char* name) {
    if (name && std::strncmp(name, "_sofbuddy_", 10) == 0)
        return name + 10;
    if (name && std::strncmp(name, "_sb_internal_", 13) == 0)
        return name + 13;
    return name ? name : "";
}

inline void CvarRender(MgCanvas& canvas, int local, int localCount, const CvarPage& span,
                       const CvarLine* rows) {
    MgCanvasClear(canvas);
    char line[96];
    MgCanvasTc(canvas, kMgColYellow);
    if (localCount <= 0 || !rows) {
        MgCanvasText(canvas, 16, 48, "no sof_buddy cvars");
        return;
    }
    if (localCount > 1)
        std::snprintf(line, sizeof(line), "[%s]  %d / %d", span.group, local + 1, localCount);
    else
        std::snprintf(line, sizeof(line), "[%s]", span.group[0] ? span.group : "cvars");
    MgCanvasText(canvas, 16, 32, line);
    int y = 48;
    for (int i = 0; i < span.count; ++i) {
        const CvarLine& row = rows[span.begin + i];
        char shown[40];
        CvarCopy(shown, static_cast<int>(sizeof(shown)), CvarTail(row.name));
        MgCanvasTc(canvas, kMgColWhite);
        if (!MgCanvasText(canvas, 8, y, shown))
            break;
        char desc[40];
        CvarCopy(desc, static_cast<int>(sizeof(desc)), row.desc ? row.desc : "");
        std::snprintf(line, sizeof(line), "%s  def %s  %s", row.value, row.deflt[0] ? row.deflt : "?",
                      desc);
        MgCanvasTc(canvas, kMgColGreen);
        if (!MgCanvasText(canvas, 8, y + 14, line))
            break;
        y += 32;
    }
    MgCanvasTc(canvas, kMgColYellow);
    if (localCount > 1)
        std::snprintf(line, sizeof(line), ".mg_cvars %s <page>", span.group);
    else
        std::snprintf(line, sizeof(line), ".mg_cvars %s", span.group[0] ? span.group : "<category>");
    MgCanvasText(canvas, 16, y + 6, line);
}

inline void CvarRenderCats(MgCanvas& canvas, const CvarCat* cats, int n, int page) {
    MgCanvasClear(canvas);
    MgCanvasTc(canvas, kMgColYellow);
    if (!cats || n <= 0) {
        MgCanvasText(canvas, 16, 48, "no sof_buddy cvars");
        return;
    }
    const int pages = CvarCatPageCount(n);
    if (page < 0)
        page = 0;
    if (page >= pages)
        page = pages - 1;
    char line[64];
    if (pages > 1)
        std::snprintf(line, sizeof(line), "[categories]  %d / %d", page + 1, pages);
    else
        std::snprintf(line, sizeof(line), "[categories]");
    MgCanvasText(canvas, 16, 32, line);
    const int begin = page * kCvarCatsPerPage;
    int y = 52;
    for (int i = begin; i < n && i < begin + kCvarCatsPerPage; ++i) {
        if (cats[i].pages > 1)
            std::snprintf(line, sizeof(line), "%s  %d pages", cats[i].group, cats[i].pages);
        else
            std::snprintf(line, sizeof(line), "%s", cats[i].group);
        MgCanvasTc(canvas, kMgColWhite);
        if (!MgCanvasText(canvas, 16, y, line))
            break;
        y += 16;
    }
    MgCanvasTc(canvas, kMgColYellow);
    if (pages > 1)
        std::snprintf(line, sizeof(line), ".mg_cvars <category>    .mg_cvars <page>");
    else
        std::snprintf(line, sizeof(line), ".mg_cvars <category>");
    MgCanvasText(canvas, 16, y + 8, line);
}
