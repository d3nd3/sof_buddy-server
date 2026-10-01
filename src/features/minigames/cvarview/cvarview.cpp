// Live pages of every _sofbuddy_* cvar. Each slot picks its own page.

#include "cvarview.h"
#include "cvarview_logic.h"
#include "../../profiles/cvar.h"

#include "buddy_import.h"
#include "log.h"
#include "../minigames_api.h"

#include <cstdlib>
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
        if (ReadCStr(name, nb, static_cast<int>(sizeof(nb))) &&
            std::strncmp(nb, "_sofbuddy_", 10) == 0 && nb[10]) {
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
    if (!g_group[slot1][0]) {
        CvarCat cats[kCvarMaxPages];
        const int nc = CvarBuildCats(pages, n, cats, kCvarMaxPages);
        int page = g_sub[slot1];
        const int pagesN = CvarCatPageCount(nc);
        if (page < 0)
            page = 0;
        if (page >= pagesN)
            page = pagesN - 1;
        g_sub[slot1] = page;
        CvarRenderCats(c, cats, nc, page);
    } else {
        CvarPick pick = CvarPickGroup(pages, n, g_group[slot1], g_sub[slot1]);
        if (!pick.found)
            CvarRender(c, 0, 0, CvarPage{}, nullptr);
        else {
            CvarCopy(g_group[slot1], static_cast<int>(sizeof(g_group[slot1])), pages[pick.index].group);
            g_sub[slot1] = pick.local;
            CvarRender(c, pick.local, pick.localCount, pages[pick.index], g_rows);
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
    MgCanvas c;
    MgCanvasClear(c);
    MgPutLayoutCache(slot1, kGameId, c);
    if (MgMinigameTabOpen(slot1))
        MgPushLayout(slot1, kGameId, c);
    MgReleaseDisplay(slot1, kGameId);
}

void OnSessionEnd() {
    for (int s = 1; s <= kMgMaxSlots; ++s) {
        if (g_armed[s])
            SetArmed(s, false);
    }
}

void CategoryList(char* dst, int cap, const CvarPage* pages, int n) {
    if (!dst || cap <= 0)
        return;
    dst[0] = '\0';
    int len = 0;
    for (int i = 0; i < n; ++i) {
        if (i > 0 && std::strcmp(pages[i].group, pages[i - 1].group) == 0)
            continue;
        const int add = static_cast<int>(std::strlen(pages[i].group));
        if (len + add + 2 >= cap) {
            if (len + 4 < cap)
                std::memcpy(dst + len, " ...", 4);
            dst[len + 4 < cap ? len + 4 : cap - 1] = '\0';
            return;
        }
        if (len)
            dst[len++] = ' ';
        std::memcpy(dst + len, pages[i].group, static_cast<std::size_t>(add));
        len += add;
        dst[len] = '\0';
    }
}

void ShowIndex(int slot1, int page) {
    if (!g_armed[slot1]) {
        g_armed[slot1] = true;
        MgTakeDisplay(slot1, kGameId);
    }
    g_group[slot1][0] = '\0';
    g_sub[slot1] = page;
    Paint(slot1, true);
}

void OnClientCmd(int slot1) {
    if (!MgEnabled())
        return;
    if (MgArgc() < 2) {
        const bool on = !g_armed[slot1];
        if (on)
            g_group[slot1][0] = '\0';
        g_sub[slot1] = 0;
        SetArmed(slot1, on);
        if (void* ent = MgEdictForSlot(slot1)) {
            if (on)
                Buddy_ClientPrintf(ent, 2, "Cvars armed\n.mg_cvars <category>\n");
            else
                Buddy_ClientPrintf(ent, 2, "Cvars off\n");
        }
        return;
    }
    int want = 0;
    if (MgArgc() == 2 && CvarParsePage(MgArgv(1), &want)) {
        ShowIndex(slot1, want);
        if (void* ent = MgEdictForSlot(slot1))
            Buddy_ClientPrintf(ent, 2, "Cvars  %d\n.mg_cvars <category>\n", g_sub[slot1] + 1);
        return;
    }
    if (MgArgc() >= 3 && !CvarParsePage(MgArgv(2), &want)) {
        if (void* ent = MgEdictForSlot(slot1))
            Buddy_ClientPrintf(ent, 2, ".mg_cvars <category> [page]\n");
        return;
    }
    Collect();
    CvarPage pages[kCvarMaxPages];
    const int n = Pages(pages);
    const CvarPick pick = CvarPickGroup(pages, n, MgArgv(1), want);
    void* ent = MgEdictForSlot(slot1);
    if (!pick.found) {
        char list[192];
        CategoryList(list, static_cast<int>(sizeof(list)), pages, n);
        if (ent)
            Buddy_ClientPrintf(ent, 2, ".mg_cvars <category> [page]\n%s\n", list[0] ? list : "none");
        return;
    }
    if (!g_armed[slot1]) {
        g_armed[slot1] = true;
        MgTakeDisplay(slot1, kGameId);
    }
    CvarCopy(g_group[slot1], static_cast<int>(sizeof(g_group[slot1])), pages[pick.index].group);
    g_sub[slot1] = pick.local;
    Paint(slot1, true);
    if (ent) {
        if (pick.localCount > 1)
            Buddy_ClientPrintf(ent, 2, "Cvars %s  %d / %d\n", pages[pick.index].group, pick.local + 1,
                               pick.localCount);
        else
            Buddy_ClientPrintf(ent, 2, "Cvars %s\n", pages[pick.index].group);
    }
}

void TryRegister() {
    if (!MgEnabled() || g_registered)
        return;
    static const MgGameOps kOps = {"mg_cvars", OnClientCmd, nullptr, OnSessionEnd,
                                   "live sof_buddy cvars"};
    if (!MgRegisterGame(&kOps)) {
        PrintOut(PRINT_BAD, "[cvars] MgRegisterGame failed (table full?)\n");
        return;
    }
    g_registered = true;
    PrintOut(PRINT_LOG, "[cvars] registered (.mg_cvars <category> [page])\n");
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
