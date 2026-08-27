# Engine↔game API tables — slot maps

Two tables cross the DLL boundary each map load. Both are documented here:

1. **`game_import_t`** ("gi") — engine→game, passed to `GetGameAPI(import)`.
2. **`game_export_t`** ("ge") — game→engine, returned by `GetGameAPI`.

---

# 1. Engine import table (`game_import_t`)

The engine hands the game DLL a `game_import_t*` on every `GetGameAPI(import)`
call. The stock DLL immediately **copies** it into a local global — verified in
IDA on retail `gamex86.dll` (`GetGameAPI @ 0x500AD9B0`, `rep movsd` with
`ecx=0x63`):

| What | Where |
|---|---|
| Import block size | **0x18C bytes = 99 slots** |
| Stock DLL's copy (`GameImport_t`) | `0x5015C9F0` (retail gamex86.dll, base 0x50000000) |
| `cvar_t::value` offset | **`+0x18`** (stock reads `fld dword ptr [eax+18h]`) |

Offsets below are identical between Windows retail `gamex86.dll`, the SDK
header (`reference/engine/gamecpp/game.h`) and Linux `game.so` — all three were
cross-checked.

Evidence legend: **[V]** = slot address xref-verified in IDA against stock
usage; names in *italics* come from the Linux `game.so` IDA work.

## Table

| Off | Slot | SDK field | Engine function (IDA) | |
|------|----|------------------------------------------------------|-------|
| 0x000 | 0 | modelindex | *SV_ModelIndex* | |
| 0x004 | 1 | soundindex | *SV_SoundIndex* | |
| 0x008 | 2 | effectindex | *SV_EffectIndex* | |
| 0x00C | 3 | imageindex | *SV_ImageIndex* | |
| 0x010 | 4 | unload_sound | *SV_UnloadSound* | |
| 0x014 | 5 | FilterPacket | *SV_FilterPacket* | |
| 0x018 | 6 | CreateGhoulConfigStrings | *PF_SetGhoulConfigStrings* | |
| 0x01C | 7 | setmodel | *PF_setmodel* | |
| 0x020 | 8 | setrendermodel | *PF_setrendermodel* | |
| 0x024 | 9 | argc | *Cmd_Argc* | |
| 0x028 | 10 | argv | *Cmd_Argv* | |
| 0x02C | 11 | args | *Cmd_Args* | |
| 0x030 | 12 | bprintf | *SV_BroadcastPrintf* | |
| 0x034 | 13 | dprintf | *PF_dprintf* | [V] 259 xrefs |
| 0x038 | 14 | cprintf | *PF_cprintf* | |
| 0x03C | 15 | clprintf | *PF_clprintf* | |
| 0x040 | 16 | welcomeprint | | |
| 0x044 | 17 | centerprintf | | |
| 0x048 | 18 | cinprintf | | |
| 0x04C | 19 | bcaption | *SV_BroadcastCaption* | |
| 0x050 | 20 | captionprintf | | |
| 0x054 | 21 | Con_ClearNotify | *PF_Con_ClearNotify* | |
| 0x058 | 22 | sound | *sound_new* | |
| 0x05C | 23 | positioned_sound | *SV_StartSound* | |
| 0x060 | 24 | DebugGraph | *(unidentified)* | |
| 0x064 | 25 | DamageTexture | *DT_DoDamage* | |
| 0x068 | 26 | SurfaceTypeList | *DT_GetSurfaceTypes* | |
| 0x06C | 27 | Update | *SCR_UpdateLoading* | |
| 0x070 | 28 | multicast | *SV_MulticastWrap* | |
| 0x074 | 29 | multicastignore | *SV_Multicast* | |
| 0x078 | 30 | unicast | *PF_Unicast* | |
| 0x07C | 31 | WriteChar | | |
| 0x080 | 32 | WriteByte | | |
| 0x084 | 33 | WriteShort | | |
| 0x088 | 34 | WriteLong | | |
| 0x08C | 35 | WriteFloat | | |
| 0x090 | 36 | WriteString | | |
| 0x094 | 37 | WritePosition | *PF_WritePos* | |
| 0x098 | 38 | WriteDir | | |
| 0x09C | 39 | WriteAngle | | |
| 0x0A0 | 40 | WriteByteSizebuf | *MSG_WriteByte* | |
| 0x0A4 | 41 | WriteShortSizebuf | *MSG_WriteShort* | |
| 0x0A8 | 42 | WriteLongSizebuf | *MSG_WriteLong* | |
| 0x0AC | 43 | ReliableWriteByteToClient | | |
| 0x0B0 | 44 | ReliableWriteDataToClient | | |
| 0x0B4 | 45 | GetNearestByteNormal | *PF_GetNearestByteNormal* | |
| 0x0B8 | 46 | sendPlayernameColors | *PF_SendPlayernameColors* | |
| 0x0BC | 47 | SP_Register | *SP_RegisterServer* | |
| 0x0C0 | 48 | SP_Print | | |
| 0x0C4 | 49 | SP_Print_Obit | | |
| 0x0C8 | 50 | SP_SPrint | | |
| 0x0CC | 51 | SP_GetStringText | | |
| 0x0D0 | 52 | trace | *SV_Trace* | |
| 0x0D4 | 53 | polyTrace | *SV_PolyTrace* | |
| 0x0D8 | 54 | pointcontents | *SV_PointContents* | |
| 0x0DC | 55 | RegionDistance | *SV_CheckRegionDistance* | |
| 0x0E0 | 56 | inPVS | *PF_inPVS* | |
| 0x0E4 | 57 | inPHS | *PF_inPHS* | |
| 0x0E8 | 58 | SetAreaPortalState | *CM_SetAreaPortalState* | |
| 0x0EC | 59 | AreasConnected | *CM_AreasConnected* | |
| 0x0F0 | 60 | GetGhoul | *GetTheGhoul* | |
| 0x0F4 | 61 | NewPlayerModelInfo | | |
| 0x0F8 | 62 | FindGSQFile | | |
| 0x0FC | 63 | ReadGsqEntry | | |
| 0x100 | 64 | PrecacheGSQFile | | |
| 0x104 | 65 | RegisterGSQSequences | | |
| 0x108 | 66 | TurnOffPartsFromGSQFile | | |
| 0x10C | 67 | **isClient** (data, `int**`) | *sv_isClient* | |
| 0x110 | 68 | configstring | *PF_Configstring* | [V] setDMMode/AssignTeam |
| 0x114 | 69 | SZ_Init | | |
| 0x118 | 70 | SZ_Clear | | |
| 0x11C | 71 | SZ_Write | | |
| 0x120 | 72 | error | *PF_error* | |
| 0x124 | 73 | Sys_ConsoleOutput | | |
| 0x128 | 74 | Sys_GetPlayerAPI | | |
| 0x12C | 75 | Sys_UnloadPlayer | | |
| 0x130 | 76 | flrand | | |
| 0x134 | 77 | irand | | |
| 0x138 | 78 | linkentity | *(absent from linux notes)* | [V] 214 xrefs |
| 0x13C | 79 | unlinkentity | *SV_UnlinkEdict* | |
| 0x140 | 80 | BoxEdicts | *SV_AreaEdicts* | |
| 0x144 | 81 | Pmove | | |
| 0x148 | 82 | TagMalloc | *Z_TagMalloc* | |
| 0x14C | 83 | TagFree | *Z_Free* | |
| 0x150 | 84 | FreeTags | *Z_FreeTags* | |
| 0x154 | 85 | AppendToSavegame | *SG_Append* | |
| 0x158 | 86 | ReadFromSavegame | *SG_Read* | |
| 0x15C | 87 | **cvar** (Cvar_Get) | *Cvar_Get* | [V] 115 xrefs |
| 0x160 | 88 | cvar_set | *Cvar_Set* | ⚠ see incident below |
| 0x164 | 89 | cvar_setvalue | *Cvar_SetValue* | |
| 0x168 | 90 | cvar_forceset | *Cvar_ForceSet* | |
| 0x16C | 91 | cvar_info | *Cvar_BitInfo* | |
| 0x170 | 92 | cvar_variablevalue | *Cvar_VariableValue* | |
| 0x174 | 93 | FS_LoadFile | | |
| 0x178 | 94 | FS_FreeFile | | |
| 0x17C | 95 | FS_Userdir | | |
| 0x180 | 96 | FS_CreatePath | | |
| 0x184 | 97 | FS_FDoesFileExist | | |
| 0x188 | 98 | Cbuf_AddText | | |

Note: the SDK header lists an optional `GetLabel` before `cvar`
(`#if !_FINAL_ && _RAVEN_`); **retail builds omit it** — that is why `cvar`
lands on 0x15C and the block ends after 99 slots.

## Incident record — why this table exists

`src/buddy_import.cpp` originally used `kGiSlotCvar = 88` (`0x160`) → called
**Cvar_Set** instead of Cvar_Get. Boot survived by accident (setting a cvar
returns a valid `cvar_t*`); the first player-connect spawn crashed calling a
garbage pointer. Fixed to slot 87 / 0x15C. Lesson: never hardcode a slot without
an entry in this table.

## Re-verifying a slot with IDA (MCP)

With `gamex86.i64` open:

1. Slot N lives at `0x5015C9F0 + 4*N` (e.g. slot 87 → `0x5015CB4C`).
2. `ida_xrefs_to(slot_ea)` — expect read-xrefs from plausible callers.
3. Cross-check the SDK order in `reference/engine/gamecpp/game.h`.

Runtime use from the shim: bind once via `Buddy_BindGameImport(import)` (called
from the first `GetGameAPI`), then read slots as in `src/buddy_import.cpp`
(`GiCvarFn()`). Copy that pattern for new accessors; keep them typed and null-
checked like `Buddy_GetEngineCvar`.

---

# 2. Game export table (`game_export_t`) — "ge"

Filled by the game DLL's `GetGameAPI`, read by the engine. Verified in IDA on
retail `gamex86.dll`: `GetGameAPI @ 0x500AD9B0` assigns every field; struct base
(`apiversion` / IDA `GameVersion`) = **`0x5015C8D0`**.

⚠ **Retail delta:** `ClientPreConnect` (+0x20) exists in retail but is missing
from the SDK header (`reference/engine/gamecpp/game.h`) — everything after it
sits 4 bytes lower than an SDK-only count would suggest. Confirmed against
Windows IDA and Linux `game.so`; all three agree.

| Off | Field | Windows EA | Notes |
|------|------|------------|-------|
| +0x00 | apiversion | `0x5015C8D0` | retail writes 3 |
| +0x04 | Init | `0x5015C8D4` | InitGame |
| +0x08 | Shutdown | `0x5015C8D8` | ShutdownGame |
| +0x0C | SpawnEntities | `0x5015C8DC` | |
| +0x10 | WriteGame | `0x5015C8E0` | |
| +0x14 | ReadGame | `0x5015C8E4` | |
| +0x18 | WriteLevel | `0x5015C8E8` | |
| +0x1C | ReadLevel | `0x5015C8EC` | |
| +0x20 | ClientPreConnect | `0x5015C8F0` | ⚠ not in SDK header |
| +0x24 | ClientConnect | `0x5015C8F4` | ent, userinfo |
| +0x28 | ClientBegin | `0x5015C8F8` | |
| +0x2C | ClientUserinfoChanged | `0x5015C8FC` | IDA global: `userinfochanged` |
| +0x30 | ClientDisconnect | `0x5015C900` | |
| +0x34 | ClientCommand | `0x5015C904` | |
| +0x38 | ClientThink | `0x5015C908` | |
| +0x3C | ResetCTFTeam | `0x5015C90C` | |
| +0x40 | GameAllowASave | `0x5015C910` | |
| +0x44 | SavesLeft | `0x5015C914` | |
| +0x48 | GetGameStats | `0x5015C918` | |
| +0x4C | UpdateInven | `0x5015C91C` | |
| +0x50 | GetDMGameName | `0x5015C920` | |
| +0x54 | GetCinematicFreeze | `0x5015C924` | |
| +0x58 | SetCinematicFreeze | `0x5015C928` | |
| +0x5C | RunFrame | `0x5015C92C` | serverframe |
| +0x60 | edicts (data ptr) | `0x5015C930` | written by `InitGame` @ `0x500AD33C`; same array as `g_edicts` @ RVA `0x15CCA0` (362 xrefs) |
| +0x64 | edict_size | `0x5015C934` | stock writes `1124` = **0x464** — independent confirmation of ctf_spawn's `kEdictStride` |
| +0x68 | num_edicts | `0x5015C938` | live example: 0x95 (149) |
| +0x6C | max_edicts | `0x5015C93C` | live example: 0x400 (1024) |

The shim itself only relies on `+0x00 apiversion` (sanity check) and passes the
pointer straight back to the engine; feature code reaches edicts via the
`g_edicts` RVA instead of `ge->edicts` — both alias the same array.
