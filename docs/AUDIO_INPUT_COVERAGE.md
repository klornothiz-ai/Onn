# Audio & Input Coverage (item #2 — libAudio / libPad)

This document records the audio-output and controller-input work added in
expansion round 7, and how to verify it. The goal of item #2 is to make guest
audio output and controller input real and observable in a headless build, not
SDL-only stubs.

## Input — libPad (`libs/controller.cpp`)

### State before
The pad state machine was already complete and injectable:
- Host side feeds events: `ControllerConnect/Disconnect`, `ControllerButton`,
  `ControllerAxis` (values already scaled to 0..255 by the host layer).
- Guest side reads them: `PadOpen` / `PadGetHandle` / `PadReadState` / `PadRead`
  / `PadGetControllerInformation`, returning a faithful 120-byte `PadData`.

But it was unusable as shipped: the `KYTY_SUBSYSTEM_INIT(Controller)` hook is
`inline` (never linked or called), so `g_controller` stayed null and every
`Pad*` call would `EXIT_IF`. There was also a latent `-Werror=sign-compare` in
`ReadStates`.

### Changes
- Added **`ControllerEnsureInitialized()`** (`libs/controller.h` /
  `controller.cpp`): a real, idempotent initializer that creates the state
  machine and connects the host-input pad. The subsystem-init hook now delegates
  to it, and the emulator/tests have a linkable entry point.
- Fixed the `sign-compare` bug (`states_num` vs `uint32_t STATES_MAX`).

### Verification — `tests/hle_libpad_input_test.cpp` (34 checks)
Handle/arg validation; button press/release edge tracking; multiple
simultaneous buttons; analog stick + trigger mapping including the
trigger→L2/R2 digital latch; the queued-sample `PadRead` history path;
disconnect stability. Runs with no SDL and no physical gamepad.

## Audio — libAudio (`AudioOut`)

### State before
`libs/audio.cpp` (the `AudioOut` implementation) is entirely SDL-gated. With no
SDL present the Makefile excludes it and substitutes no-op stubs
(`optional_stubs_sdl.cpp`), so guest audio output silently disappears and
nothing about it can be tested.

### Changes — real SDL-free core
- **`include/audio/headless_audio_sink.hpp` + `src/audio/headless_audio_sink.cpp`**:
  `HeadlessAudioSink` models a PS5 audio-out port and does the real work a
  backend needs before it ever touches a device:
  - format/channel/byte math for all sceAudioOut formats (S16 & float; mono /
    stereo / 8ch, incl. the `*Std` layouts);
  - decodes submitted guest PCM to normalized (−1..1) interleaved float;
  - accounts for output calls, frames, per-channel samples, bytes;
  - measures per-call RMS, whole-stream peak, and playback duration;
  - captures the decoded float stream for exact verification.

  On a host with an audio device the emulator can feed SDL from this same
  decoded stream; headless hosts still run and verify. This is the audio
  analogue of the headless GPU path.

### Verification — `tests/headless_audio_sink_test.cpp` (55 checks)
Format/byte math; open validation; exact int16 decode (full-scale → ±1.0) and
float passthrough; a known 480 Hz sine whose measured RMS matches
amplitude/√2 and whose peak matches the amplitude; multi-grain streaming with
frame + duration accounting; closed-port rejection.

## Honesty boundary / next steps
- The input path is fully real and headless-testable. What a *specific host*
  device or gamepad backend (SDL) maps to these injectors is host-side and
  requires SDL + hardware on the user's machine.
- The audio sink is a real decode + accounting core. It is not yet wired into
  the `AudioOut::AudioOutOutput` HLE entry points as the headless backend (the
  SDL backend is still the only registered one); that wiring — mirroring
  `HeadlessGpuBridge` for audio — is the next audio step. Format decode,
  channel handling and timing are proven now.
- No codec/decode of compressed audio (AT9/Ajm) is claimed here; this is PCM
  output only.
