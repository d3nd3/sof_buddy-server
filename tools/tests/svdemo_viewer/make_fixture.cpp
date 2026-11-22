// Writes a small .sbd fixture with the real C++ writer, so the browser parser
// is tested against the same bytes the server produces.
#include "server_demo_format.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static svdemo::ClientFrame MakeFrame(int slot, std::uint32_t frame,
                                     std::uint32_t time_ms, float x, float y,
                                     std::int16_t health, std::int16_t armor,
                                     std::int16_t frags, std::int32_t model) {
    svdemo::ClientFrame f;
    f.frame = frame;
    f.time_ms = time_ms;
    f.slot = static_cast<std::uint8_t>(slot);
    f.flags = svdemo::kClientFlagHasEntity;
    f.ps.pmove.pm_type = 0;
    f.ps.pmove.origin[0] = static_cast<std::int16_t>(x * 8);
    f.ps.pmove.origin[1] = static_cast<std::int16_t>(y * 8);
    f.ps.pmove.origin[2] = 0;
    f.ps.pmove.velocity[0] = 80;
    f.ps.pmove.gravity = 800;
    f.ps.viewangles[0] = 0.0f;
    f.ps.viewangles[1] = slot == 1 ? 90.0f : 270.0f;
    f.ps.viewangles[2] = 0.0f;
    f.ps.viewoffset[2] = 22.0f;
    f.ps.fov = 90.0f;
    f.ps.gunClip = 30;
    f.ps.gunAmmo = 90;
    f.ps.stats[svdemo::kStatHealth] = health;
    f.ps.stats[svdemo::kStatArmor] = armor;
    f.ps.stats[svdemo::kStatFrags] = frags;

    f.es.number = slot;
    f.es.origin[0] = x;
    f.es.origin[1] = y;
    f.es.origin[2] = 0.0f;
    f.es.angles[1] = f.ps.viewangles[1];
    f.es.modelindex = model;
    f.es.frame = static_cast<std::int32_t>(frame);
    return f;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <out.sbd>\n", argv[0]);
        return 1;
    }

    svdemo::FileInfo info;
    info.flags = svdemo::kChunkFlagHasEntity;
    info.max_clients = 2;
    info.tick_ms = 100;
    info.created_unix = 1700000000u;
    info.map_name = "dm/arena";
    info.level_name = "The Arena";
    info.gamedir = "base";
    info.server_name = "fixture";

    std::vector<std::uint8_t> file;
    svdemo::WriteFileHeader(file, info);

    svdemo::RosterEntry roster;
    roster.slot = 1;
    roster.name = "Grim";
    roster.userinfo = "\\name\\Grim\\model\\male\\skin\\base";
    svdemo::WriteRoster(file, roster);
    roster.slot = 2;
    roster.name = "Vex";
    roster.userinfo = "\\name\\Vex\\model\\female\\skin\\red";
    svdemo::WriteRoster(file, roster);

    for (std::uint32_t frame = 0; frame < 3; ++frame) {
        const std::uint32_t time_ms = frame * 100;
        const float x = 100.0f + static_cast<float>(frame) * 8.0f;
        const float y = -200.0f + static_cast<float>(frame) * 4.0f;
        svdemo::WriteClientFrame(
            file, MakeFrame(1, frame, time_ms, x, y, 100, 50, 7, 1));
        svdemo::WriteClientFrame(
            file, MakeFrame(2, frame, time_ms, -x, -y, 75, 25, 3, 2));
    }

    svdemo::ConfigString cs;
    cs.index = 2;  // CS_SKY
    cs.value = "sky1";
    svdemo::WriteConfigString(file, cs);
    cs.index = 35;  // CS_MODELS
    cs.value = "models/player/grim";
    svdemo::WriteConfigString(file, cs);
    cs.index = 1415;  // CS_PLAYERSKINS
    cs.value = "Grim/male/base";
    svdemo::WriteConfigString(file, cs);

    svdemo::Event event;
    event.frame = 1;
    event.time_ms = 100;
    event.kind = svdemo::kEventMark;
    event.text = "fixture mark";
    svdemo::WriteEvent(file, event);

    svdemo::EndInfo end;
    end.first_frame = 0;
    end.last_frame = 2;
    end.client_frames = 6;
    end.max_clients = 2;
    end.duration_ms = 200;
    end.reason = svdemo::kEndStopped;
    svdemo::WriteEnd(file, end);

    std::FILE* out = std::fopen(argv[1], "wb");
    if (!out) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    const bool ok = std::fwrite(file.data(), 1, file.size(), out) == file.size();
    std::fclose(out);
    return ok ? 0 : 1;
}
