// ============================================================================
// ProsperoLayer RDNA2 Core - Optional Backend Stubs (SDL absent)
// ============================================================================
// Description: Provides safe no-op registrations for HLE libraries whose real
//              implementations live behind optional dependencies (SDL2).
//              This translation unit is compiled ONLY when SDL2 is not
//              available, so it never collides with the real implementations
//              in libAudio.cpp / libNet.cpp.
// ============================================================================

#include "libs/audio_headless.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

namespace Libs {

// libAudio.cpp provides the real LibAudio HLE registration, but its backing
// audio.cpp backend requires SDL2. Without SDL2 the AudioOut surface itself
// still has a real SDL-free implementation (libs/audio_headless.cpp) backed
// by HeadlessAudioSink: register it so guest sceAudioOut* calls resolve and
// guest PCM flows to the sink on a headless host (round 8, item #2).
LIB_DEFINE(InitAudio_1) {
    LibAudioOutHeadless::InitAudio_1_AudioOut_Headless(s);
}

// libNet.cpp provides InitNet_1 / InitPlatform_1 but requires SDL2.
LIB_DEFINE(InitNet_1) {
    // No-op: network subsystem unavailable without SDL2.
}

LIB_DEFINE(InitPlatform_1) {
    // No-op: platform subsystem unavailable without SDL2.
}

} // namespace Libs
