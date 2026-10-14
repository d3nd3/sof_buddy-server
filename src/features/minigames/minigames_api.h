#pragma once

// Minigames platform API: everything a 2D minigame (tictactoe now, chess
// later) needs, without touching engine structs directly. Canvas building
// is pure string work (inline here, unit-testable); routing/visibility/push
// go through the parent feature.
//
// Token reference (SCR_ExecuteLayoutString @ 0x20014510, 640x480 baseline):
//   screen_x = xv + viddef_width/2 - 160; screen_y = yv + height/2 - 120.
//   Helpers take virtual 640x480 pixels (320,240 = centre) and emit xv/yv.

#include <cstdio>
#include <cstring>

constexpr int kMgLayoutCap = 1024;  // client layout_string is 0x400
constexpr int kMgMaxSlots = 64;

// Community-reported text colours (tc token); glyphs always carry meaning.
constexpr int kMgColBlack = 0;
constexpr int kMgColGreen = 1;
constexpr int kMgColYellow = 4;
constexpr int kMgColRed = 5;
constexpr int kMgColWhite = 6;
constexpr char kMgPWhite[] = "\001";  // P_WHITE
constexpr char kMgPRed[] = "\002";    // P_RED
constexpr char kMgPGreen[] = "\003";  // P_GREEN
constexpr char kMgPYellow[] = "\004"; // P_YELLOW
constexpr char kMgPPurple[] = "\006"; // P_PURPLE
constexpr char kMgPCyan[] = "\007";   // P_CYAN
constexpr char kMgPBlack[] = "\010";  // P_BLACK
constexpr char kMgPHWhite[] = "\011"; // P_HWHITE
constexpr const char* kMgMenuNormalColor = kMgPHWhite;
constexpr const char* kMgMenuSelectedColor = kMgPWhite;

constexpr int kMgVirtW = 640;
constexpr int kMgVirtH = 480;
constexpr int kMgLayoutXvBias = 160;  // width/2 at 640 — SCR_ExecuteLayoutString
constexpr int kMgLayoutYvBias = 120;  // height/2 at 480

// Short layout paths under sb/ (ghoul file = same + ".m32").
constexpr char kMgSbMgBg[] = "sb/mg/bg";  // opaque black640 panel
constexpr char kMgSbMgPn[] = "sb/mg/pn";  // semi-transparent tile (sof++ c.m32)
constexpr char kMgSbMgId[] = "sb/mg/id";  // idle banner strip
constexpr char kMgSbTtB[] = "sb/tt/b";    // tictactoe board
constexpr char kMgSbTtX[] = "sb/tt/x";    // tictactoe X
constexpr char kMgSbTtO[] = "sb/tt/o";    // tictactoe O
constexpr char kMgSbTtS[] = "sb/tt/s";    // tictactoe cell selector

// usercmd_t layout (SoF q_shared.h): msec+0, buttons+1, lightlevel+2, lean+3,
// angles[3]+4, forwardmove+10, sidemove+12, upmove+14.
constexpr unsigned kMgUcmdOffButtons = 1;
constexpr unsigned kMgUcmdOffLean = 3;
constexpr unsigned kMgUcmdOffForward = 10;
constexpr unsigned kMgUcmdOffSide = 12;
constexpr unsigned kMgUcmdOffUp = 14;
constexpr unsigned kMgBtnAttack = 1;
constexpr unsigned kMgBtnUse = 2;
constexpr int kMgMoveDeadzone = 10;

inline bool MgKeepMinigameOnDeath(bool minigameView, bool intermission,
                                  bool stockScoreboard) {
    return minigameView && !intermission && stockScoreboard;
}

// Parsed clc_move usercmd with edge/release detection. The platform latches
// transitions in SV_ClientThink and delivers them once before server messages.
struct MgUserCmdInput {
    bool forward = false;
    bool back = false;
    bool left = false;
    bool right = false;
    bool forwardEdge = false;
    bool backEdge = false;
    bool leftEdge = false;
    bool rightEdge = false;
    bool leanLeft = false;
    bool leanRight = false;
    bool leanLeftEdge = false;
    bool leanRightEdge = false;
    bool attack = false;
    bool attackEdge = false;
    bool use = false;
    bool useEdge = false;
    bool useRelease = false;  // -use
    bool consumed = false;  // game sets true → platform strips movement/fire
    bool preserveUse = false;  // menu filtering keeps +use for score handling
};

inline int MgLayoutXv(int virtX) {
    return virtX - kMgLayoutXvBias;
}
inline int MgLayoutYv(int virtY) {
    return virtY - kMgLayoutYvBias;
}

// A drawable layout page (stock xv/yv tokens).
struct MgCanvas {
    char text[kMgLayoutCap];
    int len = 0;
};

inline void MgCanvasClear(MgCanvas& c) {
    c.text[0] = '\0';
    c.len = 0;
}

inline bool MgCanvasAppend(MgCanvas& c, const char* token) {
    if (!token)
        return false;
    const int add = static_cast<int>(std::strlen(token));
    if (c.len + add >= kMgLayoutCap)
        return false;
    std::memcpy(c.text + c.len, token, static_cast<std::size_t>(add) + 1);
    c.len += add;
    return true;
}

// Appends one token; false when it would overflow (canvas keeps old bytes).
inline bool MgCanvasText(MgCanvas& c, int x, int y, const char* s) {
    char token[256];
    std::snprintf(token, sizeof(token), "xv %d yv %d string \"%s\" ", MgLayoutXv(x), MgLayoutYv(y),
                  s ? s : "");
    token[sizeof(token) - 1] = '\0';
    return MgCanvasAppend(c, token);
}

inline bool MgCanvasPText(MgCanvas& c, int x, int y, const char* color, const char* s) {
    char token[256];
    std::snprintf(token, sizeof(token), "xv %d yv %d string \"%s%s\" ", MgLayoutXv(x),
                  MgLayoutYv(y), color ? color : "", s ? s : "");
    token[sizeof(token) - 1] = '\0';
    return MgCanvasAppend(c, token);
}

inline bool MgCanvasPAltText(MgCanvas& c, int x, int y, const char* color, const char* s) {
    if (!s)
        s = "";
    char toggled[256];
    int n = 0;
    for (; s[n] && n < static_cast<int>(sizeof(toggled) - 1); ++n)
        toggled[n] = static_cast<char>(static_cast<unsigned char>(s[n]) | 0x80u);
    toggled[n] = '\0';
    char token[512];
    std::snprintf(token, sizeof(token), "xv %d yv %d altstring \"%s%s\" ", MgLayoutXv(x),
                  MgLayoutYv(y), color ? color : "", toggled);
    token[sizeof(token) - 1] = '\0';
    return MgCanvasAppend(c, token);
}

inline bool MgCanvasCenter(MgCanvas& c, int x, int y, const char* s) {
    if (!s)
        s = "";
    const int w = static_cast<int>(std::strlen(s)) * 8;
    return MgCanvasText(c, x - w / 2, y, s);
}

inline bool MgCanvasPCenter(MgCanvas& c, int x, int y, const char* color, const char* s) {
    if (!s)
        s = "";
    const int w = static_cast<int>(std::strlen(s)) * 8;
    return MgCanvasPText(c, x - w / 2, y, color, s);
}

inline bool MgCanvasAltText(MgCanvas& c, int x, int y, const char* s) {
    if (!s)
        s = "";
    char toggled[256];
    int n = 0;
    for (; s[n] && n < static_cast<int>(sizeof(toggled) - 1); ++n)
        toggled[n] = static_cast<char>(static_cast<unsigned char>(s[n]) | 0x80u);
    toggled[n] = '\0';
    char token[512];
    std::snprintf(token, sizeof(token), "xv %d yv %d altstring \"%s\" ", MgLayoutXv(x), MgLayoutYv(y),
                  toggled);
    token[sizeof(token) - 1] = '\0';
    return MgCanvasAppend(c, token);
}

inline bool MgCanvasAltCenter(MgCanvas& c, int x, int y, const char* s) {
    if (!s)
        s = "";
    const int w = static_cast<int>(std::strlen(s)) * 8;
    return MgCanvasAltText(c, x - w / 2, y, s);
}

inline bool MgCanvasPic(MgCanvas& c, int x, int y, const char* pic) {
    char token[256];
    std::snprintf(token, sizeof(token), "xv %d yv %d picn %s ", MgLayoutXv(x), MgLayoutYv(y),
                  pic ? pic : "");
    token[sizeof(token) - 1] = '\0';
    return MgCanvasAppend(c, token);
}

// Sets the text colour state for following strings (tc token, 0-15).
inline bool MgCanvasTc(MgCanvas& c, int color) {
    char token[32];
    std::snprintf(token, sizeof(token), "tc %d ", color & 15);
    token[sizeof(token) - 1] = '\0';
    return MgCanvasAppend(c, token);
}

// Appends a pre-composed token stream verbatim (for scripts driving
// mg_push with their own xv/yv/pic n tokens).
inline bool MgCanvasRaw(MgCanvas& c, const char* tokens) {
    return MgCanvasAppend(c, tokens);
}

// A playable page: `command` is the internal game ID, called with the
// sender's internal slot (edict index). Menu-only pages are not player input.
struct MgGameOps {
    const char* command;
    void (*onClientCmd)(int slot1);
    void (*onUserCmd)(int slot1, MgUserCmdInput* in);       // optional
    void (*onSessionEnd)(int slot1);                        // optional; per-slot preempt
    const char* desc;                                       // optional menu blurb
    const char* menuName;                                   // optional human-readable menu label
    bool menuOnly = false;                                  // internal navigation, not player input
    bool (*onBack)(int slot1) = nullptr;                    // optional nested-menu back
    int (*saveSelected)(int slot1) = nullptr;               // optional menu cursor, kept in the
                                                            // nav-stack entry and restored on pop;
                                                            // -1 = nothing to restore
};

// Returns false when the parent is disabled or the name is taken.
bool MgRegisterGame(const MgGameOps* ops);
int MgRegisteredGameCount();
const char* MgRegisteredGameCommand(int index);
const char* MgRegisteredGameDesc(int index);
const char* MgRegisteredGameMenuName(int index);

// Admin console command helper (engine xcommand_t: void (__cdecl*)(void)).
bool MgRegisterConsoleCommand(const char* name, void* fn);

// Precaches a picture/model/sound name server-side (gi.imageindex) so
// clients can load it for picn. Stock pics (pics/*, env/*) need no setup.
int MgRegisterImage(const char* name);

// Registers a file path in a free CS_GHOULFILES configstring slot (1497+,
// 2048 slots, mostly empty) so clients download it during precache. The
// client's download walker covers this range with no allow_download gate
// and uses the path verbatim (no extension appended) — pass the full name
// (e.g. "sb/tt/x.m32"). Returns the configstring index, 0 on failure.
// Idempotent: re-registering the same path returns its existing index.
// New paths append at the lowest free slot (contiguous tail when no holes).
int MgRegisterGhoulFile(const char* path);

// On CS_MAPCHECKSUM change: strip sb/mg/* + sb/tt/* ghoul slots (tail-first
// via SV_RemoveIndex), then re-register platform + enabled game files compactly.
// (Platform auto-registration disabled — use mg_ghoul_register manually if needed.)

// Current CS_MAPCHECKSUM (empty if unavailable).
const char* MgMapChecksum();

// One minigame display per client slot. `gameId` is the registered client word
// ("mg_ttt", "mg_lag", …) or "mgscript" for script mg_push. Taking display preempts any
// other game on that slot; pushes/updates are ignored unless `gameId` owns it.
bool MgTakeDisplay(int slot1, const char* gameId);
void MgReleaseDisplay(int slot1, const char* gameId);
bool MgDisplayOwnedBy(int slot1, const char* gameId);
bool MgDisplayTakenByOther(int slot1, const char* gameId);

// True while at least one slot displays gameId. Display ownership and
// preemption are per slot; one player's menu cannot end another's session.
bool MgRunningSession(const char* gameId);

// True while the player holds the minigame score tab (+use+score).
bool MgMinigameTabOpen(int slot1);

// Menu navigation. Forward pushes the current display and opens gameId.
// Back pops that stack toward the mg scoreboard root, which is unpoppable:
// back at the root is a no-op, and a refused page falls home to the root
// instead of stranding a blank view. Back never closes everything.
void MgNavigationReset(int slot1);
bool MgNavigateForward(int slot1, const char* gameId);
bool MgNavigateBack(int slot1);

// Updates layout cache + dirty only (no svc_layout). Owner + session required.
void MgPutLayoutCache(int slot1, const char* gameId, const MgCanvas& canvas);

// Layout visibility for a slot. Re-asserts ps.stats[STAT_LAYOUTS] every server
// frame for visible slots (stock G_SetStats clears it). Minigame svc_layout is
// sent only when the canvas changes (MgPushLayout / dirty), not every tick.
void MgShowLayout(int slot1, const char* gameId, bool on);

// Re-shows the current display owner's cached layout without touching the
// nav stack or re-running menu init (stack returns land here with cleared
// view bits, which would otherwise leave a blank board).
void MgReshowCurrentLayout(int slot1);

// Sends the whole canvas to one slot as svc_layout (replaces layout_string).
// On the minigame tab, transmits immediately when visible; otherwise cached.
void MgPushLayout(int slot1, const char* gameId, const MgCanvas& canvas);

// Hides the board and clears the client's stored layout.
void MgClearLayout(int slot1, const char* gameId);

// Minigame score tab with the idle placeholder (no active board).
void MgShowIdleLayout(int slot1, const char* gameId);

// Slot numbering: user-facing commands use 0-based slots (same as stufftext).
// Mg* helpers below take the internal 1-based edict index.
int MgUserToInternal(int slot0);
int MgInternalToUser(int slot1);

bool MgSlotSpawned(int slot1);
void* MgEdictForSlot(int slot1);
int MgSlotForEdict(void* ent);
int MgSlotFromClient(void* client);
void MgParseUserCmd(int slot1, void* cmd, MgUserCmdInput* in);
void MgStripUserCmd(void* cmd);
void MgStripMenuUserCmd(void* cmd);
int MgMaxClients();
int MgArgc();
const char* MgArgv(int n);
bool MgEnabled();
