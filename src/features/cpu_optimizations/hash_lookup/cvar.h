#pragma once

/** Creates the feature's gate cvar. Bootstrap-only (GameDllLoaded). */
void HashLookup_InitCvars();

/** Whether _sofbuddy_hashmap asks for the index. Read once, at install time:
 *  the patches go in or they do not, and nothing re-reads this per lookup. */
bool HashLookup_Enabled();
