#pragma once

// Predicted typical cost of the first token of a Cmd_ExecuteString line.
// 0 if unknown or fewer than 4 samples. Built from live EMA in cmd_cost.
namespace cmdcost {
float PredictMs(const char* text);
void Dump();
void Reset();
void WrapAll();    // sync + patch handlers (live measure only)
void UnwrapAll();  // restore handlers; keep orig table for spin
void Spin(const char* name, int n, int slot = 0);  // n<=0: grow until ~2ms
void InstallOrig(const char* name, void (*fn)());  // tests / wrap table
}
