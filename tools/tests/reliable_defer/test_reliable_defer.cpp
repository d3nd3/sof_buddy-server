#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

#include "engine.h"
#include "reliable_defer_logic.h"

constexpr int kCsConnected = 2;

static int g_fails = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("FAIL %d: %s\n", __LINE__, #cond);                     \
            ++g_fails;                                                         \
        }                                                                      \
    } while (0)

int main() {
    RelDefPolicy p;
    p.reserveBytes = 256;
    p.onePerTick = true;
    p.maxQueue = 4;
    p.maxQueueBytes = 1000;

    const RelDefLimits sp = RelDef_ComputeLimits(16384, p);  // maxclients==1
    CHECK(sp.msgMaxsize == 16368);
    CHECK(sp.softCap == 8192);
    CHECK(sp.frameReserve == 8192);
    CHECK(sp.maxDripBytes == 16384 - 8 - 8192);
    CHECK(sp.stagingReserve == 2046);

    const RelDefLimits mp = RelDef_ComputeLimits(1400, p);  // maxclients>1
    CHECK(mp.msgMaxsize == 1384);
    CHECK(mp.softCap == 700);
    CHECK(mp.maxDripBytes == 1400 - 8 - 700);
    CHECK(mp.stagingReserve == 256);

    p.onePerTick = false;
    CHECK(RelDef_Classify(0, 50, 0, 0, 0, mp, p, kCsSpawned) == RELDEF_WRITE_NOW);
    p.onePerTick = true;
    CHECK(RelDef_Classify(0, 50, 0, 0, 0, mp, p, kCsSpawned) == RELDEF_QUEUE);
    CHECK(RelDef_Classify(0, 50, 0, 0, 0, mp, p, kCsConnected) == RELDEF_WRITE_NOW);

    CHECK(RelDef_Classify(0, 50, 1, 0, 0, mp, p) == RELDEF_QUEUE);
    CHECK(RelDef_Classify(10, 50, 0, 0, 0, mp, p) == RELDEF_QUEUE);
    CHECK(RelDef_Classify(0, 50, 0, 2, 0, mp, p) == RELDEF_QUEUE);
    CHECK(RelDef_Classify(1200, 50, 0, 0, 0, mp, p) == RELDEF_QUEUE);
    CHECK(RelDef_Classify(0, 20000, 0, 0, 0, mp, p) == RELDEF_DROP);

    CHECK(RelDef_CanDrip(0, 0, 800, sp));
    CHECK(RelDef_CanDrip(0, 4000, 4000, sp));
    CHECK(RelDef_CanDrip(0, 0, 9000, sp));
    CHECK(!RelDef_CanDrip(1, 0, 100, mp));
    CHECK(RelDef_CanDrip(0, 5, 100, mp));
    CHECK(RelDef_CanDrip(0, mp.maxDripBytes - 10, 20, mp));
    CHECK(RelDef_CanDrip(0, 0, 800, mp));
    CHECK(!RelDef_CanDrip(0, 0, 20000, sp));

    // Frame-first drip: a fat blob waits so the snapshot keeps flowing,
    // with a starvation escape so big mail is delayed, never stuck.
    // (mp.maxDripBytes == 692.)
    {
        CHECK(RelDef_CanDrip(0, 0, 600, mp, true, 0, 10));
        CHECK(!RelDef_CanDrip(0, 0, 1000, mp, true, 0, 10));
        CHECK(!RelDef_CanDrip(0, 0, 1000, mp, true, 9, 10));
        CHECK(RelDef_CanDrip(0, 0, 1000, mp, true, 10, 10));
        CHECK(!RelDef_CanDrip(0, 0, 20000, mp, true, 99, 10));
        CHECK(RelDef_CanDrip(0, 0, 1000, mp));
        CHECK(!RelDef_CanDrip(1, 0, 100, mp, true, 99, 10));
    }

    std::uint8_t buf[16] = {};
    int len = 0;
    RelDef_CaptureAppend(buf, len, 16, "ab", 2);
    RelDef_CaptureAppend(buf, len, 16, "cd", 2);
    CHECK(len == 4);
    CHECK(buf[0] == 'a' && buf[3] == 'd');

    // CaptureAppend must not split one MSG_Write* chunk across queued blobs.
    {
        std::vector<std::uint8_t> cap;
        std::vector<std::vector<std::uint8_t>> blobs;
        const int capMax = 100;
        auto flush = [&]() {
            if (!cap.empty()) {
                blobs.push_back(cap);
                cap.clear();
            }
        };
        auto append = [&](const void* data, int len) {
            if (!data || len <= 0 || len > capMax)
                return;
            int room = capMax - static_cast<int>(cap.size());
            if (len > room) {
                flush();
                room = capMax;
            }
            if (len > room)
                return;
            const auto* p = static_cast<const std::uint8_t*>(data);
            cap.insert(cap.end(), p, p + len);
            if (static_cast<int>(cap.size()) >= capMax)
                flush();
        };
        char chunk[80];
        std::memset(chunk, 'a', sizeof(chunk));
        append(chunk, 80);
        append(chunk, 80);  // would split mid-chunk at 100 without flush-first
        flush();
        CHECK(blobs.size() == 2);
        CHECK(blobs[0].size() == 80);
        CHECK(blobs[1].size() == 80);
    }

    // Lockstep bypass phase machine: 4-write units go NOW; short units
    // disarm and classify; stray phases never bypass.
    {
        int ph = 0;
        CHECK(RelDef_DlStep(ph, false) == RELDEF_DL_CLASSIFY);
        CHECK(ph == 0);
        ph = 3;  // opcode armed
        CHECK(RelDef_DlStep(ph, false) == RELDEF_DL_NOW && ph == 2);
        CHECK(RelDef_DlStep(ph, false) == RELDEF_DL_NOW && ph == 1);
        CHECK(RelDef_DlStep(ph, true) == RELDEF_DL_NOW && ph == 0);
        ph = 3;
        RelDef_DlStep(ph, false);
        RelDef_DlStep(ph, false);
        CHECK(RelDef_DlStep(ph, false) == RELDEF_DL_CLASSIFY && ph == 0);
        CHECK(RelDef_IsLockstepOp(0x13) && RelDef_IsLockstepOp(0x24) &&
              RelDef_IsLockstepOp(0x25));
        CHECK(!RelDef_IsLockstepOp(0x0B) && !RelDef_IsLockstepOp(0x00));
    }

    // String-terminated messages: blobs must end at message ends, and an
    // overflow must split at the last complete svc_print (0x0B), never
    // leaving orphan opcode+level at a blob end (client "Illegible").
    {
        const std::uint8_t kSvcPrint = 0x0B;
        const int capMax = 300;
        std::vector<std::uint8_t> cap;
        std::vector<std::vector<std::uint8_t>> blobs;
        auto flushAll = [&]() {
            if (!cap.empty()) {
                blobs.push_back(cap);
                cap.clear();
            }
        };
        auto lastPrintEnd = [&]() {
            int pos = 0, lastEnd = 0;
            const int n = static_cast<int>(cap.size());
            while (pos < n) {
                if (cap[pos] != kSvcPrint)
                    break;
                if (pos + 2 > n)
                    break;
                int k = pos + 2;
                while (k < n && cap[k] != 0)
                    ++k;
                if (k >= n)
                    break;
                pos = k + 1;
                lastEnd = pos;
            }
            return lastEnd;
        };
        auto flushPrefix = [&]() {
            const int end = lastPrintEnd();
            const int n = static_cast<int>(cap.size());
            if (end <= 0 || end >= n) {
                flushAll();
                return;
            }
            blobs.emplace_back(cap.begin(), cap.begin() + end);
            cap.erase(cap.begin(), cap.begin() + end);
        };
        auto append = [&](const void* data, int len, bool endMessage) {
            if (!data || len <= 0 || len > capMax)
                return;
            int room = capMax - static_cast<int>(cap.size());
            if (len > room) {
                flushPrefix();
                room = capMax - static_cast<int>(cap.size());
            }
            if (len > room) {
                flushAll();
                room = capMax;
            }
            if (len > room)
                return;
            const auto* p = static_cast<const std::uint8_t*>(data);
            cap.insert(cap.end(), p, p + len);
            if (endMessage)
                flushAll();
            else if (static_cast<int>(cap.size()) >= capMax)
                flushPrefix();
        };
        // Five prints, each 2 header bytes + ~118B string: total > capMax,
        // each single print fits.
        std::vector<std::uint8_t> stock;
        for (int i = 0; i < 5; ++i) {
            std::uint8_t hdr[2] = {kSvcPrint, 2};
            std::vector<std::uint8_t> str(118, static_cast<std::uint8_t>('a' + i));
            str.push_back(0);
            append(hdr, 1, false);
            append(hdr + 1, 1, false);  // split headers like MSG_WriteByte x2
            append(str.data(), static_cast<int>(str.size()), true);
            stock.push_back(hdr[0]);
            stock.push_back(hdr[1]);
            stock.insert(stock.end(), str.begin(), str.end());
        }
        flushAll();
        CHECK(blobs.size() == 5);
        std::vector<std::uint8_t> joined;
        for (const auto& b : blobs) {
            CHECK(!b.empty() && b.size() <= static_cast<std::size_t>(capMax));
            CHECK(b[0] == kSvcPrint);
            CHECK(b.back() == 0);  // ends at string end: no orphan headers
            joined.insert(joined.end(), b.begin(), b.end());
        }
        CHECK(joined == stock);  // FIFO reassembly == stock byte order
    }

    // svc_equip (0x06) spans WriteString + unhooked WriteLong pairs; per-string
    // blob sealing would split it and corrupt the client parser.
    {
        const std::uint8_t kSvcEquip = 0x06;
        std::vector<std::uint8_t> cap;
        std::vector<std::vector<std::uint8_t>> blobs;
        auto flushAll = [&]() {
            if (!cap.empty()) {
                blobs.push_back(cap);
                cap.clear();
            }
        };
        auto append = [&](const void* data, int len) {
            const auto* p = static_cast<const std::uint8_t*>(data);
            cap.insert(cap.end(), p, p + len);
        };
        std::vector<std::uint8_t> stock;
        append(&kSvcEquip, 1);
        stock.push_back(kSvcEquip);
        const std::uint8_t hdr[] = {1, 2};
        append(hdr, 2);
        stock.insert(stock.end(), hdr, hdr + 2);
        for (int i = 0; i < 2; ++i) {
            const char* name = (i == 0) ? "jackhammer" : "minimi";
            const int nlen = static_cast<int>(std::strlen(name)) + 1;
            append(name, nlen);
            stock.insert(stock.end(), name, name + nlen);
            const std::uint8_t price[4] = {0, 0, 0, static_cast<std::uint8_t>(i + 1)};
            append(price, 4);
            stock.insert(stock.end(), price, price + 4);
        }
        flushAll();
        CHECK(blobs.size() == 1);
        CHECK(blobs[0] == stock);
        CHECK(blobs[0][0] == kSvcEquip);
    }

    return g_fails ? 1 : 0;
}
