#pragma once

void lag_EnsureRegistered();
void lag_MaintainForSlot(int slot1);

void lag_ReadPacketsPre();
void lag_ReadPacketsPost();
void lag_RunGameFramePost();
void lag_NoteRunGameFrameBodyEnd();
bool lag_TickBodyActive();
bool lag_TickArmed();
void lag_NoteSimFrameWallMs(float wall_ms);
void lag_NoteHighclampLostMs(int lost_ms);
void lag_NoteGameEvent(const char* tag);
void lag_PutClientInServerPost(void* ent);
short lag_RespawnPost(short result, void* ent);
void lag_PlayerDiePost(void* self, void* inflictor, void* attacker, int damage, float* point);
void lag_ClcMovePre(void*& client);
void lag_ClcMovePost(void* client);
void lag_SendClientMessagesPre();
void lag_SendClientMessagesPost();
void lag_NoteTickCmdDrain(float cmd_ms);
void lag_SvFramePre(int& msec);
void lag_SvFramePost(int msec);
