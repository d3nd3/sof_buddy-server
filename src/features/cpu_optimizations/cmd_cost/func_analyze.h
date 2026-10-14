#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace addoncost {

struct Cost {
    double min = 0, avg = 0, max = 0;
};

struct FuncCost {
    std::string name;
    std::string file;
    Cost cost;
};

struct EventCost {
    std::string event;
    Cost cost;
    std::vector<std::string> funcs;
};

struct Opts {
    int maxclients = 12;
    int maxwhile = 100000;  // sofplus abort
    int defaultWhile = 64;  // unknown bound
    int eventSlot = 0;      // ~par_slot for event handlers
    int defaultStrLen = 32; // estimated literal/cvar payload when unknown
};

struct Report {
    std::vector<FuncCost> funcs;
    std::vector<EventCost> events;
    int unknownCmds = 0;
    int foldedIfs = 0;
    int files = 0;
};

// Unique first tokens from all parsed .func bodies (for spin priming).
std::vector<std::string> CollectCommands(const char* dir, const Opts& opt = {});

Report AnalyzeSource(const char* file, const char* text, const Opts& opt = {});
Report AnalyzeDir(const char* dir, const Opts& opt = {});

// func name -> source basename (e.g. "foo.func"); first file wins on duplicates.
std::unordered_map<std::string, std::string> FuncFileIndex(const char* dir);
// Merge several dirs; later entries override earlier on duplicate func names.
std::unordered_map<std::string, std::string> FuncFileIndexMerge(const char* const* dirs, size_t n);
// Dot commands: // comment text above `function .name` (nearest non-empty // line).
std::unordered_map<std::string, std::string> FuncUserCmdDescIndex(const char* dir);
std::unordered_map<std::string, std::string> FuncUserCmdDescIndexMerge(const char* const* dirs,
                                                                        size_t n);

}  // namespace addoncost
