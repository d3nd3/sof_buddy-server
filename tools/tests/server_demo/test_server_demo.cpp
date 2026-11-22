// Host-side tests for src/features/server_demo/server_demo_format.h.
//
// The container is the contract between the server recorder, the future
// replay server, and a web renderer. These tests pin the header, the chunk
// framing and the two fixed game-structure mirrors.

#include "server_demo_format.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static int fails = 0;

#define CHECK(cond, msg)                    \
    do {                                    \
        if (!(cond)) {                      \
            std::printf("FAIL: %s\n", msg); \
            ++fails;                        \
        }                                   \
    } while (0)

static bool Near(float a, float b) {
    return std::fabs(a - b) < 0.0001f;
}

static svdemo::FileInfo MakeInfo() {
    svdemo::FileInfo info;
    info.flags = svdemo::kChunkFlagHasEntity;
    info.max_clients = 12;
    info.tick_ms = 100;
    info.created_unix = 123456789u;
    info.map_name = "dm/arena";
    info.level_name = "The Arena";
    info.gamedir = "base";
    info.server_name = "test server";
    return info;
}

static void test_header_roundtrip() {
    std::vector<std::uint8_t> file;
    svdemo::WriteFileHeader(file, MakeInfo());
    CHECK(file.size() > 8, "header: bytes written");
    CHECK(std::memcmp(file.data(), svdemo::kMagic, sizeof(svdemo::kMagic)) == 0,
          "header: magic");

    svdemo::FileInfo out;
    std::vector<svdemo::OwnedChunk> chunks;
    CHECK(svdemo::ReadWholeFile(file, out, chunks), "header: parses");
    CHECK(out.flags == svdemo::kChunkFlagHasEntity, "header: flags");
    CHECK(out.max_clients == 12, "header: max clients");
    CHECK(out.tick_ms == 100, "header: tick");
    CHECK(out.created_unix == 123456789u, "header: created");
    CHECK(out.map_name == "dm/arena", "header: map");
    CHECK(out.level_name == "The Arena", "header: level");
    CHECK(out.gamedir == "base", "header: gamedir");
    CHECK(out.server_name == "test server", "header: server");
    CHECK(chunks.empty(), "header: no chunks yet");
}

static svdemo::ClientFrame MakeFrame() {
    svdemo::ClientFrame f;
    f.frame = 900;
    f.time_ms = 90000;
    f.slot = 3;
    f.flags = svdemo::kClientFlagHasEntity | svdemo::kClientFlagSpectator;
    f.ps.pmove.pm_type = 2;
    f.ps.pmove.origin[0] = 100;
    f.ps.pmove.origin[1] = -200;
    f.ps.pmove.origin[2] = 300;
    f.ps.pmove.velocity[0] = 4;
    f.ps.pmove.delta_angles[2] = 7;
    f.ps.pmove.moveScale = 9;
    f.ps.viewangles[0] = 12.5f;
    f.ps.viewangles[1] = 90.0f;
    f.ps.viewangles[2] = -3.25f;
    f.ps.viewoffset[2] = 22.0f;
    f.ps.gunUUID = 44;
    f.ps.gunType = 5;
    f.ps.gunClip = 8;
    f.ps.gunAmmo = 30;
    f.ps.gunReload = 1;
    f.ps.restart_count = 2;
    f.ps.buttons_inhibit = 0x10;
    f.ps.bodUUID = 55;
    f.ps.blend[0] = 0.1f;
    f.ps.blend[3] = 0.9f;
    f.ps.fov = 91.0f;
    f.ps.rdflags = 0x40;
    f.ps.soundID = 3;
    f.ps.musicID = 2;
    f.ps.damageLoc = 6;
    f.ps.damageDir = 7;
    for (int i = 0; i < 16; ++i)
        f.ps.stats[i] = static_cast<std::int16_t>(i * 100);
    f.ps.dmRank = 4;
    f.ps.dmRankedPlyrs = 8;
    f.ps.spectatorId = 2;
    f.ps.cinematicfreeze = 1;
    f.es.number = 3;
    f.es.origin[0] = 1.5f;
    f.es.origin[1] = 2.5f;
    f.es.origin[2] = 3.5f;
    f.es.angles[1] = 45.0f;
    f.es.modelindex = 77;
    f.es.frame = 12;
    f.es.effects = 0x8000;
    f.es.effectData[2].effectId = 9;
    f.es.effectData[2].bolt = 10;
    f.es.effectData[2].size = 11;
    return f;
}

static void test_client_frame_roundtrip() {
    std::vector<std::uint8_t> file;
    svdemo::WriteFileHeader(file, MakeInfo());
    const svdemo::ClientFrame in = MakeFrame();
    svdemo::WriteClientFrame(file, in);

    svdemo::FileInfo info;
    std::vector<svdemo::OwnedChunk> chunks;
    CHECK(svdemo::ReadWholeFile(file, info, chunks), "frame: parses");
    CHECK(chunks.size() == 1, "frame: one chunk");
    CHECK(chunks[0].type == svdemo::kChunkClientFrame, "frame: type");
    CHECK(chunks[0].flags == svdemo::kChunkFlagHasEntity, "frame: entity chunk flag");
    CHECK(chunks[0].payload.size() == 12 + svdemo::kPlayerStateSize + svdemo::kEntityStateSize,
          "frame: payload size");

    svdemo::ClientFrame out;
    CHECK(svdemo::ParseClientFrame(chunks[0], out), "frame: parse payload");
    CHECK(out.frame == in.frame, "frame: number");
    CHECK(out.time_ms == in.time_ms, "frame: time");
    CHECK(out.slot == in.slot, "frame: slot");
    CHECK(out.flags == in.flags, "frame: flags");
    CHECK(out.ps.pmove.pm_type == 2, "frame: pm_type");
    CHECK(out.ps.pmove.origin[0] == 100 && out.ps.pmove.origin[1] == -200 &&
              out.ps.pmove.origin[2] == 300,
          "frame: origin");
    CHECK(out.ps.pmove.delta_angles[2] == 7, "frame: delta angles");
    CHECK(out.ps.pmove.moveScale == 9, "frame: move scale");
    CHECK(Near(out.ps.viewangles[0], 12.5f) && Near(out.ps.viewangles[1], 90.0f) &&
              Near(out.ps.viewangles[2], -3.25f),
          "frame: view angles");
    CHECK(Near(out.ps.viewoffset[2], 22.0f), "frame: view offset");
    CHECK(out.ps.gunUUID == 44 && out.ps.gunType == 5 && out.ps.gunClip == 8 &&
              out.ps.gunAmmo == 30,
          "frame: gun");
    CHECK(out.ps.gunReload == 1 && out.ps.restart_count == 2 &&
              out.ps.buttons_inhibit == 0x10,
          "frame: reload/restart/inhibit");
    CHECK(out.ps.bodUUID == 55, "frame: body");
    CHECK(Near(out.ps.blend[0], 0.1f) && Near(out.ps.blend[3], 0.9f), "frame: blend");
    CHECK(Near(out.ps.fov, 91.0f), "frame: fov");
    CHECK(out.ps.rdflags == 0x40, "frame: rdflags");
    CHECK(out.ps.soundID == 3 && out.ps.musicID == 2, "frame: sound");
    CHECK(out.ps.damageLoc == 6 && out.ps.damageDir == 7, "frame: damage");
    CHECK(out.ps.stats[0] == 0 && out.ps.stats[9] == 900 && out.ps.stats[15] == 1500,
          "frame: stats");
    CHECK(out.ps.dmRank == 4 && out.ps.dmRankedPlyrs == 8 && out.ps.spectatorId == 2 &&
              out.ps.cinematicfreeze == 1,
          "frame: dm fields");
    CHECK(out.ps.gun == 0 && out.ps.bod == 0, "frame: pointers zeroed");
    CHECK(out.es.number == 3, "frame: entity number");
    CHECK(Near(out.es.origin[0], 1.5f) && Near(out.es.origin[2], 3.5f), "frame: entity origin");
    CHECK(Near(out.es.angles[1], 45.0f), "frame: entity angles");
    CHECK(out.es.modelindex == 77 && out.es.frame == 12, "frame: entity model/frame");
    CHECK(out.es.effects == 0x8000, "frame: entity effects");
    CHECK(out.es.effectData[2].effectId == 9 && out.es.effectData[2].bolt == 10 &&
              out.es.effectData[2].size == 11,
          "frame: entity effect data");
}

static void test_client_frame_without_entity() {
    std::vector<std::uint8_t> file;
    svdemo::WriteFileHeader(file, MakeInfo());
    svdemo::ClientFrame in = MakeFrame();
    in.flags = svdemo::kClientFlagNone;
    svdemo::WriteClientFrame(file, in);

    svdemo::FileInfo info;
    std::vector<svdemo::OwnedChunk> chunks;
    CHECK(svdemo::ReadWholeFile(file, info, chunks), "no-entity: parses");
    CHECK(chunks.size() == 1, "no-entity: one chunk");
    CHECK(chunks[0].flags == svdemo::kChunkFlagNone, "no-entity: chunk flag");
    CHECK(chunks[0].payload.size() == 12 + svdemo::kPlayerStateSize,
          "no-entity: payload size");
    svdemo::ClientFrame out;
    CHECK(svdemo::ParseClientFrame(chunks[0], out), "no-entity: parse");
    CHECK(out.flags == svdemo::kClientFlagNone, "no-entity: flags");
}

static void test_roster_roundtrip() {
    std::vector<std::uint8_t> file;
    svdemo::WriteFileHeader(file, MakeInfo());
    svdemo::RosterEntry in;
    in.frame = 0;
    in.time_ms = 0;
    in.slot = 1;
    in.flags = svdemo::kClientFlagNone;
    in.name = "Grim";
    in.userinfo = "\\name\\Grim\\model\\male\\skin\\base";
    svdemo::WriteRoster(file, in);

    svdemo::FileInfo info;
    std::vector<svdemo::OwnedChunk> chunks;
    CHECK(svdemo::ReadWholeFile(file, info, chunks), "roster: parses");
    CHECK(chunks.size() == 1 && chunks[0].type == svdemo::kChunkRoster, "roster: type");
    svdemo::RosterEntry out;
    CHECK(svdemo::ParseRoster(chunks[0], out), "roster: parse");
    CHECK(out.slot == 1 && out.name == "Grim", "roster: name");
    CHECK(out.userinfo == in.userinfo, "roster: userinfo");
}

static void test_configstring_roundtrip() {
    std::vector<std::uint8_t> file;
    svdemo::WriteFileHeader(file, MakeInfo());
    svdemo::ConfigString in;
    in.index = 35;
    in.value = "models/player/grim";
    svdemo::WriteConfigString(file, in);

    svdemo::FileInfo info;
    std::vector<svdemo::OwnedChunk> chunks;
    CHECK(svdemo::ReadWholeFile(file, info, chunks), "configstring: parses");
    CHECK(chunks.size() == 1 && chunks[0].type == svdemo::kChunkConfigString,
          "configstring: type");
    svdemo::ConfigString out;
    CHECK(svdemo::ParseConfigString(chunks[0], out), "configstring: parse");
    CHECK(out.index == 35 && out.value == in.value, "configstring: value");
}

static void test_event_roundtrip() {
    std::vector<std::uint8_t> file;
    svdemo::WriteFileHeader(file, MakeInfo());
    svdemo::Event in;
    in.frame = 12;
    in.time_ms = 1200;
    in.kind = svdemo::kEventMark;
    in.text = "nice shot";
    svdemo::WriteEvent(file, in);

    svdemo::FileInfo info;
    std::vector<svdemo::OwnedChunk> chunks;
    CHECK(svdemo::ReadWholeFile(file, info, chunks), "event: parses");
    svdemo::Event out;
    CHECK(chunks.size() == 1 && svdemo::ParseEvent(chunks[0], out), "event: parse");
    CHECK(out.frame == 12 && out.time_ms == 1200 && out.kind == svdemo::kEventMark,
          "event: fields");
    CHECK(out.text == "nice shot", "event: text");
}

static void test_end_roundtrip() {
    std::vector<std::uint8_t> file;
    svdemo::WriteFileHeader(file, MakeInfo());
    svdemo::EndInfo in;
    in.first_frame = 10;
    in.last_frame = 99;
    in.client_frames = 320;
    in.max_clients = 6;
    in.duration_ms = 8900;
    in.reason = svdemo::kEndMapChange;
    svdemo::WriteEnd(file, in);

    svdemo::FileInfo info;
    std::vector<svdemo::OwnedChunk> chunks;
    CHECK(svdemo::ReadWholeFile(file, info, chunks), "end: parses");
    svdemo::EndInfo out;
    CHECK(chunks.size() == 1 && svdemo::ParseEnd(chunks[0], out), "end: parse");
    CHECK(out.first_frame == 10 && out.last_frame == 99, "end: frames");
    CHECK(out.client_frames == 320 && out.max_clients == 6, "end: counts");
    CHECK(out.duration_ms == 8900 && out.reason == svdemo::kEndMapChange, "end: reason");
}

static void test_rejects_bad_magic_and_truncation() {
    std::vector<std::uint8_t> file;
    svdemo::WriteFileHeader(file, MakeInfo());
    svdemo::WriteClientFrame(file, MakeFrame());

    std::vector<std::uint8_t> bad = file;
    bad[0] = 'X';
    svdemo::FileInfo info;
    std::vector<svdemo::OwnedChunk> chunks;
    CHECK(!svdemo::ReadWholeFile(bad, info, chunks), "bad magic rejected");

    std::vector<std::uint8_t> truncated = file;
    truncated.resize(truncated.size() - 7);
    CHECK(!svdemo::ReadWholeFile(truncated, info, chunks), "truncated chunk rejected");

    std::vector<std::uint8_t> bad_version = file;
    bad_version[8] = 99;
    bad_version[9] = 0;
    CHECK(!svdemo::ReadWholeFile(bad_version, info, chunks), "bad version rejected");
}

static void test_structure_sizes() {
    CHECK(sizeof(svdemo::PmoveState) == 28, "pmove size");
    CHECK(sizeof(svdemo::PlayerState) == 200, "player state size");
    CHECK(offsetof(svdemo::PlayerState, viewangles) == 28, "viewangles offset");
    CHECK(offsetof(svdemo::PlayerState, blend) == 132, "blend offset");
    CHECK(offsetof(svdemo::PlayerState, fov) == 148, "fov offset");
    CHECK(offsetof(svdemo::PlayerState, stats) == 164, "stats offset");
    CHECK(offsetof(svdemo::PlayerState, cinematicfreeze) == 199, "cinematicfreeze offset");
    CHECK(sizeof(svdemo::EntityState) == 116, "entity state size");
    CHECK(offsetof(svdemo::EntityState, effectData) == 92, "effectData offset");
}

int main() {
    test_structure_sizes();
    test_header_roundtrip();
    test_client_frame_roundtrip();
    test_client_frame_without_entity();
    test_roster_roundtrip();
    test_configstring_roundtrip();
    test_event_roundtrip();
    test_end_roundtrip();
    test_rejects_bad_magic_and_truncation();
    if (fails == 0)
        std::printf("server_demo: all tests passed\n");
    return fails == 0 ? 0 : 1;
}
