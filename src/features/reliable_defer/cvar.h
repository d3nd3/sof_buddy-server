#pragma once

#include "reliable_defer_logic.h"

void RelDef_InitCvars();
void RelDef_PublishGauges(long long queued, long long dripped, long long dropped,
                          int captureBytes, int oldestWait);
RelDefPolicy RelDef_ReadPolicy();

#ifdef __cplusplus
extern "C" void RelDef_Shutdown(void);
#else
void RelDef_Shutdown(void);
#endif
