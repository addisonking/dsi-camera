#ifndef APTINA_H
#define APTINA_H

#include "pxi_vars.h"

#include <nds.h>

#ifdef __cplusplus
extern "C" {
#endif

void init(u8 device);
void activate(u8 device);
void deactivate(u8 device);
bool setRearLight(bool enabled);
bool setMode(CaptureMode mode);
bool setExposure(unsigned lines, unsigned gain);
unsigned exposureError(ExposureErrorField field);

#ifdef __cplusplus
}
#endif

#endif // APTINA_H
