// Model of COM_Parse's com_token loops (SoF.exe @ 0x20055470, IDA-verified).
// Proves the parse-guard bound: stock `cmp edx, 0x100` stores 256 chars and
// drops the NUL at [256] (one past the 256-byte token); the fixed
// `cmp edx, 0xFF` keeps 255 chars with the NUL at [255].

#include <cstdint>
#include <cstdio>
#include <cstring>

static int fails = 0;

#define CHECK(cond, msg)                          \
    do {                                          \
        if (!(cond)) {                            \
            std::printf("FAIL: %s\n", msg);       \
            ++fails;                              \
        }                                         \
    } while (0)

namespace {

// Mirrors the quoted loop:
//   cmp edx, cap / jge skip-store / mov token[edx], cl / inc edx
//   ... epilogue: mov token[edx], 0
// Returns the NUL index. `buf` must have room for cap+1 so the stock
// overflow is observable via the sentinel instead of UB.
int Quoted(const char* text, int len, std::uint8_t* buf, int cap) {
    int edx = 0;
    for (int i = 0; i < len; ++i) {
        if (edx >= cap)
            continue;  // keep scanning, stop storing (jge)
        buf[edx++] = static_cast<std::uint8_t>(text[i]);
    }
    buf[edx] = 0;
    return edx;
}

// Mirrors the unquoted loop plus its exit gate:
//   cmp edx, exitCap / jnz nul / xor edx, edx(=empty on overflow)
//   nul: mov token[edx], 0
int Unquoted(const char* text, int len, std::uint8_t* buf, int storeCap,
             int exitCap) {
    int edx = 0;
    for (int i = 0; i < len; ++i) {
        if (edx >= storeCap)
            continue;
        buf[edx++] = static_cast<std::uint8_t>(text[i]);
    }
    if (edx == exitCap)
        edx = 0;
    buf[edx] = 0;
    return edx;
}

}  // namespace

int main() {
    constexpr int kToken = 256;
    // +1 so the stock NUL@[256] hits the sentinel instead of UB.
    std::uint8_t buf[kToken + 1];

    char long300[301];
    std::memset(long300, 'a', 300);
    long300[300] = '\0';
    char tok255[256];
    std::memset(tok255, 'b', 255);
    tok255[255] = '\0';

    // --- stock bound (cap 0x100): reproduces the bug ---
    std::memset(buf, 0xAA, sizeof(buf));
    int nul = Quoted(long300, 300, buf, 0x100);
    CHECK(nul == 256, "stock quoted 300-char token: NUL at [256]");
    CHECK(buf[kToken] == 0, "stock quoted: sentinel clobbered (OOB write)");
    for (int i = 0; i < 256; ++i)
        CHECK(buf[i] == 'a', "stock quoted: 256 chars stored");

    // --- fixed bound (cap 0xFF): 255 chars, NUL at [255] ---
    std::memset(buf, 0xAA, sizeof(buf));
    nul = Quoted(long300, 300, buf, 0xFF);
    CHECK(nul == 255, "fixed quoted 300-char token: NUL at [255]");
    CHECK(buf[kToken] == 0xAA, "fixed quoted: sentinel intact, no OOB");
    for (int i = 0; i < 255; ++i)
        CHECK(buf[i] == 'a', "fixed quoted: first 255 chars kept");
    CHECK(buf[255] == 0, "fixed quoted: NUL at [255]");

    // A 255-char token still passes through whole.
    std::memset(buf, 0xAA, sizeof(buf));
    nul = Quoted(tok255, 255, buf, 0xFF);
    CHECK(nul == 255, "fixed quoted 255-char token: NUL at [255]");
    for (int i = 0; i < 255; ++i)
        CHECK(buf[i] == 'b', "fixed quoted: 255-char token preserved");
    CHECK(buf[kToken] == 0xAA, "fixed quoted: no OOB on exact-fit token");

    // --- unquoted path ---
    // Stock: 300-char unquoted token trips the exit gate (edx == 0x100)
    // and comes back empty.
    std::memset(buf, 0xAA, sizeof(buf));
    nul = Unquoted(long300, 300, buf, 0x100, 0x100);
    CHECK(nul == 0 && buf[0] == 0, "stock unquoted overflow: empty token");
    // Fixed: store gates cap at 0xFF, exit gate (still 0x100) goes dead,
    // so the token truncates to 255 like the quoted path.
    std::memset(buf, 0xAA, sizeof(buf));
    nul = Unquoted(long300, 300, buf, 0xFF, 0x100);
    CHECK(nul == 255, "fixed unquoted 300-char token: truncates, NUL at [255]");
    CHECK(buf[kToken] == 0xAA, "fixed unquoted: sentinel intact");
    std::memset(buf, 0xAA, sizeof(buf));
    nul = Unquoted(tok255, 255, buf, 0xFF, 0x100);
    CHECK(nul == 255, "fixed unquoted 255-char token: NUL at [255]");

    if (fails) {
        std::printf("%d test(s) failed\n", fails);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
