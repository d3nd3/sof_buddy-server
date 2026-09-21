#pragma once

namespace cmdcost {

void InitCvars();
bool Enabled();
void Publish(float maxMs, const char* maxName, float emaMs, const char* emaName, int n);

}  // namespace cmdcost

extern "C" {
void CmdCost_Shutdown();
}
