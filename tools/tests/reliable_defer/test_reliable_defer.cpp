#include <cstdio>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

#include "engine.h"
#include "reliable_defer_logic.h"

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

    {
        RelDefPolicy tick0 = p;
        const RelDefLimits lim0 = RelDef_ComputeLimits(1400, tick0);
        tick0.reserveBytes = 512;
        const RelDefLimits lim1 = RelDef_ComputeLimits(1400, tick0);
        CHECK(lim0.stagingReserve != lim1.stagingReserve);
        const RelDefLimits sp0 = RelDef_ComputeLimits(16384, tick0);
        const RelDefLimits sp1 = RelDef_ComputeLimits(1400, tick0);
        CHECK(sp0.msgMaxsize != sp1.msgMaxsize);
    }

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
        CHECK(RelDef_IsLockstepOp(0x13));
        CHECK(!RelDef_IsLockstepOp(0x24) && !RelDef_IsLockstepOp(0x25));
        CHECK(!RelDef_IsLockstepOp(0x0B) && !RelDef_IsLockstepOp(0x00));
        CHECK(RelDef_WriteThroughOp(0x1A));
        CHECK(!RelDef_WriteThroughOp(0x1B) && !RelDef_WriteThroughOp(0x0B));
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
        auto flushPrefix = [&]() {
            const int n = static_cast<int>(cap.size());
            const int end = RelDef_LastCompleteEnd(cap.data(), n);
            if (end <= 0)
                return;
            if (end >= n) {
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
        const std::uint8_t tail[] = {0, 0};
        append(tail, 2);
        stock.insert(stock.end(), tail, tail + 2);
        flushAll();
        CHECK(blobs.size() == 1);
        CHECK(blobs[0] == stock);
        CHECK(blobs[0][0] == kSvcEquip);
        CHECK(RelDef_LastCompleteEnd(stock.data(), static_cast<int>(stock.size())) ==
              static_cast<int>(stock.size()));
    }

    // svc_equip in capture: WriteLong must join capture (not staging alone).
    {
        std::vector<std::uint8_t> cap;
        const std::uint8_t op = 0x06;
        cap.push_back(op);
        const char name[] = "jackhammer";
        cap.insert(cap.end(), name, name + sizeof(name));
        const std::uint8_t price[4] = {0, 0, 0, 1};
        cap.insert(cap.end(), price, price + 4);
        CHECK(cap.size() == 1 + sizeof(name) + 4);
        CHECK(RelDef_LastCompleteEnd(cap.data(), static_cast<int>(cap.size())) == 0);
    }

    // SP_Print 0x24 via one SZ_Write: cutter keeps counted payload (not lockstep).
    {
        const std::uint8_t spPkt[] = {0x24, 0x00, 0x07, 3, 0x0B, 0x00, 0x0D};
        CHECK(RelDef_LastCompleteEnd(spPkt, 7) == 7);
        CHECK(RelDef_SealEnd(spPkt, 7) == 7);
        CHECK(!RelDef_IsLockstepOp(0x24));
    }

    // Seal drops orphan opcode after finished layout (no svc_bad header ship).
    {
        const std::uint8_t cap[] = {0x02, 'x', 0, 0x0B, 2};
        CHECK(RelDef_SealEnd(cap, 5) == 3);
        const std::uint8_t onlyHdr[] = {0x0B, 2};
        CHECK(RelDef_SealEnd(onlyHdr, 2) == 0);
    }

    // Boundaries the client actually parses. A 0 inside a header (nameprint
    // team, configstring index) must not look like the end of the message.
    {
        const std::uint8_t name[] = {0x0C, 1, 0, 'h', 'i', 0};
        CHECK(RelDef_LastCompleteEnd(name, 6) == 6);
        const std::uint8_t cfg[] = {0x0F, 0, 0, 'a', 0};
        CHECK(RelDef_LastCompleteEnd(cfg, 5) == 5);
        const std::uint8_t orphan[] = {0x0D, 'a', 'b', 0, 0x0D};
        CHECK(RelDef_LastCompleteEnd(orphan, 5) == 4);
        const std::uint8_t layout[] = {0x02, 'x', 0, 0x11, 'y', 0};
        CHECK(RelDef_LastCompleteEnd(layout, 6) == 6);
        const std::uint8_t layoutHi[] = {0x02, 'x', 0xC8, 0x82, 0, 0x0B};
        CHECK(RelDef_LastCompleteEnd(layoutHi, 6) == 5);
        const std::uint8_t ffStr[] = {0x0D, 'a', 0xFF, 'b', 0};
        CHECK(RelDef_LastCompleteEnd(ffStr, 5) == 3);
        // Altstring after a finished layout is not a command. Seal keeps the
        // layout and drops the tail; an opcode tail stays with it.
        const std::uint8_t after[] = {0x02, 'x', 0xC8, 0x82, 0, 0xC8};
        CHECK(RelDef_SealEnd(after, 6) == 5);
        CHECK(RelDef_SealEnd(layoutHi, 6) == 5);
        const std::uint8_t frag[] = {0xC8, 0x82};
        CHECK(RelDef_SealEnd(frag, 2) == 0);
        CHECK(!RelDef_StringContinues(after, 5));
        const std::uint8_t hdr[] = {0x0B, 2};
        CHECK(RelDef_StringContinues(hdr, 2));
        // SP_Print ships these via SZ_GetSpace; the payload is counted
        // bytes, not a C string. A NUL or 0x0B inside must not end it.
        const std::uint8_t sp[] = {0x22, 0x2b, 0x00};
        CHECK(RelDef_LastCompleteEnd(sp, 3) == 3);
        const std::uint8_t d1[] = {0x24, 0x00, 0x07, 4, 0x0B, 0x00, 0x0D, 0x00};
        CHECK(RelDef_LastCompleteEnd(d1, 8) == 8);
        const std::uint8_t d1ff[] = {0x24, 0x00, 0x07, 4, 0xFF, 0x0D, 0x00, 0x0B};
        CHECK(RelDef_LastCompleteEnd(d1ff, 8) == 8);
        const std::uint8_t d1tail[] = {0x24, 0x00, 0x07, 4, 0x0B, 0x00, 0x0D, 0x00, 0x0B};
        CHECK(RelDef_LastCompleteEnd(d1tail, 9) == 8);
        const std::uint8_t d2[] = {0x25, 0x00, 0x07, 0x02, 0x00, 0x0B, 0x00};
        CHECK(RelDef_LastCompleteEnd(d2, 7) == 7);
        const std::uint8_t d1short[] = {0x24, 0x00, 0x07, 4, 0x0B, 0x00};
        CHECK(RelDef_LastCompleteEnd(d1short, 6) == 0);
        // ric: count + records. A non-opcode after a finished ric is the
        // "Last command was svc_ric" drop; a short arg must stay inside.
        const std::uint8_t ric[] = {0x1C, 1, 0x04, 1, 0xC8};
        CHECK(RelDef_LastCompleteEnd(ric, 5) == 4);
        CHECK(RelDef_SealEnd(ric, 5) == 4);
        const std::uint8_t hide[] = {0x1C, 1, 0x05};
        CHECK(RelDef_LastCompleteEnd(hide, 3) == 3);
        const std::uint8_t ricShort[] = {0x1C, 1, 0x04};
        CHECK(RelDef_LastCompleteEnd(ricShort, 3) == 0);
        const std::uint8_t ric16[] = {0x1C, 1, 0x14, 0x0B, 0x00};
        CHECK(RelDef_LastCompleteEnd(ric16, 5) == 5);
        // ghoul reliable: short count, then that many bytes. A non-opcode
        // after it must not ride along (string-table underflow if the
        // count and the body disagree).
        const std::uint8_t ghl[] = {0x1A, 0x04, 0x00, 0x0B, 0x00, 0x0D, 0xFF, 0xC8};
        CHECK(RelDef_LastCompleteEnd(ghl, 8) == 7);
        CHECK(RelDef_SealEnd(ghl, 8) == 7);
        const std::uint8_t ghlShort[] = {0x1A, 0x04, 0x00, 0x0B};
        CHECK(RelDef_LastCompleteEnd(ghlShort, 4) == 0);
        const std::uint8_t dc[] = {0x08, 0x34, 0x12};
        CHECK(RelDef_LastCompleteEnd(dc, 3) == 3);
        const std::uint8_t cd[] = {0x1F, 0x0A, 0x00, 0x00, 0x00};
        CHECK(RelDef_LastCompleteEnd(cd, 5) == 5);
        const std::uint8_t rb[] = {0x1E, 2};
        CHECK(RelDef_LastCompleteEnd(rb, 2) == 2);
        const std::uint8_t pnc[] = {0x21, 2, 5, 3, 0x80, 1, 4};
        CHECK(RelDef_LastCompleteEnd(pnc, 7) == 7);
        const std::uint8_t equipSub[] = {0x06, 0};
        CHECK(RelDef_LastCompleteEnd(equipSub, 2) == 2);
        const std::uint8_t dlMiss[] = {0x13, 0xFF, 0xFF, 0};
        CHECK(RelDef_LastCompleteEnd(dlMiss, 4) == 4);
        const std::uint8_t dlChunk[] = {0x13, 0x03, 0x00, 0x32, 'a', 'b', 'c'};
        CHECK(RelDef_LastCompleteEnd(dlChunk, 7) == 7);
        const std::uint8_t ghu[] = {0x1B, 0x02, 0x00, 0x0B, 0x00};
        CHECK(RelDef_LastCompleteEnd(ghu, 5) == 5);
        const std::uint8_t cul[] = {0x18, 1, 2, 3, 4};
        CHECK(RelDef_LastCompleteEnd(cul, 5) == 5);
        const std::uint8_t snd[] = {0x0A, 0, 1};
        CHECK(RelDef_LastCompleteEnd(snd, 3) == 3);
        const std::uint8_t sinfo[] = {0x04, 0x03, 10, 20};
        CHECK(RelDef_LastCompleteEnd(sinfo, 4) == 4);
    }

    // Overflow split matches production CaptureAppend: peel only a complete
    // prefix, never seal a trailing opcode. Stufftext (0x0D) used to miss
    // the print-only scan, so the opcode shipped in one blob and the string
    // in the next — client "Illegible server message".
    {
        const int capMax = 250;
        std::vector<std::uint8_t> cap;
        std::vector<std::vector<std::uint8_t>> blobs;
        std::vector<std::uint8_t> stock;
        auto take = [&](int end) {
            if (end <= 0)
                return;
            blobs.emplace_back(cap.begin(), cap.begin() + end);
            cap.erase(cap.begin(), cap.begin() + end);
        };
        auto append = [&](const void* data, int len) {
            if (!data || len <= 0 || len > capMax)
                return;
            if (len > capMax - static_cast<int>(cap.size())) {
                const int end = RelDef_LastCompleteEnd(
                    cap.data(), static_cast<int>(cap.size()));
                take(end);
            }
            if (len > capMax - static_cast<int>(cap.size()))
                return;
            const auto* p = static_cast<const std::uint8_t*>(data);
            cap.insert(cap.end(), p, p + len);
            if (static_cast<int>(cap.size()) >= capMax)
                take(RelDef_LastCompleteEnd(cap.data(), static_cast<int>(cap.size())));
        };
        for (int i = 0; i < 4; ++i) {
            const std::uint8_t op = 0x0D;
            std::vector<std::uint8_t> str(118, static_cast<std::uint8_t>('a' + i));
            str.push_back(0);
            append(&op, 1);  // fits in the slack; string does not
            append(str.data(), static_cast<int>(str.size()));
            stock.push_back(op);
            stock.insert(stock.end(), str.begin(), str.end());
        }
        take(static_cast<int>(cap.size()));
        std::vector<std::uint8_t> joined;
        CHECK(!blobs.empty());
        for (const auto& b : blobs) {
            CHECK(RelDef_LastCompleteEnd(b.data(), static_cast<int>(b.size())) ==
                  static_cast<int>(b.size()));
            CHECK(b[0] == 0x0D);
            joined.insert(joined.end(), b.begin(), b.end());
        }
        CHECK(joined == stock);
    }

    // SV_Map stuffs "changing" (and later "reconnect") as one SZ_Write while
    // the client is still spawned, then flushes before SpawnServer. Vanilla
    // puts that in the mailbox immediately. A fat parcel ahead of it is held
    // until max_drip_wait, so the client stays on the old map through the load.
    {
        RelDefPolicy live;
        const RelDefLimits lim = RelDef_ComputeLimits(1400, live);
        const int changing = 12;  // 0x0D + "changing\n\0"
        CHECK(RelDef_Classify(0, changing, 0, 0, 0, lim, live) == RELDEF_WRITE_NOW);
        CHECK(RelDef_Classify(0, changing, 1, 0, 0, lim, live) == RELDEF_QUEUE);
        CHECK(RelDef_Classify(0, changing, 0, 1, 800, lim, live) == RELDEF_QUEUE);
        const int fat = 800;
        int ticks = 0;
        for (; ticks < 30; ++ticks) {
            if (RelDef_CanDrip(0, 0, fat, lim, live.frameFirst, ticks,
                               live.maxDripWaitTicks))
                break;
        }
        CHECK(ticks == live.maxDripWaitTicks);
        CHECK(RelDef_CanDrip(0, fat, changing, lim, live.frameFirst, ticks,
                             live.maxDripWaitTicks));
        const std::uint8_t chg[] = {0x0D, 'c','h','a','n','g','i','n','g','\n', 0};
        const std::uint8_t rec[] = {0x0D, 'r','e','c','o','n','n','e','c','t','\n', 0};
        CHECK(RelDef_IsLevelChange(chg, 11));
        CHECK(RelDef_IsLevelChange(rec, 12));
        CHECK(!RelDef_IsLevelChange(chg, 10));
        const std::uint8_t lay[] = {0x02, 'x', 0};
        CHECK(!RelDef_IsLevelChange(lay, 3));
        CHECK(RelDef_HasLevelChange(chg, 11));
        CHECK(RelDef_HasLevelChange(rec, 12));
        std::uint8_t prefixed[12] = {0x07};
        std::memcpy(prefixed + 1, chg, 11);
        CHECK(RelDef_HasLevelChange(prefixed, 12));
        CHECK(!RelDef_HasLevelChange(lay, 3));
        CHECK(RelDef_SuppressSnapshot(kCsSpawned, true));
        CHECK(!RelDef_SuppressSnapshot(kCsSpawned, false));
        CHECK(!RelDef_SuppressSnapshot(kCsConnected, true));
    }

    // FIFO queue: mirrors SlotQueue deque+pop_front (production helpers need
    // engine RVAs; semantics match QueuePush/QueuePopWrite eviction/drip).
    {
        struct Q {
            std::deque<std::vector<std::uint8_t>> blobs;
            std::deque<int> enqueued;
            int bytes = 0;
            void push(int id) {
                blobs.push_back({static_cast<std::uint8_t>(id)});
                enqueued.push_back(id);
                bytes += 1;
            }
            int pop() {
                const int id = blobs.front()[0];
                bytes -= static_cast<int>(blobs.front().size());
                blobs.pop_front();
                enqueued.pop_front();
                return id;
            }
        } q;
        for (int i = 0; i < 40; ++i)
            q.push(i);
        CHECK(q.bytes == 40);
        for (int i = 0; i < 40; ++i)
            CHECK(q.pop() == i);
        CHECK(q.bytes == 0);
        CHECK(q.blobs.empty());
    }

    return g_fails ? 1 : 0;
}
