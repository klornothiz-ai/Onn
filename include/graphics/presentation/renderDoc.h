#pragma once
// ProsperoLayer PS5 emulator - RenderDoc integration stub (Kyty-compatible)
#include "common/common.h"

namespace Graphics {

// Initializes RenderDoc capture support if the library is loaded.
void RenderDocInit();
// Returns true when a capture is in progress.
bool RenderDocIsCaptureInProgress();

} // namespace Graphics
