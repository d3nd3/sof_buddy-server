#pragma once

// Engine layout for dedicated SoF.exe (IDA-verified).

constexpr unsigned kRvaSvsClients = 0x396EEC;
constexpr unsigned kRvaBuffersize = 0x136A0C;  // int; NET_Config: 16384 if maxclients==1 else 1400
constexpr unsigned kRvaSvFramenum = 0x3A1F30;  // int; ++ each SV_RunGameFrame
constexpr unsigned kClientStride = 0xD2AC;
constexpr unsigned kClientStateOfs = 0x0;            // client_state_t (0..3)
constexpr int kCsConnected = 2;
constexpr int kCsSpawned = 3;
constexpr unsigned kClientMessageOfs = 0x52B4;       // netchan.message (sizebuf_t)
constexpr unsigned kClientReliableLenOfs = 0x92B8;   // netchan.reliable_length

constexpr unsigned kRvaMulticastData = 0x3F6C38;
constexpr unsigned kRvaMulticastCursize = 0x3F6C40;
constexpr unsigned kRvaSvMulticast = 0x39DF04;

// sizebuf_t @ Netchan_Setup / SZ_GetSpace
constexpr unsigned kSzAllowoverflow = 0x00;
constexpr unsigned kSzOverflowed = 0x01;
constexpr unsigned kSzData = 0x04;
constexpr unsigned kSzMaxsize = 0x08;
constexpr unsigned kSzCursize = 0x0C;

// NET_Config: message.maxsize = buffersize - kMsgInlineReserve (Netchan_Setup @ 0x2004D3E4)
constexpr int kMsgInlineReserve = 16;
// Server→connected client: seq+ack only (qport @ netchan+4 set in Netchan_Setup)
constexpr int kWireHdrBytes = 8;
