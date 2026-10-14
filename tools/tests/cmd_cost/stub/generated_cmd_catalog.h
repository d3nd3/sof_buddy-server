#pragma once
namespace cmdcost {
constexpr int kCmdCount = 3;
inline const char* CmdName(int i) {
    static const char* const k[] = {"echo", "sp_sc_cvar_math_add", "wait"};
    return (i >= 0 && i < kCmdCount) ? k[i] : "?";
}
}
