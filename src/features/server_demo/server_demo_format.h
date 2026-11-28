#pragma once

// On-disk container for server-side state demos (`.sbd`).
//
// The recorder stores what the *server* knows after each client's
// ClientEndServerFrame: the finished player_state_t, the player edict's
// entity_state_t, plus enough metadata to make the stream self-describing.
// It deliberately does not store network packets, so a web renderer can read
// it without a SoF protocol implementation.
//
// Layout is little-endian and explicitly serialized; no compiler struct is
// memcpy'd to disk except through the fixed-layout mirrors below, whose
// offsets are checked at compile time.
//
// File:
//   FileHeader (fixed fields + length-prefixed strings)
//   Chunk* (type, flags, u16 payload size, payload)
//
// Chunk payloads:
//   CLIENT_FRAME: frame, time_ms, slot, flags, PlayerState, [EntityState]
//   ROSTER:       frame, time_ms, slot, flags, name, userinfo
//   EVENT:        frame, time_ms, kind, text
//   END:          first_frame, last_frame, client_frames, max_clients,
//                 duration_ms, reason
//   CONFIGSTRING: u16 index, str16 value
//
// The format is versioned. Bump kVersion when a field changes meaning or a
// payload grows; readers must reject an unknown version rather than guess.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace svdemo {

constexpr char kMagic[8] = {'S', 'O', 'F', 'D', 'E', 'M', 'O', '\0'};
constexpr std::uint16_t kVersion = 1;
constexpr std::uint32_t kPlayerStateSize = 200;
constexpr std::uint32_t kEntityStateSize = 116;

enum ChunkType : std::uint8_t {
    kChunkClientFrame = 1,
    kChunkRoster = 2,
    kChunkEvent = 3,
    kChunkEnd = 4,
    // One configstring per chunk: u16 index, str16 value. Splitting entries
    // avoids the 64 KiB chunk cap when a map precaches a lot of assets.
    kChunkConfigString = 5,
    // Reserved for the in-game playback side channel. A v1 reader skips
    // unknown chunks, so adding this later does not break the state stream.
    kChunkPacketFrame = 6,
};

enum ChunkFlags : std::uint8_t {
    kChunkFlagNone = 0,
    kChunkFlagHasEntity = 1u << 0,
};

enum ClientFlags : std::uint8_t {
    kClientFlagNone = 0,
    kClientFlagHasEntity = 1u << 0,
    kClientFlagSpectator = 1u << 1,
};

enum EventKind : std::uint8_t {
    kEventMark = 0,
    kEventChat = 1,
    kEventJoin = 2,
    kEventLeave = 3,
};

enum EndReason : std::uint8_t {
    kEndStopped = 0,
    kEndMapChange = 1,
    kEndDisabled = 2,
    kEndSizeCap = 3,
    kEndShutdown = 4,
    kEndError = 5,
    kEndIdle = 6,
};

// ---------------------------------------------------------------------------
// Fixed-layout mirrors of the game structures we record.
//
// The retail SoF game DLL is 32-bit x86. Pointers in player_state_t are
// process-local and meaningless in a file, so they are kept as 32-bit slots
// and zeroed by the recorder. Everything else is copied verbatim.
// ---------------------------------------------------------------------------

#pragma pack(push, 1)

struct PmoveState {
    std::int32_t pm_type;
    std::int16_t origin[3];
    std::int16_t velocity[3];
    std::uint8_t pm_flags;
    std::uint8_t pm_time;
    std::int16_t gravity;
    std::int16_t delta_angles[3];
    std::uint8_t moveScale;
    std::uint8_t pad[1];  // natural alignment to 28
};

// player_state_t.stats[] indexes (SoF q_sh_interface.h).
enum StatIndex : int {
    kStatInvType = 0,
    kStatHealth = 1,
    kStatClipAmmo = 2,
    kStatAmmo = 3,
    kStatClipMax = 4,
    kStatArmor = 5,
    kStatWeapon = 6,
    kStatInvCount = 7,
    kStatFlags = 8,
    kStatLayouts = 9,
    kStatFrags = 10,
    kStatStealth = 11,
    kStatForceHud = 12,
    kStatDamageLoc = 14,
    kStatDamageDir = 15,
};

struct PlayerState {
    PmoveState pmove;                 // 0
    float viewangles[3];              // 28
    float viewoffset[3];              // 40
    float kick_angles[3];             // 52
    float weaponkick_angles[3];       // 64
    float remote_vieworigin[3];       // 76
    float remote_viewangles[3];       // 88
    std::int32_t remote_id;           // 100
    std::uint8_t remote_type;         // 104
    std::uint8_t pad0[3];             // 105
    std::uint32_t gun;                // 108: pointer in game, zeroed on disk
    std::int16_t gunUUID;             // 112
    std::int16_t gunType;             // 114
    std::int16_t gunClip;             // 116
    std::int16_t gunAmmo;             // 118
    std::uint8_t gunReload;           // 120
    std::uint8_t restart_count;       // 121
    std::uint8_t buttons_inhibit;     // 122
    std::uint8_t pad1;                // 123
    std::uint32_t bod;                // 124: pointer in game, zeroed on disk
    std::int16_t bodUUID;             // 128
    std::uint8_t pad2[2];             // 130
    float blend[4];                   // 132
    float fov;                        // 148
    std::int32_t rdflags;             // 152
    std::int16_t soundID;             // 156
    std::uint8_t musicID;             // 158
    std::uint8_t pad3;                // 159
    std::int16_t damageLoc;           // 160
    std::int16_t damageDir;           // 162
    std::int16_t stats[16];           // 164
    std::uint8_t dmRank;              // 196
    std::uint8_t dmRankedPlyrs;       // 197
    std::uint8_t spectatorId;         // 198
    std::uint8_t cinematicfreeze;     // 199
};

struct EffectInfo {
    std::int16_t effectId;
    std::int16_t bolt;
    std::int16_t size;
};

struct EntityState {
    std::int32_t number;              // 0
    float origin[3];                  // 4
    float angles[3];                  // 16
    float angle_diff;                 // 28
    std::int32_t modelindex;          // 32
    std::int32_t renderindex;         // 36
    std::int32_t frame;               // 40
    std::int32_t skinnum;             // 44
    std::int32_t effects;             // 48
    std::int32_t renderfx;            // 52
    std::int32_t solid;               // 56
    std::int32_t sound;               // 60
    std::int32_t sound_data;          // 64
    std::int32_t event;               // 68
    std::int32_t event2;              // 72
    std::int32_t event3;              // 76
    std::int32_t data;                // 80
    std::int32_t data2;               // 84
    std::int32_t data3;               // 88
    EffectInfo effectData[4];         // 92..115
};

#pragma pack(pop)

static_assert(sizeof(PmoveState) == 28, "SoF pmove_state_t is 28 bytes");
static_assert(sizeof(PlayerState) == kPlayerStateSize,
              "SoF player_state_t is 200 bytes");
static_assert(offsetof(PlayerState, viewangles) == 28, "player_state_t viewangles");
static_assert(offsetof(PlayerState, blend) == 132, "player_state_t blend");
static_assert(offsetof(PlayerState, fov) == 148, "player_state_t fov");
static_assert(offsetof(PlayerState, stats) == 164, "player_state_t stats");
static_assert(offsetof(PlayerState, dmRank) == 196, "player_state_t dmRank");
static_assert(offsetof(PlayerState, cinematicfreeze) == 199,
              "player_state_t cinematicfreeze");
static_assert(sizeof(EntityState) == kEntityStateSize,
              "SoF entity_state_t is 116 bytes");
static_assert(offsetof(EntityState, effectData) == 92, "entity_state_t effectData");

// ---------------------------------------------------------------------------
// Serialization primitives.
// ---------------------------------------------------------------------------

inline void PutU8(std::vector<std::uint8_t>& out, std::uint8_t v) {
    out.push_back(v);
}

inline void PutU16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xffu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xffu));
}

inline void PutU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xffu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xffu));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xffu));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xffu));
}

inline void PutI16(std::vector<std::uint8_t>& out, std::int16_t v) {
    PutU16(out, static_cast<std::uint16_t>(v));
}

inline void PutF32(std::vector<std::uint8_t>& out, float v) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    PutU32(out, bits);
}

inline void PutBytes(std::vector<std::uint8_t>& out, const void* data, std::size_t n) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    out.insert(out.end(), p, p + n);
}

inline void PutString16(std::vector<std::uint8_t>& out, const std::string& s) {
    const std::size_t n = s.size() > 0xffffu ? 0xffffu : s.size();
    PutU16(out, static_cast<std::uint16_t>(n));
    if (n)
        PutBytes(out, s.data(), n);
}

inline void SetU16At(std::vector<std::uint8_t>& out, std::size_t off, std::uint16_t v) {
    if (off + 2 > out.size())
        return;
    out[off + 0] = static_cast<std::uint8_t>(v & 0xffu);
    out[off + 1] = static_cast<std::uint8_t>((v >> 8) & 0xffu);
}

inline std::size_t BeginChunk(std::vector<std::uint8_t>& out, std::uint8_t type,
                              std::uint8_t flags) {
    PutU8(out, type);
    PutU8(out, flags);
    const std::size_t size_off = out.size();
    PutU16(out, 0);
    return size_off;
}

inline void EndChunk(std::vector<std::uint8_t>& out, std::size_t size_off) {
    const std::size_t payload = out.size() - size_off - 2;
    SetU16At(out, size_off, static_cast<std::uint16_t>(payload > 0xffffu ? 0xffffu : payload));
}

// ---------------------------------------------------------------------------
// Writer.
// ---------------------------------------------------------------------------

struct FileInfo {
    std::uint32_t flags = 0;
    std::uint32_t max_clients = 0;
    std::uint32_t tick_ms = 100;
    std::uint32_t created_unix = 0;
    std::string map_name;
    std::string level_name;
    std::string gamedir;
    std::string server_name;
};

struct ClientFrame {
    std::uint32_t frame = 0;
    std::uint32_t time_ms = 0;
    std::uint8_t slot = 0;
    std::uint8_t flags = kClientFlagNone;
    PlayerState ps = {};
    EntityState es = {};
};

struct RosterEntry {
    std::uint32_t frame = 0;
    std::uint32_t time_ms = 0;
    std::uint8_t slot = 0;
    std::uint8_t flags = kClientFlagNone;
    std::string name;
    std::string userinfo;
};

struct Event {
    std::uint32_t frame = 0;
    std::uint32_t time_ms = 0;
    std::uint8_t kind = kEventMark;
    std::string text;
};

struct ConfigString {
    std::uint16_t index = 0;
    std::string value;
};

struct EndInfo {
    std::uint32_t first_frame = 0;
    std::uint32_t last_frame = 0;
    std::uint32_t client_frames = 0;
    std::uint32_t max_clients = 0;
    std::uint32_t duration_ms = 0;
    std::uint8_t reason = kEndStopped;
};

inline void WriteFileHeader(std::vector<std::uint8_t>& out, const FileInfo& info) {
    const std::size_t start = out.size();
    PutBytes(out, kMagic, sizeof(kMagic));
    PutU16(out, kVersion);
    const std::size_t header_size_off = out.size();
    PutU16(out, 0);  // patched after strings
    PutU32(out, info.flags);
    PutU32(out, kPlayerStateSize);
    PutU32(out, kEntityStateSize);
    PutU32(out, info.max_clients);
    PutU32(out, info.tick_ms);
    PutU32(out, info.created_unix);
    PutU32(out, 0);  // reserved
    PutU32(out, 0);  // reserved
    PutU32(out, 0);  // reserved
    PutString16(out, info.map_name);
    PutString16(out, info.level_name);
    PutString16(out, info.gamedir);
    PutString16(out, info.server_name);
    const std::size_t total = out.size() - start;
    SetU16At(out, header_size_off,
             static_cast<std::uint16_t>(total > 0xffffu ? 0xffffu : total));
}

inline void WriteClientFrame(std::vector<std::uint8_t>& out, const ClientFrame& frame) {
    const std::uint8_t chunk_flags =
        (frame.flags & kClientFlagHasEntity) ? kChunkFlagHasEntity : kChunkFlagNone;
    const std::size_t size_off = BeginChunk(out, kChunkClientFrame, chunk_flags);
    PutU32(out, frame.frame);
    PutU32(out, frame.time_ms);
    PutU8(out, frame.slot);
    PutU8(out, frame.flags);
    PutU16(out, 0);
    PutBytes(out, &frame.ps, sizeof(frame.ps));
    if (frame.flags & kClientFlagHasEntity)
        PutBytes(out, &frame.es, sizeof(frame.es));
    EndChunk(out, size_off);
}

inline void WriteRoster(std::vector<std::uint8_t>& out, const RosterEntry& entry) {
    const std::size_t size_off = BeginChunk(out, kChunkRoster, kChunkFlagNone);
    PutU32(out, entry.frame);
    PutU32(out, entry.time_ms);
    PutU8(out, entry.slot);
    PutU8(out, entry.flags);
    PutString16(out, entry.name);
    PutString16(out, entry.userinfo);
    EndChunk(out, size_off);
}

inline void WriteEvent(std::vector<std::uint8_t>& out, const Event& event) {
    const std::size_t size_off = BeginChunk(out, kChunkEvent, kChunkFlagNone);
    PutU32(out, event.frame);
    PutU32(out, event.time_ms);
    PutU8(out, event.kind);
    PutU8(out, 0);
    PutString16(out, event.text);
    EndChunk(out, size_off);
}

inline void WriteConfigString(std::vector<std::uint8_t>& out, const ConfigString& cs) {
    const std::size_t size_off = BeginChunk(out, kChunkConfigString, kChunkFlagNone);
    PutU16(out, cs.index);
    PutString16(out, cs.value);
    EndChunk(out, size_off);
}

inline void WriteEnd(std::vector<std::uint8_t>& out, const EndInfo& end) {
    const std::size_t size_off = BeginChunk(out, kChunkEnd, kChunkFlagNone);
    PutU32(out, end.first_frame);
    PutU32(out, end.last_frame);
    PutU32(out, end.client_frames);
    PutU32(out, end.max_clients);
    PutU32(out, end.duration_ms);
    PutU8(out, end.reason);
    PutU8(out, 0);
    PutU16(out, 0);
    EndChunk(out, size_off);
}

// ---------------------------------------------------------------------------
// Reader (host tests and future tooling).
// ---------------------------------------------------------------------------

class Cursor {
  public:
    Cursor(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    bool U8(std::uint8_t& v) {
        if (!Have(1))
            return false;
        v = data_[pos_++];
        return true;
    }
    bool U16(std::uint16_t& v) {
        if (!Have(2))
            return false;
        v = static_cast<std::uint16_t>(data_[pos_]) |
            static_cast<std::uint16_t>(data_[pos_ + 1] << 8);
        pos_ += 2;
        return true;
    }
    bool U32(std::uint32_t& v) {
        if (!Have(4))
            return false;
        v = static_cast<std::uint32_t>(data_[pos_]) |
            (static_cast<std::uint32_t>(data_[pos_ + 1]) << 8) |
            (static_cast<std::uint32_t>(data_[pos_ + 2]) << 16) |
            (static_cast<std::uint32_t>(data_[pos_ + 3]) << 24);
        pos_ += 4;
        return true;
    }
    bool I16(std::int16_t& v) {
        std::uint16_t u = 0;
        if (!U16(u))
            return false;
        v = static_cast<std::int16_t>(u);
        return true;
    }
    bool F32(float& v) {
        std::uint32_t u = 0;
        if (!U32(u))
            return false;
        std::memcpy(&v, &u, sizeof(v));
        return true;
    }
    bool Bytes(void* dst, std::size_t n) {
        if (!Have(n))
            return false;
        std::memcpy(dst, data_ + pos_, n);
        pos_ += n;
        return true;
    }
    bool String16(std::string& s) {
        std::uint16_t n = 0;
        if (!U16(n) || !Have(n))
            return false;
        s.assign(reinterpret_cast<const char*>(data_ + pos_), n);
        pos_ += n;
        return true;
    }
    bool Skip(std::size_t n) {
        if (!Have(n))
            return false;
        pos_ += n;
        return true;
    }
    std::size_t pos() const { return pos_; }
    std::size_t remaining() const { return size_ - pos_; }

  private:
    bool Have(std::size_t n) const { return n <= size_ - pos_; }
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_ = 0;
};

inline bool ReadFileHeader(Cursor& cur, FileInfo& info) {
    char magic[8] = {};
    if (!cur.Bytes(magic, sizeof(magic)) || std::memcmp(magic, kMagic, sizeof(magic)) != 0)
        return false;
    std::uint16_t version = 0;
    std::uint16_t header_size = 0;
    if (!cur.U16(version) || version != kVersion)
        return false;
    if (!cur.U16(header_size))
        return false;
    std::uint32_t ps_size = 0;
    std::uint32_t es_size = 0;
    if (!cur.U32(info.flags) || !cur.U32(ps_size) || !cur.U32(es_size) ||
        !cur.U32(info.max_clients) || !cur.U32(info.tick_ms) ||
        !cur.U32(info.created_unix))
        return false;
    std::uint32_t reserved = 0;
    if (!cur.U32(reserved) || !cur.U32(reserved) || !cur.U32(reserved))
        return false;
    if (ps_size != kPlayerStateSize || es_size != kEntityStateSize)
        return false;
    if (!cur.String16(info.map_name) || !cur.String16(info.level_name) ||
        !cur.String16(info.gamedir) || !cur.String16(info.server_name))
        return false;
    // header_size includes everything from magic through the last string.
    const std::size_t expected = cur.pos();
    if (header_size != expected)
        return false;
    return true;
}

// Convenience parser used by tests: validates the whole file and returns
// chunk payloads as owned byte strings.
struct OwnedChunk {
    std::uint8_t type = 0;
    std::uint8_t flags = 0;
    std::vector<std::uint8_t> payload;
};

inline bool ReadWholeFile(const std::vector<std::uint8_t>& data, FileInfo& info,
                          std::vector<OwnedChunk>& chunks) {
    Cursor cur(data.data(), data.size());
    if (!ReadFileHeader(cur, info))
        return false;
    chunks.clear();
    while (cur.remaining() > 0) {
        std::uint8_t type = 0;
        std::uint8_t flags = 0;
        std::uint16_t size = 0;
        if (!cur.U8(type) || !cur.U8(flags) || !cur.U16(size))
            return false;
        OwnedChunk chunk;
        chunk.type = type;
        chunk.flags = flags;
        chunk.payload.resize(size);
        if (size && !cur.Bytes(chunk.payload.data(), size))
            return false;
        chunks.push_back(std::move(chunk));
    }
    return true;
}

inline bool ParseClientFrame(const OwnedChunk& chunk, ClientFrame& frame) {
    if (chunk.type != kChunkClientFrame)
        return false;
    Cursor cur(chunk.payload.data(), chunk.payload.size());
    std::uint16_t reserved = 0;
    if (!cur.U32(frame.frame) || !cur.U32(frame.time_ms) || !cur.U8(frame.slot) ||
        !cur.U8(frame.flags) || !cur.U16(reserved))
        return false;
    if (!cur.Bytes(&frame.ps, sizeof(frame.ps)))
        return false;
    if (frame.flags & kClientFlagHasEntity) {
        if (!cur.Bytes(&frame.es, sizeof(frame.es)))
            return false;
    }
    return cur.remaining() == 0;
}

inline bool ParseRoster(const OwnedChunk& chunk, RosterEntry& entry) {
    if (chunk.type != kChunkRoster)
        return false;
    Cursor cur(chunk.payload.data(), chunk.payload.size());
    if (!cur.U32(entry.frame) || !cur.U32(entry.time_ms) || !cur.U8(entry.slot) ||
        !cur.U8(entry.flags) || !cur.String16(entry.name) ||
        !cur.String16(entry.userinfo))
        return false;
    return cur.remaining() == 0;
}

inline bool ParseEvent(const OwnedChunk& chunk, Event& event) {
    if (chunk.type != kChunkEvent)
        return false;
    Cursor cur(chunk.payload.data(), chunk.payload.size());
    std::uint8_t reserved = 0;
    if (!cur.U32(event.frame) || !cur.U32(event.time_ms) || !cur.U8(event.kind) ||
        !cur.U8(reserved) || !cur.String16(event.text))
        return false;
    return cur.remaining() == 0;
}

inline bool ParseConfigString(const OwnedChunk& chunk, ConfigString& cs) {
    if (chunk.type != kChunkConfigString)
        return false;
    Cursor cur(chunk.payload.data(), chunk.payload.size());
    if (!cur.U16(cs.index) || !cur.String16(cs.value))
        return false;
    return cur.remaining() == 0;
}

inline bool ParseEnd(const OwnedChunk& chunk, EndInfo& end) {
    if (chunk.type != kChunkEnd)
        return false;
    Cursor cur(chunk.payload.data(), chunk.payload.size());
    std::uint8_t reserved = 0;
    std::uint16_t reserved16 = 0;
    if (!cur.U32(end.first_frame) || !cur.U32(end.last_frame) ||
        !cur.U32(end.client_frames) || !cur.U32(end.max_clients) ||
        !cur.U32(end.duration_ms) || !cur.U8(end.reason) || !cur.U8(reserved) ||
        !cur.U16(reserved16))
        return false;
    return cur.remaining() == 0;
}

}  // namespace svdemo
