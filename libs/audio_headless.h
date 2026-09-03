#ifndef PROSPEROLAYER_LIBS_AUDIO_HEADLESS_H_
#define PROSPEROLAYER_LIBS_AUDIO_HEADLESS_H_

// ============================================================================
// ProsperoLayer RDNA2 Core - headless AudioOut backend declarations (round 8)
// ----------------------------------------------------------------------------
// Introspection surface of the SDL-free AudioOut backend
// (libs/audio_headless.cpp). Not guest symbols: these exist so tests and
// tools can observe the HeadlessAudioSink behind a guest port handle. Only
// the headless backend implements them; the SDL path (libs/audio.cpp) does
// not, and nothing in the SDL configuration references them.
// ============================================================================

#include "audio/headless_audio_sink.hpp"

#include <vector>

namespace Loader {
class SymbolDatabase;
} // namespace Loader

namespace Libs::Audio::AudioOut {

// Statistics of the sink bound to a guest audio-out port handle
// (1-based, as returned by AudioOutOpen). Returns nullptr when the handle
// does not name an open port.
const PS5::Audio::AudioSinkStats* HeadlessAudioOutStats(int handle);

// The captured, normalized interleaved float stream of that port's sink
// (nullptr when the handle does not name an open port).
const std::vector<float>* HeadlessAudioOutCaptured(int handle);

} // namespace Libs::Audio::AudioOut

namespace Libs::LibAudioOutHeadless {

// LIB_DEFINE(InitAudio_1_AudioOut_Headless) in libs/audio_headless.cpp:
// registers the AudioOut guest surface (the same NIDs libs/libAudio.cpp
// registers under SDL2) against a caller-supplied symbol database.
void InitAudio_1_AudioOut_Headless(Loader::SymbolDatabase* s);

} // namespace Libs::LibAudioOutHeadless

#endif // PROSPEROLAYER_LIBS_AUDIO_HEADLESS_H_
