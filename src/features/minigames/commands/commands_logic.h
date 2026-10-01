#pragma once

// Pure commands catalog + layout renderer. No engine calls, no windows.h —
// unit-testable on Linux. Rows are single-line "name syntax - blurb" strings
// kept short so 8 rows + title/footer stay under the 1024 canvas cap
// (client layout_string is 0x400).

#include "../minigames_api.h"

#include <cstdio>
#include <cstring>

constexpr int kCmdsPerPage = 8;

struct CmdsEntry {
    const char* name;    // server console command, or client word
    const char* syntax;  // short args hint, no double quotes
    const char* blurb;   // <= ~36 chars, no double quotes
    const char* side;    // S = server console, C = client console word
};

// Full sof_buddy command catalog (server console unless side=C).
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
    {"profile_del", "<guid|nick>", "delete roster entry", "S"},
    {"profile_query", "", "diagnose slots", "S"},
    {"profile_signin", "<slot>", "push guid to slot", "S"},
    {"profile_signoff", "<slot|guid|nick>", "clear slot binding", "S"},
    {"profile_get_slot_by_id", "<guid>", "slot for guid", "S"},
    {"profile_get_slot_by_nick", "<nick>", "slot for nick", "S"},
    {"profile_save", "", "save roster file", "S"},
    {"profile_load", "", "load roster file", "S"},
    {"prof_admin_add", "<guid> <nick>", "alias of import", "S"},
    {"prof_admin_del", "<guid|nick>", "alias of del", "S"},
    {"prof_apply", "<slot>", "alias of signin", "S"},
    {"prof_enforce", "", "alias of query", "S"},
    {"prof_register", "<slot>", "alias of signin", "S"},
    {"prof_get_slot_by_id", "<guid>", "alias lookup id", "S"},
    {"prof_get_slot_by_nick", "<nick>", "alias lookup nick", "S"},
    {"prof_admin_save", "", "alias of save", "S"},
    {"prof_admin_load", "", "alias of load", "S"},
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
    // ---- client words (minigames routing) ----
    {"ttt", "<1-9>", "tictactoe move", "C"},
    {"lag", "", "toggle lagometer", "C"},
    {"cmds", "[page|next|prev]", "this viewer", "C"},
};

constexpr int kCmdsEntryCount = static_cast<int>(sizeof(kCmdsEntries) / sizeof(kCmdsEntries[0]));

inline int CmdsPageCount() {
    return (kCmdsEntryCount + kCmdsPerPage - 1) / kCmdsPerPage;
}

inline int CmdsClampPage(int page) {
    const int n = CmdsPageCount();
    if (page < 0)
        return 0;
    if (page >= n)
        return n - 1;
    return page;
}

// Parses "2", "next", "prev" relative to cur. Returns clamped page.
// Returns -1 when the token is not a page selector.
inline int CmdsParsePageArg(const char* tok, int cur) {
    if (!tok || !tok[0])
        return -1;
    if (tok[0] == 'n' || tok[0] == 'N')
        return CmdsClampPage(cur + 1);
    if (tok[0] == 'p' && (tok[1] == 'r' || tok[1] == 'R'))
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

inline void CmdsRender(int page, MgCanvas& c) {
    MgCanvasClear(c);
    page = CmdsClampPage(page);
    const int pages = CmdsPageCount();
    MgCanvasTc(c, kMgColYellow);
    MgCanvasCenter(c, 320, 36, "SERVER COMMANDS");
    MgCanvasTc(c, kMgColWhite);
    char sub[64];
    std::snprintf(sub, sizeof(sub), "page %d/%d  (%d cmds)", page + 1, pages, kCmdsEntryCount);
    MgCanvasCenter(c, 320, 54, sub);
    char row[128];
    int y = 78;
    const int start = page * kCmdsPerPage;
    for (int i = 0; i < kCmdsPerPage; ++i) {
        const int idx = start + i;
        if (idx >= kCmdsEntryCount)
            break;
        const CmdsEntry& e = kCmdsEntries[idx];
        if (e.syntax && e.syntax[0])
            std::snprintf(row, sizeof(row), "%s %s - %s", e.name, e.syntax, e.blurb);
        else
            std::snprintf(row, sizeof(row), "%s - %s", e.name, e.blurb);
        row[sizeof(row) - 1] = '\0';
        MgCanvasText(c, 24, y, row);
        y += 20;
    }
    MgCanvasTc(c, kMgColGreen);
    MgCanvasCenter(c, 320, 268, "cmds <n>|next|prev  |  +use+score");
    MgCanvasTc(c, kMgColWhite);
}
