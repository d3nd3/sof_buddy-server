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

inline bool MgClientCmdIsSay(const char* arg0) {
    return arg0 && (!MG_STRCMPI(arg0, "say") || !MG_STRCMPI(arg0, "say_team"));
}

// Chat-box text arrives as one say argument (".mg_lag next"). Split it into words.
// Strips one pair of wrapping quotes. Words point into `buf`.
inline int MgSplitCmdWords(const char* text, char* buf, int cap, const char** words, int maxWords) {
    if (!text || !buf || cap < 2 || !words || maxWords < 1)
        return 0;
    const char* s = text;
    while (*s == ' ' || *s == '\t')
        ++s;
    int n = 0;
    while (s[n])
        ++n;
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t'))
        --n;
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        ++s;
        n -= 2;
    }
    if (n >= cap)
        n = cap - 1;
    if (n < 0)
        n = 0;
    std::memcpy(buf, s, static_cast<std::size_t>(n));
    buf[n] = '\0';
    int argc = 0;
    for (char* p = buf; *p && argc < maxWords;) {
        while (*p == ' ' || *p == '\t')
            ++p;
        if (!*p)
            break;
        words[argc++] = p;
        while (*p && *p != ' ' && *p != '\t')
            ++p;
        if (*p) {
            *p = '\0';
            ++p;
        }
    }
    return argc;
}

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
    return MgClientCmdMatches(word, "mg") || MgClientCmdMatches(word, "mg_lag") ||
           MgClientCmdMatches(word, "mg_cvars") || MgClientCmdMatches(word, "mg_ttt") ||
           MgClientCmdMatches(word, "mg_cmds");
}
