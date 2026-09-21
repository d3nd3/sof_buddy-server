#pragma once

/** Register server-console commands once per shim bootstrap. Safe across map
 *  rotates: removes stale engine entries before re-adding (handler pointers
 *  live in this DLL generation). Call from stufftext_OnGameDllLoaded only. */
void StuffText_RegisterConsoleCommands();
