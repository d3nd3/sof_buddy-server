// clsv: _sp_cl_sv_* stufftext pipeline.
//
// SoFplus servers set client cvars _sp_cl_sv_0 .. _sp_cl_sv_99 with
//   sp_sv_client_cvar_set SLOT NUMBER STRING
// (STRING is one COM_Parse token: 255 bytes + NUL). The client runs a small
// .func listener that registers sp_sc_on_change handlers on those cvars.
//
// This shim has no SoFplus server-script VM, so it implements the
// observationally identical transport one level down: each send goes out as
// stufftext `set _sp_cl_sv_N "text"`, which sets the same client cvar and
// fires the same on_change handler. Wire format, pacing model and the
// 1-vs-90 bandwidth analysis live in README.md; the pure sequencing lives
// in clsv_logic.h. This file is only engine glue:
//
//   clsv_load <path>     read a func_parser/rfm_parser .cfg into the bank
//   clsv_send <slot|all> start a paced job (INFO..DATA..CMD..DONE)
//   clsv_cancel [slot|all]
//   clsv_status
//
// New spawns auto-start when _sofbuddy_clsv_auto is 1 (rising spawned edge
// seen from the SV_Frame Post ticker). Completion needs no retransmit
// layer: the reliable channel delivers in order, so DONE-last means all
// prior sends arrived; the client's `pipeack <got>` (forwarded unknown
// command, Q2 Cmd_ForwardToServer behaviour) is best-effort confirmation,
// and a linger timeout finishes the job either way.

#include "cvar.h"
#include "clsv_logic.h"
#include "dl_unlock.h"
#include "buddy_import.h"
#include "generated_engine_pointers.h"
#include "generated_registrations.h"
#include "log.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <windows.h>

extern "C" HMODULE Buddy_GetGameDllHandle(void);

namespace {

// ---------------------------------------------------------------------------
// Engine layout (same verified constants as stufftext / reconnect).
// ---------------------------------------------------------------------------
constexpr unsigned kEdictStride = 0x464;
constexpr unsigned kEdictClient = 0x74;
constexpr unsigned kEdictInuse = 0x78;
constexpr unsigned kCvarValueOfs = 0x18;
constexpr unsigned kRvaMaxclientsCvar = 0x15D9B4;
constexpr unsigned kRvaGEdicts = 0x15CCA0;

constexpr unsigned kRvaSvsClients = 0x396EEC;  // client_t* svs.clients
constexpr unsigned kClientStride = 0xD2AC;     // sizeof(client_t)
constexpr unsigned kClientStateOfs = 0x0;
constexpr int kCsSpawned = 3;

constexpr unsigned kRvaCmdFunctions = 0x241840;  // engine cmd_function_t list
constexpr unsigned kCmdNameOfs = 0x04;
constexpr unsigned kCmdFnOfs = 0x08;

constexpr unsigned kExportClientCommandOfs = 0x34;
constexpr unsigned kExportEdictsOfs = 0x60;
constexpr unsigned kExportEdictSizeOfs = 0x64;

constexpr std::size_t kMaxFileBytes = 512u * 1024u;

// ---------------------------------------------------------------------------
// Safety helpers (same pattern as stufftext.cpp).
// ---------------------------------------------------------------------------
inline bool IsValidUserPointer(const void* ptr) {
    auto addr = reinterpret_cast<uintptr_t>(ptr);
    return (addr >= 0x10000 && addr <= 0x7FFFFFFF);
}

inline bool IsSafeMemoryBlock(const void* ptr, size_t size) {
    if (!IsValidUserPointer(ptr) || size == 0)
        return false;
    auto addr = reinterpret_cast<uintptr_t>(ptr);
    if (addr + size < addr || addr + size > 0x7FFFFFFF)
        return false;
    MEMORY_BASIC_INFORMATION mbi = {0};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0)
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        return false;
    uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return (addr + size <= regionEnd);
}

inline bool IsExecutableCodeAddress(const void* ptr) {
    if (!IsValidUserPointer(ptr))
        return false;
    MEMORY_BASIC_INFORMATION mbi = {0};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0)
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    const DWORD execMask = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & execMask) != 0;
}

inline bool Readable(const void* p, unsigned len) {
    return !IsBadReadPtr(const_cast<void*>(p), len);
}

HMODULE ExeMod() {
    if (HMODULE h = GetModuleHandleA("SoF.exe"))
        return h;
    if (HMODULE h = GetModuleHandleA("SoF-spsv.exe"))
        return h;
    return GetModuleHandleA(nullptr);
}

HMODULE GameMod() {
    if (HMODULE shim = GetModuleHandleA("gamex86.dll")) {
        using handle_fn = HMODULE (*)();
        auto fn = reinterpret_cast<handle_fn>(GetProcAddress(shim, "Buddy_GetGameDllHandle"));
        if (fn) {
            if (HMODULE stock = fn())
                return stock;
        }
    }
    if (HMODULE h = GetModuleHandleA("oldgamex86.dll"))
        return h;
    return GetModuleHandleA("OLDGAMEX86.DLL");
}

bool IsValidModuleRva(HMODULE h, unsigned rva, unsigned size) {
    if (!h || size == 0 || !IsSafeMemoryBlock(h, sizeof(IMAGE_DOS_HEADER))) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(h);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    if (dos->e_lfanew <= 0 || dos->e_lfanew > 0x10000000) return false;
    if (!IsSafeMemoryBlock(reinterpret_cast<const char*>(dos) + dos->e_lfanew, sizeof(IMAGE_NT_HEADERS))) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const char*>(dos) + dos->e_lfanew);
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE) return false;
    if (rva >= nt->OptionalHeader.SizeOfImage) return false;
    if (rva + size > nt->OptionalHeader.SizeOfImage || rva + size < rva) return false;
    return true;
}

int GameMaxClients() {
    HMODULE h = GameMod();
    if (!h || !IsValidModuleRva(h, kRvaMaxclientsCvar, sizeof(void*))) return 0;
    void* cv = *reinterpret_cast<void**>(reinterpret_cast<char*>(h) + kRvaMaxclientsCvar);
    if (!IsSafeMemoryBlock(cv, kCvarValueOfs + sizeof(float))) return 0;
    int v = static_cast<int>(*reinterpret_cast<float*>(static_cast<char*>(cv) + kCvarValueOfs));
    if (v < 1) return 0;
    if (v > clsv::kMaxSlots) return clsv::kMaxSlots;
    return v;
}

char* GameEdicts() {
    HMODULE h = GameMod();
    if (!h || !IsValidModuleRva(h, kRvaGEdicts, sizeof(void*))) return nullptr;
    void* edicts = *reinterpret_cast<void**>(reinterpret_cast<char*>(h) + kRvaGEdicts);
    if (!IsValidUserPointer(edicts)) return nullptr;
    return static_cast<char*>(edicts);
}

bool EdictInUse(const void* edict) {
    if (!IsSafeMemoryBlock(edict, kEdictInuse + sizeof(int)))
        return false;
    if (!*reinterpret_cast<const int*>(static_cast<const char*>(edict) + kEdictInuse))
        return false;
    if (!IsSafeMemoryBlock(static_cast<const char*>(edict) + kEdictClient, sizeof(void*)))
        return false;
    void* cl = *reinterpret_cast<void* const*>(static_cast<const char*>(edict) + kEdictClient);
    return IsValidUserPointer(cl);
}

void* SvsClientsPtr() {
    HMODULE h = ExeMod();
    if (!h || !IsValidModuleRva(h, kRvaSvsClients, sizeof(void*)))
        return nullptr;
    void* clients = *reinterpret_cast<void**>(reinterpret_cast<char*>(h) + kRvaSvsClients);
    if (!IsValidUserPointer(clients))
        return nullptr;
    return clients;
}

int EngineClientState(void* clients, int slot) {
    if (!clients || slot < 0 || slot >= clsv::kMaxSlots)
        return -1;
    const void* cell = static_cast<const char*>(clients) +
                       static_cast<unsigned>(slot) * kClientStride + kClientStateOfs;
    if (!IsSafeMemoryBlock(cell, sizeof(int)))
        return -1;
    const int st = *static_cast<const int*>(cell);
    if (st < 0 || st > 3)
        return -1;
    return st;
}

bool EngineCmdReady() {
    return detour_Cmd_Argc::oCmd_Argc && detour_Cmd_Argv::oCmd_Argv &&
           IsExecutableCodeAddress(reinterpret_cast<const void*>(detour_Cmd_Argc::oCmd_Argc)) &&
           IsExecutableCodeAddress(reinterpret_cast<const void*>(detour_Cmd_Argv::oCmd_Argv));
}

// ---------------------------------------------------------------------------
// Console command registration (same repoint-safe pattern as stufftext).
// ---------------------------------------------------------------------------
void** CmdFunctionsHead() {
    HMODULE h = ExeMod();
    if (!h)
        return nullptr;
    return reinterpret_cast<void**>(reinterpret_cast<char*>(h) + kRvaCmdFunctions);
}

void* FindCmdNode(const char* name) {
    void** head = CmdFunctionsHead();
    if (!head)
        return nullptr;
    for (void* node = *head; node; node = *reinterpret_cast<void**>(node)) {
        const char* n = *reinterpret_cast<const char**>(
            static_cast<char*>(node) + kCmdNameOfs);
        if (n && strcmp(n, name) == 0)
            return node;
    }
    return nullptr;
}

void InstallCommand(const char* name, void* fn) {
    void* node = FindCmdNode(name);
    if (node) {
        *reinterpret_cast<void**>(static_cast<char*>(node) + kCmdFnOfs) = fn;
        return;
    }
    SOF_EP_Cmd_AddCommand(const_cast<char*>(name), fn);
}

// ---------------------------------------------------------------------------
// Pipeline state.
// ---------------------------------------------------------------------------
long long g_sent = 0;
long long g_jobsDone = 0;
long long g_errors = 0;

clsv::ClsvContent g_content;
bool g_hasContent = false;
std::string g_contentPath;

clsv::ClsvJob g_jobs[clsv::kMaxSlots];
bool g_prevSpawned[clsv::kMaxSlots] = {};
unsigned long long g_tick = 0;
unsigned g_gen = 0;

using clientcmd_fn = void(__cdecl*)(void*);
clientcmd_fn g_originalClientCommand = nullptr;
void* g_geClientCommandSlot = nullptr;
char* g_edictsCache = nullptr;
int g_edictSizeCache = 0;

void NoteSent(long long n) {
    g_sent += n;
    Clsv_SetOutputs(g_sent, g_jobsDone, g_errors);
}

void NoteError() {
    ++g_errors;
    Clsv_SetOutputs(g_sent, g_jobsDone, g_errors);
}

void NoteJobDone() {
    ++g_jobsDone;
    Clsv_SetOutputs(g_sent, g_jobsDone, g_errors);
}

bool SameNoCase(const char* a, const char* b) {
    if (!a || !b)
        return false;
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? static_cast<char>(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? static_cast<char>(*b + 32) : *b;
        if (ca != cb)
            return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

const char* TailName(const char* path) {
    if (!path)
        return "";
    const char* tail = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '/' || *p == '\\')
            tail = p + 1;
    }
    return tail;
}

int SlotForEdict(void* ent) {
    char* edicts = GameEdicts();
    if (!edicts)
        edicts = g_edictsCache;
    const int stride = g_edictSizeCache > 0 ? g_edictSizeCache : static_cast<int>(kEdictStride);
    if (!ent || !edicts || stride <= 0)
        return -1;
    const auto diff = static_cast<char*>(ent) - edicts;
    if (diff > 0 && diff % stride == 0) {
        const int slot1 = static_cast<int>(diff / stride);  // 1-based
        const int maxClients = GameMaxClients();
        if (slot1 >= 1 && maxClients > 0 && slot1 <= maxClients)
            return slot1 - 1;
    }
    return -1;
}

bool StartJob(int slot, const char* why) {
    if (!g_hasContent) {
        Buddy_DebugPrintf("clsv: no content loaded (clsv_load <path> first)\n");
        return false;
    }
    const int maxClients = GameMaxClients();
    char* edicts = GameEdicts();
    if (!IsValidUserPointer(edicts) || maxClients < 1 || slot < 0 || slot >= maxClients) {
        Buddy_DebugPrintf("clsv: server not ready / slot %d out of range\n", slot);
        return false;
    }
    char* edict = edicts + static_cast<unsigned>(slot + 1) * kEdictStride;
    if (!EdictInUse(edict)) {
        Buddy_DebugPrintf("clsv: slot %d is not in use\n", slot);
        return false;
    }
    clsv::ClsvJob& job = g_jobs[slot];
    if (job.phase != clsv::CLSV_IDLE) {
        Buddy_DebugPrintf("clsv: slot %d already has an active job (gen %u)\n", slot, job.gen);
        return false;
    }
    clsv::ClsvConfig cfg;
    cfg.lanes = Clsv_Lanes();
    cfg.perTick = Clsv_PerTick();
    cfg.intervalFrames = Clsv_Interval();
    cfg.maxEsc = Clsv_MaxEsc();
    cfg = clsv::SanitizeConfig(cfg);

    std::vector<clsv::ClsvSend> plan;
    std::string err;
    ++g_gen;
    if (g_gen == 0)
        ++g_gen;  // never send gen 0 (stale DONE could equal it)
    if (!clsv::BuildPlan(g_content, cfg, g_gen, plan, err)) {
        Buddy_DebugPrintf("clsv: plan failed for '%s': %s\n", g_content.name.c_str(), err.c_str());
        NoteError();
        return false;
    }
    clsv::ClsvJobBegin(job, g_gen, plan, g_tick);
    PrintOut(PRINT_LOG, "[clsv] slot %d: job gen %u started (%s): %u sends, %u chunks, lanes %d x %d/tick\n",
             slot, job.gen, why ? why : "manual", static_cast<unsigned>(plan.size()),
             static_cast<unsigned>(g_content.names.size()), cfg.lanes, cfg.perTick);
    return true;
}

void CancelJob(int slot, const char* why) {
    clsv::ClsvJob& job = g_jobs[slot];
    if (job.phase == clsv::CLSV_IDLE)
        return;
    PrintOut(PRINT_LOG, "[clsv] slot %d: job gen %u cancelled (%s)\n", slot, job.gen,
             why ? why : "operator");
    clsv::ClsvJobReset(job);
}

void FinishJob(int slot, bool acked) {
    clsv::ClsvJob& job = g_jobs[slot];
    const unsigned gen = job.gen;
    const std::size_t total = job.plan.size();
    const int ackGot = job.ackGot;
    clsv::ClsvJobReset(job);
    NoteJobDone();
    if (acked)
        PrintOut(PRINT_LOG, "[clsv] slot %d: job gen %u complete, client acked %d/%u sends\n",
                 slot, gen, ackGot, static_cast<unsigned>(total));
    else
        PrintOut(PRINT_LOG, "[clsv] slot %d: job gen %u complete (no ack, linger elapsed), %u sends\n",
                 slot, gen, static_cast<unsigned>(total));
}

// One ticker pass over all slots: spawn-edge autostart, paced batches,
// linger completion, disconnect cancel. Runs on the engine thread (SV_Frame
// Post), same context stock game code calls gi.unicast from.
void TickJobs() {
    if (!Clsv_IsEnabled())
        return;
    void* clients = SvsClientsPtr();
    const int maxClients = GameMaxClients();
    char* edicts = GameEdicts();
    if (!clients || !IsValidUserPointer(edicts) || maxClients < 1)
        return;

    const bool autoSend = Clsv_AutoSend();
    const unsigned ackWait = Clsv_AckWaitMs();
    const std::uint32_t nowMs = GetTickCount();

    // Slots past maxClients are not addressable: forget their edges so a
    // later maxclients raise re-autostarts cleanly.
    for (int s = maxClients; s < clsv::kMaxSlots; ++s)
        g_prevSpawned[s] = false;

    clsv::ClsvConfig cfg;
    cfg.lanes = Clsv_Lanes();
    cfg.perTick = Clsv_PerTick();
    cfg.intervalFrames = Clsv_Interval();
    cfg.maxEsc = Clsv_MaxEsc();
    cfg = clsv::SanitizeConfig(cfg);

    for (int slot = 0; slot < maxClients; ++slot) {
        const int st = EngineClientState(clients, slot);
        const bool spawned = (st == kCsSpawned);
        clsv::ClsvJob& job = g_jobs[slot];

        if (!spawned) {
            if (job.phase != clsv::CLSV_IDLE)
                CancelJob(slot, "slot left the game");
            g_prevSpawned[slot] = false;
            continue;
        }

        if (!g_prevSpawned[slot]) {
            // Rising spawned edge: fresh connect (or map re-spawn).
            g_prevSpawned[slot] = true;
            if (autoSend && g_hasContent && job.phase == clsv::CLSV_IDLE)
                StartJob(slot, "auto-send on spawn");
            continue;  // first tick after spawn: let baselines settle
        }

        if (job.phase == clsv::CLSV_SENDING) {
            char* edict = edicts + static_cast<unsigned>(slot + 1) * kEdictStride;
            if (!EdictInUse(edict)) {
                CancelJob(slot, "edict went out of use mid-job");
                continue;
            }
            const std::size_t n = clsv::ClsvBatchCount(job, cfg, g_tick);
            bool failed = false;
            for (std::size_t k = 0; k < n; ++k) {
                const std::string wire = clsv::FormatWire(job.plan[job.pos]);
                if (!Buddy_StuffText(edict, wire.c_str())) {
                    Buddy_DebugPrintf("clsv: delivery to slot %d failed at send %u\n", slot,
                                      static_cast<unsigned>(job.pos));
                    failed = true;
                    break;
                }
                ++job.pos;
            }
            if (failed) {
                NoteError();
                CancelJob(slot, "delivery failure");
                continue;
            }
            if (n > 0) {
                job.lastTick = g_tick;
                job.firstBatch = false;
                NoteSent(static_cast<long long>(n));
            }
            if (job.pos >= job.plan.size()) {
                job.phase = clsv::CLSV_LINGER;
                job.doneMs = nowMs;
            }
        } else if (job.phase == clsv::CLSV_LINGER) {
            if (job.acked) {
                FinishJob(slot, true);
            } else if (clsv::ClsvElapsed(nowMs, job.doneMs, ackWait)) {
                FinishJob(slot, false);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// ClientCommand snoop: best-effort `pipeack <got>` confirmation.
// The client listener emits it as a bare command; stock Q2 forwards unknown
// client commands to the server (Cmd_ForwardToServer), where they land in
// the game DLL's ClientCommand. Always calls through to the original.
// ---------------------------------------------------------------------------
void __cdecl HkClientCommand(void* ent) {
    bool ours = false;
    int got = -1;
    int slot = -1;
    if (Clsv_IsEnabled() && ent) {
        const char* cmd = Buddy_ClientArgv(0);
        if (cmd && (SameNoCase(cmd, "pipeack") || SameNoCase(cmd, ".pipeack"))) {
            ours = true;
            slot = SlotForEdict(ent);
            const char* a1 = Buddy_ClientArgv(1);
            if (a1 && *a1)
                got = std::atoi(a1);
        }
    }
    if (g_originalClientCommand)
        g_originalClientCommand(ent);
    if (ours && slot >= 0 && slot < clsv::kMaxSlots) {
        clsv::ClsvJob& job = g_jobs[slot];
        if (job.phase == clsv::CLSV_SENDING || job.phase == clsv::CLSV_LINGER) {
            job.acked = true;
            job.ackGot = got;
            PrintOut(PRINT_LOG, "[clsv] slot %d: ack gen %u got=%d\n", slot, job.gen, got);
        }
    }
}

void InstallClientCommandHook(void* gameExport) {
    if (g_originalClientCommand)
        return;
    if (!gameExport) {
        PrintOut(PRINT_BAD, "[clsv] ClientCommand snoop skipped: no game export\n");
        return;
    }
    if (!Readable(static_cast<char*>(gameExport) + kExportClientCommandOfs, sizeof(void*)) ||
        !Readable(static_cast<char*>(gameExport) + kExportEdictsOfs, sizeof(void*)) ||
        !Readable(static_cast<char*>(gameExport) + kExportEdictSizeOfs, sizeof(int))) {
        PrintOut(PRINT_BAD, "[clsv] ClientCommand snoop skipped: unreadable ge table\n");
        return;
    }
    void** hookSlot =
        reinterpret_cast<void**>(static_cast<char*>(gameExport) + kExportClientCommandOfs);
    void* target = *hookSlot;
    char* edicts = *reinterpret_cast<char**>(static_cast<char*>(gameExport) + kExportEdictsOfs);
    const int edictSize =
        *reinterpret_cast<int*>(static_cast<char*>(gameExport) + kExportEdictSizeOfs);
    if (!target || !edicts || edictSize <= 0) {
        PrintOut(PRINT_BAD, "[clsv] ClientCommand snoop skipped: bad target/edicts\n");
        return;
    }
    if (!IsExecutableCodeAddress(target)) {
        PrintOut(PRINT_BAD, "[clsv] ClientCommand snoop skipped: target not executable\n");
        return;
    }
    g_edictsCache = edicts;
    g_edictSizeCache = edictSize;
    g_originalClientCommand = reinterpret_cast<clientcmd_fn>(target);
    g_geClientCommandSlot = hookSlot;
    *hookSlot = reinterpret_cast<void*>(&HkClientCommand);
    PrintOut(PRINT_LOG, "[clsv] ClientCommand snooped (ge+0x34, ack only, call-through)\n");
}

// ---------------------------------------------------------------------------
// Server-console commands.
// ---------------------------------------------------------------------------
bool LoadContentFile(const char* path) {
    if (!path || !*path) {
        if (g_contentPath.empty()) {
            Buddy_DebugPrintf("usage: clsv_load <path>  (parser .cfg with set chunks)\n");
            return false;
        }
        path = g_contentPath.c_str();  // reload last
    }
    FILE* f = fopen(path, "rb");
    if (!f) {
        Buddy_DebugPrintf("clsv: cannot open '%s'\n", path);
        NoteError();
        return false;
    }
    std::string text;
    text.reserve(8192);
    char buf[4096];
    std::size_t total = 0;
    std::size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        total += n;
        if (total > kMaxFileBytes) {
            fclose(f);
            Buddy_DebugPrintf("clsv: file too large (>%u bytes)\n",
                              static_cast<unsigned>(kMaxFileBytes));
            NoteError();
            return false;
        }
        text.append(buf, n);
    }
    fclose(f);

    clsv::ClsvContent content;
    std::string err;
    clsv::ClsvConfig cfg;
    cfg.maxEsc = Clsv_MaxEsc();
    cfg = clsv::SanitizeConfig(cfg);
    if (!clsv::ParseLoader(text, TailName(path), cfg.maxEsc, content, err)) {
        Buddy_DebugPrintf("clsv: load failed: %s\n", err.c_str());
        NoteError();
        return false;
    }
    g_content = content;
    g_hasContent = true;
    g_contentPath = path;
    PrintOut(PRINT_LOG, "[clsv] loaded '%s': %u chunks, %u entries (maxEsc %d)\n",
             content.name.c_str(), static_cast<unsigned>(content.names.size()),
             static_cast<unsigned>(content.entries.size()), cfg.maxEsc);
    return true;
}

extern "C" void __cdecl clsv_Load_f();
extern "C" void __cdecl clsv_Send_f();
extern "C" void __cdecl clsv_Cancel_f();
extern "C" void __cdecl clsv_Status_f();

extern "C" void __cdecl clsv_Load_f() {
    if (!Clsv_IsEnabled()) {
        Buddy_DebugPrintf("clsv is disabled (_sofbuddy_clsv 0)\n");
        NoteError();
        return;
    }
    if (!EngineCmdReady()) {
        Buddy_DebugPrintf("clsv: engine arg accessors unavailable\n");
        NoteError();
        return;
    }
    const int argc = SOF_EP_Cmd_Argc();
    if (argc > 2) {
        Buddy_DebugPrintf("usage: clsv_load <path>\n");
        NoteError();
        return;
    }
    const char* path = argc == 2 ? SOF_EP_Cmd_Argv(1) : nullptr;
    if (path && (!IsValidUserPointer(path) || !IsSafeMemoryBlock(path, 1)))
        path = nullptr;
    LoadContentFile(path);
}

extern "C" void __cdecl clsv_Send_f() {
    if (!Clsv_IsEnabled()) {
        Buddy_DebugPrintf("clsv is disabled (_sofbuddy_clsv 0)\n");
        NoteError();
        return;
    }
    if (!EngineCmdReady()) {
        Buddy_DebugPrintf("clsv: engine arg accessors unavailable\n");
        NoteError();
        return;
    }
    const int argc = SOF_EP_Cmd_Argc();
    if (argc != 2) {
        Buddy_DebugPrintf("usage: clsv_send <slot|all>\n");
        NoteError();
        return;
    }
    const char* arg = SOF_EP_Cmd_Argv(1);
    if (!arg || !IsValidUserPointer(arg) || !IsSafeMemoryBlock(arg, 1)) {
        NoteError();
        return;
    }
    const int maxClients = GameMaxClients();
    if (maxClients < 1) {
        Buddy_DebugPrintf("clsv: server not ready\n");
        NoteError();
        return;
    }
    if (_stricmp(arg, "all") == 0) {
        if (!g_hasContent) {
            Buddy_DebugPrintf("clsv: no content loaded (clsv_load <path> first)\n");
            NoteError();
            return;
        }
        int started = 0;
        for (int slot = 0; slot < maxClients; ++slot) {
            char* edicts = GameEdicts();
            if (!IsValidUserPointer(edicts))
                break;
            char* edict = edicts + static_cast<unsigned>(slot + 1) * kEdictStride;
            if (EdictInUse(edict) && g_jobs[slot].phase == clsv::CLSV_IDLE &&
                StartJob(slot, "manual all"))
                ++started;
        }
        if (started == 0) {
            Buddy_DebugPrintf("clsv: nothing started (no idle spawned slots / no content)\n");
            NoteError();
        }
        return;
    }
    char* end = nullptr;
    const long v = std::strtol(arg, &end, 10);
    if (!end || *end != '\0' || v < 0) {
        Buddy_DebugPrintf("usage: clsv_send <slot|all>  (slot is 0-based)\n");
        NoteError();
        return;
    }
    int slot = static_cast<int>(v);
    if (slot == maxClients)
        slot = maxClients - 1;
    if (slot < 0 || slot >= maxClients) {
        Buddy_DebugPrintf("clsv: slot out of range (0-%d)\n", maxClients - 1);
        NoteError();
        return;
    }
    if (!StartJob(slot, "manual"))
        NoteError();
}

extern "C" void __cdecl clsv_Cancel_f() {
    if (!EngineCmdReady()) {
        Buddy_DebugPrintf("clsv: engine arg accessors unavailable\n");
        return;
    }
    const int argc = SOF_EP_Cmd_Argc();
    if (argc > 2) {
        Buddy_DebugPrintf("usage: clsv_cancel [slot|all]\n");
        return;
    }
    const int maxClients = GameMaxClients();
    if (maxClients < 1)
        return;
    if (argc == 1) {
        for (int slot = 0; slot < maxClients; ++slot)
            CancelJob(slot, "cancel all");
        return;
    }
    const char* arg = SOF_EP_Cmd_Argv(1);
    if (!arg || !IsValidUserPointer(arg) || !IsSafeMemoryBlock(arg, 1))
        return;
    if (_stricmp(arg, "all") == 0) {
        for (int slot = 0; slot < maxClients; ++slot)
            CancelJob(slot, "cancel all");
        return;
    }
    char* end = nullptr;
    const long v = std::strtol(arg, &end, 10);
    if (!end || *end != '\0' || v < 0 || v >= maxClients) {
        Buddy_DebugPrintf("usage: clsv_cancel [slot|all]\n");
        return;
    }
    CancelJob(static_cast<int>(v), "operator");
}

extern "C" void __cdecl clsv_Status_f() {
    Buddy_DebugPrintf("[clsv] enabled=%d auto=%d lanes=%d/tick=%d/interval=%d chunk=%d\n",
                      Clsv_IsEnabled() ? 1 : 0, Clsv_AutoSend() ? 1 : 0, Clsv_Lanes(),
                      Clsv_PerTick(), Clsv_Interval(), Clsv_MaxEsc());
    if (g_hasContent)
        Buddy_DebugPrintf("[clsv] content '%s': %u chunks %u entries (sent=%lld jobs=%lld errors=%lld)\n",
                          g_content.name.c_str(), static_cast<unsigned>(g_content.names.size()),
                          static_cast<unsigned>(g_content.entries.size()), g_sent, g_jobsDone,
                          g_errors);
    else
        Buddy_DebugPrintf("[clsv] content: none (sent=%lld jobs=%lld errors=%lld)\n", g_sent,
                          g_jobsDone, g_errors);
    const int maxClients = GameMaxClients();
    for (int slot = 0; slot < maxClients && slot < clsv::kMaxSlots; ++slot) {
        const clsv::ClsvJob& job = g_jobs[slot];
        if (job.phase == clsv::CLSV_IDLE)
            continue;
        Buddy_DebugPrintf("[clsv] slot %d: gen %u %s %u/%u%s\n", slot, job.gen,
                          job.phase == clsv::CLSV_SENDING ? "sending" : "linger",
                          static_cast<unsigned>(job.pos),
                          static_cast<unsigned>(job.plan.size()),
                          job.acked ? " acked" : "");
    }
    DlUnlock_Status();
}

}  // namespace

// SV_Frame Post: the whole pipeline ticks here. Near-zero cost when idle
// (one client-table scan, no sends without an active job). The download
// unlock ticks independently of the clsv master switch (own master cvar).
void clsv_SvFramePost(int msec) {
    (void)msec;
    ++g_tick;
    DlUnlock_Tick();
    if (!Clsv_IsEnabled())
        return;
    TickJobs();
}

void clsv_OnGameDllLoaded(void* gameExport) {
    Clsv_InitCvars();
    Clsv_SetOutputs(g_sent, g_jobsDone, g_errors);
    if (!detour_Cmd_AddCommand::oCmd_AddCommand ||
        !IsExecutableCodeAddress(
            reinterpret_cast<const void*>(detour_Cmd_AddCommand::oCmd_AddCommand))) {
        PrintOut(PRINT_BAD, "[clsv] Cmd_AddCommand unavailable - commands not registered\n");
        return;
    }
    InstallCommand("clsv_load", reinterpret_cast<void*>(&clsv_Load_f));
    InstallCommand("clsv_send", reinterpret_cast<void*>(&clsv_Send_f));
    InstallCommand("clsv_cancel", reinterpret_cast<void*>(&clsv_Cancel_f));
    InstallCommand("clsv_status", reinterpret_cast<void*>(&clsv_Status_f));
    InstallClientCommandHook(gameExport);
    DlUnlock_Tick();  // install the download unlock now if enabled
    PrintOut(PRINT_LOG, "[clsv] server commands registered\n");
}

extern "C" void Clsv_Shutdown() {
    DlUnlock_Shutdown();
    for (int i = 0; i < clsv::kMaxSlots; ++i)
        clsv::ClsvJobReset(g_jobs[i]);
    if (g_originalClientCommand && g_geClientCommandSlot &&
        Readable(g_geClientCommandSlot, sizeof(void*))) {
        *reinterpret_cast<void**>(g_geClientCommandSlot) =
            reinterpret_cast<void*>(g_originalClientCommand);
    }
    g_originalClientCommand = nullptr;
    g_geClientCommandSlot = nullptr;
    g_edictsCache = nullptr;
    g_edictSizeCache = 0;
    // cvar.h's detach contract: hand the engine back its strings.
    Clsv_ShutdownCvars();
}
