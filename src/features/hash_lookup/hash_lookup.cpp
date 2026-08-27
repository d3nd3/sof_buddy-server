// Cvar / command / alias lookups as hash maps.
//
// SoF inherits Quake 2's dictionaries-as-linked-lists: every cvar read, every
// cvar write and every console command walks a list and strcmps each node.
// That is fine for a client with a couple of hundred cvars and a human at the
// keyboard. It is not fine for a server running sofplus scripting, where the
// script engine is the one typing: each script variable is a cvar, each script
// line is a console command, and a miss on a command scans the command list,
// then the alias list, then every cvar. The lists grow with the script set, so
// the cost per lookup grows with it too, and all of it lands in the gap
// between server ticks.
//
// This feature replaces the *search* in those functions with an O(1) hash
// lookup and leaves everything else as literal engine code. See README.md for
// the patch sites and the register contract, index.cpp for how the tables stay
// coherent with the engine's lists, and stubs.cpp for the bridges.

#include "cvar.h"
#include "patch.h"
#include "log.h"

void hashlookup_OnGameDllLoaded(void* game_export) {
    (void)game_export;
    HashLookup_InitCvars();

    if (!HashLookup_Enabled()) {
        PrintOutConsole(PRINT_DEV, "[hash_lookup] off (_sofbuddy_hashmap 0)\n");
        return;
    }
    hashlookup::Install();
}

/** Detach hook, called from DllMain. Restoring the engine's bytes here is not
 *  optional: spsv FreeLibrary/reloads this DLL between games, and the jmps we
 *  wrote point into this image. */
extern "C" void HashLookup_Shutdown(void) {
    hashlookup::Revert();
}
