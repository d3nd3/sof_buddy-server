#pragma once

// ---------------------------------------------------------------------------
// Engine state this feature reads and (for svs.realtime) writes.
//
// SoF.exe / SoF-spsv.exe share the same engine .text and the image is not
// relocatable (it always lands at 0x20000000), but we still express these as
// RVAs and add the real module base at bind time - it costs nothing and stays
// correct if an image ever does get relocated.
//
// sv.state / sv.time / svs.realtime are the same three globals clamp_monitor
// documents (SpawnServer writes all three; SV_Frame and SV_RunGameFrame make
// every tick decision out of the last two). `dedicated` is the cvar_t* the
// engine's own WinMain loop tests before it calls Sleep(1).
//
// Anchors, all in SoF.exe:
//   WinMain        @ 0x20066300  Sleep(1) -> PeekMessage -> spin until
//                                (Sys_Milliseconds() - oldtime) >= 1 ->
//                                Qcommon_frame(msec) -> oldtime = newtime
//   Qcommon_frame  @ 0x2001F720  Sys_ConsoleInput/Cbuf_Execute @0x2001F885,
//                                then SV_Frame(msec) @0x2001F8BB
//   SV_Frame       @ 0x2005F5B0  svs.realtime += msec; SV_ReadPackets();
//                                if (svs.realtime < sv.time) { lowclamp } else
//                                SV_RunGameFrame()
//   SV_RunGameFrame@ 0x2005F3F0  sv.framenum++; sv.time = framenum*100;
//                                RunFrame(); if (sv.time < svs.realtime)
//                                { "sv highclamp"; svs.realtime = sv.time; }
// ---------------------------------------------------------------------------

#include <cstdint>

namespace tickpace {

constexpr unsigned kRvaSvState     = 0x3A1F20;  // sv.state (server_state_t)
constexpr unsigned kRvaSvTime      = 0x3A1F28;  // sv.time      (unsigned ms)

// svs.initialized / svs.realtime - the first two fields of server_static_t,
// adjacent exactly as in stock Quake 2. SV_Frame's own precondition is
// `if (!svs.initialized) return;` at 0x2005F5BF/0x2005F5D2, taken *before* the
// `svs.realtime += msec` at 0x2005F5E2: on that path the engine never
// accumulates the clock, so this feature must not move it either.
constexpr unsigned kRvaSvsInitialized = 0x396DE0;  // svs.initialized (qboolean)
constexpr unsigned kRvaSvsRealtime    = 0x396DE4;  // svs.realtime (unsigned ms)
constexpr unsigned kRvaDedicated   = 0x249618;  // cvar_t *dedicated (WinMain @0x20066384)

// The console command buffer, as Cbuf_Execute @0x20018530 uses it.
//
// cmd_text.cursize is read at the function's first instruction and is the
// engine's own "is there anything queued" test - zero means the whole call is
// fourteen instructions and a return.
//
// cmd_wait (0x2023F838) is deliberately NOT touched. Cbuf_Execute honours it
// as "stop draining, keep the rest", which looks like exactly the lever this
// feature wants - and using it corrupts sofplus's function arguments. See the
// "Drains are atomic" note in tick_pacing.cpp before reaching for it again.
constexpr unsigned kRvaCmdTextCursize = 0x23F830;  // cmd_text.cursize (int)

// server_state_t: ss_dead=0, ss_loading=1, ss_game=2.
constexpr std::int32_t kSvStateGame = 2;

struct EngineGlobals {
    volatile const std::uint32_t* svTime      = nullptr;
    volatile std::uint32_t*       svsRealtime = nullptr;  // the one field we write
    volatile const std::int32_t*  svState     = nullptr;
    volatile const std::int32_t*  svsInitialized = nullptr;
    void* const*                  dedicated   = nullptr;  // cvar_t**
    volatile const std::int32_t*  cmdTextCursize = nullptr;
};

/** Resolves and validates the engine image once. False forever after (with a
 *  single log line) if the image can't be validated - the feature then stays
 *  completely inert. */
bool EngineReady();

/** Only valid once EngineReady() has returned true. */
const EngineGlobals& Engine();

/** True when a map is actually running *and* SV_Frame will accumulate the
 *  clock: sv.state == ss_game and svs.initialized != 0. Every write this
 *  feature makes to svs.realtime is gated on it - on SV_Frame's early-return
 *  path the engine never adds msec, so a correction applied there would sit in
 *  the engine's clock unopposed. */
bool ServerRunning();

/** True when the engine's own dedicated cvar is nonzero. */
bool IsDedicated();

/** True when the console command buffer has anything queued. */
bool CommandsQueued();

}  // namespace tickpace
