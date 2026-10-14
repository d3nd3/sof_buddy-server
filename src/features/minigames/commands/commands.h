#pragma once

// Called from minigames ClientCommand before dispatch (lazy register).
void cmds_EnsureRegistered();
void cmds_OnGameDllLoaded(void* gameExport);
void cmds_MaintainForSlot(int slot1);
