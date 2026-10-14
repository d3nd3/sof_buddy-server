#pragma once

// Called from the outer ClientUserinfoChanged wrapper (installed on GameDllLoaded).
void skinteam_UserinfoScopeBegin(void* ent);
void skinteam_UserinfoScopeEnd(void* ent);

// True when player_die should be suppressed (userinfo teamplay suicide).
bool skinteam_ShouldBlockUserinfoSuicide(void* self, void* inflictor, void* attacker, int damage);
