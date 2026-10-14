#pragma once

// Pure commands catalog + layout renderer. No engine calls, no windows.h —
// unit-testable on Linux. Rows are single-line "name syntax - blurb" strings
// kept short so 8 rows + title/footer stay under the 1024 canvas cap
// (client layout_string is 0x400).

#include "../minigames_api.h"

#include <cstdio>
#include <cstring>

constexpr int kCmdsPerPage = 8;
constexpr int kCmdsActionCount = 2;

struct CmdsEntry {
    const char* name;    // server console command, or client word
    const char* syntax;  // short args hint, no double quotes
    const char* blurb;   // <= ~36 chars, no double quotes
    const char* side;    // S = server console, C = client console word
};

// Full sof_buddy server-side command catalog.
// Keep syntax/blurb short: rendered rows must stay <= 64 chars.
static const CmdsEntry kCmdsEntries[] = {
    // ---- stufftext (server) ----
    {"stufftext", "<slot|all|name> <cmd>", "stuff text to client", "S"},
    {"stufftext_reconnect", "<slot|cancel>", "chained reconnect fix", "S"},
    {"stufftext_audit_v1", "<candidate>", "hardening audit V1", "S"},
    {"stufftext_audit_v2", "<candidate>", "hardening audit V2", "S"},
    {"stufftext_audit_v3", "<candidate>", "hardening audit V3", "S"},
    {"stufftext_audit_v4", "<candidate>", "hardening audit V4", "S"},
    {"stufftext_audit_v5", "<candidate>", "hardening audit V5", "S"},
    {"stufftext_audit_v6", "<candidate>", "hardening audit V6", "S"},
    {"stufftext_audit_state", "", "audit state dump", "S"},
    // ---- profiles (server) ----
    {"profile_register", "<nick>", "mint fresh guid", "S"},
    {"profile_import", "<guid> <nick>", "import guid+nick", "S"},
    {"profile_set_privilege", "<guid|number|nick> <0|1|2>", "set privilege", "S"},
    {"profile_del", "<guid|number|nick>", "delete roster entry", "S"},
    {"profile_query", "", "diagnose slots", "S"},
    {"profile_signin", "<slot>", "push guid to slot", "S"},
    {"profile_signout", "<slot|guid|number|nick|all>", "sign out one or all", "S"},
    {"profile_get_slot_by_id", "<guid|number>", "slot for guid", "S"},
    {"profile_get_slot_by_nick", "<nick>", "slot for nick", "S"},
    {"profile_save", "", "save roster file", "S"},
    {"profile_load", "", "load roster file", "S"},
    // ---- tracktime (server) ----
    {"sv_tracktime", "[slot]", "frametime table", "S"},
    {"sv_tracktime_reset", "[slot|all]", "reset windows", "S"},
    // ---- cpuopt (server) ----
    {"sofbuddy_cpuopt_status", "", "dump cpuopt gauges", "S"},
    // ---- cmd_cost (server) ----
    {"sofbuddy_cmdcost_dump", "", "per-name stats", "S"},
    {"sofbuddy_cmdcost_reset", "", "clear samples", "S"},
    {"sofbuddy_cmdcost_wrap", "", "rewalk and patch", "S"},
    {"sofbuddy_cmdcost_spin", "<name> [n] [slot]", "bench one handler", "S"},
    {"sofbuddy_cmdcost_spinall", "", "spin safe catalog", "S"},
    {"sofbuddy_cmdcost_blob", "<len> [piece]", "prep string blob", "S"},
    {"sofbuddy_cmdcost_slots", "[slot] [reps]", "queue slot cmds", "S"},
    {"sofbuddy_cmdcost_usercmds", "", "list usercmd cmds", "S"},
    {"usercmds", "", "same as above", "S"},
    {"sofbuddy_cmdcost_timers", "", "list timer cmds", "S"},
    {"timers", "", "same as above", "S"},
    {"sofbuddy_cmdcost_events", "", "list event cmds", "S"},
    {"events", "", "same as above", "S"},
    {"sofbuddy_cmdcost_addons", "", "analyze addons", "S"},
    // ---- clsv_pipe (server) ----
    {"clsv_load", "<path>", "load cfg chunks", "S"},
    {"clsv_send", "<slot|all>", "send staged file", "S"},
    {"clsv_cancel", "[slot|all]", "cancel transfer", "S"},
    {"clsv_status", "", "transfer status", "S"},
    // ---- minigames platform (server) ----
    {"mg_push", "<slot> <tokens>", "push layout tokens", "S"},
    {"mg_show", "<slot> <0|1>", "layout on/off", "S"},
    {"mg_clear", "<slot>", "hide and wipe layout", "S"},
    {"mg_idle", "<slot>", "idle placeholder", "S"},
    {"mg_ghoul_list", "[filter]", "list ghoul slots", "S"},
    {"mg_ghoul_register", "<path>", "register download", "S"},
    {"mg_sp_register", "<package>", "register string pkg", "S"},
    {"mg_print", "<slot|all> <lvl> <t>", "chat print", "S"},
    {"mg_center", "<slot|all> <text>", "center screen text", "S"},
    {"mg_caption", "<slot|all> <id>", "caption line", "S"},
    {"mg_cin", "<slot|all> <x y s t>", "cinematic text", "S"},
    {"mg_welcome", "<slot|all>", "welcome banner", "S"},
    {"mg_name", "<slot> <from> <c> <t>", "attributed line", "S"},
    {"mg_countdown", "<slot|all> <sec>", "on-screen timer", "S"},
    {"mg_test", "<slot>", "hello placeholder", "S"},
    {"lag_show", "<slot>", "arm lagometer", "S"},
    {"ttt_start", "<slotX> <slotO>", "start tictactoe", "S"},
    {"ttt_end", "", "end tictactoe", "S"},
    {"ttt_move", "<slot> <1-9>", "play cell", "S"},
    {"cmds_show", "<slot> [page]", "arm commands view", "S"},
    {"cmds_list", "", "dump this catalog", "S"},
};

constexpr int kCmdsEntryCount = static_cast<int>(sizeof(kCmdsEntries) / sizeof(kCmdsEntries[0]));

struct CmdsCategory {
    const char* name;
    const char* desc;
};

static const CmdsCategory kCmdsCategories[] = {
    {"STUFFTEXT", "stuff text to clients"},
    {"PROFILES", "player identity registry"},
    {"TRACKTIME", "frametime tables"},
    {"CPU OPTIMIZATIONS", "cpuopt gauges"},
    {"COMMAND COST", "command cost stats"},
    {"CLSV PIPE", "file transfer staging"},
    {"MINIGAMES", "layout push tools"},
};

constexpr int kCmdsCategoryCount =
    static_cast<int>(sizeof(kCmdsCategories) / sizeof(kCmdsCategories[0]));

inline bool CmdsStartsWith(const char* value, const char* prefix) {
    return value && prefix && std::strncmp(value, prefix, std::strlen(prefix)) == 0;
}

inline int CmdsCategoryFor(const char* name) {
    if (!name)
        return -1;
    if (CmdsStartsWith(name, "stufftext"))
        return 0;
    if (CmdsStartsWith(name, "profile"))
        return 1;
    if (CmdsStartsWith(name, "sv_tracktime"))
        return 2;
    if (CmdsStartsWith(name, "sofbuddy_cpuopt"))
        return 3;
    if (CmdsStartsWith(name, "sofbuddy_cmdcost") || !std::strcmp(name, "usercmds") ||
        !std::strcmp(name, "timers") || !std::strcmp(name, "events"))
        return 4;
    if (CmdsStartsWith(name, "clsv_"))
        return 5;
    return 6;
}

inline const char* CmdsCategoryName(int category) {
    return category >= 0 && category < kCmdsCategoryCount
        ? kCmdsCategories[category].name : "";
}

inline int CmdsCategoryEntryCount(int category) {
    int count = 0;
    for (const CmdsEntry& entry : kCmdsEntries)
        if (CmdsCategoryFor(entry.name) == category)
            ++count;
    return count;
}

inline const CmdsEntry* CmdsCategoryEntryAt(int category, int local) {
    if (category < 0 || category >= kCmdsCategoryCount || local < 0)
        return nullptr;
    for (const CmdsEntry& entry : kCmdsEntries) {
        if (CmdsCategoryFor(entry.name) != category)
            continue;
        if (local-- == 0)
            return &entry;
    }
    return nullptr;
}

inline int CmdsCategoryPageCount(int category) {
    const int count = CmdsCategoryEntryCount(category);
    return (count + kCmdsPerPage - 1) / kCmdsPerPage;
}

inline int CmdsClampCategoryPage(int category, int page) {
    const int pages = CmdsCategoryPageCount(category);
    if (page < 0)
        return 0;
    return page >= pages ? pages - 1 : page;
}

inline int CmdsPageCount() {
    return (kCmdsEntryCount + kCmdsPerPage - 1) / kCmdsPerPage;
}

inline int CmdsClampPage(int page) {
    const int pages = CmdsPageCount();
    if (page < 0)
        return 0;
    return page >= pages ? pages - 1 : page;
}

inline bool CmdsWordNoCase(const char* a, const char* b) {
    if (!a || !b)
        return false;
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'A' && ca <= 'Z')
            ca = static_cast<char>(ca + 32);
        if (cb >= 'A' && cb <= 'Z')
            cb = static_cast<char>(cb + 32);
        if (ca != cb)
            return false;
    }
    return *a == *b;
}

// Parses "2", "next", "prev" relative to cur. Returns clamped page.
// Returns -1 when the token is not a page selector.
inline int CmdsParsePageArg(const char* tok, int cur) {
    if (!tok || !tok[0])
        return -1;
    if (CmdsWordNoCase(tok, "next"))
        return CmdsClampPage(cur + 1);
    if (CmdsWordNoCase(tok, "prev"))
        return CmdsClampPage(cur - 1);
    // 1-based page number from the user; also accept 0-based slot-style.
    int v = 0;
    bool any = false;
    bool neg = false;
    const char* p = tok;
    if (*p == '-' || *p == '+') {
        neg = (*p == '-');
        ++p;
    }
    for (; *p; ++p) {
        if (*p < '0' || *p > '9')
            return -1;
        v = v * 10 + (*p - '0');
        any = true;
        if (v > 1000)
            break;
    }
    if (!any)
        return -1;
    if (neg)
        return CmdsClampPage(0);
    if (v >= 1)
        return CmdsClampPage(v - 1);
    return CmdsClampPage(v);
}

inline void CmdsRenderCategories(MgCanvas& c, int selected = 0, bool showTips = true) {
    MgCanvasClear(c);
    MgCanvasPCenter(c, 320, 36, kMgPGreen, "SERVER-SIDE COMMANDS");
    MgCanvasPCenter(c, 320, 54, kMgPWhite, "select a category");
    int y = 78;
    for (int i = 0; i < kCmdsCategoryCount; ++i) {
        char head[64];
        std::snprintf(head, sizeof(head), "%s%s  ", selected == i ? "> " : "",
                      CmdsCategoryName(i));
        head[sizeof(head) - 1] = '\0';
        if (!MgCanvasPText(c, 24, y,
                           selected == i ? kMgMenuSelectedColor : kMgMenuNormalColor,
                           head))
            break;
        const char* desc = kCmdsCategories[i].desc;
        if (desc && desc[0] &&
            !MgCanvasPText(c, 24 + static_cast<int>(std::strlen(head)) * 8, y,
                           kMgPYellow, desc))
            break;
        y += 20;
    }
    if (showTips)
        MgCanvasPCenter(c, 320, y + 8, kMgPGreen,
                        "tap +use select | hold +use forward/back | hold +use +moveleft back");
}

inline void CmdsRender(int category, int page, MgCanvas& c, int selectedAction = 0,
                       bool showTips = true) {
    MgCanvasClear(c);
    category = category < 0 || category >= kCmdsCategoryCount ? 0 : category;
    page = CmdsClampCategoryPage(category, page);
    const int pages = CmdsCategoryPageCount(category);
    const int count = CmdsCategoryEntryCount(category);
    MgCanvasPCenter(c, 320, 36, kMgPGreen, "SERVER-SIDE COMMANDS");
    char sub[96];
    std::snprintf(sub, sizeof(sub), "%s  page %d/%d  (%d cmds)",
                  CmdsCategoryName(category), page + 1, pages, count);
    MgCanvasPCenter(c, 320, 54, kMgPWhite, sub);
    char row[128];
    int y = 78;
    const int start = page * kCmdsPerPage;
    for (int i = 0; i < kCmdsPerPage; ++i) {
        const CmdsEntry* entry = CmdsCategoryEntryAt(category, start + i);
        if (!entry)
            break;
        if (entry->syntax && entry->syntax[0])
            std::snprintf(row, sizeof(row), "%s %s - ", entry->name, entry->syntax);
        else
            std::snprintf(row, sizeof(row), "%s - ", entry->name);
        row[sizeof(row) - 1] = '\0';
        if (!MgCanvasPText(c, 24, y, kMgPWhite, row))
            break;
        if (entry->blurb && entry->blurb[0] &&
            !MgCanvasPText(c, 24 + static_cast<int>(std::strlen(row)) * 8, y,
                           kMgPYellow, entry->blurb))
            break;
        y += 20;
    }
    y += 4;
    const char* actions[] = {"next page", "previous page"};
    for (int i = 0; i < kCmdsActionCount; ++i) {
        char action[40];
        std::snprintf(action, sizeof(action), "%s%s", selectedAction == i ? "> " : "",
                      actions[i]);
        MgCanvasPText(c, 24, y, selectedAction == i ? kMgMenuSelectedColor : kMgMenuNormalColor,
                      action);
        y += 18;
    }
    if (showTips)
        MgCanvasPCenter(c, 320, y + 6, kMgPGreen,
                        "tap +use select | hold +use forward/back | hold +use +moveleft back");
}
