#pragma once

// Pure TicTacToe rules + layout renderer. No engine calls, no windows.h —
// unit-testable on Linux. Board drawing uses the platform canvas API.

#include "../minigames_api.h"

#include <cstdio>
#include <cstring>

constexpr int kTttCells = 9;
constexpr int kTttEmpty = 0;
constexpr int kTttX = 1;
constexpr int kTttO = 2;
constexpr int kTttNoWinner = 0;
constexpr int kTttDraw = 3;

// Client layout_string is 0x400; stay far below it (board is ~250 chars).
constexpr int kTttLayoutCap = 1024;

// Board geometry: 64px picn cells with 8px gaps, centered on 640x480.
constexpr int kTttCellPx = 64;
constexpr int kTttGapPx = 8;
constexpr int kTttBoardPad = 8;
constexpr int kTttBoardPicW = 224;  // 208 grid + 8px pad each side
constexpr int kTttGridX0 = 320 - (3 * kTttCellPx + 2 * kTttGapPx) / 2;  // 216
constexpr int kTttBoardPicY = (kMgVirtH - kTttBoardPicW) / 2;            // 128, centred on 480
constexpr int kTttGridY0 = kTttBoardPicY + kTttBoardPad;                   // 136
constexpr int kTttBoardPicX = kTttGridX0 - kTttBoardPad;                   // 208
constexpr int kTttBoardTitleY = 48;
constexpr int kTttBoardStatusY = kTttBoardPicY + kTttBoardPicW + 16;       // 368
constexpr char kTttPicBoard[] = "sb/tt/b";
constexpr char kTttPicX[] = "sb/tt/x";
constexpr char kTttPicO[] = "sb/tt/o";
constexpr char kTttPicSel[] = "sb/tt/s";

struct TttGame {
    int cell[kTttCells];
    int turn;    // kTttX or kTttO: who moves next
    bool over;
    int winner;  // kTttNoWinner / kTttX / kTttO / kTttDraw
    int slotX;   // internal edict slot (1-based)
    int slotO;
    int cursor;  // 0-8 grid selector for usercmd controls
};

inline void TttReset(TttGame& g, int slotX, int slotO) {
    for (int i = 0; i < kTttCells; ++i)
        g.cell[i] = kTttEmpty;
    g.turn = kTttX;
    g.over = false;
    g.winner = kTttNoWinner;
    g.slotX = slotX;
    g.slotO = slotO;
    g.cursor = 4;
}

inline int TttWinnerOf(const int cell[kTttCells]) {
    static const int kLines[8][3] = {
        {0, 1, 2}, {3, 4, 5}, {6, 7, 8},  // rows
        {0, 3, 6}, {1, 4, 7}, {2, 5, 8},  // cols
        {0, 4, 8}, {2, 4, 6},              // diags
    };
    for (const auto& ln : kLines) {
        const int a = cell[ln[0]];
        if (a != kTttEmpty && a == cell[ln[1]] && a == cell[ln[2]])
            return a;
    }
    for (int i = 0; i < kTttCells; ++i)
        if (cell[i] == kTttEmpty)
            return kTttNoWinner;
    return kTttDraw;
}

// Applies cellIdx (0-based) for the side to move. False = illegal.
inline bool TttPlay(TttGame& g, int cellIdx) {
    if (g.over || cellIdx < 0 || cellIdx >= kTttCells)
        return false;
    if (g.cell[cellIdx] != kTttEmpty)
        return false;
    g.cell[cellIdx] = g.turn;
    g.winner = TttWinnerOf(g.cell);
    g.over = (g.winner != kTttNoWinner);
    if (!g.over)
        g.turn = (g.turn == kTttX) ? kTttO : kTttX;
    return true;
}

inline char TttCellGlyph(const int cell[kTttCells], int i) {
    if (cell[i] == kTttX)
        return 'X';
    if (cell[i] == kTttO)
        return 'O';
    return static_cast<char>('1' + i);
}

inline void TttStatusText(const TttGame& g, char* out, int cap) {
    if (cap <= 0)
        return;
    if (!g.over) {
        const int slot = (g.turn == kTttX) ? g.slotX : g.slotO;
        std::snprintf(out, static_cast<std::size_t>(cap), "%c (slot %d) to move",
                      g.turn == kTttX ? 'X' : 'O', slot - 1);
    } else if (g.winner == kTttDraw) {
        std::snprintf(out, static_cast<std::size_t>(cap), "DRAW!");
    } else {
        const int slot = (g.winner == kTttX) ? g.slotX : g.slotO;
        std::snprintf(out, static_cast<std::size_t>(cap), "%c (slot %d) WINS!",
                      g.winner == kTttX ? 'X' : 'O', slot - 1);
    }
    out[cap - 1] = '\0';
}

// Renders the board onto a platform canvas: picn sprites for X/O,
// numbered text for empty cells, coloured title + status. All tokens
// verified against SCR_ExecuteLayoutString. cursorCell < 0 hides selector.
inline void TttRenderBoard(const TttGame& g, MgCanvas& c, const char* statusOverride = nullptr,
                           int cursorCell = -1) {
    MgCanvasClear(c);
    MgCanvasPic(c, kTttBoardPicX, kTttBoardPicY, kTttPicBoard);
    char title[64];
    std::snprintf(title, sizeof(title), "TIC-TAC-TOE  X=slot%d O=slot%d",
                  g.slotX - 1, g.slotO - 1);
    MgCanvasTc(c, kMgColYellow);
    MgCanvasCenter(c, 320, kTttBoardTitleY, title);
    MgCanvasTc(c, kMgColWhite);
    const int pitch = kTttCellPx + kTttGapPx;
    for (int i = 0; i < kTttCells; ++i) {
        const int cx = kTttGridX0 + (i % 3) * pitch;
        const int cy = kTttGridY0 + (i / 3) * pitch;
        if (g.cell[i] == kTttX)
            MgCanvasPic(c, cx, cy, kTttPicX);
        else if (g.cell[i] == kTttO)
            MgCanvasPic(c, cx, cy, kTttPicO);
        else {
            char num[2] = {static_cast<char>('1' + i), '\0'};
            MgCanvasText(c, cx + 28, cy + 28, num);  // 8px glyph in 64px cell
        }
    }
    if (cursorCell >= 0 && cursorCell < kTttCells && !g.over) {
        const int cx = kTttGridX0 + (cursorCell % 3) * pitch;
        const int cy = kTttGridY0 + (cursorCell / 3) * pitch;
        MgCanvasPic(c, cx, cy, kTttPicSel);
    }
    char status[96];
    if (statusOverride && statusOverride[0])
        std::snprintf(status, sizeof(status), "%s", statusOverride);
    else
        TttStatusText(g, status, sizeof(status));
    const int scol = !g.over ? kMgColYellow
                     : g.winner == kTttDraw ? kMgColWhite
                     : g.winner == kTttX    ? kMgColRed
                                            : kMgColGreen;
    MgCanvasTc(c, scol);
    MgCanvasCenter(c, 320, kTttBoardStatusY, status);
    MgCanvasTc(c, kMgColWhite);
}
