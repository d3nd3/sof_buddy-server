#pragma once

/** Creates the pool's gate cvar. Bootstrap-only (GameDllLoaded). */
void ZPool_InitCvars();

/** Whether _sofbuddy_zpool asks for recycling. Read once, at bootstrap. */
bool ZPool_Enabled();
