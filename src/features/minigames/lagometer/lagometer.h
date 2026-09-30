#pragma once

void lag_EnsureRegistered();
void lag_OnMinigameTabOpened(int slot1);
void lag_MaintainForSlot(int slot1);

void lag_ReadPacketsPre();
void lag_ReadPacketsPost();
bool lag_TickBodyActive();
void lag_NoteSimFrameWallMs(float wall_ms);
void lag_SvClientThinkPre(void*& client, void*& cmd);
void lag_SvClientThinkPost(void* client, void* cmd);
void lag_NoteTickCmdDrain(float cmd_ms);
void lag_SvFramePre(int& msec);
void lag_SvFramePost(int msec);
