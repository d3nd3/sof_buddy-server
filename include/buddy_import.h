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

#ifdef __cplusplus
}
#endif
