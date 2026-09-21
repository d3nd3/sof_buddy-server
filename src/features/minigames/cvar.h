#pragma once

/** Master enable for the minigames platform (routing + visibility). */
bool MgPlatformEnabled();

/** `_sofbuddy_minigames_bg`: 0 = sb/mg/pn tile, 1 = sb/mg/bg panel. */
bool MgBgUseBlack640();

/** Detach-time teardown: removes the ClientCommand detour. */
extern "C" void Minigames_Shutdown();
