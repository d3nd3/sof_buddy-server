#pragma once

namespace cmdpark {

struct Config {
    bool  enabled    = false;
    bool  strict     = false;
    float reserveMs  = 0.0f;
    bool  dedicated  = false;
};

void InitCvars();
Config ReadConfig();
void SetOutputs(float cbufMaxMs, long long defers, int cursize, int fillMax);
void RestoreOutputs();

}  // namespace cmdpark

extern "C" {
void CmdPark_Shutdown();
}
