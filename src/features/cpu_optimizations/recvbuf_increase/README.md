# recvbuf_increase

Raises the engine's UDP sockets' `SO_RCVBUF` so burst `clc_move` traffic
survives main-thread stalls instead of being dropped by the kernel.

## Why (IDA, SoF.exe base `0x20000000`)

* `setupIpUdpSocket @0x2004ECB0` is Quake 2's `UDP_OpenSocket`: `socket()` →
  `ioctlsocket(FIONBIO)` → `setsockopt(SO_BROADCAST)` → `bind()`. Returns the
  `SOCKET`, or `0` on every failure path. Single caller `NET_OpenIP
  @0x2004EE30`, which stores it in `ip_sockets[]` (base `0x20390118`,
  server slot `0x2039011C` — verified via xrefs from `NET_GetPacket` /
  `NET_SendPacket`).
* The engine sets `SO_BROADCAST` only — never `SO_RCVBUF`. Every socket keeps
  the OS default.
* `SV_ReadPackets` drains the socket once per `SV_Frame`, but `Cbuf_Execute`,
  `G_RunFrame` and sofplus timers share that thread. While the server scripts,
  packets queue in the kernel. Past `SO_RCVBUF` the kernel drops them and
  `Netchan_Process` reports `Dropped %i` / `Out of order` — the micro-stutter.

## What it does

* `Post` hook on `SetupIpUdpSocket`: `setsockopt(SO_RCVBUF)` on every newly
  opened socket (covers reconnects / `NET_Config` re-opens).
* Sweep of `ip_sockets[2]` at `GameDllLoaded`: engine `NET_Init` runs before
  the game DLL loads, so the hook alone would miss the live sockets.
* Reads the size back with `getsockopt` (kernel clamps to `net.core.rmem_max`
  on Linux/Wine) and publishes it, so over-asking is visible, not silent.

## Cvars

| cvar | default | notes |
|---|---|---|
| `_sofbuddy_cpuopt` | 1 | master gate (shared `cpu_optimizations` folder) |
| `_sofbuddy_recvbuf_kb` | 3107 | requested KiB per socket, live. `0` = off. Clamped to 64–4096 KiB |
| `_sofbuddy_recvbuf_applied` | — | gauge: actual bytes after kernel clamp |
| `_sofbuddy_recvbuf_sockets` | — | gauge: sockets touched |

Linux/Wine note: `getsockopt` reports double what was requested (kernel
bookkeeping), and values above `net.core.rmem_max` are clamped — raise it
(`sysctl net.core.rmem_max=16777216`) if the gauge comes back smaller than
requested. Bigger buffers convert loss into delay; the existing `net_drop`
replay still applies, so keep script drains short too.
