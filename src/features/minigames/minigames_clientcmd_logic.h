#pragma once

// Sofplus / spsv often forwards client dot-commands as "say .word ..." before
// the game DLL's ClientCommand runs (see detours.yaml Cbuf_AddText note).
#if defined(_WIN32)
#include <cstring>
#define MG_STRCMPI _stricmp
#else
#include <cstring>
#define MG_STRCMPI strcasecmp
#endif

/** argv index of the command word (0, or 1 after say / say_team). */
inline int MgClientCmdArgvBase(const char* arg0, const char* arg1) {
    if (!arg0 || !arg0[0])
        return 0;
    if (MG_STRCMPI(arg0, "say") && MG_STRCMPI(arg0, "say_team"))
        return 0;
    if (!arg1 || !arg1[0])
        return 0;
    return 1;
}

inline bool MgClientCmdMatches(const char* word, const char* registered) {
    if (!word || !word[0] || !registered || !registered[0])
        return false;
    if (MG_STRCMPI(word, registered) == 0)
        return true;
    if (word[0] == '.' && MG_STRCMPI(word + 1, registered) == 0)
        return true;
    if (registered[0] == '.' && MG_STRCMPI(word, registered + 1) == 0)
        return true;
    return false;
}

inline bool MgClientCmdIsMinigameChatWord(const char* word) {
    return MgClientCmdMatches(word, "lag") || MgClientCmdMatches(word, "sofbuddy_lag") ||
           MgClientCmdMatches(word, ".lag") || MgClientCmdMatches(word, "ttt");
}
