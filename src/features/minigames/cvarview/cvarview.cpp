// Live pages of every _sofbuddy_* cvar. Each slot picks its own page.

#include "cvarview.h"
#include "cvarview_logic.h"
#include "../../profiles/cvar.h"

#include "log.h"
#include "../minigames_api.h"

#include <cstring>
#include <windows.h>

namespace {

constexpr unsigned kRvaCvarVars = 0x24B1D8;
constexpr unsigned kCvarNextOfs = 0x1C;
constexpr int kCvarMax = 256;
constexpr char kGameId[] = "mg_cvars";

CvarLine g_rows[kCvarMax];
int g_rowCount = 0;
char g_group[kMgMaxSlots + 1][16] = {};
int g_sub[kMgMaxSlots + 1] = {};
int g_catPage[kMgMaxSlots + 1] = {};
int g_catSelection[kMgMaxSlots + 1] = {};
int g_subSelection[kMgMaxSlots + 1] = {};
int g_groupEntry[kMgMaxSlots + 1] = {};
bool g_armed[kMgMaxSlots + 1] = {};
bool g_registered = false;

bool ReadCStr(const char* p, char* dst, int cap) {
    if (!dst || cap < 2)
        return false;
    dst[0] = '\0';
    if (!p || IsBadReadPtr(const_cast<char*>(p), 1))
        return false;
    int i = 0;
    for (; i < cap - 1; ++i) {
        if (IsBadReadPtr(const_cast<char*>(p + i), 1))
            return false;
        char c = p[i];
        if (!c) {
            dst[i] = '\0';
            return i > 0;
        }
        if (static_cast<unsigned char>(c) < 32 || c == '"' || c == '\\')
            c = ' ';
        dst[i] = c;
    }
    dst[i] = '\0';
    return true;
}

int CmpRow(const void* a, const void* b) {
    return std::strcmp(static_cast<const CvarLine*>(a)->name, static_cast<const CvarLine*>(b)->name);
}

void Collect() {
    g_rowCount = 0;
    HMODULE exe = GetModuleHandleA("SoF.exe");
    if (!exe)
        exe = GetModuleHandleA("SoF-spsv.exe");
    if (!exe)
        return;
    auto** head = reinterpret_cast<void**>(reinterpret_cast<char*>(exe) + kRvaCvarVars);
    if (IsBadReadPtr(head, sizeof(void*)))
        return;
    void* node = *head;
    for (int guard = 0; node && guard < 4096 && g_rowCount < kCvarMax; ++guard) {
        if (IsBadReadPtr(node, kCvarNextOfs + sizeof(void*)))
            break;
        char* name = *reinterpret_cast<char**>(node);
        char* str = *reinterpret_cast<char**>(static_cast<char*>(node) + 4);
        void* next = *reinterpret_cast<void**>(static_cast<char*>(node) + kCvarNextOfs);
        char nb[kCvarNameCap];
        if (ReadCStr(name, nb, static_cast<int>(sizeof(nb)))) {
            // Public admin cvars (_sofbuddy_) plus internal plumbing
            // (_sb_internal_); both belong to sof_buddy.
            const bool buddy =
                (std::strncmp(nb, "_sofbuddy_", 10) == 0 && nb[10]) ||
                (std::strncmp(nb, "_sb_internal_", 13) == 0 && nb[13]);
            if (!buddy) {
                node = next;
                continue;
            }
            CvarLine& row = g_rows[g_rowCount++];
            CvarCopy(row.name, kCvarNameCap, nb);
            if (!ReadCStr(str, row.value, kCvarValueCap))
                CvarCopy(row.value, kCvarValueCap, "0");
            CvarAnnotate(row);
        }
        node = next;
    }
    if (g_rowCount > 1)
        std::qsort(g_rows, static_cast<std::size_t>(g_rowCount), sizeof(CvarLine), CmpRow);
}

int Pages(CvarPage* pages) {
    return CvarBuildPages(g_rows, g_rowCount, pages, kCvarMaxPages);
}

void Paint(int slot1, bool push) {
    if (slot1 < 1 || slot1 > kMgMaxSlots || !g_armed[slot1])
        return;
    Collect();
    CvarPage pages[kCvarMaxPages];
    const int n = Pages(pages);
    MgCanvas c;
    const bool showTips = !Profiles_SlotHideTips(slot1 - 1);
    if (!g_group[slot1][0]) {
        CvarCat cats[kCvarMaxPages];
        const int nc = CvarBuildCats(pages, n, cats, kCvarMaxPages);
        const int catPages = CvarCatPageCount(nc);
        if (g_catPage[slot1] >= catPages)
            g_catPage[slot1] = catPages - 1;
        if (g_catPage[slot1] < 0)
            g_catPage[slot1] = 0;
        const int begin = g_catPage[slot1] * kCvarCatsPerPage;
        const int visible = nc - begin < kCvarCatsPerPage ? nc - begin : kCvarCatsPerPage;
        const int actions = catPages > 1 ? 2 : 0;
        const int count = visible + actions;
        if (g_catSelection[slot1] < 0 || g_catSelection[slot1] >= count)
            g_catSelection[slot1] = count > 0 ? 0 : -1;
        const int selected = g_catSelection[slot1] >= 0 &&
                                     g_catSelection[slot1] < visible
                                 ? begin + g_catSelection[slot1]
                                 : -1;
        const int selectedAction = g_catSelection[slot1] >= visible
                                       ? g_catSelection[slot1] - visible
                                       : -1;
        CvarRenderCats(c, cats, nc, g_catPage[slot1], selected, selectedAction, showTips);
    } else {
        CvarPick pick = CvarPickGroup(pages, n, g_group[slot1], g_sub[slot1]);
        if (!pick.found)
            CvarRender(c, 0, 0, CvarPage{}, nullptr, 0, showTips);
        else {
            CvarCopy(g_group[slot1], static_cast<int>(sizeof(g_group[slot1])), pages[pick.index].group);
            g_sub[slot1] = pick.local;
            if (pick.localCount > 1 &&
                (g_subSelection[slot1] < 0 || g_subSelection[slot1] >= 2))
                g_subSelection[slot1] = 0;
            else if (pick.localCount <= 1)
                g_subSelection[slot1] = -1;
            CvarRender(c, pick.local, pick.localCount, pages[pick.index], g_rows,
                       g_subSelection[slot1], showTips);
        }
    }
    MgPutLayoutCache(slot1, kGameId, c);
    if (push && MgMinigameTabOpen(slot1))
        MgPushLayout(slot1, kGameId, c);
}

void SetArmed(int slot1, bool on) {
    if (slot1 < 1 || slot1 > kMgMaxSlots)
        return;
    g_armed[slot1] = on;
    if (on) {
        MgTakeDisplay(slot1, kGameId);
        Paint(slot1, true);
        return;
    }
    if (!MgDisplayOwnedBy(slot1, kGameId))
        return;
    MgClearLayout(slot1, kGameId);
}

void OnSessionEnd(int slot1) {
    if (slot1 >= 1 && slot1 <= kMgMaxSlots && g_armed[slot1])
        g_armed[slot1] = false;
}

void MoveCategory(int slot1, int delta) {
    Collect();
    CvarPage pages[kCvarMaxPages];
    CvarCat cats[kCvarMaxPages];
    const int n = Pages(pages);
    const int nc = CvarBuildCats(pages, n, cats, kCvarMaxPages);
    if (nc <= 0)
        return;
    const int catPages = CvarCatPageCount(nc);
    const int page = g_catPage[slot1] < catPages ? g_catPage[slot1] : catPages - 1;
    const int begin = page * kCvarCatsPerPage;
    const int visible = nc - begin < kCvarCatsPerPage ? nc - begin : kCvarCatsPerPage;
    const int actions = catPages > 1 ? 2 : 0;
    const int count = visible + actions;
    int& selected = g_catSelection[slot1];
    if (selected < 0 || selected >= count)
        selected = delta < 0 ? count - 1 : 0;
    else
        selected = (selected + delta + count) % count;
    g_catPage[slot1] = page;
    Paint(slot1, true);
}

void TurnCategoryPage(int slot1, int delta) {
    Collect();
    CvarPage pages[kCvarMaxPages];
    CvarCat cats[kCvarMaxPages];
    const int n = Pages(pages);
    const int nc = CvarBuildCats(pages, n, cats, kCvarMaxPages);
    const int catPages = CvarCatPageCount(nc);
    if (catPages <= 1)
        return;
    const int oldBegin = g_catPage[slot1] * kCvarCatsPerPage;
    const int oldVisible =
        nc - oldBegin < kCvarCatsPerPage ? nc - oldBegin : kCvarCatsPerPage;
    const int action = g_catSelection[slot1] >= oldVisible
                           ? g_catSelection[slot1] - oldVisible
                           : -1;
    g_catPage[slot1] = (g_catPage[slot1] + delta + catPages) % catPages;
    const int newBegin = g_catPage[slot1] * kCvarCatsPerPage;
    const int newVisible =
        nc - newBegin < kCvarCatsPerPage ? nc - newBegin : kCvarCatsPerPage;
    g_catSelection[slot1] = action >= 0 ? newVisible + action : (newVisible > 0 ? 0 : -1);
    Paint(slot1, true);
}

void OpenCategory(int slot1) {
    Collect();
    CvarPage pages[kCvarMaxPages];
    CvarCat cats[kCvarMaxPages];
    const int n = Pages(pages);
    const int nc = CvarBuildCats(pages, n, cats, kCvarMaxPages);
    if (nc <= 0)
        return;
    const int catPages = CvarCatPageCount(nc);
    const int page = g_catPage[slot1] < catPages ? g_catPage[slot1] : 0;
    const int begin = page * kCvarCatsPerPage;
    const int visible = nc - begin < kCvarCatsPerPage ? nc - begin : kCvarCatsPerPage;
    int selected = g_catSelection[slot1];
    if (selected < 0 || selected >= visible)
        selected = 0;
    selected += begin;
    g_groupEntry[slot1] = selected;  // global cat index, restored on back
    g_catPage[slot1] = page;
    CvarCopy(g_group[slot1], static_cast<int>(sizeof(g_group[slot1])),
             cats[selected].group);
    g_sub[slot1] = 0;
    g_subSelection[slot1] = 0;
    Paint(slot1, true);
}

void TurnSubPage(int slot1, int delta) {
    Collect();
    CvarPage pages[kCvarMaxPages];
    const int n = Pages(pages);
    const CvarPick pick = CvarPickGroup(pages, n, g_group[slot1], g_sub[slot1]);
    if (!pick.found || pick.localCount <= 0)
        return;
    int next = pick.local + delta;
    if (next < 0)
        next = pick.localCount - 1;
    else if (next >= pick.localCount)
        next = 0;
    g_sub[slot1] = next;
    Paint(slot1, true);
}

void MoveSubSelection(int slot1, int delta) {
    Collect();
    CvarPage pages[kCvarMaxPages];
    const int n = Pages(pages);
    const CvarPick pick = CvarPickGroup(pages, n, g_group[slot1], g_sub[slot1]);
    if (!pick.found || pick.localCount <= 1)
        return;
    int& selected = g_subSelection[slot1];
    if (selected < 0)
        selected = delta < 0 ? 1 : 0;
    else
        selected = (selected + delta + 2) % 2;
    Paint(slot1, true);
}

bool OnBack(int slot1) {
    if (slot1 < 1 || slot1 > kMgMaxSlots || !g_armed[slot1] ||
        !g_group[slot1][0] || !MgDisplayOwnedBy(slot1, kGameId))
        return false;
    // Back out of the group: the cursor returns to the category just left
    // instead of resetting to the top (Paint clamps a stale entry).
    const int entry = g_groupEntry[slot1];
    g_group[slot1][0] = '\0';
    g_sub[slot1] = 0;
    g_catPage[slot1] = entry >= 0 ? entry / kCvarCatsPerPage : 0;
    g_catSelection[slot1] = entry >= 0 ? entry % kCvarCatsPerPage : 0;
    g_subSelection[slot1] = -1;
    Paint(slot1, true);
    return true;
}

void OnUserCmd(int slot1, MgUserCmdInput* in) {
    if (!in || (!in->use && !in->useRelease) || slot1 < 1 || slot1 > kMgMaxSlots ||
        !g_armed[slot1] || !MgMinigameTabOpen(slot1) ||
        !MgDisplayOwnedBy(slot1, kGameId))
        return;
    in->consumed = true;
    in->preserveUse = true;
    const bool choose = in->useRelease;
    if (!g_group[slot1][0]) {
        if (in->use && in->forwardEdge)
            MoveCategory(slot1, -1);
        else if (in->use && in->backEdge)
            MoveCategory(slot1, 1);
        else if (choose) {
            if (g_catSelection[slot1] < 0) {
                MoveCategory(slot1, 1);
                return;
            }
            Collect();
            CvarPage pages[kCvarMaxPages];
            CvarCat cats[kCvarMaxPages];
            const int n = Pages(pages);
            const int nc = CvarBuildCats(pages, n, cats, kCvarMaxPages);
            const int catPages = CvarCatPageCount(nc);
            const int begin = g_catPage[slot1] * kCvarCatsPerPage;
            const int visible =
                nc - begin < kCvarCatsPerPage ? nc - begin : kCvarCatsPerPage;
            if (catPages > 1 && g_catSelection[slot1] >= visible)
                TurnCategoryPage(slot1, g_catSelection[slot1] == visible ? 1 : -1);
            else
                OpenCategory(slot1);
        }
        return;
    }
    if (in->use && in->forwardEdge)
        MoveSubSelection(slot1, -1);
    else if (in->use && in->backEdge)
        MoveSubSelection(slot1, 1);
    else if (choose) {
        if (g_subSelection[slot1] < 0)
            MoveSubSelection(slot1, 1);
        else
            TurnSubPage(slot1, g_subSelection[slot1] == 0 ? 1 : -1);
    }
}

void OnClientCmd(int slot1) {
    if (!MgEnabled())
        return;
    const bool on = !g_armed[slot1];
    if (on) {
        g_group[slot1][0] = '\0';
        g_sub[slot1] = 0;
        g_catPage[slot1] = 0;
        g_catSelection[slot1] = -1;
        g_subSelection[slot1] = -1;
    }
    SetArmed(slot1, on);
}

void TryRegister() {
    if (!MgEnabled() || g_registered)
        return;
    static const MgGameOps kOps = {"mg_cvars", OnClientCmd, OnUserCmd, OnSessionEnd,
                                   "browse server-side cvars", "server-side cvars", true, OnBack};
    if (!MgRegisterGame(&kOps)) {
        PrintOut(PRINT_BAD, "[cvars] MgRegisterGame failed (table full?)\n");
        return;
    }
    g_registered = true;
    PrintOut(PRINT_LOG, "[cvars] registered (menu page)\n");
}

}  // namespace

void cvars_EnsureRegistered() {
    TryRegister();
}

void cvars_OnGameDllLoaded(void* gameExport) {
    (void)gameExport;
    TryRegister();
}

void cvars_MaintainForSlot(int slot1) {
    if (slot1 >= 1 && !Profiles_SlotActive(slot1 - 1)) {
        if (g_armed[slot1])
            SetArmed(slot1, false);
        return;
    }
    if (slot1 >= 1 && !Profiles_IsLevel0Admin(slot1 - 1)) {
        if (g_armed[slot1]) {
            if (!MgDisplayOwnedBy(slot1, kGameId) || !MgMinigameTabOpen(slot1) ||
                !MgNavigateBack(slot1))
                SetArmed(slot1, false);
        }
        return;
    }
    if (!MgEnabled() || slot1 < 1 || !g_armed[slot1])
        return;
    if (!MgMinigameTabOpen(slot1) || MgDisplayTakenByOther(slot1, kGameId))
        return;
    if (!MgDisplayOwnedBy(slot1, kGameId))
        MgTakeDisplay(slot1, kGameId);
    if (!MgRunningSession(kGameId))
        return;
    Paint(slot1, false);
}
