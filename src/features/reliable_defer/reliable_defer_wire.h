#pragma once

#include <cstdint>

// Message-boundary helpers mirroring CL_ParseServerMessage @ 0x2000ee30 (SoF.exe).
// Used only to find safe cut points — never executed on the client.

inline int RelDef_WireNeed(const std::uint8_t* p, int n, int i, int k) {
    return p && i >= 0 && i + k <= n ? i + k : -1;
}

inline int RelDef_WireCStrEnd(const std::uint8_t* p, int n, int i) {
    for (; i < n; ++i) {
        const std::uint8_t c = p[i];
        if (!c || c == 0xFF)
            return i + 1;
    }
    return -1;
}

inline int RelDef_WireReadEntityNumber(const std::uint8_t* p, int n, int& i, int& ent) {
    if (i >= n)
        return -1;
    ent = p[i++];
    if (static_cast<signed char>(ent) < 0) {
        if (i >= n)
            return -1;
        ent |= static_cast<int>(p[i++]) << 8;
    }
    if (static_cast<signed char>(ent >> 8) < 0) {
        if (i >= n)
            return -1;
        ent |= static_cast<int>(p[i++]) << 16;
    }
    if (ent & 0x800000) {
        if (i >= n)
            return -1;
        ent |= static_cast<int>(p[i++]) << 24;
    }
    if (ent & 0x1000000) {
        if (i + 2 > n)
            return -1;
        i += 2;
    } else {
        if (i >= n)
            return -1;
        ++i;
    }
    return i;
}

inline int RelDef_WireSkipModelBytes(const std::uint8_t* p, int n, int& i) {
    if (i >= n)
        return -1;
    const signed char r1 = static_cast<signed char>(p[i++]);
    if (i >= n)
        return -1;
    ++i;
    if (r1 < 0) {
        if (i + 1 >= n)
            return -1;
        const signed char r3 = static_cast<signed char>(p[i]);
        i += 2;
        if (r3 < 0) {
            if (i + 1 >= n)
                return -1;
            i += 2;
        }
    }
    return i;
}

inline int RelDef_WireParseDeltaSkip(const std::uint8_t* p, int n, int i, unsigned ebx) {
    auto byte = [&]() {
        if (i >= n)
            return -1;
        ++i;
        return 0;
    };
    auto bytes = [&](int k) {
        if (RelDef_WireNeed(p, n, i, k) < 0)
            return -1;
        i += k;
        return 0;
    };
    if (ebx & 0x100000u) {
        if (byte() < 0)
            return -1;
    }
    if (ebx & 0x200u) {
        if (byte() < 0)
            return -1;
    }
    if (ebx & 0x20000u) {
        if (byte() < 0)
            return -1;
    } else if (ebx & 0x1000u) {
        if (bytes(2) < 0)
            return -1;
    }
    if (ebx & 0x80000u) {
        if (ebx & 0x2000000u) {
            if (bytes(4) < 0)
                return -1;
        } else if (byte() < 0)
            return -1;
    } else if (ebx & 0x2000000u) {
        if (bytes(2) < 0)
            return -1;
    }
    if ((ebx & 0x14000u) == 0x14000u) {
        if (bytes(4) < 0)
            return -1;
    } else if (ebx & 0x10000u) {
        if (byte() < 0)
            return -1;
    } else if (ebx & 0x4000u) {
        if (bytes(2) < 0)
            return -1;
    }
    if ((ebx & 0x500u) == 0x500u) {
        if (bytes(4) < 0)
            return -1;
    } else if (ebx & 0x400u) {
        if (byte() < 0)
            return -1;
    } else if (ebx & 0x100u) {
        if (bytes(2) < 0)
            return -1;
    }
    if (ebx & 1u) {
        if (bytes(2) < 0)
            return -1;
    }
    if (ebx & 2u) {
        if (bytes(2) < 0)
            return -1;
    }
    if (ebx & 0x20u) {
        if (bytes(2) < 0)
            return -1;
    }
    if (ebx & 0x10u) {
        if (byte() < 0)
            return -1;
    }
    if (ebx & 4u) {
        if (byte() < 0)
            return -1;
    }
    if (ebx & 0x4000000u) {
        if (byte() < 0)
            return -1;
    }
    if (ebx & 8u) {
        if (byte() < 0)
            return -1;
    }
    if (ebx & 0x200000u) {
        if (byte() < 0)
            return -1;
        if (ebx & 0x400000u) {
            if (byte() < 0)
                return -1;
        }
    }
    if (ebx & 0x40u) {
        if (RelDef_WireSkipModelBytes(p, n, i) < 0)
            return -1;
    }
    if (ebx & 0x40000u) {
        if (bytes(4) < 0)
            return -1;
    }
    if (ebx & 0x2000u) { // bh&20h — groups of 3 bits in a short mask
        if (bytes(2) < 0)
            return -1;
        const int mask = p[i - 2] | (p[i - 1] << 8);
        for (int base = 0; base < 12; base += 3) {
            for (int b = 0; b < 3; ++b) {
                if (mask & (1 << (base + b))) {
                    if (bytes(2) < 0)
                        return -1;
                }
            }
        }
    }
    return i;
}

inline int RelDef_WireSpawnBaselineEnd(const std::uint8_t* p, int n, int i) {
    int ent = 0;
    if (RelDef_WireReadEntityNumber(p, n, i, ent) < 0)
        return -1;
    return RelDef_WireParseDeltaSkip(p, n, i, static_cast<unsigned>(ent));
}

inline int RelDef_WireServerDataEnd(const std::uint8_t* p, int n, int i) {
    if (RelDef_WireNeed(p, n, i, 8) < 0)
        return -1;
    i += 8;
    if (RelDef_WireNeed(p, n, i, 2) < 0)
        return -1;
    i += 2;
    i = RelDef_WireCStrEnd(p, n, i);
    if (i < 0 || RelDef_WireNeed(p, n, i, 2) < 0)
        return -1;
    i += 2;
    return RelDef_WireCStrEnd(p, n, i);
}

inline int RelDef_WireDownloadEnd(const std::uint8_t* p, int n, int i) {
    if (RelDef_WireNeed(p, n, i, 3) < 0)
        return -1;
    const int size = static_cast<signed short>(p[i] | (p[i + 1] << 8));
    i += 3;
    if (size < 0)
        return i;
    return RelDef_WireNeed(p, n, i, size);
}

inline int RelDef_WireSoundEnd(const std::uint8_t* p, int n, int i) {
    if (RelDef_WireNeed(p, n, i, 2) < 0)
        return -1;
    const unsigned bl = p[i++];
    ++i;
    if (bl & 1u) {
        if (i >= n)
            return -1;
        ++i;
    }
    if (bl & 2u) {
        if (i >= n)
            return -1;
        ++i;
    }
    if (bl & 0x10u) {
        if (i >= n)
            return -1;
        ++i;
    }
    if (bl & 8u) {
        if (RelDef_WireNeed(p, n, i, 2) < 0)
            return -1;
        i += 2;
    }
    if (bl & 4u) {
        if (RelDef_WireNeed(p, n, i, 6) < 0)
            return -1;
        i += 6;
    }
    return i;
}

inline int RelDef_WireSoundInfoEnd(const std::uint8_t* p, int n, int i) {
    if (i >= n)
        return -1;
    unsigned bl = p[i++];
    while (bl) {
        if (bl & 1u) {
            if (i >= n)
                return -1;
            ++i;
            bl &= ~1u;
        } else if (bl & 2u) {
            if (i >= n)
                return -1;
            ++i;
            bl &= ~2u;
        } else if (bl & 4u) {
            if (i >= n)
                return -1;
            ++i;
            bl &= ~4u;
        } else if (bl & 8u) {
            if (i >= n)
                return -1;
            ++i;
            bl &= ~8u;
        } else
            return -1;
    }
    return i;
}

inline int RelDef_WireCulledEventEnd(const std::uint8_t* p, int n, int i) {
    if (RelDef_WireNeed(p, n, i, 4) < 0)
        return -1;
    return i + 4;
}

inline int RelDef_WireCountedShortEnd(const std::uint8_t* p, int n, int i) {
    if (RelDef_WireNeed(p, n, i, 2) < 0)
        return -1;
    const int count = p[i] | (p[i + 1] << 8);
    return RelDef_WireNeed(p, n, i + 2, count);
}
