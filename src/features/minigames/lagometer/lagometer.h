#pragma once

void lag_EnsureRegistered();
void lag_OnMinigameTabOpened(int slot1);
void lag_MaintainForSlot(int slot1);

void lag_NoteGameFrameWallMs(float wall_ms);
void lag_NoteTickCmdDrain(float cmd_ms);
void lag_SvFramePre(int& msec);
void lag_SvFramePost(int msec);
