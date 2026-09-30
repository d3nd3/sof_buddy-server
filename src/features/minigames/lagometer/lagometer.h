#pragma once

// Called from minigames ClientCommand before dispatch (lazy register).
void lag_EnsureRegistered();

void lag_OnMinigameTabOpened(int slot1);
// Refresh cache while +use+score tab is open (called from minigames MaintainLayoutClient).
void lag_MaintainForSlot(int slot1);
