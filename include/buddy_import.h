#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Call once from GetGameApi(import) when game DLL is initialized. */
void Buddy_BindGameImport(void* import);

/** Get raw game_import_t pointer. */
void* Buddy_GetGameImport(void);

/** Get or register an engine cvar via game_import_t::cvar (slot 87). */
void* Buddy_GetEngineCvar(const char* var_name, const char* value, int flags, void* command);

/** gi.cvar_setvalue (slot 89): set a cvar's numeric value by name.
 *  CAUTION: engine cvar API calls crash when made from inside frame-path
 *  hooks on some Wine builds - bootstrap context only. For hot-path output
 *  cvars write cvar_t.value (+0x18) directly instead (see clamp_monitor). */
void Buddy_SetEngineCvarValue(const char* var_name, float value);

/** gi.bprintf (slot 12): broadcast printf to all connected clients. */
void Buddy_BroadcastPrintf(int print_level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

/** gi.dprintf (slot 13): printf to the server console/log only, no client ever sees it. */
void Buddy_DebugPrintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

/** gi.cprintf (slot 14): printf to a single client (edict_t*); ent == NULL means server console. */
void Buddy_ClientPrintf(void* ent, int print_level, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

/** gi.clprintf (slot 15): name/color-tagged print to one client, attributed to `from` (e.g. chat-style lines). */
void Buddy_NamePrintf(void* ent, void* from, int color, const char* fmt, ...) __attribute__((format(printf, 4, 5)));

/** gi.welcomeprint (slot 16): sends the server's configured welcome message to one client. No format args. */
void Buddy_WelcomePrintf(void* ent);

/** gi.centerprintf (slot 17): centered screen text to one client. */
void Buddy_CenterPrintf(void* ent, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

/** gi.cinprintf (slot 18): cinematic-style typeamatic text to one client at (x,y) with the given text speed. Not a printf - `text` is sent as-is. */
void Buddy_CinPrintf(void* ent, int x, int y, int textspeed, const char* text);

/** gi.bcaption (slot 19): broadcast a StringPackage caption (by string ID, see SP_Register) to all clients at print_level. */
void Buddy_BroadcastCaption(int print_level, unsigned short string_id);

/** gi.captionprintf (slot 20): send a StringPackage caption (by string ID) to one client. */
void Buddy_CaptionPrintf(void* ent, unsigned short string_id);

/** Helper to read a float value from an engine cvar pointer (+0x18). Returns default_val if cv is null. */
float Buddy_ReadCvarValue(void* cv, float default_val);

/** svc_stufftext opcode (qcommon.h svc_ops_e): WriteByte + WriteString + unicast. */
#define BUDDY_SVC_STUFFTEXT 13

/** gi.WriteByte (slot 32): append a byte to the current network message. */
void Buddy_WriteByte(int c);

/** gi.WriteString (slot 36): append a NUL-terminated string to the current message. */
void Buddy_WriteString(const char* s);

/** gi.unicast (slot 30): send the current message to one client. reliable != 0. */
void Buddy_Unicast(void* ent, int reliable);

/** WriteByte(svc_stufftext) + WriteString(text) + unicast(ent, reliable).
 *  Returns false when ent/text is null or any engine slot is unresolvable. */
bool Buddy_StuffText(void* ent, const char* text);

/** gi.argc (slot 9): argument count of the current client/server command. */
int Buddy_ClientArgc();

/** gi.argv (slot 10): argument n of the current command ("" when missing). */
const char* Buddy_ClientArgv(int n);

/** gi.args (slot 11): argv[1..] concatenated ("" when missing). */
const char* Buddy_ClientArgs();

/** gi.configstring (slot 68): set a server configstring by index. */
bool Buddy_Configstring(int num, const char* s);

/** SV_RemoveIndex @ SoF.exe: drop one configstring in [start, start+max) by path
 *  and multicast svc_removeconfigstring to connected clients. */
bool Buddy_RemoveIndex(const char* path, int start, int max_count);

/** gi.imageindex (slot 3): precache a pic/model/sound name, returns index. */
int Buddy_ImageIndex(const char* name);

/** gi.FS_LoadFile (slot 93): load a file into memory; returns length or -1. */
int Buddy_FS_LoadFile(const char* path, void** buffer, bool override_pak);

/** gi.FS_FreeFile (slot 94): release FS_LoadFile buffer. */
void Buddy_FS_FreeFile(void* buffer);

/** gi.FS_CreatePath (slot 96): create parent dirs for a file path. */
void Buddy_FS_CreatePath(const char* path);

/** gi.FS_Userdir (slot 95): writable user directory (e.g. "User"). */
const char* Buddy_FS_Userdir();

/** gi.SP_Register (slot 47 / SP_RegisterServer): FS_LoadFile("strip/name") then register. */
bool Buddy_SP_Register(const char* package);

/** gi.SP_Print (slot 48): StringPackage print; id is (package_id<<8)|index. */
void Buddy_SP_Print(void* ent, unsigned short id);

/** SP_Print with one layout "%s" arg (sofbuddy.sp LAYOUT_RAW entry). */
void Buddy_SP_PrintLayout(void* ent, unsigned short id, const char* text);

#ifdef __cplusplus
}
#endif
