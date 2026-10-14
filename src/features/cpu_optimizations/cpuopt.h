#pragma once

#include "buddy_import.h"
#include "log.h"

#include <cstddef>
#include <windows.h>

// Folder-wide gates for cpu_optimizations. State lives in cpuopt.cpp (one copy).

void* CpuOpt_MasterCvar();
void* CpuOpt_StrictCvar();
bool CpuOpt_Enabled();
bool CpuOpt_Strict();

using CpuOptFatalFn = void (*)(const char*);
CpuOptFatalFn& CpuOpt_OnFatal();

constexpr int kCpuOptCbufHist = 5;

void CpuOpt_ResetCbufHist();
void CpuOpt_NoteCbuf(int bytes);
void CpuOpt_Fatal(const char* why);

void ClampMonitor_LogSessionSummary();
void TickPace_LogSessionSummary();
