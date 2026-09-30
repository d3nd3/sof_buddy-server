#pragma once

// Called from minigames ClientCommand before dispatch (lazy register).
void lag_EnsureRegistered();

void lag_OnMinigameTabOpened(int slot1);
