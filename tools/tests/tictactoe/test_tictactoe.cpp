// Pure-logic tests: rules, win/draw detection, layout rendering.
#include "tictactoe_logic.h"
#include "minigames_api.h"

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

static void test_x_row_win() {
    TttGame g;
    TttReset(g, 2, 5);
    CHECK(TttPlay(g, 0), "x1");
    CHECK(TttPlay(g, 3), "o1");
    CHECK(TttPlay(g, 1), "x2");
    CHECK(TttPlay(g, 4), "o2");
    CHECK(TttPlay(g, 2), "x3 wins");
    CHECK(g.over && g.winner == kTttX, "x row win detected");
    CHECK(!TttPlay(g, 5), "no moves after win");
}

static void test_o_diag_win() {
    TttGame g;
    TttReset(g, 1, 2);
    CHECK(TttPlay(g, 0), "x");
    CHECK(TttPlay(g, 2), "o");
    CHECK(TttPlay(g, 1), "x");
    CHECK(TttPlay(g, 4), "o");
    CHECK(TttPlay(g, 3), "x");
    CHECK(TttPlay(g, 6), "o wins anti-diag");
    CHECK(g.over && g.winner == kTttO, "o diag win detected");
}

static void test_draw() {
    TttGame g;
    TttReset(g, 1, 2);
    const int seq[9] = {0, 1, 2, 4, 3, 5, 7, 6, 8};
    for (int i = 0; i < 9; ++i)
        CHECK(TttPlay(g, seq[i]), "draw seq");
    CHECK(g.over && g.winner == kTttDraw, "draw detected");
}

static void test_illegal() {
    TttGame g;
    TttReset(g, 1, 2);
    CHECK(!TttPlay(g, -1), "negative cell");
    CHECK(!TttPlay(g, 9), "cell 9 oob");
    CHECK(TttPlay(g, 4), "center ok");
    CHECK(!TttPlay(g, 4), "occupied rejected");
    CHECK(g.turn == kTttO, "turn alternates");
}

static void test_layout() {
    TttGame g;
    TttReset(g, 2, 5);
    TttPlay(g, 0);
    TttPlay(g, 4);
    MgCanvas c;
    TttRenderBoard(g, c);
    CHECK(c.len > 0 && c.len < 1024, "board fits client 0x400 cap with margin");
    CHECK(std::strstr(c.text, "picn sb/tt/x") != nullptr, "X sprite drawn");
    CHECK(std::strstr(c.text, "picn sb/tt/o") != nullptr, "O sprite drawn");
    CHECK(std::strstr(c.text, "string \"3\"") != nullptr, "empty cell shows number");
    CHECK(std::strstr(c.text, "slot1") != nullptr && std::strstr(c.text, "slot4") != nullptr,
          "slots shown (0-based in title)");
    CHECK(std::strstr(c.text, "to move") != nullptr, "status shown");
    MgCanvas cur;
    TttRenderBoard(g, cur, nullptr, 4);
    CHECK(std::strstr(cur.text, "picn sb/tt/s") != nullptr, "cursor selector drawn");
    char st[96];
    TttStatusText(g, st, sizeof(st));
    CHECK(std::strstr(st, "X") != nullptr, "x to move");
}

static void test_canvas() {
    MgCanvas c;
    MgCanvasClear(c);
    CHECK(MgCanvasText(c, 170, 140, "hi"), "text appends");
    CHECK(std::strstr(c.text, "xv 10 yv 20 string \"hi\"") != nullptr, "text token");
    CHECK(MgCanvasCenter(c, 320, 70, "title"), "center appends");
    CHECK(std::strstr(c.text, "xv 160 yv -50 cstring \"title\"") != nullptr, "center token");
    CHECK(MgCanvasPic(c, 165, 126, "pics/x"), "pic appends");
    CHECK(std::strstr(c.text, "xv 5 yv 6 picn pics/x") != nullptr, "picn token (not pic)");
    CHECK(MgCanvasTc(c, 5), "tc appends");
    CHECK(std::strstr(c.text, "tc 5") != nullptr, "tc token");
    // Overflow is refused, old bytes kept (tokens are stack-capped at 255,
    // so overflow only happens by accumulation).
    MgCanvas big;
    MgCanvasClear(big);
    int fills = 0;
    while (MgCanvasText(big, 0, 0, "0123456789"))
        ++fills;
    CHECK(fills > 0, "canvas fills up");
    CHECK(big.len > 0 && big.len < kMgLayoutCap, "len within cap");
    CHECK(big.text[big.len] == '\0', "NUL-terminated at len");
}

static void test_win_status() {
    TttGame g;
    TttReset(g, 2, 5);
    TttPlay(g, 0);
    TttPlay(g, 3);
    TttPlay(g, 1);
    TttPlay(g, 4);
    TttPlay(g, 2);
    CHECK(g.over && g.winner == kTttX, "x wins");
    char st[96];
    TttStatusText(g, st, sizeof(st));
    CHECK(std::strstr(st, "WINS") != nullptr, "win banner text");
    CHECK(std::strstr(st, "slot 1") != nullptr, "winner slot in banner");
}

int main() {
    test_x_row_win();
    test_o_diag_win();
    test_draw();
    test_illegal();
    test_layout();
    test_canvas();
    test_win_status();
    if (fails) {
        std::printf("%d test(s) failed\n", fails);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
